/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Ported from DPDK 26.07 drivers/net/intel/common/tx_scalar.h (ci_xmit_pkts(),
 * ci_tx_xmit_cleanup()) and iavf/iavf_rxtx.c (iavf_tx_done_cleanup_full()).
 */

#include "common/ut_tx_ring.h"

#include <stdbool.h>
#include <stdlib.h>

#include "common/ut_common.h"

#define UT_ETH_PORTS (2)
#define UT_ETH_TX_QUEUES (8)

struct ut_txr_slot {
  struct rte_mbuf* mbuf;
  bool freed;   /* the cleanup freed mbuf and left it in the slot */
  bool written; /* a reset ring reads every descriptor done */
  bool rs;
  uint64_t done_ns;
};

struct ut_txr {
  enum ut_txr_mode mode;
  uint16_t nb_desc;
  uint16_t rs_thresh;
  uint16_t free_thresh;
  uint16_t tail;
  uint16_t nb_free;
  uint16_t last_cleaned;
  uint16_t* rs_last_id;
  uint64_t drain_ns;
  uint64_t last_done_ns;
  struct ut_txr_slot* slots;
  uint64_t descs;
  uint64_t xmits;
  uint64_t double_frees;
};

static struct ut_txr* ut_eth_rings[UT_ETH_PORTS][UT_ETH_TX_QUEUES];
static int ut_eth_done_cleanup_calls;

struct ut_txr* ut_txr_create(enum ut_txr_mode mode, uint16_t nb_desc, uint16_t rs_thresh,
                             uint16_t free_thresh, uint64_t drain_ns) {
  if (!rs_thresh || nb_desc % rs_thresh) return NULL;
  struct ut_txr* r = calloc(1, sizeof(*r));
  if (!r) return NULL;
  r->slots = calloc(nb_desc, sizeof(*r->slots));
  r->rs_last_id = calloc(nb_desc / rs_thresh, sizeof(*r->rs_last_id));
  if (!r->slots || !r->rs_last_id) {
    ut_txr_destroy(r);
    return NULL;
  }
  r->mode = mode;
  r->nb_desc = nb_desc;
  r->rs_thresh = rs_thresh;
  r->free_thresh = free_thresh;
  r->last_cleaned = nb_desc - 1;
  r->nb_free = nb_desc - 1;
  r->drain_ns = drain_ns;
  return r;
}

void ut_txr_destroy(struct ut_txr* r) {
  if (!r) return;
  for (uint16_t i = 0; r->slots && i < r->nb_desc; i++) {
    if (r->slots[i].mbuf && !r->slots[i].freed) rte_pktmbuf_free_seg(r->slots[i].mbuf);
  }
  free(r->rs_last_id);
  free(r->slots);
  free(r);
}

void ut_txr_set_drain_ns(struct ut_txr* r, uint64_t drain_ns) {
  r->drain_ns = drain_ns;
}

static bool txr_desc_done(const struct ut_txr_slot* slot, uint64_t now_ns) {
  return !slot->written || (slot->rs && now_ns >= slot->done_ns);
}

static void txr_free(struct ut_txr* r, struct ut_txr_slot* slot) {
  if (slot->freed)
    r->double_frees++;
  else
    rte_pktmbuf_free_seg(slot->mbuf);
}

/* ci_tx_xmit_cleanup() */
static int txr_cleanup(struct ut_txr* r, uint64_t now_ns) {
  uint16_t rs_idx =
      r->last_cleaned == r->nb_desc - 1 ? 0 : (r->last_cleaned + 1) / r->rs_thresh;
  uint16_t first = rs_idx * r->rs_thresh;

  if (!txr_desc_done(&r->slots[r->rs_last_id[rs_idx]], now_ns)) return -1;
  for (uint16_t i = 0; i < r->rs_thresh && r->mode == UT_TXR_FREE_ON_CLEANUP; i++) {
    struct ut_txr_slot* slot = &r->slots[first + i];
    if (!slot->mbuf) continue;
    txr_free(r, slot);
    slot->freed = true;
  }
  r->last_cleaned = first + r->rs_thresh - 1;
  r->nb_free += r->rs_thresh;
  return 0;
}

/* ci_xmit_pkts() of single and multi segment pkts, no context descriptors */
uint16_t ut_txr_xmit(struct ut_txr* r, struct rte_mbuf** pkts, uint16_t nb_pkts,
                     uint64_t now_ns) {
  uint16_t nb_tx;

  r->xmits++;
  if (r->nb_free < r->free_thresh) txr_cleanup(r, now_ns);
  for (nb_tx = 0; nb_tx < nb_pkts; nb_tx++) {
    struct rte_mbuf* m = pkts[nb_tx];
    uint16_t nb_used = m->nb_segs;
    uint16_t tx_last = (r->tail + nb_used - 1) % r->nb_desc;
    uint16_t pkt_rs_idx = r->tail / r->rs_thresh;

    if (nb_used > r->nb_free) {
      if (txr_cleanup(r, now_ns)) break;
      bool stuck = false;
      while (nb_used > r->rs_thresh && nb_used > r->nb_free && !stuck)
        stuck = txr_cleanup(r, now_ns) != 0;
      if (stuck) break;
    }
    for (; m; m = m->next) {
      struct ut_txr_slot* slot = &r->slots[r->tail];
      if (slot->mbuf && r->mode == UT_TXR_FREE_ON_REUSE) txr_free(r, slot);
      slot->mbuf = m;
      slot->freed = false;
      slot->written = true;
      slot->rs = false;
      if (r->drain_ns == UT_TXR_HUNG || r->last_done_ns == UT_TXR_HUNG)
        r->last_done_ns = UT_TXR_HUNG;
      else
        r->last_done_ns = RTE_MAX(r->last_done_ns, now_ns) + r->drain_ns;
      slot->done_ns = r->last_done_ns;
      r->tail = (r->tail + 1) % r->nb_desc;
      r->descs++;
    }
    r->nb_free -= nb_used;
    if ((tx_last + 1) / r->rs_thresh != pkt_rs_idx) {
      r->slots[tx_last].rs = true;
      r->rs_last_id[pkt_rs_idx] = tx_last;
    }
  }
  return nb_tx;
}

/* iavf_tx_done_cleanup_full() */
int ut_txr_done_cleanup(struct ut_txr* r, uint32_t free_cnt, uint64_t now_ns) {
  uint16_t tx_id = r->tail;
  uint16_t tx_last = tx_id;
  uint32_t pkt_cnt = 0;

  if (r->nb_free == 0 && txr_cleanup(r, now_ns)) return 0;
  uint16_t nb_to_clean = r->nb_free;
  uint16_t nb_free_last = r->nb_free;
  if (!free_cnt) free_cnt = r->nb_desc;

  while (pkt_cnt < free_cnt) {
    do {
      struct ut_txr_slot* slot = &r->slots[tx_id];
      if (slot->mbuf) {
        /* a freed mbuf reads as one segment */
        if (slot->freed || !slot->mbuf->next) pkt_cnt++;
        txr_free(r, slot);
        slot->mbuf = NULL;
        slot->freed = false;
      }
      tx_id = (tx_id + 1) % r->nb_desc;
    } while (--nb_to_clean && pkt_cnt < free_cnt && tx_id != tx_last);

    if (r->rs_thresh > r->nb_desc - r->nb_free || tx_id == tx_last) break;
    if (pkt_cnt < free_cnt) {
      if (txr_cleanup(r, now_ns)) break;
      nb_to_clean = r->nb_free - nb_free_last;
      nb_free_last = r->nb_free;
    }
  }
  return (int)pkt_cnt;
}

uint64_t ut_txr_descs(const struct ut_txr* r) {
  return r->descs;
}

uint64_t ut_txr_xmits(const struct ut_txr* r) {
  return r->xmits;
}

uint64_t ut_txr_double_frees(const struct ut_txr* r) {
  return r->double_frees;
}

uint16_t ut_txr_holds(const struct ut_txr* r, const struct rte_mbuf* m) {
  uint16_t holds = 0;
  for (uint16_t i = 0; i < r->nb_desc; i++)
    if (r->slots[i].mbuf == m && !r->slots[i].freed) holds++;
  return holds;
}

uint16_t ut_txr_in_use(const struct ut_txr* r) {
  return r->nb_desc - 1 - r->nb_free;
}

void ut_eth_attach_tx_ring(uint16_t port_id, uint16_t queue_id, struct ut_txr* r) {
  if (port_id < UT_ETH_PORTS && queue_id < UT_ETH_TX_QUEUES)
    ut_eth_rings[port_id][queue_id] = r;
}

static struct ut_txr* ut_eth_ring(uint16_t port_id, uint16_t queue_id) {
  if (port_id >= UT_ETH_PORTS || queue_id >= UT_ETH_TX_QUEUES) return NULL;
  return ut_eth_rings[port_id][queue_id];
}

uint16_t ut_eth_tx_burst(uint16_t port_id, uint16_t queue_id, struct rte_mbuf** tx_pkts,
                         uint16_t nb_pkts) {
  struct ut_txr* r = ut_eth_ring(port_id, queue_id);
  return r ? ut_txr_xmit(r, tx_pkts, nb_pkts, ut_clock_ns()) : 0;
}

int ut_eth_tx_done_cleanup(uint16_t port_id, uint16_t queue_id, uint32_t free_cnt) {
  struct ut_txr* r = ut_eth_ring(port_id, queue_id);
  ut_eth_done_cleanup_calls++;
  return r ? ut_txr_done_cleanup(r, free_cnt, ut_clock_ns()) : 0;
}

int ut_eth_tx_done_cleanup_calls(void) {
  return ut_eth_done_cleanup_calls;
}

void ut_eth_reset(void) {
  for (int p = 0; p < UT_ETH_PORTS; p++)
    for (int q = 0; q < UT_ETH_TX_QUEUES; q++) ut_eth_rings[p][q] = NULL;
  ut_eth_done_cleanup_calls = 0;
}
