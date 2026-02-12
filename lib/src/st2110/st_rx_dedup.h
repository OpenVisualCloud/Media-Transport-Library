/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 *
 * Shared ST 2022-7 dedup logic for ST30 (audio), ST40 (ancillary) and ST41
 * (fast-metadata) RX sessions.
 *
 * ST20 video uses a per-frame bitmap approach and is not covered here.
 */

#ifndef _MT_LIB_ST_RX_DEDUP_H_
#define _MT_LIB_ST_RX_DEDUP_H_

#include <rte_mbuf.h>
#include <stdbool.h>
#include <stdint.h>

#include "../mt_log.h"   /* warn() */
#include "mtl_api.h"     /* MTL_SESSION_PORT_MAX, enum mtl_session_port */
#include "mtl_sch_api.h" /* MTL_TASKLET_ALL_DONE, MTL_TASKLET_HAS_PENDING */
#include "st_api.h"      /* struct st_rfc3550_rtp_hdr */
#include "st_pkt.h"      /* struct st_rfc3550_hdr */

#ifndef ST_SESSION_REDUNDANT_ERROR_THRESHOLD
#define ST_SESSION_REDUNDANT_ERROR_THRESHOLD (20)
#endif

/** Width of the received-sequence bitmap (must be power of 2). */
#define ST_RX_DEDUP_BITMAP_BITS (64)

/*
 * Wrap-around-safe sequence comparisons.
 * Duplicated from mt_util.h to avoid a circular include path
 * (mt_util.h → mt_main.h → st_header.h → st_rx_dedup.h).
 */
#ifndef _MT_SEQ_CMP_DEFINED_
#define _MT_SEQ_CMP_DEFINED_
static inline bool st_dedup_seq16_gt(uint16_t a, uint16_t b) {
  return ((uint16_t)(a - b) & 0x8000u) == 0 && a != b;
}
static inline bool st_dedup_seq32_gt(uint32_t a, uint32_t b) {
  return ((uint32_t)(a - b) & 0x80000000u) == 0 && a != b;
}
#endif

/* Forward declaration — defined in datapath/mt_queue.h */
struct mt_rxq_entry;
uint16_t mt_rxq_burst(struct mt_rxq_entry* entry, struct rte_mbuf** rx_pkts,
                      const uint16_t nb_pkts);

/* ── dedup mode ─────────────────────────────────────────────────────────── */

/** How the dedup decides a packet is redundant. */
enum st_rx_dedup_mode {
  /** ST30: timestamp-only – drop when timestamp is not strictly advancing. */
  ST_RX_DEDUP_MODE_TIMESTAMP,
  /** ST40 / ST41: both timestamp and seq_id must advance. */
  ST_RX_DEDUP_MODE_TIMESTAMP_AND_SEQ,
};

/* ── dedup state (embed in each session struct) ─────────────────────────── */

struct st_rx_dedup {
  /** Dedup mode for this session. */
  enum st_rx_dedup_mode mode;
  /** Number of ports (1 = single, 2 = redundant). */
  int num_port;
  /** Session index – for log messages only. */
  int idx;

  /* --- per-port sequence tracking --- */
  int latest_seq_id[MTL_SESSION_PORT_MAX];

  /* --- session-level state --- */
  int session_seq_id;
  int64_t tmstamp;

  /* --- redundant-error threshold guard --- */
  int redundant_error_cnt[MTL_SESSION_PORT_MAX];

  /* --- 64-bit received-sequence bitmap (TIMESTAMP_AND_SEQ mode only) ---
   * Tracks which of the last 64 sequence numbers have been received.
   * Enables cross-burst gap-fill: a late R packet whose seq is behind
   * session_seq_id but whose bitmap bit is NOT set is a gap-fill, not a dup.
   */
  uint64_t recv_bitmap;
  uint16_t bitmap_base; /**< seq corresponding to bit 0 */
};

/* ── dedup check result ─────────────────────────────────────────────────── */

/** Returned by st_rx_dedup_check(). */
struct st_rx_dedup_result {
  /** true → drop this packet (redundant). */
  bool drop;
  /** true → per-port sequence was non-continuous. */
  bool port_seq_discontinuity;
  /** true → session-level sequence was non-continuous (only when !drop). */
  bool session_seq_discontinuity;
  /** true → redundant threshold was reached and the packet is force-accepted. */
  bool threshold_override;
};

/* ── helpers ────────────────────────────────────────────────────────────── */

static inline void st_rx_dedup_init(struct st_rx_dedup* d, enum st_rx_dedup_mode mode,
                                    int num_port, int idx) {
  d->mode = mode;
  d->num_port = num_port;
  d->idx = idx;
  d->session_seq_id = -1;
  d->tmstamp = -1;
  d->recv_bitmap = 0;
  d->bitmap_base = 0;
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    d->latest_seq_id[i] = -1;
    d->redundant_error_cnt[i] = 0;
  }
}

static inline void st_rx_dedup_reset(struct st_rx_dedup* d) {
  d->session_seq_id = -1;
  d->tmstamp = -1;
  d->recv_bitmap = 0;
  d->bitmap_base = 0;
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    d->latest_seq_id[i] = -1;
    d->redundant_error_cnt[i] = 0;
  }
}

/**
 * Slide the bitmap window so that @p seq fits, then set its bit.
 * Call only for accepted packets in TIMESTAMP_AND_SEQ mode.
 */
static inline void st_rx_dedup_bitmap_mark(struct st_rx_dedup* d, uint16_t seq) {
  uint16_t offset = (uint16_t)(seq - d->bitmap_base);

  /* If seq is ahead of the window, slide forward */
  if (offset >= ST_RX_DEDUP_BITMAP_BITS) {
    uint16_t shift = offset - (ST_RX_DEDUP_BITMAP_BITS - 1);
    if (shift >= ST_RX_DEDUP_BITMAP_BITS)
      d->recv_bitmap = 0; /* full jump — clear everything */
    else
      d->recv_bitmap >>= shift;
    d->bitmap_base = (uint16_t)(d->bitmap_base + shift);
    offset = (uint16_t)(seq - d->bitmap_base);
  }

  d->recv_bitmap |= (1ULL << offset);
}

/**
 * Check whether @p seq was already received.
 * Returns true if the bit is set (true duplicate) or if seq fell off the
 * bitmap tail (too old — treat as stale).
 */
static inline bool st_rx_dedup_bitmap_test(const struct st_rx_dedup* d, uint16_t seq) {
  uint16_t offset = (uint16_t)(seq - d->bitmap_base);

  /* Behind the bitmap window → too old, treat as already-received */
  if (offset & 0x8000u) return true; /* negative in unsigned = wrapped */

  /* Beyond bitmap range shouldn't happen for a "not advancing" seq,
   * but be safe: if it's ahead, it's definitely not received yet. */
  if (offset >= ST_RX_DEDUP_BITMAP_BITS) return false;

  return (d->recv_bitmap & (1ULL << offset)) != 0;
}

/**
 * Core dedup check.  Call for every accepted (payload-type / ssrc validated)
 * packet.  Returns a result struct telling the caller whether to drop and
 * what counters to bump.
 *
 * @param d      Dedup state.
 * @param seq_id RTP sequence number (already in host order).
 * @param tmstamp RTP timestamp (already in host order).
 * @param s_port Session port index (MTL_SESSION_PORT_P or _R).
 */
static inline struct st_rx_dedup_result st_rx_dedup_check(struct st_rx_dedup* d,
                                                          uint16_t seq_id,
                                                          uint32_t tmstamp,
                                                          enum mtl_session_port s_port) {
  struct st_rx_dedup_result r = {false, false, false, false};

  /* --- first-packet initialisation --- */
  if (unlikely(d->latest_seq_id[s_port] == -1)) d->latest_seq_id[s_port] = seq_id - 1;
  if (unlikely(d->session_seq_id == -1)) d->session_seq_id = seq_id - 1;
  if (unlikely(d->tmstamp == -1)) d->tmstamp = (int64_t)tmstamp - 1;

  /* ── 1. per-port sequence continuity ────────────────────────────────── */
  if (seq_id != (uint16_t)(d->latest_seq_id[s_port] + 1)) {
    r.port_seq_discontinuity = true;
  }
  d->latest_seq_id[s_port] = seq_id;

  /* ── 2. redundancy check ────────────────────────────────────────────── */
  bool is_redundant = false;

  if (d->mode == ST_RX_DEDUP_MODE_TIMESTAMP_AND_SEQ) {
    /* ST40 / ST41: check bitmap — a packet whose seq is behind the
     * high-water mark is only redundant if we already received it. */
    if (st_dedup_seq32_gt(d->tmstamp, tmstamp)) {
      /* Timestamp is strictly older than anything we've seen — but the
       * bitmap can still tell us whether this exact seq was received. */
      is_redundant = st_rx_dedup_bitmap_test(d, seq_id);
    } else if (!st_dedup_seq16_gt(seq_id, d->session_seq_id)) {
      /* seq_id is not advancing — redundant only if bitmap bit is set */
      is_redundant = st_rx_dedup_bitmap_test(d, seq_id);
    }
  } else {
    /* ST30: only check timestamp */
    if (!st_dedup_seq32_gt(tmstamp, d->tmstamp)) {
      is_redundant = true;
    }
  }

  if (is_redundant) {
    d->redundant_error_cnt[s_port]++;

    /* Check whether ALL ports exceeded the threshold */
    bool all_exceeded = true;
    for (int i = 0; i < d->num_port; i++) {
      if (d->redundant_error_cnt[i] < ST_SESSION_REDUNDANT_ERROR_THRESHOLD) {
        all_exceeded = false;
        break;
      }
    }

    if (!all_exceeded) {
      r.drop = true;
      return r;
    }

    /* Threshold override – force-accept to avoid deadlock on stream reset */
    r.threshold_override = true;
    warn("%s(%d), redundant threshold reached, accept seq %u (old %d) ts %u (old %ld)\n",
         __func__, d->idx, seq_id, d->session_seq_id, tmstamp, d->tmstamp);
  }

  d->redundant_error_cnt[s_port] = 0;

  /* ── 3. session-level sequence continuity (only for accepted packets) ── */
  if (seq_id != (uint16_t)(d->session_seq_id + 1)) {
    r.session_seq_discontinuity = true;
  }

  /* Update session state */
  if (d->mode == ST_RX_DEDUP_MODE_TIMESTAMP_AND_SEQ) {
    st_rx_dedup_bitmap_mark(d, seq_id);
    /* Only advance high-water marks — gap-fill packets must not regress them */
    if (st_dedup_seq16_gt(seq_id, (uint16_t)d->session_seq_id))
      d->session_seq_id = seq_id;
    if (st_dedup_seq32_gt(tmstamp, (uint32_t)d->tmstamp)) d->tmstamp = (int64_t)tmstamp;
  } else {
    d->session_seq_id = seq_id;
    d->tmstamp = (int64_t)tmstamp;
  }

  return r;
}

/* ── Merge-sort burst helper for ST 2022-7 ──────────────────────────────
 *
 * When num_port == 2, burst from both port queues and feed packets into the
 * per-packet handler in RTP sequence-number order.  This ensures gap-filling
 * packets from the redundant path are processed BEFORE later packets from the
 * primary path advance session_seq_id.
 *
 * The per-packet handler signature must be:
 *   int handler(void* impl, void* session, struct rte_mbuf* mbuf,
 *               enum mtl_session_port s_port);
 *
 * Returns MTL_TASKLET_ALL_DONE or MTL_TASKLET_HAS_PENDING.
 */

/** Extract RTP seq_number from any ST2110 mbuf (universally at the same offset). */
static inline uint16_t st_rx_dedup_mbuf_seq(struct rte_mbuf* mbuf) {
  size_t hdr_off = sizeof(struct st_rfc3550_hdr) - sizeof(struct st_rfc3550_rtp_hdr);
  struct st_rfc3550_rtp_hdr* rtp =
      rte_pktmbuf_mtod_offset(mbuf, struct st_rfc3550_rtp_hdr*, hdr_off);
  return ntohs(rtp->seq_number);
}

/**
 * Callback type for per-packet handling during merge-sort burst.
 *
 * @param impl   Opaque implementation context (e.g. struct mtl_main_impl*).
 * @param session Opaque session pointer.
 * @param mbuf   The packet.
 * @param s_port Session port the packet arrived on.
 * @return 0 on success, negative on error.
 */
typedef int (*st_rx_dedup_pkt_handler)(void* impl, void* session, struct rte_mbuf* mbuf,
                                       enum mtl_session_port s_port);

/**
 * Merge-sort tasklet helper.
 *
 * @param rxq        Per-port RX queue entries (array of MTL_SESSION_PORT_MAX).
 * @param num_port   Number of active ports (1 or 2).
 * @param burst_size Max burst size per port.
 * @param impl       Opaque impl pointer forwarded to handler.
 * @param session    Opaque session pointer forwarded to handler.
 * @param handler    Per-packet callback.
 * @return MTL_TASKLET_ALL_DONE or MTL_TASKLET_HAS_PENDING.
 */
static inline int st_rx_dedup_tasklet(struct mt_rxq_entry* rxq[MTL_SESSION_PORT_MAX],
                                      int num_port, uint16_t burst_size, void* impl,
                                      void* session, st_rx_dedup_pkt_handler handler) {
  struct rte_mbuf* mbuf_p[burst_size];
  struct rte_mbuf* mbuf_r[burst_size];
  uint16_t rv_p = 0, rv_r = 0;

  /* ── single port fast path ── */
  if (num_port <= 1) {
    if (rxq[MTL_SESSION_PORT_P])
      rv_p = mt_rxq_burst(rxq[MTL_SESSION_PORT_P], &mbuf_p[0], burst_size);
    if (rv_p) {
      for (uint16_t i = 0; i < rv_p; i++)
        handler(impl, session, mbuf_p[i], MTL_SESSION_PORT_P);
      rte_pktmbuf_free_bulk(&mbuf_p[0], rv_p);
      return MTL_TASKLET_HAS_PENDING;
    }
    return MTL_TASKLET_ALL_DONE;
  }

  /* ── burst from both ports ── */
  if (rxq[MTL_SESSION_PORT_P])
    rv_p = mt_rxq_burst(rxq[MTL_SESSION_PORT_P], &mbuf_p[0], burst_size);
  if (rxq[MTL_SESSION_PORT_R])
    rv_r = mt_rxq_burst(rxq[MTL_SESSION_PORT_R], &mbuf_r[0], burst_size);

  if (!rv_p && !rv_r) return MTL_TASKLET_ALL_DONE;

  /* Only one port delivered – no merge needed */
  if (!rv_r) {
    for (uint16_t i = 0; i < rv_p; i++)
      handler(impl, session, mbuf_p[i], MTL_SESSION_PORT_P);
    rte_pktmbuf_free_bulk(&mbuf_p[0], rv_p);
    return MTL_TASKLET_HAS_PENDING;
  }
  if (!rv_p) {
    for (uint16_t i = 0; i < rv_r; i++)
      handler(impl, session, mbuf_r[i], MTL_SESSION_PORT_R);
    rte_pktmbuf_free_bulk(&mbuf_r[0], rv_r);
    return MTL_TASKLET_HAS_PENDING;
  }

  /* ── two-way merge by seq_id ── */
  uint16_t i_p = 0, i_r = 0;

  while (i_p < rv_p && i_r < rv_r) {
    uint16_t seq_p = st_rx_dedup_mbuf_seq(mbuf_p[i_p]);
    uint16_t seq_r = st_rx_dedup_mbuf_seq(mbuf_r[i_r]);

    if (seq_p == seq_r) {
      /* Same seq – process both; dedup keeps the first */
      handler(impl, session, mbuf_p[i_p], MTL_SESSION_PORT_P);
      handler(impl, session, mbuf_r[i_r], MTL_SESSION_PORT_R);
      i_p++;
      i_r++;
    } else if (st_dedup_seq16_gt(seq_r, seq_p)) {
      handler(impl, session, mbuf_p[i_p], MTL_SESSION_PORT_P);
      i_p++;
    } else {
      handler(impl, session, mbuf_r[i_r], MTL_SESSION_PORT_R);
      i_r++;
    }
  }

  while (i_p < rv_p) {
    handler(impl, session, mbuf_p[i_p], MTL_SESSION_PORT_P);
    i_p++;
  }
  while (i_r < rv_r) {
    handler(impl, session, mbuf_r[i_r], MTL_SESSION_PORT_R);
    i_r++;
  }

  rte_pktmbuf_free_bulk(&mbuf_p[0], rv_p);
  rte_pktmbuf_free_bulk(&mbuf_r[0], rv_r);
  return MTL_TASKLET_HAS_PENDING;
}

#endif /* _MT_LIB_ST_RX_DEDUP_H_ */
