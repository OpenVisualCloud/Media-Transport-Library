/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 * The data path based on linux kernel socket interface
 */

#include "mt_af_xdp.h"

#include "../mt_log.h"

struct mt_xdp_queue {
  struct xsk_umem_info* umem;
  struct xsk_socket* xsk;
};

struct mt_xdp_priv {
  enum mtl_port port;
  uint8_t start_queue;
  uint16_t queues_cnt;
  struct mt_xdp_queue* queues_info;
};

static int xdp_free(struct mt_xdp_priv* xdp) {
  if (xdp->queues_info) {
    mt_rte_free(xdp->queues_info);
    xdp->queues_info = NULL;
  }

  mt_rte_free(xdp);
  return 0;
}

int mt_dev_xdp_init(struct mt_interface* inf) {
  struct mtl_main_impl* impl = inf->parent;
  enum mtl_port port = inf->port;
  struct mtl_init_params* p = mt_get_user_params(impl);

  if (!mt_pmd_is_native_af_xdp(impl, port)) {
    err("%s(%d), not native af_xdp\n", __func__, port);
    return -EIO;
  }

  struct mt_xdp_priv* xdp = mt_rte_zmalloc_socket(sizeof(*xdp), mt_socket_id(impl, port));
  if (!xdp) {
    err("%s(%d), xdp malloc fail\n", __func__, port);
    return -ENOMEM;
  }
  xdp->port = port;
  xdp->start_queue = p->xdp_info[port].start_queue;
  xdp->queues_cnt = RTE_MAX(inf->max_tx_queues, inf->max_rx_queues);
  xdp->queues_info = mt_rte_zmalloc_socket(sizeof(*xdp->queues_info) * xdp->queues_cnt,
                                           mt_socket_id(impl, port));
  if (!xdp->queues_info) {
    err("%s(%d), xdp queues_info malloc fail\n", __func__, port);
    xdp_free(xdp);
    return -ENOMEM;
  }
  for (uint16_t q = 0; q < xdp->queues_cnt; q++) {
    struct mt_xdp_queue* xq = &xdp->queues_info[q];

    xq->umem = NULL;
    xq->xsk = NULL;
  }

  inf->xdp = xdp;
  info("%s(%d), start queue %u cnt %u\n", __func__, port, xdp->start_queue,
       xdp->queues_cnt);
  return 0;
}

int mt_dev_xdp_uinit(struct mt_interface* inf) {
  struct mt_xdp_priv* xdp = inf->xdp;
  if (!xdp) return 0;

  xdp_free(xdp);
  inf->xdp = NULL;
  dbg("%s(%d), succ\n", __func__, inf->port);
  return 0;
}

struct mt_tx_xdp_entry* mt_tx_xdp_get(struct mtl_main_impl* impl, enum mtl_port port,
                                      struct mt_txq_flow* flow) {
  if (!mt_pmd_is_native_af_xdp(impl, port)) {
    err("%s(%d), this pmd is not native xdp\n", __func__, port);
    return NULL;
  }

  struct mt_tx_xdp_entry* entry =
      mt_rte_zmalloc_socket(sizeof(*entry), mt_socket_id(impl, port));
  if (!entry) {
    err("%s(%d), entry malloc fail\n", __func__, port);
    return NULL;
  }
  entry->parent = impl;
  entry->port = port;
  rte_memcpy(&entry->flow, flow, sizeof(entry->flow));

  uint8_t* ip = flow->dip_addr;
  info("%s(%d), ip %u.%u.%u.%u, port %u, queue %u\n", __func__, port, ip[0], ip[1], ip[2],
       ip[3], flow->dst_port, entry->queue_id);
  return entry;
}

int mt_tx_xdp_put(struct mt_tx_xdp_entry* entry) {
  mt_rte_free(entry);
  return 0;
}

uint16_t mt_tx_xdp_burst(struct mt_tx_xdp_entry* entry, struct rte_mbuf** tx_pkts,
                         uint16_t nb_pkts) {
  MTL_MAY_UNUSED(entry);
  rte_pktmbuf_free_bulk(tx_pkts, nb_pkts);
  return nb_pkts;
}

struct mt_rx_xdp_entry* mt_rx_xdp_get(struct mtl_main_impl* impl, enum mtl_port port,
                                      struct mt_rxq_flow* flow) {
  if (!mt_pmd_is_native_af_xdp(impl, port)) {
    err("%s(%d), this pmd is not native xdp\n", __func__, port);
    return NULL;
  }

  struct mt_rx_xdp_entry* entry =
      mt_rte_zmalloc_socket(sizeof(*entry), mt_socket_id(impl, port));
  if (!entry) {
    err("%s(%d), entry malloc fail\n", __func__, port);
    return NULL;
  }
  entry->parent = impl;
  entry->port = port;
  rte_memcpy(&entry->flow, flow, sizeof(entry->flow));

  uint8_t* ip = flow->dip_addr;
  info("%s(%d), ip %u.%u.%u.%u port %u queue %d\n", __func__, port, ip[0], ip[1], ip[2],
       ip[3], flow->dst_port, entry->queue_id);
  return entry;
}

int mt_rx_xdp_put(struct mt_rx_xdp_entry* entry) {
  mt_rte_free(entry);
  return 0;
}

uint16_t mt_rx_xdp_burst(struct mt_rx_xdp_entry* entry, struct rte_mbuf** rx_pkts,
                         const uint16_t nb_pkts) {
  MTL_MAY_UNUSED(entry);
  MTL_MAY_UNUSED(rx_pkts);
  MTL_MAY_UNUSED(nb_pkts);
  return 0;
}
