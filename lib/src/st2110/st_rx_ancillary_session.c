/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_rx_ancillary_session.h"

#include "../datapath/mt_queue.h"
#include "../mt_log.h"
#include "../mt_stat.h"
#include "st_ancillary_transmitter.h"

#ifdef MTL_ENABLE_FUZZING_ST40
#define ST40_FUZZ_LOG(...) info(__VA_ARGS__)
#else
#define ST40_FUZZ_LOG(...) \
  do {                     \
  } while (0)
#endif

/* call rx_ancillary_session_put always if get successfully */
static inline struct st_rx_ancillary_session_impl* rx_ancillary_session_get(
    struct st_rx_ancillary_sessions_mgr* mgr, int idx) {
  rte_spinlock_lock(&mgr->mutex[idx]);
  struct st_rx_ancillary_session_impl* s = mgr->sessions[idx];
  if (!s) rte_spinlock_unlock(&mgr->mutex[idx]);
  return s;
}

/* call rx_ancillary_session_put always if get successfully */
static inline struct st_rx_ancillary_session_impl* rx_ancillary_session_try_get(
    struct st_rx_ancillary_sessions_mgr* mgr, int idx) {
  if (!rte_spinlock_trylock(&mgr->mutex[idx])) return NULL;
  struct st_rx_ancillary_session_impl* s = mgr->sessions[idx];
  if (!s) rte_spinlock_unlock(&mgr->mutex[idx]);
  return s;
}

/* call rx_ancillary_session_put always if get successfully */
static inline struct st_rx_ancillary_session_impl* rx_ancillary_session_get_timeout(
    struct st_rx_ancillary_sessions_mgr* mgr, int idx, int timeout_us) {
  if (!mt_spinlock_lock_timeout(mgr->parent, &mgr->mutex[idx], timeout_us)) return NULL;
  struct st_rx_ancillary_session_impl* s = mgr->sessions[idx];
  if (!s) rte_spinlock_unlock(&mgr->mutex[idx]);
  return s;
}

/* call rx_ancillary_session_put always if get successfully */
static inline bool rx_ancillary_session_get_empty(
    struct st_rx_ancillary_sessions_mgr* mgr, int idx) {
  rte_spinlock_lock(&mgr->mutex[idx]);
  struct st_rx_ancillary_session_impl* s = mgr->sessions[idx];
  if (s) {
    rte_spinlock_unlock(&mgr->mutex[idx]); /* not null, unlock it */
    return false;
  } else {
    return true;
  }
}

static inline void rx_ancillary_session_put(struct st_rx_ancillary_sessions_mgr* mgr,
                                            int idx) {
  rte_spinlock_unlock(&mgr->mutex[idx]);
}

static inline uint16_t rx_ancillary_queue_id(struct st_rx_ancillary_session_impl* s,
                                             enum mtl_session_port s_port) {
  return mt_rxq_queue_id(s->rxq[s_port]);
}

static int rx_ancillary_session_init(struct st_rx_ancillary_sessions_mgr* mgr,
                                     struct st_rx_ancillary_session_impl* s, int idx) {
  MTL_MAY_UNUSED(mgr);
  s->idx = idx;
  return 0;
}

static int rx_ancillary_sessions_tasklet_start(void* priv) {
  struct st_rx_ancillary_sessions_mgr* mgr = priv;
  int idx = mgr->idx;

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int rx_ancillary_sessions_tasklet_stop(void* priv) {
  struct st_rx_ancillary_sessions_mgr* mgr = priv;
  int idx = mgr->idx;

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

/* T4: FRAME_LEVEL assembler — ports the body of pipeline rx_st40p_rtp_ready()
 * into the transport. Owns the frame slot pool allocated in T2; on marker,
 * fills cur_meta and calls ops->notify_frame_ready(priv, addr, &meta).
 *
 * Slot lifecycle (single writer per side):
 *   FREE      → tasklet picks for new frame      → RECEIVING
 *   RECEIVING → marker arrives                    → READY (notify) → IN_USER
 *   RECEIVING → tmstamp change, num_port>1        → PENDING (wait late marker)
 *   PENDING   → matching late marker arrives      → READY (notify) → IN_USER
 *   IN_USER   → app calls st40_rx_put_framebuff   → FREE
 */

static struct st_rx_anc_frame_slot* rx_anc_get_free_slot(
    struct st_rx_ancillary_session_impl* s) {
  for (uint16_t i = 0; i < s->frame_slots_cnt; i++) {
    struct st_rx_anc_frame_slot* slot = &s->frame_slots[i];
    if (slot->state == ST_RX_ANC_SLOT_FREE) return slot;
  }
  return NULL;
}

static void rx_anc_slot_init_frame(struct st_rx_anc_frame_slot* slot, uint32_t tmstamp,
                                   bool interlaced, bool second_field, uint64_t recv_ts) {
  slot->state = ST_RX_ANC_SLOT_RECEIVING;
  slot->rtp_timestamp = tmstamp;
  slot->udw_buffer_fill = 0;
  slot->meta_num = 0;
  slot->pkts_total = 0;
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) slot->pkts_recv[i] = 0;
  slot->receive_timestamp = recv_ts;
  slot->interlaced = interlaced;
  slot->second_field = second_field;
  slot->rtp_marker = false;
  /* T5 seq trackers */
  slot->seq_bitmap = 0;
  slot->seq_base = 0;
  slot->seq_max_offset = 0;
  slot->seq_base_valid = false;
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    slot->port_last_seq[i] = -1;
    slot->port_seq_lost[i] = 0;
    slot->port_seq_discont[i] = false;
  }
}

/* T5: record one accepted packet's seq into the slot's per-frame trackers.
 * - Updates the session-merged bitmap (handles backward arrivals via rebase).
 * - Updates per-port last_seq + per-port lost gaps.
 * Backward seqs that don't fit (frame too large for 64-bit bitmap) silently
 * do not contribute to the bitmap; they still update per-port. */
static void rx_anc_slot_record_seq(struct st_rx_anc_frame_slot* slot, uint16_t seq_id,
                                   enum mtl_session_port s_port) {
  if (!slot->seq_base_valid) {
    slot->seq_base = seq_id;
    slot->seq_base_valid = true;
    slot->seq_bitmap = 1; /* offset 0 set */
    slot->seq_max_offset = 0;
  } else {
    int32_t delta = (int16_t)(seq_id - slot->seq_base);
    if (delta < 0) {
      /* Late backward arrival: rebase if shift fits the bitmap. */
      uint32_t shift = (uint32_t)(-delta);
      if (shift < ST_RX_ANC_BITMAP_BITS) {
        slot->seq_bitmap <<= shift;
        slot->seq_bitmap |= 1ULL; /* new offset 0 = the late seq */
        slot->seq_base = seq_id;
        slot->seq_max_offset += shift;
      }
      /* else: too far back to track; bitmap unchanged. */
    } else {
      uint32_t off = (uint32_t)delta;
      if (off < ST_RX_ANC_BITMAP_BITS) {
        slot->seq_bitmap |= ((uint64_t)1 << off);
        if (off > slot->seq_max_offset) slot->seq_max_offset = (uint16_t)off;
      }
    }
  }

  if (s_port < MTL_SESSION_PORT_MAX) {
    if (slot->port_last_seq[s_port] >= 0) {
      uint16_t expected = (uint16_t)(slot->port_last_seq[s_port] + 1);
      if (expected != seq_id) {
        slot->port_seq_discont[s_port] = true;
        if (mt_seq16_greater(seq_id, expected))
          slot->port_seq_lost[s_port] += (uint32_t)(uint16_t)(seq_id - expected);
      }
    }
    /* Only advance — don't rewind on backward arrivals. */
    if (slot->port_last_seq[s_port] < 0 ||
        mt_seq16_greater(seq_id, (uint16_t)slot->port_last_seq[s_port]))
      slot->port_last_seq[s_port] = seq_id;
  }
}

/* Parse one RTP packet's ANC chunks into the slot. Returns 0 on full parse,
 * <0 if any ANC chunk failed validation (UDW parity, checksum, overflow).
 * Best-effort: partial successes are kept (matches pipeline behavior). */
static int rx_anc_slot_parse_pkt(struct st_rx_ancillary_session_impl* s,
                                 struct st_rx_anc_frame_slot* slot,
                                 struct st40_rfc8331_rtp_hdr* hdr, uint16_t len) {
  uint32_t anc_count = hdr->first_hdr_chunk.anc_count;
  uint8_t* payload = (uint8_t*)(hdr + 1);
  uint32_t payload_room = (len > sizeof(*hdr)) ? (len - sizeof(*hdr)) : 0;
  uint32_t payload_offset = 0;
  int ret = 0;

  for (uint32_t anc_idx = 0; anc_idx < anc_count; anc_idx++) {
    if (slot->meta_num >= ST40_MAX_META) {
      warn("%s(%d), meta slots exhausted at %u\n", __func__, s->idx, slot->meta_num);
      ret = -ENOSPC;
      break;
    }
    if (payload_offset + sizeof(struct st40_rfc8331_payload_hdr) > payload_room) {
      warn("%s(%d), payload offset %u exceeds room %u\n", __func__, s->idx,
           payload_offset, payload_room);
      ret = -EINVAL;
      break;
    }

    struct st40_rfc8331_payload_hdr* payload_hdr =
        (struct st40_rfc8331_payload_hdr*)(payload + payload_offset);
    struct st40_rfc8331_payload_hdr hdr_local = {0};
    hdr_local.swapped_first_hdr_chunk = ntohl(payload_hdr->swapped_first_hdr_chunk);
    hdr_local.swapped_second_hdr_chunk = ntohl(payload_hdr->swapped_second_hdr_chunk);

    uint16_t udw_words = hdr_local.second_hdr_chunk.data_count & 0xFF;
    struct st40_meta* meta_entry = &slot->meta[slot->meta_num];
    meta_entry->c = hdr_local.first_hdr_chunk.c;
    meta_entry->line_number = hdr_local.first_hdr_chunk.line_number;
    meta_entry->hori_offset = hdr_local.first_hdr_chunk.horizontal_offset;
    meta_entry->s = hdr_local.first_hdr_chunk.s;
    meta_entry->stream_num = hdr_local.first_hdr_chunk.stream_num;
    meta_entry->did = hdr_local.second_hdr_chunk.did & 0xFF;
    meta_entry->sdid = hdr_local.second_hdr_chunk.sdid & 0xFF;
    meta_entry->udw_size = udw_words;
    meta_entry->udw_offset = slot->udw_buffer_fill;

    /* Match TX padding: floor to bytes then pad to next 4-byte multiple */
    uint32_t total_bits = (3 + udw_words + 1) * 10;
    uint32_t total_size = total_bits / 8;
    uint32_t total_size_aligned = (total_size + 3) & ~0x3U;
    uint32_t anc_packet_bytes =
        sizeof(struct st40_rfc8331_payload_hdr) - 4 + total_size_aligned;
    if (payload_offset + anc_packet_bytes > payload_room) {
      warn("%s(%d), anc bytes %u + offset %u > room %u\n", __func__, s->idx,
           anc_packet_bytes, payload_offset, payload_room);
      ret = -EINVAL;
      break;
    }

    bool meta_valid = true;
    if (udw_words > 0) {
      uint8_t* udw_src = (uint8_t*)&payload_hdr->second_hdr_chunk;
      uint32_t original_fill = slot->udw_buffer_fill;
      for (uint16_t udw_idx = 0; udw_idx < udw_words; udw_idx++) {
        uint16_t udw = st40_get_udw(udw_idx + 3, udw_src);
        if (!st40_check_parity_bits(udw)) {
          warn("%s(%d), UDW parity fail anc=%u word=%u\n", __func__, s->idx, anc_idx,
               udw_idx);
          meta_valid = false;
          break;
        }
        if (slot->udw_buffer_fill >= slot->udw_buf_size) {
          warn("%s(%d), UDW buffer overflow anc=%u\n", __func__, s->idx, anc_idx);
          meta_valid = false;
          break;
        }
        slot->udw_buf[slot->udw_buffer_fill++] = (uint8_t)(udw & 0xFF);
      }
      if (meta_valid) {
        uint16_t cs_udw = st40_get_udw(udw_words + 3, udw_src);
        uint16_t cs_calc = st40_calc_checksum(3 + udw_words, udw_src);
        if (cs_udw != cs_calc) {
          warn("%s(%d), checksum mismatch anc=%u (0x%03x != 0x%03x)\n", __func__, s->idx,
               anc_idx, cs_udw, cs_calc);
          meta_valid = false;
        }
      }
      if (!meta_valid) {
        slot->udw_buffer_fill = original_fill;
        s->stat_anc_pkt_parse_err++;
        ret = -EINVAL;
        break;
      }
    }

    slot->meta_num++;
    payload_offset += anc_packet_bytes;
  }
  return ret;
}

static void rx_anc_slot_deliver(struct mtl_main_impl* impl,
                                struct st_rx_ancillary_session_impl* s,
                                struct st_rx_anc_frame_slot* slot) {
  struct st40_rx_ops* ops = &s->ops;
  struct st40_rx_frame_meta* meta = &slot->cur_meta;

  memset(meta, 0, sizeof(*meta));
  meta->meta = slot->meta;
  meta->meta_num = slot->meta_num;
  meta->udw_buffer_fill = slot->udw_buffer_fill;
  meta->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
  meta->timestamp = slot->rtp_timestamp;
  meta->rtp_timestamp = slot->rtp_timestamp;
  meta->interlaced = slot->interlaced;
  meta->second_field = slot->second_field;
  meta->rtp_marker = slot->rtp_marker;
  /* T5: derive seq_lost / seq_discont from this slot's own per-frame bitmap.
   * expected = max_offset + 1 (slots with at least one packet).
   * seen     = popcount(bitmap).
   * Cross-port redundancy is naturally accounted for: late R packets that
   * fill in P gaps flip the corresponding bit, so they don't show as loss. */
  if (slot->seq_base_valid) {
    uint32_t expected = (uint32_t)slot->seq_max_offset + 1;
    uint32_t seen = (uint32_t)__builtin_popcountll(slot->seq_bitmap);
    meta->seq_lost = expected > seen ? (expected - seen) : 0;
  } else {
    meta->seq_lost = 0;
  }
  meta->seq_discont = meta->seq_lost > 0;
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    meta->port_seq_lost[i] = slot->port_seq_lost[i];
    meta->port_seq_discont[i] = slot->port_seq_discont[i];
    meta->pkts_recv[i] = slot->pkts_recv[i];
  }
  meta->pkts_total = slot->pkts_total;
  meta->status = meta->seq_discont ? ST_FRAME_STATUS_CORRUPTED : ST_FRAME_STATUS_COMPLETE;

  s->stat_anc_frames_ready++;
  /* Note: stat_frames_received is incremented in handle_pkt() on each new
   * timestamp (i.e. once per unique frame seen on the wire). Do NOT bump it
   * again here — that double-counted FRAME_LEVEL deliveries 2x in the periodic
   * stat dump and led to bogus "regression" reports vs the real pipeline get
   * count. The pipeline's stat_anc_frames_ready / get_succ are the source of
   * truth for delivered frames. */

  /* Hand off to app. Slot ownership transfers until put_framebuff. */
  enum st_rx_anc_slot_state prev_state = slot->state;
  slot->state = ST_RX_ANC_SLOT_IN_USER;
  uint64_t tsc_start = mt_sessions_time_measure(impl) ? mt_get_tsc(impl) : 0;
  int ret = ops->notify_frame_ready
                ? ops->notify_frame_ready(ops->priv, slot->udw_buf, meta)
                : -EIO;
  if (tsc_start) {
    uint32_t delta_us = (mt_get_tsc(impl) - tsc_start) / NS_PER_US;
    s->stat_max_notify_rtp_us = RTE_MAX(s->stat_max_notify_rtp_us, delta_us);
  }
  if (ret < 0) {
    /* App rejected — reclaim slot ourselves so the pool isn't starved. */
    err("%s(%d), notify_frame_ready ret %d, reclaim slot %u\n", __func__, s->idx, ret,
        slot->idx);
    slot->state = ST_RX_ANC_SLOT_FREE;
    MTL_MAY_UNUSED(prev_state);
  }
}

static int rx_ancillary_session_assemble_pkt(struct mtl_main_impl* impl,
                                             struct st_rx_ancillary_session_impl* s,
                                             struct rte_mbuf* mbuf, uint16_t seq_id,
                                             uint32_t tmstamp, uint16_t pkt_len,
                                             enum mtl_session_port s_port) {
  s->stat_assemble_dispatched++;
  MTL_MAY_UNUSED(seq_id);
  MTL_MAY_UNUSED(pkt_len);

  /* Re-derive header pointer (handle_pkt has already ntohl'd first chunk). */
  size_t hdr_offset = sizeof(struct st_rfc3550_hdr) - sizeof(struct st_rfc3550_rtp_hdr);
  struct st40_rfc8331_rtp_hdr* hdr =
      rte_pktmbuf_mtod_offset(mbuf, struct st40_rfc8331_rtp_hdr*, hdr_offset);

  uint8_t f_bits = hdr->first_hdr_chunk.f & 0x3;
  bool pkt_interlaced = (f_bits & 0x2) ? true : false;
  bool pkt_second_field = pkt_interlaced && (f_bits & 0x1);
  bool marker = hdr->base.marker ? true : false;
  uint16_t len = mbuf->data_len - hdr_offset;
  uint64_t recv_ts = 0; /* mt_mbuf_time_stamp needs phy_port; skip for now */

  /* Pick or allocate the target slot. */
  struct st_rx_anc_frame_slot* slot = NULL;

  if (s->anc_pending_slot && s->anc_pending_slot->rtp_timestamp == tmstamp) {
    /* Late packet for the pending frame — feed it. */
    slot = s->anc_pending_slot;
  } else if (s->anc_inflight_slot) {
    if (s->anc_inflight_slot->rtp_timestamp == tmstamp) {
      slot = s->anc_inflight_slot;
    } else {
      /* Timestamp moved on. Roll inflight to PENDING (multi-port) or READY (single). */
      struct st_rx_anc_frame_slot* old = s->anc_inflight_slot;
      s->anc_inflight_slot = NULL;
      if (s->ops.num_port > 1) {
        if (s->anc_pending_slot) {
          /* Force-deliver existing pending — no late marker arrived. */
          struct st_rx_anc_frame_slot* prev_pending = s->anc_pending_slot;
          s->anc_pending_slot = NULL;
          prev_pending->state = ST_RX_ANC_SLOT_READY;
          rx_anc_slot_deliver(impl, s, prev_pending);
        }
        old->state = ST_RX_ANC_SLOT_PENDING;
        s->anc_pending_slot = old;
      } else {
        old->state = ST_RX_ANC_SLOT_READY;
        rx_anc_slot_deliver(impl, s, old);
      }
    }
  }

  if (!slot) {
    /* Need a fresh slot for this rtp_timestamp. */
    slot = rx_anc_get_free_slot(s);
    if (!slot) {
      s->stat_anc_frames_dropped++;
      dbg("%s(%d), frame slot pool exhausted, drop tmstamp %u\n", __func__, s->idx,
          tmstamp);
      return -EBUSY;
    }
    rx_anc_slot_init_frame(slot, tmstamp, pkt_interlaced, pkt_second_field, recv_ts);
    s->anc_inflight_slot = slot;
  } else {
    /* Existing slot: keep the earliest receive_timestamp seen. */
    if (recv_ts && (!slot->receive_timestamp || slot->receive_timestamp > recv_ts))
      slot->receive_timestamp = recv_ts;
    /* Refresh interlace metadata if a later packet disagrees with our guess. */
    if (slot->interlaced != pkt_interlaced || slot->second_field != pkt_second_field) {
      slot->interlaced = pkt_interlaced;
      slot->second_field = pkt_second_field;
    }
  }

  slot->pkts_total++;
  if (s_port < MTL_SESSION_PORT_MAX) slot->pkts_recv[s_port]++;
  rx_anc_slot_record_seq(slot, seq_id, s_port);

  /* Parse ANC payload into slot. Errors are recorded; assembly continues
   * because partial frames are still useful (mirrors pipeline behavior). */
  rx_anc_slot_parse_pkt(s, slot, hdr, len);

  if (marker) {
    slot->rtp_marker = true;
    if (slot == s->anc_pending_slot)
      s->anc_pending_slot = NULL;
    else
      s->anc_inflight_slot = NULL;
    slot->state = ST_RX_ANC_SLOT_READY;
    rx_anc_slot_deliver(impl, s, slot);
  }
  return 0;
}

static int rx_ancillary_session_handle_pkt(struct mtl_main_impl* impl,
                                           struct st_rx_ancillary_session_impl* s,
                                           struct rte_mbuf* mbuf,
                                           enum mtl_session_port s_port) {
  struct st40_rx_ops* ops = &s->ops;
  size_t hdr_offset = sizeof(struct st_rfc3550_hdr) - sizeof(struct st_rfc3550_rtp_hdr);
  struct st_rfc3550_rtp_hdr* rtp =
      rte_pktmbuf_mtod_offset(mbuf, struct st_rfc3550_rtp_hdr*, hdr_offset);
  uint16_t seq_id = ntohs(rtp->seq_number);
  uint8_t payload_type = rtp->payload_type;
  struct st40_rfc8331_rtp_hdr* rfc8331 = (struct st40_rfc8331_rtp_hdr*)rtp;
  rfc8331->swapped_first_hdr_chunk = ntohl(rfc8331->swapped_first_hdr_chunk);
  MTL_MAY_UNUSED(s_port);
  uint32_t pkt_len = mbuf->data_len - sizeof(struct st40_rfc8331_rtp_hdr);
  MTL_MAY_UNUSED(pkt_len);
  uint32_t tmstamp = ntohl(rtp->tmstamp);
  bool threshold_bypass = false;

  if (ops->payload_type && (payload_type != ops->payload_type)) {
    ST40_FUZZ_LOG("%s(%d,%d), drop payload_type %u expected %u\n", __func__, s->idx,
                  s_port, payload_type, ops->payload_type);
    dbg("%s(%d,%d), get payload_type %u but expect %u\n", __func__, s->idx, s_port,
        payload_type, ops->payload_type);
    s->port_user_stats.common.stat_pkts_wrong_pt_dropped++;
    return -EINVAL;
  }
  if (ops->ssrc) {
    uint32_t ssrc = ntohl(rtp->ssrc);
    if (ssrc != ops->ssrc) {
      ST40_FUZZ_LOG("%s(%d,%d), drop ssrc %u expected %u\n", __func__, s->idx, s_port,
                    ssrc, ops->ssrc);
      dbg("%s(%d,%d), get ssrc %u but expect %u\n", __func__, s->idx, s_port, ssrc,
          ops->ssrc);
      s->port_user_stats.common.stat_pkts_wrong_ssrc_dropped++;
      return -EINVAL;
    }
  }

  uint8_t f_bits = rfc8331->first_hdr_chunk.f; /* 2-bit field, no mask needed */

  /* Drop if F is 0b01 (invalid: bit 0 set, bit 1 clear) */
  if (f_bits == 0x1) {
    ST40_FUZZ_LOG("%s(%d,%d), drop invalid field bits 0x%x\n", __func__, s->idx, s_port,
                  rfc8331->first_hdr_chunk.f);
    s->port_user_stats.stat_pkts_wrong_interlace_dropped++;
    return -EINVAL;
  }
  /* Auto-detect interlace if enabled */
  bool pkt_interlaced = f_bits & 0x2;
  if (s->interlace_auto) {
    if (!s->interlace_detected || s->interlace_interlaced != pkt_interlaced) {
      s->interlace_detected = true;
      s->interlace_interlaced = pkt_interlaced;
      s->ops.interlaced = pkt_interlaced;
      info("%s(%d,%d), detected %s stream (F=0x%x)\n", __func__, s->idx, s_port,
           pkt_interlaced ? "interlaced" : "progressive", f_bits);
    }
  }

  /* Count field polarity when interlaced frames are accepted */
  if (pkt_interlaced) {
    if (f_bits & 0x1) {
      s->port_user_stats.stat_interlace_second_field++;
    } else {
      s->port_user_stats.stat_interlace_first_field++;
    }
  }

  /* Cross-port F-bit divergence: a mismatch on the same timestamp means the
   * producer sends different field bits per port — a SMPTE 2110-40 violation.
   * Rate-limit the warn to one per second. MTL only ever has P/R (max 2). */
  if (s->ops.num_port > 1) {
    int other = s_port ^ 1;
    if (s->last_f_bits[other] != 0xff && s->last_f_tmstamp[other] == tmstamp &&
        s->last_f_bits[other] != f_bits) {
      s->stat_internal_field_bit_mismatch++;
      uint64_t now_ns = mt_get_monotonic_time();
      if (now_ns - s->f_mismatch_warn_last_ns > NS_PER_S) {
        err("RX_ANC_SESSION(%d): redundant ports disagree on F bits at ts %u "
            "(port%d=F=0x%x port%d=F=0x%x) — SMPTE 2110-40 producer violation\n",
            s->idx, tmstamp, other, s->last_f_bits[other], s_port, f_bits);
        s->f_mismatch_warn_last_ns = now_ns;
      }
    }
  }
  s->last_f_bits[s_port] = f_bits;
  s->last_f_tmstamp[s_port] = tmstamp;

  if (unlikely(s->latest_seq_id[s_port] == -1)) s->latest_seq_id[s_port] = seq_id - 1;
  if (unlikely(s->session_seq_id == -1)) s->session_seq_id = seq_id - 1;
  if (unlikely(s->tmstamp == -1)) s->tmstamp = tmstamp - 1;

  /* not a big deal as long as stream is continous */
  if (seq_id != (uint16_t)(s->latest_seq_id[s_port] + 1) &&
      mt_seq16_greater(seq_id, s->latest_seq_id[s_port])) {
    uint16_t gap = (uint16_t)(seq_id - s->latest_seq_id[s_port] - 1);
    dbg("%s(%d,%d), non-continuous seq now %u last %d\n", __func__, s->idx, s_port,
        seq_id, s->latest_seq_id[s_port]);
    s->port_user_stats.common.port[s_port].lost_packets += gap;
    s->port_user_stats.common.stat_lost_packets += gap;
  } else if (seq_id == (uint16_t)s->latest_seq_id[s_port]) {
    /* exact same seq seen again on the same port — a real same-port duplicate
     * (distinct from a duplicate arriving on the redundant port) */
    s->port_user_stats.common.port[s_port].duplicates_same_port++;
  } else if (!mt_seq16_greater(seq_id, s->latest_seq_id[s_port])) {
    /* backward arrival on the same port — genuine intra-port reorder */
    s->port_user_stats.common.port[s_port].reordered_packets++;
  }
  if (mt_seq16_greater(seq_id, s->latest_seq_id[s_port]))
    s->latest_seq_id[s_port] = seq_id;

  /* count per-port stats before redundancy filtering for consistent reporting */
  s->port_user_stats.common.port[s_port].packets++;
  s->port_user_stats.common.port[s_port].bytes += mbuf->pkt_len;

  /* in ancillary we assume packet is redundant when the seq_id is old (it's possible to
  get multiple packets with the same timestamp) */
  if ((mt_seq32_greater(s->tmstamp, tmstamp)) ||
      !mt_seq16_greater(seq_id, s->session_seq_id)) {
    /* Check per-frame bitmap: if this is a same-frame late arrival carrying unique data,
     * accept it instead of filtering.  This handles cross-port reordering where port R
     * delivers seq N+2..N+5 before port P delivers seq N..N+1. */
    if (s->anc_window_cur.valid && tmstamp == s->anc_window_cur.tmstamp &&
        !mt_seq32_greater(s->tmstamp, tmstamp)) {
      uint16_t offset = (uint16_t)(seq_id - s->anc_window_cur.base_seq);
      if (offset < ST_RX_ANC_BITMAP_BITS &&
          !(s->anc_window_cur.bitmap & ((uint64_t)1 << offset))) {
        /* bit is clear — this seq was never delivered, accept as late arrival.
         * This packet was previously counted as unrecovered when the gap was seen,
         * so undo that count now that it has arrived. */
        if (s->port_user_stats.common.stat_pkts_unrecovered > 0)
          s->port_user_stats.common.stat_pkts_unrecovered--;
        dbg("%s(%d,%d), late arrival seq %u accepted via bitmap (offset %u)\n", __func__,
            s->idx, s_port, seq_id, offset);
        goto accept_pkt;
      }
    }

    /* Previous-timestamp window: accept unique late packets for the immediately
     * previous frame.  This enables the pipeline layer to receive late-arriving
     * marker packets from the redundant port after the primary has advanced. */
    if (s->prev_tmstamp >= 0 && tmstamp == (uint32_t)s->prev_tmstamp &&
        s->anc_window_prev.valid && tmstamp == s->anc_window_prev.tmstamp) {
      uint16_t offset = (uint16_t)(seq_id - s->anc_window_prev.base_seq);
      if (offset < ST_RX_ANC_BITMAP_BITS &&
          !(s->anc_window_prev.bitmap & ((uint64_t)1 << offset))) {
        if (s->port_user_stats.common.stat_pkts_unrecovered > 0)
          s->port_user_stats.common.stat_pkts_unrecovered--;
        dbg("%s(%d,%d), prev-frame late arrival seq %u accepted (offset %u)\n", __func__,
            s->idx, s_port, seq_id, offset);
        goto accept_pkt;
      }
    }

    if (!mt_seq16_greater(seq_id, s->session_seq_id)) {
      ST40_FUZZ_LOG("%s(%d,%d), redundant seq %u last %d\n", __func__, s->idx, s_port,
                    seq_id, s->session_seq_id);
      dbg("%s(%d,%d), redundant seq now %u session last %d\n", __func__, s->idx, s_port,
          seq_id, s->session_seq_id);
    } else {
      ST40_FUZZ_LOG("%s(%d,%d), redundant ts %u last %ld\n", __func__, s->idx, s_port,
                    tmstamp, s->tmstamp);
      dbg("%s(%d,%d), redundant tmstamp now %u session last %ld\n", __func__, s->idx,
          s_port, tmstamp, s->tmstamp);
    }

    s->redundant_error_cnt[s_port]++;
    s->port_user_stats.common.stat_pkts_redundant++;

    for (int i = 0; i < s->ops.num_port; i++) {
      if (s->redundant_error_cnt[i] < ST_SESSION_REDUNDANT_ERROR_THRESHOLD) {
        return -EIO;
      }
    }
    /* threshold exceeded on all ports — accept the packet and undo the redundant count
     * so the packet is only counted as received, not both */
    s->port_user_stats.common.stat_pkts_redundant--;
    threshold_bypass = true;
    warn(
        "%s(%d), redundant error threshold reached, accept packet seq %u (old seq_id "
        "%d), timestamp %u (old timestamp %ld)\n",
        __func__, s->idx, seq_id, s->session_seq_id, tmstamp, s->tmstamp);
  }

accept_pkt:
  s->redundant_error_cnt[s_port] = 0;

  /* Save session_seq before update — needed for bitmap base calculation */
  int old_session_seq = s->session_seq_id;

  /* hole in seq id packets going into the session check if the seq_id of the session is
   * consistent.  Note: do NOT reset interlace_detected here — a session-merged seq gap
   * does not imply the producer changed interlacing.  Resetting caused spurious
   * "detected interlaced stream (F=0x?)" log lines on every seq gap, which on a
   * redundant stream looked like the two ports disagreed on interlacing. */
  if (seq_id != (uint16_t)(s->session_seq_id + 1) &&
      mt_seq16_greater(seq_id, s->session_seq_id)) {
    dbg("%s(%d,%d), session seq_id %u out of order %d\n", __func__, s->idx, s_port,
        seq_id, s->session_seq_id);
    s->port_user_stats.common.stat_pkts_unrecovered +=
        (uint16_t)(seq_id - s->session_seq_id - 1);
  }

  /* update seq id — only advance, never lower for late arrivals */
  if (mt_seq16_greater(seq_id, s->session_seq_id)) s->session_seq_id = seq_id;

  /* Update per-frame bitmap: track which seq offsets have been delivered.
   * base_seq = old_session_seq + 1 = first expected seq of this frame.
   * This ensures late arrivals (seq < first accepted seq) still fit in the bitmap. */
  if (s->anc_window_prev.valid && tmstamp == s->anc_window_prev.tmstamp) {
    /* This packet belongs to the previous frame — update prev bitmap only */
    uint16_t offset = (uint16_t)(seq_id - s->anc_window_prev.base_seq);
    if (offset < ST_RX_ANC_BITMAP_BITS)
      s->anc_window_prev.bitmap |= ((uint64_t)1 << offset);
  } else {
    if (!s->anc_window_cur.valid || tmstamp != s->anc_window_cur.tmstamp) {
      /* Save current bitmap as prev before resetting for new frame */
      if (s->anc_window_cur.valid) s->anc_window_prev = s->anc_window_cur;
      s->anc_window_cur.bitmap = 0;
      s->anc_window_cur.tmstamp = tmstamp;
      s->anc_window_cur.base_seq = (uint16_t)(old_session_seq + 1);
      s->anc_window_cur.valid = true;
    }
    {
      uint16_t offset = (uint16_t)(seq_id - s->anc_window_cur.base_seq);
      if (offset < ST_RX_ANC_BITMAP_BITS)
        s->anc_window_cur.bitmap |= ((uint64_t)1 << offset);
    }
  }

  /* enqueue to packet ring (RTP_LEVEL) or hand off to assembler (FRAME_LEVEL) */
  if (s->ops.type == ST40_TYPE_FRAME_LEVEL) {
    /* T3: dispatch only — assembler stub does not take an mbuf refcnt; the
     * caller's burst-free reclaims it. RTP-style notify is skipped. */
    rx_ancillary_session_assemble_pkt(impl, s, mbuf, seq_id, tmstamp, pkt_len, s_port);
  } else {
    int ret = rte_ring_sp_enqueue(s->packet_ring, (void*)mbuf);
    if (ret < 0) {
      err("%s(%d), can not enqueue to the rte ring, packet drop, pkt seq %d\n", __func__,
          s->idx, seq_id);
      ST40_FUZZ_LOG("%s(%d,%d), enqueue failure for seq %u len %u\n", __func__, s->idx,
                    s_port, seq_id, pkt_len);
      s->port_user_stats.stat_pkts_enqueue_fail++;
      MT_USDT_ST40_RX_MBUF_ENQUEUE_FAIL(s->mgr->idx, s->idx, mbuf, tmstamp);
      return 0;
    }
    rte_mbuf_refcnt_update(mbuf, 1); /* free when app put */
  }

  /* Only advance timestamp forward — prev-frame late arrivals must not roll back.
   * Exception: threshold bypass is a recovery mechanism that may legitimately
   * move the timestamp backward when the session is stuck. */
  if (tmstamp != s->tmstamp &&
      (mt_seq32_greater(tmstamp, s->tmstamp) || threshold_bypass)) {
    s->prev_tmstamp = s->tmstamp;

    rte_atomic32_inc(&s->stat_frames_received);
    s->port_user_stats.common.port[s_port].frames++;
    s->tmstamp = tmstamp;
  }
  s->port_user_stats.common.stat_pkts_received++;

  /* get a valid packet */
  uint64_t tsc_start = 0;
  bool time_measure = mt_sessions_time_measure(impl);
  if (time_measure) tsc_start = mt_get_tsc(impl);
  if (s->ops.type != ST40_TYPE_FRAME_LEVEL) {
    ops->notify_rtp_ready(ops->priv);
  }
  if (time_measure) {
    uint32_t delta_us = (mt_get_tsc(impl) - tsc_start) / NS_PER_US;
    s->stat_max_notify_rtp_us = RTE_MAX(s->stat_max_notify_rtp_us, delta_us);
  }

  MT_USDT_ST40_RX_MBUF_AVAILABLE(s->mgr->idx, s->idx, mbuf, tmstamp, pkt_len);
  ST40_FUZZ_LOG("%s(%d,%d), fuzz enqueued seq %u len %u\n", __func__, s->idx, s_port,
                seq_id, pkt_len);
  return 0;
}

static void rx_ancillary_session_reset(struct st_rx_ancillary_session_impl* s,
                                       bool init_stat_time_now) {
  if (!s) return;

  s->session_seq_id = -1;
  s->tmstamp = -1;
  s->prev_tmstamp = -1;
  memset(&s->anc_window_cur, 0, sizeof(s->anc_window_cur));
  memset(&s->anc_window_prev, 0, sizeof(s->anc_window_prev));
  s->stat_last_time = init_stat_time_now ? mt_get_monotonic_time() : 0;
  s->stat_max_notify_rtp_us = 0;
  s->stat_assemble_dispatched = 0;
  rte_atomic32_set(&s->stat_frames_received, 0);
  mt_stat_u64_init(&s->stat_time);
  memset(&s->port_user_stats, 0, sizeof(s->port_user_stats));
  memset(&s->stat_snapshot, 0, sizeof(s->stat_snapshot));

  /* Reset interlace detection state */
  s->interlace_detected = !s->interlace_auto;
  s->interlace_interlaced = s->ops.interlaced;

  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    s->latest_seq_id[i] = -1;
    s->redundant_error_cnt[i] = 0;
    s->last_f_bits[i] = 0xff; /* sentinel: no F bits seen yet on this port */
    s->last_f_tmstamp[i] = 0;
  }
  s->f_mismatch_warn_last_ns = 0;
}

static int rx_ancillary_session_handle_mbuf(void* priv, struct rte_mbuf** mbuf,
                                            uint16_t nb) {
  struct st_rx_session_priv* s_priv = priv;
  struct st_rx_ancillary_session_impl* s = s_priv->session;
  struct mtl_main_impl* impl = s_priv->impl;
  enum mtl_session_port s_port = s_priv->s_port;

  if (!s->attached) {
    dbg("%s(%d,%d), session not ready\n", __func__, s->idx, s_port);
    return -EIO;
  }

  for (uint16_t i = 0; i < nb; i++) {
    if (rx_ancillary_session_handle_pkt(impl, s, mbuf[i], s_port) < 0)
      s->port_user_stats.common.port[s_port].err_packets++;
  }

  return 0;
}

static int rx_ancillary_session_tasklet(struct st_rx_ancillary_session_impl* s) {
  struct rte_mbuf* mbuf[ST_RX_ANCILLARY_BURST_SIZE];
  uint16_t rv;
  int num_port = s->ops.num_port;
  bool done = true;

  for (int s_port = 0; s_port < num_port; s_port++) {
    if (!s->rxq[s_port]) continue;

    rv = mt_rxq_burst(s->rxq[s_port], &mbuf[0], ST_RX_ANCILLARY_BURST_SIZE);
    if (rv) {
      rx_ancillary_session_handle_mbuf(&s->priv[s_port], &mbuf[0], rv);
      rte_pktmbuf_free_bulk(&mbuf[0], rv);
    }

    if (rv) done = false;
  }

  return done ? MTL_TASKLET_ALL_DONE : MTL_TASKLET_HAS_PENDING;
}

static int rx_ancillary_sessions_tasklet_handler(void* priv) {
  struct st_rx_ancillary_sessions_mgr* mgr = priv;
  struct mtl_main_impl* impl = mgr->parent;
  struct st_rx_ancillary_session_impl* s;
  int pending = MTL_TASKLET_ALL_DONE;
  uint64_t tsc_s = 0;
  bool time_measure = mt_sessions_time_measure(impl);

  for (int sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = rx_ancillary_session_try_get(mgr, sidx);
    if (!s) continue;
    if (time_measure) tsc_s = mt_get_tsc(impl);

    pending += rx_ancillary_session_tasklet(s);

    if (time_measure) {
      uint64_t delta_ns = mt_get_tsc(impl) - tsc_s;
      mt_stat_u64_update(&s->stat_time, delta_ns);
    }
    rx_ancillary_session_put(mgr, sidx);
  }

  return pending;
}

static int rx_ancillary_session_uinit_hw(struct st_rx_ancillary_session_impl* s) {
  int num_port = s->ops.num_port;

  for (int i = 0; i < num_port; i++) {
    if (s->rxq[i]) {
      mt_rxq_put(s->rxq[i]);
      s->rxq[i] = NULL;
    }
  }

  return 0;
}

static int rx_ancillary_session_init_hw(struct mtl_main_impl* impl,
                                        struct st_rx_ancillary_session_impl* s) {
  int idx = s->idx, num_port = s->ops.num_port;
  struct mt_rxq_flow flow;
  enum mtl_port port;

  for (int i = 0; i < num_port; i++) {
    port = mt_port_logic2phy(s->port_maps, i);

    s->priv[i].session = s;
    s->priv[i].impl = impl;
    s->priv[i].s_port = i;

    memset(&flow, 0, sizeof(flow));
    rte_memcpy(flow.dip_addr, s->ops.ip_addr[i], MTL_IP_ADDR_LEN);
    if (mt_is_multicast_ip(flow.dip_addr))
      rte_memcpy(flow.sip_addr, s->ops.mcast_sip_addr[i], MTL_IP_ADDR_LEN);
    else
      rte_memcpy(flow.sip_addr, mt_sip_addr(impl, port), MTL_IP_ADDR_LEN);
    flow.dst_port = s->st40_dst_port[i];
    if (mt_has_cni_rx(impl, port)) flow.flags |= MT_RXQ_FLOW_F_FORCE_CNI;

    /* no flow for data path only */
    if (s->ops.flags & ST40_RX_FLAG_DATA_PATH_ONLY) {
      info("%s(%d), rxq get without flow for port %d as data path only\n", __func__,
           s->idx, i);
      s->rxq[i] = mt_rxq_get(impl, port, NULL);
    } else {
      s->rxq[i] = mt_rxq_get(impl, port, &flow);
    }
    if (!s->rxq[i]) {
      rx_ancillary_session_uinit_hw(s);
      return -EIO;
    }

    info("%s(%d), port(l:%d,p:%d), queue %d udp %d\n", __func__, idx, i, port,
         rx_ancillary_queue_id(s, i), flow.dst_port);
  }

  return 0;
}

static int rx_ancillary_session_uinit_mcast(struct mtl_main_impl* impl,
                                            struct st_rx_ancillary_session_impl* s) {
  struct st40_rx_ops* ops = &s->ops;
  enum mtl_port port;

  for (int i = 0; i < ops->num_port; i++) {
    if (!s->mcast_joined[i]) continue;
    port = mt_port_logic2phy(s->port_maps, i);
    mt_mcast_leave(impl, mt_ip_to_u32(ops->ip_addr[i]),
                   mt_ip_to_u32(ops->mcast_sip_addr[i]), port);
  }

  return 0;
}

static int rx_ancillary_session_init_mcast(struct mtl_main_impl* impl,
                                           struct st_rx_ancillary_session_impl* s) {
  struct st40_rx_ops* ops = &s->ops;
  int ret;
  enum mtl_port port;

  for (int i = 0; i < ops->num_port; i++) {
    if (!mt_is_multicast_ip(ops->ip_addr[i])) continue;
    port = mt_port_logic2phy(s->port_maps, i);
    if (ops->flags & ST20_RX_FLAG_DATA_PATH_ONLY) {
      info("%s(%d), skip mcast join for port %d\n", __func__, s->idx, i);
      return 0;
    }
    ret = mt_mcast_join(impl, mt_ip_to_u32(ops->ip_addr[i]),
                        mt_ip_to_u32(ops->mcast_sip_addr[i]), port);
    if (ret < 0) return ret;
    s->mcast_joined[i] = true;
  }

  return 0;
}

static int rx_ancillary_session_uinit_frames(struct st_rx_ancillary_session_impl* s) {
  if (s->frame_slots) {
    for (uint16_t i = 0; i < s->frame_slots_cnt; i++) {
      if (s->frame_slots[i].udw_buf) {
        mt_rte_free(s->frame_slots[i].udw_buf);
        s->frame_slots[i].udw_buf = NULL;
      }
    }
    mt_rte_free(s->frame_slots);
    s->frame_slots = NULL;
  }
  s->frame_slots_cnt = 0;
  return 0;
}

static int rx_ancillary_session_init_frames(struct st_rx_ancillary_session_impl* s) {
  uint16_t cnt = s->ops.framebuff_cnt;
  size_t buf_sz = s->ops.framebuff_size;
  int idx = s->idx;

  s->frame_slots = mt_rte_zmalloc_socket(sizeof(*s->frame_slots) * cnt, s->socket_id);
  if (!s->frame_slots) {
    err("%s(%d), frame_slots alloc fail (%u slots)\n", __func__, idx, cnt);
    return -ENOMEM;
  }
  s->frame_slots_cnt = cnt;

  for (uint16_t i = 0; i < cnt; i++) {
    struct st_rx_anc_frame_slot* slot = &s->frame_slots[i];
    slot->idx = i;
    slot->state = ST_RX_ANC_SLOT_FREE;
    slot->udw_buf_size = buf_sz;
    slot->udw_buf = mt_rte_zmalloc_socket(buf_sz, s->socket_id);
    if (!slot->udw_buf) {
      err("%s(%d), udw_buf alloc fail (%zu bytes) for slot %u\n", __func__, idx, buf_sz,
          i);
      rx_ancillary_session_uinit_frames(s);
      return -ENOMEM;
    }
  }

  info("%s(%d), %u frame slots, %zu bytes UDW each\n", __func__, idx, cnt, buf_sz);
  return 0;
}

static int rx_ancillary_session_init_sw(struct st_rx_ancillary_sessions_mgr* mgr,
                                        struct st_rx_ancillary_session_impl* s) {
  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count;
  int mgr_idx = mgr->idx, idx = s->idx;

  if (s->ops.type == ST40_TYPE_FRAME_LEVEL) {
    return rx_ancillary_session_init_frames(s);
  }

  snprintf(ring_name, 32, "%sM%dS%d_PKT", ST_RX_ANCILLARY_PREFIX, mgr_idx, idx);
  flags = RING_F_SP_ENQ | RING_F_SC_DEQ; /* single-producer and single-consumer */
  count = s->ops.rtp_ring_size;
  ring = rte_ring_create(ring_name, count, s->socket_id, flags);
  if (count <= 0) {
    err("%s(%d,%d), invalid rtp_ring_size %d\n", __func__, mgr_idx, idx, count);
    return -ENOMEM;
  }
  if (!ring) {
    err("%s(%d,%d), rte_ring_create fail\n", __func__, mgr_idx, idx);
    return -ENOMEM;
  }
  s->packet_ring = ring;
  info("%s(%d,%d), rtp_ring_size %d\n", __func__, mgr_idx, idx, count);
  return 0;
}

static int rx_ancillary_session_uinit_sw(struct st_rx_ancillary_session_impl* s) {
  if (s->packet_ring) {
    mt_ring_dequeue_clean(s->packet_ring);
    rte_ring_free(s->packet_ring);
    s->packet_ring = NULL;
  }
  rx_ancillary_session_uinit_frames(s);

  return 0;
}

static int rx_ancillary_session_uinit(struct mtl_main_impl* impl,
                                      struct st_rx_ancillary_session_impl* s) {
  rx_ancillary_session_uinit_mcast(impl, s);
  rx_ancillary_session_uinit_sw(s);
  rx_ancillary_session_uinit_hw(s);
  return 0;
}

static int rx_ancillary_session_attach(struct mtl_main_impl* impl,
                                       struct st_rx_ancillary_sessions_mgr* mgr,
                                       struct st_rx_ancillary_session_impl* s,
                                       struct st40_rx_ops* ops) {
  int ret;
  int idx = s->idx, num_port = ops->num_port;
  char* ports[MTL_SESSION_PORT_MAX];

  for (int i = 0; i < num_port; i++) ports[i] = ops->port[i];
  ret = mt_build_port_map(impl, ports, s->port_maps, num_port);
  if (ret < 0) return ret;

  s->mgr = mgr;
  if (ops->name) {
    snprintf(s->ops_name, sizeof(s->ops_name), "%s", ops->name);
  } else {
    snprintf(s->ops_name, sizeof(s->ops_name), "RX_ANC_M%dS%d", mgr->idx, idx);
  }
  s->ops = *ops;
  s->interlace_auto = !(ops->flags & ST40_RX_FLAG_DISABLE_AUTO_DETECT);
  s->interlace_detected = !s->interlace_auto;
  s->interlace_interlaced = ops->interlaced;
  for (int i = 0; i < num_port; i++) {
    s->st40_dst_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (30000 + idx * 2);
  }

  rx_ancillary_session_reset(s, true);

  ret = rx_ancillary_session_init_hw(impl, s);
  if (ret < 0) {
    err("%s(%d), rx_audio_session_init_hw fail %d\n", __func__, idx, ret);
    rx_ancillary_session_uinit(impl, s);
    return ret;
  }

  ret = rx_ancillary_session_init_sw(mgr, s);
  if (ret < 0) {
    err("%s(%d), rx_ancillary_session_init_rtps fail %d\n", __func__, idx, ret);
    rx_ancillary_session_uinit(impl, s);
    return ret;
  }

  ret = rx_ancillary_session_init_mcast(impl, s);
  if (ret < 0) {
    err("%s(%d), rx_ancillary_session_init_mcast fail %d\n", __func__, idx, ret);
    rx_ancillary_session_uinit(impl, s);
    return -EIO;
  }

  s->attached = true;
  info("%s(%d), flags 0x%x pt %u, %s\n", __func__, idx, ops->flags, ops->payload_type,
       s->interlace_auto ? "auto" : (ops->interlaced ? "interlace" : "progressive"));
  return 0;
}

static void rx_ancillary_session_stat(struct st_rx_ancillary_session_impl* s) {
  int idx = s->idx;
  uint64_t cur_time_ns = mt_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->stat_last_time) / NS_PER_S;
  int frames_received = rte_atomic32_read(&s->stat_frames_received);
  double framerate = frames_received / time_sec;

  rte_atomic32_set(&s->stat_frames_received, 0);

  struct st40_rx_user_stats* us = &s->port_user_stats;
  struct st40_rx_user_stats* snap = &s->stat_snapshot;

  uint64_t pkts_received =
      us->common.stat_pkts_received - snap->common.stat_pkts_received;
  uint64_t pkts_redundant =
      us->common.stat_pkts_redundant - snap->common.stat_pkts_redundant;
  uint64_t lost_pkts = us->common.stat_lost_packets - snap->common.stat_lost_packets;
  uint64_t pkts_unrecovered =
      us->common.stat_pkts_unrecovered - snap->common.stat_pkts_unrecovered;
  uint64_t pkts_dropped = us->stat_pkts_dropped - snap->stat_pkts_dropped;
  uint64_t pkts_wrong_pt_dropped =
      us->common.stat_pkts_wrong_pt_dropped - snap->common.stat_pkts_wrong_pt_dropped;
  uint64_t pkts_wrong_ssrc_dropped =
      us->common.stat_pkts_wrong_ssrc_dropped - snap->common.stat_pkts_wrong_ssrc_dropped;
  uint64_t pkts_enqueue_fail = us->stat_pkts_enqueue_fail - snap->stat_pkts_enqueue_fail;
  uint64_t pkts_wrong_interlace_dropped =
      us->stat_pkts_wrong_interlace_dropped - snap->stat_pkts_wrong_interlace_dropped;
  uint64_t interlace_first_field =
      us->stat_interlace_first_field - snap->stat_interlace_first_field;
  uint64_t interlace_second_field =
      us->stat_interlace_second_field - snap->stat_interlace_second_field;
  uint64_t f_mismatch =
      s->stat_internal_field_bit_mismatch - s->stat_internal_field_bit_mismatch_snap;
  s->stat_internal_field_bit_mismatch_snap = s->stat_internal_field_bit_mismatch;

  if (pkts_redundant) {
    notice("RX_ANC_SESSION(%d:%s): fps %f frames %d pkts %" PRIu64 " (redundant %" PRIu64
           ")\n",
           idx, s->ops_name, framerate, frames_received, pkts_received, pkts_redundant);
  } else {
    notice("RX_ANC_SESSION(%d:%s): fps %f frames %d pkts %" PRIu64 "\n", idx, s->ops_name,
           framerate, frames_received, pkts_received);
  }
  s->stat_last_time = cur_time_ns;

  if (pkts_dropped) {
    notice("RX_ANC_SESSION(%d): dropped pkts %" PRIu64 "\n", idx, pkts_dropped);
  }

  /* Per-port packet/frame line: port-balance visible at a glance. */
  uint64_t port_pkts[MTL_SESSION_PORT_MAX] = {0};
  uint64_t port_frames[MTL_SESSION_PORT_MAX] = {0};
  uint64_t port_lost[MTL_SESSION_PORT_MAX] = {0};
  for (int i = 0; i < s->ops.num_port; i++) {
    port_pkts[i] = us->common.port[i].packets - snap->common.port[i].packets;
    port_frames[i] = us->common.port[i].frames - snap->common.port[i].frames;
    port_lost[i] = us->common.port[i].lost_packets - snap->common.port[i].lost_packets;
  }
  if (s->ops.num_port > 1) {
    notice("RX_ANC_SESSION(%d): per-port arrivals P=%" PRIu64 " pkts (%" PRIu64
           " frames first), R=%" PRIu64 " pkts (%" PRIu64 " frames first)\n",
           idx, port_pkts[MTL_SESSION_PORT_P], port_frames[MTL_SESSION_PORT_P],
           port_pkts[MTL_SESSION_PORT_R], port_frames[MTL_SESSION_PORT_R]);
  }

  if (lost_pkts) {
    uint64_t total_pkts = port_pkts[MTL_SESSION_PORT_P] + port_pkts[MTL_SESSION_PORT_R];
    if (s->ops.num_port > 1) {
      double pct_p =
          port_pkts[MTL_SESSION_PORT_P]
              ? 100.0 * port_lost[MTL_SESSION_PORT_P] /
                    (port_pkts[MTL_SESSION_PORT_P] + port_lost[MTL_SESSION_PORT_P])
              : 0.0;
      double pct_r =
          port_pkts[MTL_SESSION_PORT_R]
              ? 100.0 * port_lost[MTL_SESSION_PORT_R] /
                    (port_pkts[MTL_SESSION_PORT_R] + port_lost[MTL_SESSION_PORT_R])
              : 0.0;
      double save_rate =
          (lost_pkts + pkts_unrecovered)
              ? 100.0 * (double)lost_pkts / (double)(lost_pkts + pkts_unrecovered)
              : 100.0;
      warn("RX_ANC_SESSION(%d): per-port loss %" PRIu64 " of %" PRIu64 " pkts (P:%" PRIu64
           "=%.1f%%, R:%" PRIu64 "=%.1f%%), unrecovered (lost on both) %" PRIu64
           ", save_rate=%.1f%%\n",
           idx, lost_pkts, total_pkts + lost_pkts, port_lost[MTL_SESSION_PORT_P], pct_p,
           port_lost[MTL_SESSION_PORT_R], pct_r, pkts_unrecovered, save_rate);
    } else {
      warn("RX_ANC_SESSION(%d): per-port lost pkts %" PRIu64 ", unrecovered %" PRIu64
           "\n",
           idx, lost_pkts, pkts_unrecovered);
    }
  } else if (pkts_unrecovered) {
    /* unrecovered without per-port loss in this window — orphan, surface separately */
    err("RX_ANC_SESSION(%d): unrecovered pkts (lost on all ports) %" PRIu64 "\n", idx,
        pkts_unrecovered);
  }
  if (f_mismatch) {
    err("RX_ANC_SESSION(%d): F-bit mismatches between redundant ports %" PRIu64 "\n", idx,
        f_mismatch);
  }

  if (pkts_wrong_pt_dropped) {
    notice("RX_ANC_SESSION(%d): wrong hdr payload_type dropped pkts %" PRIu64 "\n", idx,
           pkts_wrong_pt_dropped);
  }
  if (pkts_wrong_ssrc_dropped) {
    notice("RX_ANC_SESSION(%d): wrong hdr ssrc dropped pkts %" PRIu64 "\n", idx,
           pkts_wrong_ssrc_dropped);
  }
  if (pkts_wrong_interlace_dropped) {
    notice("RX_ANC_SESSION(%d): wrong hdr interlace dropped pkts %" PRIu64 "\n", idx,
           pkts_wrong_interlace_dropped);
  }
  if (pkts_enqueue_fail) {
    notice("RX_ANC_SESSION(%d): enqueue failed pkts %" PRIu64 "\n", idx,
           pkts_enqueue_fail);
  }
  if (s->ops.interlaced) {
    notice("RX_ANC_SESSION(%d): interlace first field %" PRIu64 " second field %" PRIu64
           "\n",
           idx, interlace_first_field, interlace_second_field);
  }

  /* T4/T5: assembler observability for FRAME_LEVEL transport */
  if (s->stat_anc_frames_dropped || s->stat_anc_pkt_parse_err ||
      s->stat_assemble_dispatched) {
    notice("RX_ANC_SESSION(%d): assembler dispatched %" PRIu64 " ready %" PRIu64
           " dropped %" PRIu64 " parse_err %" PRIu64 "\n",
           idx, s->stat_assemble_dispatched, s->stat_anc_frames_ready,
           s->stat_anc_frames_dropped, s->stat_anc_pkt_parse_err);
    s->stat_assemble_dispatched = 0;
    s->stat_anc_frames_ready = 0;
    s->stat_anc_frames_dropped = 0;
    s->stat_anc_pkt_parse_err = 0;
  }

  memcpy(snap, us, sizeof(*snap));

  struct mt_stat_u64* stat_time = &s->stat_time;
  if (stat_time->cnt) {
    uint64_t avg_ns = stat_time->sum / stat_time->cnt;
    notice("RX_ANC_SESSION(%d): tasklet time avg %.2fus max %.2fus min %.2fus\n", idx,
           (float)avg_ns / NS_PER_US, (float)stat_time->max / NS_PER_US,
           (float)stat_time->min / NS_PER_US);
    mt_stat_u64_init(stat_time);
  }
  if (s->stat_max_notify_rtp_us > 8) {
    notice("RX_ANC_SESSION(%d): notify rtp max %uus\n", idx, s->stat_max_notify_rtp_us);
  }
  s->stat_max_notify_rtp_us = 0;
}

static int rx_ancillary_session_detach(struct mtl_main_impl* impl,
                                       struct st_rx_ancillary_session_impl* s) {
  s->attached = false;
  rx_ancillary_session_stat(s);
  rx_ancillary_session_uinit(impl, s);
  return 0;
}

static int rx_ancillary_session_update_src(struct mtl_main_impl* impl,
                                           struct st_rx_ancillary_session_impl* s,
                                           struct st_rx_source_info* src) {
  int ret = -EIO;
  int idx = s->idx, num_port = s->ops.num_port;
  struct st40_rx_ops* ops = &s->ops;

  rx_ancillary_session_uinit_mcast(impl, s);
  rx_ancillary_session_uinit_hw(s);

  /* update ip and port */
  for (int i = 0; i < num_port; i++) {
    memcpy(ops->ip_addr[i], src->ip_addr[i], MTL_IP_ADDR_LEN);
    memcpy(ops->mcast_sip_addr[i], src->mcast_sip_addr[i], MTL_IP_ADDR_LEN);
    ops->udp_port[i] = src->udp_port[i];
    s->st40_dst_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (30000 + idx * 2);
  }
  /* reset seq id */

  s->session_seq_id = -1;
  s->latest_seq_id[MTL_SESSION_PORT_P] = -1;
  s->latest_seq_id[MTL_SESSION_PORT_R] = -1;
  s->tmstamp = -1;
  if (s->interlace_auto) {
    s->interlace_detected = false;
    s->interlace_interlaced = s->ops.interlaced;
  }

  ret = rx_ancillary_session_init_hw(impl, s);
  if (ret < 0) {
    err("%s(%d), init hw fail %d\n", __func__, idx, ret);
    return ret;
  }

  ret = rx_ancillary_session_init_mcast(impl, s);
  if (ret < 0) {
    err("%s(%d), init mcast fail %d\n", __func__, idx, ret);
    return ret;
  }

  return 0;
}

static int rx_ancillary_sessions_mgr_update_src(struct st_rx_ancillary_sessions_mgr* mgr,
                                                struct st_rx_ancillary_session_impl* s,
                                                struct st_rx_source_info* src) {
  int ret = -EIO, midx = mgr->idx, idx = s->idx;

  s = rx_ancillary_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }

  ret = rx_ancillary_session_update_src(mgr->parent, s, src);
  rx_ancillary_session_put(mgr, idx);
  if (ret < 0) {
    err("%s(%d,%d), fail %d\n", __func__, midx, idx, ret);
    return ret;
  }

  return 0;
}

static int st_rx_ancillary_sessions_stat(void* priv) {
  struct st_rx_ancillary_sessions_mgr* mgr = priv;
  struct st_rx_ancillary_session_impl* s;

  for (int j = 0; j < mgr->max_idx; j++) {
    s = rx_ancillary_session_get_timeout(mgr, j, ST_SESSION_STAT_TIMEOUT_US);
    if (!s) continue;
    rx_ancillary_session_stat(s);
    rx_ancillary_session_put(mgr, j);
  }

  return 0;
}

static int rx_ancillary_sessions_mgr_init(struct mtl_main_impl* impl,
                                          struct mtl_sch_impl* sch,
                                          struct st_rx_ancillary_sessions_mgr* mgr) {
  int idx = sch->idx;
  struct mtl_tasklet_ops ops;

  mgr->parent = impl;
  mgr->idx = idx;

  for (int i = 0; i < ST_MAX_RX_ANC_SESSIONS; i++) {
    rte_spinlock_init(&mgr->mutex[i]);
  }

  memset(&ops, 0x0, sizeof(ops));
  ops.priv = mgr;
  ops.name = "rx_anc_sessions_mgr";
  ops.start = rx_ancillary_sessions_tasklet_start;
  ops.stop = rx_ancillary_sessions_tasklet_stop;
  ops.handler = rx_ancillary_sessions_tasklet_handler;

  mgr->tasklet = mtl_sch_register_tasklet(sch, &ops);
  if (!mgr->tasklet) {
    err("%s(%d), mtl_sch_register_tasklet fail\n", __func__, idx);
    return -EIO;
  }

  mt_stat_register(mgr->parent, st_rx_ancillary_sessions_stat, mgr, "rx_anc");
  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static struct st_rx_ancillary_session_impl* rx_ancillary_sessions_mgr_attach(
    struct mtl_sch_impl* sch, struct st40_rx_ops* ops) {
  struct st_rx_ancillary_sessions_mgr* mgr = &sch->rx_anc_mgr;
  int midx = mgr->idx;
  int ret;
  struct st_rx_ancillary_session_impl* s;
  int socket = mt_sch_socket_id(sch);

  /* find one empty slot in the mgr */
  for (int i = 0; i < ST_MAX_RX_ANC_SESSIONS; i++) {
    if (!rx_ancillary_session_get_empty(mgr, i)) continue;

    s = mt_rte_zmalloc_socket(sizeof(*s), socket);
    if (!s) {
      err("%s(%d), session malloc fail on %d\n", __func__, midx, i);
      rx_ancillary_session_put(mgr, i);
      return NULL;
    }
    s->socket_id = socket;
    ret = rx_ancillary_session_init(mgr, s, i);
    if (ret < 0) {
      err("%s(%d), init fail on %d\n", __func__, midx, i);
      rx_ancillary_session_put(mgr, i);
      mt_rte_free(s);
      return NULL;
    }
    ret = rx_ancillary_session_attach(mgr->parent, mgr, s, ops);
    if (ret < 0) {
      err("%s(%d), attach fail on %d\n", __func__, midx, i);
      rx_ancillary_session_put(mgr, i);
      mt_rte_free(s);
      return NULL;
    }

    mgr->sessions[i] = s;
    mgr->max_idx = RTE_MAX(mgr->max_idx, i + 1);
    rx_ancillary_session_put(mgr, i);
    return s;
  }

  err("%s(%d), fail\n", __func__, midx);
  return NULL;
}

static int rx_ancillary_sessions_mgr_detach(struct st_rx_ancillary_sessions_mgr* mgr,
                                            struct st_rx_ancillary_session_impl* s) {
  int midx = mgr->idx;
  int idx = s->idx;

  s = rx_ancillary_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }

  rx_ancillary_session_detach(mgr->parent, s);
  mgr->sessions[idx] = NULL;
  mt_rte_free(s);

  rx_ancillary_session_put(mgr, idx);

  return 0;
}

static int rx_ancillary_sessions_mgr_update(struct st_rx_ancillary_sessions_mgr* mgr) {
  int max_idx = 0;

  for (int i = 0; i < ST_MAX_RX_ANC_SESSIONS; i++) {
    if (mgr->sessions[i]) max_idx = i + 1;
  }

  mgr->max_idx = max_idx;
  return 0;
}

static int rx_ancillary_sessions_mgr_uinit(struct st_rx_ancillary_sessions_mgr* mgr) {
  int m_idx = mgr->idx;
  struct st_rx_ancillary_session_impl* s;

  mt_stat_unregister(mgr->parent, st_rx_ancillary_sessions_stat, mgr);

  if (mgr->tasklet) {
    mtl_sch_unregister_tasklet(mgr->tasklet);
    mgr->tasklet = NULL;
  }

  for (int i = 0; i < ST_MAX_RX_ANC_SESSIONS; i++) {
    s = rx_ancillary_session_get(mgr, i);
    if (!s) continue;

    warn("%s(%d), session %d still attached\n", __func__, m_idx, i);
    rx_ancillary_sessions_mgr_detach(mgr, s);
    rx_ancillary_session_put(mgr, i);
  }

  info("%s(%d), succ\n", __func__, m_idx);
  return 0;
}

/* Remove any session ports that map to a down physical port.
 * When a down port is found at index i, all further entries are shifted down
 * one slot (port[i] = port[i+1], ...) and num_port is decremented.
 * Only ops->port[] is shifted — all other per-port arrays (ip_addr, udp_port, etc.)
 * are left untouched; with the reduced num_port they are simply never indexed.
 * Returns -EIO if every port is down (caller must abort). */
/* Prune down ports that are not available. Shifts port names, IP addresses,
 * UDP ports, and multicast source IP addresses for remaining ports. */
static int rx_ancillary_ops_prune_down_ports(struct mtl_main_impl* impl,
                                             struct st40_rx_ops* ops) {
  int num_ports = ops->num_port;

  if (num_ports > MTL_SESSION_PORT_MAX || num_ports <= 0) {
    err("%s, invalid num_ports %d\n", __func__, num_ports);
    return -EINVAL;
  }

  for (int i = 0; i < num_ports; i++) {
    enum mtl_port phy = mt_port_by_name(impl, ops->port[i]);
    if (phy >= MTL_PORT_MAX || !mt_if_port_is_down(impl, phy)) continue;

    warn("%s(%d), port %s is down, it will not be used\n", __func__, i, ops->port[i]);

    /* shift all further port-indexed fields one slot down */
    for (int j = i; j < num_ports - 1; j++) {
      rte_memcpy(ops->port[j], ops->port[j + 1], MTL_PORT_MAX_LEN);
      rte_memcpy(ops->ip_addr[j], ops->ip_addr[j + 1], MTL_IP_ADDR_LEN);
      rte_memcpy(ops->mcast_sip_addr[j], ops->mcast_sip_addr[j + 1], MTL_IP_ADDR_LEN);
      ops->udp_port[j] = ops->udp_port[j + 1];
    }

    num_ports--;
    i--;
  }

  if (num_ports == 0) {
    err("%s, all %d port(s) are down, cannot create session\n", __func__, ops->num_port);
    return -EIO;
  }

  if (num_ports < ops->num_port) {
    info("%s, reduced num_port %d -> %d after pruning down ports\n", __func__,
         ops->num_port, num_ports);
    ops->num_port = num_ports;
  }

  return 0;
}

static int rx_ancillary_ops_check(struct st40_rx_ops* ops) {
  int num_ports = ops->num_port, ret;
  uint8_t* ip = NULL;

  /* Back-compat: enum st40_type mirrors ST30 (FRAME_LEVEL=0). Existing ST40 RX
   * callers (pipeline RX, RxTxApp, gstreamer plugin) zero-init their ops and
   * set notify_rtp_ready, expecting legacy RTP_LEVEL behavior. Treat such
   * ops as RTP_LEVEL until callers explicitly opt into FRAME_LEVEL (T6+). */
  if (ops->type == ST40_TYPE_FRAME_LEVEL && !ops->notify_frame_ready &&
      ops->framebuff_cnt == 0) {
    ops->type = ST40_TYPE_RTP_LEVEL;
  }

  if ((num_ports > MTL_SESSION_PORT_MAX) || (num_ports <= 0)) {
    err("%s, invalid num_ports %d\n", __func__, num_ports);
    return -EINVAL;
  }

  for (int i = 0; i < num_ports; i++) {
    ip = ops->ip_addr[i];
    ret = mt_ip_addr_check(ip);
    if (ret < 0) {
      err("%s(%d), invalid ip %d.%d.%d.%d\n", __func__, i, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (num_ports > 1) {
    if (0 == memcmp(ops->ip_addr[0], ops->ip_addr[1], MTL_IP_ADDR_LEN)) {
      err("%s, same %d.%d.%d.%d for both ip\n", __func__, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (ops->type == ST40_TYPE_FRAME_LEVEL) {
    if (!ops->notify_frame_ready) {
      err("%s, FRAME_LEVEL: pls set notify_frame_ready\n", __func__);
      return -EINVAL;
    }
    if (ops->framebuff_cnt < 2) {
      err("%s, FRAME_LEVEL: framebuff_cnt %u must be >= 2\n", __func__,
          ops->framebuff_cnt);
      return -EINVAL;
    }
    if (ops->framebuff_size == 0) {
      err("%s, FRAME_LEVEL: framebuff_size must be > 0\n", __func__);
      return -EINVAL;
    }
  } else { /* ST40_TYPE_RTP_LEVEL */
    if (ops->rtp_ring_size <= 0) {
      err("%s, invalid rtp_ring_size %d\n", __func__, ops->rtp_ring_size);
      return -EINVAL;
    }
    if (!ops->notify_rtp_ready) {
      err("%s, pls set notify_rtp_ready\n", __func__);
      return -EINVAL;
    }
  }

  /* Zero means disable the payload_type check */
  if (!st_is_valid_payload_type(ops->payload_type)) {
    err("%s, invalid payload_type %d\n", __func__, ops->payload_type);
    return -EINVAL;
  }

  return 0;
}

static int st_rx_anc_init(struct mtl_main_impl* impl, struct mtl_sch_impl* sch) {
  int ret;

  if (sch->rx_anc_init) return 0;

  /* create rx ancillary context */
  ret = rx_ancillary_sessions_mgr_init(impl, sch, &sch->rx_anc_mgr);
  if (ret < 0) {
    err("%s, rx_ancillary_sessions_mgr_init fail\n", __func__);
    return ret;
  }

  sch->rx_anc_init = true;
  return 0;
}

int st_rx_ancillary_sessions_sch_uinit(struct mtl_sch_impl* sch) {
  if (!sch->rx_anc_init) return 0;

  rx_ancillary_sessions_mgr_uinit(&sch->rx_anc_mgr);

  sch->rx_anc_init = false;
  return 0;
}

st40_rx_handle st40_rx_create(mtl_handle mt, struct st40_rx_ops* ops) {
  struct mtl_main_impl* impl = mt;
  struct mtl_sch_impl* sch;
  struct st_rx_ancillary_session_handle_impl* s_impl;
  struct st_rx_ancillary_session_impl* s;
  int ret;
  int quota_mbs;

  notice("%s, start for %s\n", __func__, mt_string_safe(ops->name));

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  ret = rx_ancillary_ops_prune_down_ports(impl, ops);
  if (ret < 0) {
    err("%s, rx_ancillary_ops_prune_down_ports fail %d\n", __func__, ret);
    return NULL;
  }

  ret = rx_ancillary_ops_check(ops);
  if (ret < 0) {
    err("%s, st_rx_audio_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  enum mtl_port port = mt_port_by_name(impl, ops->port[MTL_SESSION_PORT_P]);
  if (port >= MTL_PORT_MAX) return NULL;
  int socket = mt_socket_id(impl, port);

  s_impl = mt_rte_zmalloc_socket(sizeof(*s_impl), socket);
  if (!s_impl) {
    err("%s, s_impl malloc fail on socket %d\n", __func__, socket);
    return NULL;
  }

  quota_mbs = 0;
  sch =
      mt_sch_get_by_socket(impl, quota_mbs, MT_SCH_TYPE_DEFAULT, MT_SCH_MASK_ALL, socket);
  if (!sch) {
    mt_rte_free(s_impl);
    err("%s, get sch fail\n", __func__);
    return NULL;
  }

  mt_pthread_mutex_lock(&sch->rx_anc_mgr_mutex);
  ret = st_rx_anc_init(impl, sch);
  mt_pthread_mutex_unlock(&sch->rx_anc_mgr_mutex);
  if (ret < 0) {
    err("%s, st_rx_anc_init fail %d\n", __func__, ret);
    mt_sch_put(sch, quota_mbs);
    mt_rte_free(s_impl);
    return NULL;
  }

  mt_pthread_mutex_lock(&sch->rx_anc_mgr_mutex);
  s = rx_ancillary_sessions_mgr_attach(sch, ops);
  mt_pthread_mutex_unlock(&sch->rx_anc_mgr_mutex);
  if (!s) {
    err("%s, rx_ancillary_sessions_mgr_attach fail\n", __func__);
    mt_sch_put(sch, quota_mbs);
    mt_rte_free(s_impl);
    return NULL;
  }

  s_impl->parent = impl;
  s_impl->type = MT_HANDLE_RX_ANC;
  s_impl->sch = sch;
  s_impl->quota_mbs = quota_mbs;
  s_impl->impl = s;
  s->st40_handle = s_impl;

  rte_atomic32_inc(&impl->st40_rx_sessions_cnt);
  notice("%s(%d,%d), succ on %p\n", __func__, sch->idx, s->idx, s);
  return s_impl;
}

int st40_rx_update_source(st40_rx_handle handle, struct st_rx_source_info* src) {
  struct st_rx_ancillary_session_handle_impl* s_impl = handle;
  struct st_rx_ancillary_session_impl* s;
  struct mtl_sch_impl* sch;
  int idx, ret, sch_idx;

  if (s_impl->type != MT_HANDLE_RX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;
  idx = s->idx;
  sch = s_impl->sch;
  sch_idx = sch->idx;

  ret = st_rx_source_info_check(src, s->ops.num_port);
  if (ret < 0) return ret;

  ret = rx_ancillary_sessions_mgr_update_src(&sch->rx_anc_mgr, s, src);
  if (ret < 0) {
    err("%s(%d,%d), online update fail %d\n", __func__, sch_idx, idx, ret);
    return ret;
  }

  info("%s(%d,%d), succ\n", __func__, sch_idx, idx);
  return 0;
}

int st40_rx_free(st40_rx_handle handle) {
  struct st_rx_ancillary_session_handle_impl* s_impl = handle;
  struct mtl_main_impl* impl;
  struct st_rx_ancillary_session_impl* s;
  struct mtl_sch_impl* sch;
  int ret, idx;
  int sch_idx;

  if (s_impl->type != MT_HANDLE_RX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parent;
  s = s_impl->impl;
  idx = s->idx;
  sch = s_impl->sch;
  sch_idx = sch->idx;
  notice("%s(%d,%d), start\n", __func__, sch_idx, idx);

  mt_pthread_mutex_lock(&sch->rx_anc_mgr_mutex);
  ret = rx_ancillary_sessions_mgr_detach(&sch->rx_anc_mgr, s);
  mt_pthread_mutex_unlock(&sch->rx_anc_mgr_mutex);
  if (ret < 0) err("%s(%d, %d), mgr detach fail\n", __func__, sch_idx, idx);

  ret = mt_sch_put(sch, s_impl->quota_mbs);
  if (ret < 0) err("%s(%d, %d), mt_sch_put fail\n", __func__, sch_idx, idx);

  mt_rte_free(s_impl);

  /* update max idx */
  mt_pthread_mutex_lock(&sch->rx_anc_mgr_mutex);
  rx_ancillary_sessions_mgr_update(&sch->rx_anc_mgr);
  mt_pthread_mutex_unlock(&sch->rx_anc_mgr_mutex);

  rte_atomic32_dec(&impl->st40_rx_sessions_cnt);
  notice("%s(%d,%d), succ\n", __func__, sch_idx, idx);
  return 0;
}

void* st40_rx_get_mbuf(st40_rx_handle handle, void** usrptr, uint16_t* len) {
  struct st_rx_ancillary_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt;
  struct st_rx_ancillary_session_impl* s;
  struct rte_ring* packet_ring;
  int idx, ret;

  if (s_impl->type != MT_HANDLE_RX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;
  idx = s->idx;
  packet_ring = s->packet_ring;
  if (!packet_ring) {
    err("%s(%d), packet ring is not created\n", __func__, idx);
    return NULL;
  }

  ret = rte_ring_sc_dequeue(packet_ring, (void**)&pkt);
  if (ret == 0) {
    int header_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                     sizeof(struct rte_udp_hdr);
    *len = pkt->data_len - header_len;
    *usrptr = rte_pktmbuf_mtod_offset(pkt, void*, header_len);
    return (void*)pkt;
  }

  return NULL;
}

void st40_rx_put_mbuf(st40_rx_handle handle, void* mbuf) {
  struct st_rx_ancillary_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = (struct rte_mbuf*)mbuf;
  struct st_rx_ancillary_session_impl* s;

  if (s_impl->type != MT_HANDLE_RX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return;
  }

  s = s_impl->impl;
  MTL_MAY_UNUSED(s);

  if (pkt) rte_pktmbuf_free(pkt);
  MT_USDT_ST40_RX_MBUF_PUT(s->mgr->idx, s->idx, mbuf);
}

int st40_rx_put_framebuff(st40_rx_handle handle, void* frame) {
  struct st_rx_ancillary_session_handle_impl* s_impl = handle;
  struct st_rx_ancillary_session_impl* s;

  if (s_impl->type != MT_HANDLE_RX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;
  if (!s->frame_slots) {
    err("%s(%d), session is not in FRAME_LEVEL mode\n", __func__, s->idx);
    return -EIO;
  }

  for (uint16_t i = 0; i < s->frame_slots_cnt; i++) {
    struct st_rx_anc_frame_slot* slot = &s->frame_slots[i];
    if (slot->udw_buf == frame) {
      slot->state = ST_RX_ANC_SLOT_FREE;
      return 0;
    }
  }

  err("%s(%d), invalid frame %p\n", __func__, s->idx, frame);
  return -EIO;
}

int st40_rx_get_queue_meta(st40_rx_handle handle, struct st_queue_meta* meta) {
  struct st_rx_ancillary_session_handle_impl* s_impl = handle;
  struct st_rx_ancillary_session_impl* s;

  if (s_impl->type != MT_HANDLE_RX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;

  memset(meta, 0x0, sizeof(*meta));
  meta->num_port = RTE_MIN(s->ops.num_port, MTL_SESSION_PORT_MAX);
  for (uint8_t i = 0; i < meta->num_port; i++) {
    meta->queue_id[i] = rx_ancillary_queue_id(s, i);
  }

  return 0;
}

int st40_rx_get_session_stats(st40_rx_handle handle, struct st40_rx_user_stats* stats) {
  struct st_rx_ancillary_session_handle_impl* s_impl = handle;

  if (!handle || !stats) {
    err("%s, invalid handle %p or stats %p\n", __func__, handle, stats);
    return -EINVAL;
  }

  if (s_impl->type != MT_HANDLE_RX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }
  struct st_rx_ancillary_session_impl* s = s_impl->impl;

  rte_spinlock_lock(&s->mgr->mutex[s->idx]);
  memcpy(stats, &s->port_user_stats, sizeof(*stats));
  rte_spinlock_unlock(&s->mgr->mutex[s->idx]);
  return 0;
}

int st40_rx_reset_session_stats(st40_rx_handle handle) {
  struct st_rx_ancillary_session_handle_impl* s_impl = handle;

  if (!handle) {
    err("%s, invalid handle %p\n", __func__, handle);
    return -EINVAL;
  }

  if (s_impl->type != MT_HANDLE_RX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }
  struct st_rx_ancillary_session_impl* s = s_impl->impl;

  rte_spinlock_lock(&s->mgr->mutex[s->idx]);
  memset(&s->port_user_stats, 0, sizeof(s->port_user_stats));
  memset(&s->stat_snapshot, 0, sizeof(s->stat_snapshot));
  rte_atomic32_set(&s->stat_frames_received, 0);
  rte_spinlock_unlock(&s->mgr->mutex[s->idx]);
  return 0;
}
