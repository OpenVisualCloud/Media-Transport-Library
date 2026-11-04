/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "st_rx_mgr_queue.h"

#include "../datapath/mt_queue.h"
#include "../mt_flow.h"
#include "../mt_log.h"

/* the plain dpdk queue path of mt_rxq_get(), with a flow rule that matches the IP: only
 * there a flow rule can steer more than one session to one queue, and only with the IP
 * the dispatch gives a packet to the same session as the rule */
static bool rx_mgr_queue_usable(struct mtl_main_impl* impl, enum mtl_port port) {
  if (mt_pmd_is_kernel_socket(impl, port) || mt_has_srss(impl, port) ||
      mt_user_shared_rxq(impl, port) || mt_pmd_is_native_af_xdp(impl, port) ||
      mt_pmd_is_dpdk_af_packet(impl, port) || mt_has_cni_rx(impl, port))
    return false;
  if (mt_if(impl, port)->drv_info.flow_type == MT_FLOW_NO_IP) return false;
  return !(mt_get_user_params(impl)->flags & MTL_FLAG_RX_UDP_PORT_ONLY);
}

int st_rx_mgr_queue_prepare(struct mtl_main_impl* impl, struct st_rx_mgr_queue* q,
                            enum mtl_port port) {
  if (atomic_load_explicit(&q->rxq[port], memory_order_relaxed)) return 0;
  if (!rx_mgr_queue_usable(impl, port)) return -ENOTSUP;

  _Atomic uint32_t* map =
      mt_rte_zmalloc_socket(sizeof(*map) * (UINT16_MAX + 1), mt_socket_id(impl, port));
  if (!map) return -ENOMEM;
  struct mt_rxq_entry* rxq = mt_rxq_get(impl, port, NULL);
  if (!rxq) {
    mt_rte_free((void*)map);
    return -EIO;
  }
  q->map[port] = map;
  /* the tasklet reads the map once it sees the queue */
  atomic_store_explicit(&q->rxq[port], rxq, memory_order_release);
  info("%s(%d), queue %u\n", __func__, port, mt_rxq_queue_id(rxq));
  return 0;
}

int st_rx_mgr_queue_add(struct mtl_main_impl* impl, struct st_rx_mgr_queue* q,
                        enum mtl_port port, struct mt_rxq_flow* flow, uint32_t key,
                        struct mt_rx_flow_rsp** rsp) {
  /* update_src calls this without the manager mutex, while prepare can publish */
  struct mt_rxq_entry* rxq = atomic_load_explicit(&q->rxq[port], memory_order_acquire);
  _Atomic uint32_t* slot;
  uint32_t v = 0;

  if (!rxq) return -ENOENT;
  slot = &q->map[port][flow->dst_port];
  /* a udp port that another session of the manager has: that session gets the packets */
  if (!atomic_compare_exchange_strong_explicit(slot, &v, key + 1, memory_order_acq_rel,
                                               memory_order_acquire))
    return -EEXIST;
  *rsp = mt_rx_flow_create(impl, port, mt_rxq_queue_id(rxq), flow);
  if (!*rsp) {
    atomic_store_explicit(slot, 0, memory_order_release);
    return -EIO;
  }
  return 0;
}

void st_rx_mgr_queue_del(struct mtl_main_impl* impl, struct st_rx_mgr_queue* q,
                         enum mtl_port port, uint16_t dst_port, uint32_t key,
                         struct mt_rx_flow_rsp* rsp) {
  uint32_t v = key + 1;

  /* first the map, so the tasklet gives no packet to the session any more */
  atomic_compare_exchange_strong_explicit(&q->map[port][dst_port], &v, 0,
                                          memory_order_acq_rel, memory_order_acquire);
  mt_rx_flow_free(impl, port, rsp);
}

static inline struct mt_udp_hdr* rx_mgr_queue_hdr(struct rte_mbuf* pkt) {
  if (rte_pktmbuf_data_len(pkt) < sizeof(struct mt_udp_hdr)) return NULL;
  return rte_pktmbuf_mtod(pkt, struct mt_udp_hdr*);
}

/* a packet of a udp port that has a kept packet waits behind it: the session gets its
 * packets in the order of the queue */
static bool rx_mgr_queue_waits(struct rte_mbuf** defer, uint16_t kept,
                               struct rte_mbuf* pkt) {
  struct mt_udp_hdr* hdr = rx_mgr_queue_hdr(pkt);

  if (!hdr) return false;
  for (uint16_t i = 0; i < kept; i++) {
    if (rte_pktmbuf_mtod(defer[i], struct mt_udp_hdr*)->udp.dst_port == hdr->udp.dst_port)
      return true;
  }
  return false;
}

static inline int rx_mgr_queue_dispatch(_Atomic uint32_t* map, enum mtl_port port,
                                        struct rte_mbuf* pkt,
                                        st_rx_mgr_queue_dispatch_fn fn, void* priv) {
  struct mt_udp_hdr* hdr = rx_mgr_queue_hdr(pkt);
  uint32_t v;

  if (!hdr) return -ENOENT;
  v = atomic_load_explicit(&map[ntohs(hdr->udp.dst_port)], memory_order_acquire);
  if (!v) return -ENOENT;
  return fn(priv, port, v - 1, pkt);
}

int st_rx_mgr_queue_tasklet(struct st_rx_mgr_queue* q, enum mtl_port port, uint16_t burst,
                            st_rx_mgr_queue_dispatch_fn fn, void* priv) {
  struct mt_rxq_entry* rxq = atomic_load_explicit(&q->rxq[port], memory_order_acquire);
  struct rte_mbuf* pkts[ST_RX_MGR_QUEUE_BURST_MAX];
  struct rte_mbuf** defer = q->defer[port];
  _Atomic uint32_t* map = q->map[port];
  uint16_t nd = q->defer_n[port], kept = 0, given = 0, rv = 0;

  if (!rxq) return MTL_TASKLET_ALL_DONE;
  burst = RTE_MIN(RTE_MAX(burst, ST_RX_MGR_QUEUE_BURST_MIN), ST_RX_MGR_QUEUE_BURST_MAX);

  for (uint16_t i = 0; i < nd; i++) {
    if (given >= burst || (kept && rx_mgr_queue_waits(defer, kept, defer[i])) ||
        rx_mgr_queue_dispatch(map, port, defer[i], fn, priv) == -EBUSY) {
      defer[kept++] = defer[i];
      continue;
    }
    given++;
    rte_pktmbuf_free(defer[i]);
  }

  /* the work of one call stays bounded: read the queue only when the list gave less */
  if (given < burst) rv = mt_rxq_burst(rxq, pkts, burst);
  for (uint16_t i = 0; i < rv; i++) {
    if ((kept && rx_mgr_queue_waits(defer, kept, pkts[i])) ||
        rx_mgr_queue_dispatch(map, port, pkts[i], fn, priv) == -EBUSY) {
      if (kept < ST_RX_MGR_QUEUE_DEFER_MAX) {
        defer[kept++] = pkts[i];
        continue;
      }
      atomic_fetch_add_explicit(&q->stat_defer_drop, 1, memory_order_relaxed);
    }
    rte_pktmbuf_free(pkts[i]);
  }
  q->defer_n[port] = kept;

  /* work is left when the list gave a full burst, the queue gave one, or packets wait */
  return (given >= burst || rv == burst || kept) ? MTL_TASKLET_HAS_PENDING
                                                 : MTL_TASKLET_ALL_DONE;
}

void st_rx_mgr_queue_uinit(struct st_rx_mgr_queue* q) {
  for (int port = 0; port < MTL_PORT_MAX; port++) {
    struct mt_rxq_entry* rxq = atomic_load_explicit(&q->rxq[port], memory_order_relaxed);

    for (uint16_t i = 0; i < q->defer_n[port]; i++) rte_pktmbuf_free(q->defer[port][i]);
    q->defer_n[port] = 0;
    if (rxq) {
      mt_rxq_put(rxq);
      atomic_store_explicit(&q->rxq[port], NULL, memory_order_relaxed);
    }
    if (q->map[port]) {
      mt_rte_free((void*)q->map[port]);
      q->map[port] = NULL;
    }
  }
  atomic_store_explicit(&q->stat_defer_drop, 0, memory_order_relaxed);
  atomic_store_explicit(&q->own_rxq_cnt, 0, memory_order_relaxed);
}
