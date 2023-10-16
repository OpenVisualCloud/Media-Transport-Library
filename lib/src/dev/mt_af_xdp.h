/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _MT_LIB_DEV_AF_XDP_HEAD_H_
#define _MT_LIB_DEV_AF_XDP_HEAD_H_

#include "../mt_main.h"

#ifdef MTL_HAS_XDP_BACKEND

int mt_dev_xdp_init(struct mt_interface* inf);
int mt_dev_xdp_uinit(struct mt_interface* inf);

struct mt_tx_xdp_entry* mt_tx_xdp_get(struct mtl_main_impl* impl, enum mtl_port port,
                                      struct mt_txq_flow* flow);
int mt_tx_xdp_put(struct mt_tx_xdp_entry* entry);
uint16_t mt_tx_xdp_burst(struct mt_tx_xdp_entry* entry, struct rte_mbuf** tx_pkts,
                         uint16_t nb_pkts);

struct mt_rx_xdp_entry* mt_rx_xdp_get(struct mtl_main_impl* impl, enum mtl_port port,
                                      struct mt_rxq_flow* flow);
int mt_rx_xdp_put(struct mt_rx_xdp_entry* entry);
uint16_t mt_rx_xdp_burst(struct mt_rx_xdp_entry* entry, struct rte_mbuf** rx_pkts,
                         const uint16_t nb_pkts);
#else

static inline int mt_dev_xdp_init(struct mt_interface* inf) {
  MTL_MAY_UNUSED(inf);
  return -ENOTSUP;
}

static inline int mt_dev_xdp_uinit(struct mt_interface* inf) {
  MTL_MAY_UNUSED(inf);
  return -ENOTSUP;
}

static inline struct mt_tx_xdp_entry* mt_tx_xdp_get(struct mtl_main_impl* impl,
                                                    enum mtl_port port,
                                                    struct mt_txq_flow* flow) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(port);
  MTL_MAY_UNUSED(flow);
  return NULL;
}

static inline int mt_tx_xdp_put(struct mt_tx_xdp_entry* entry) {
  MTL_MAY_UNUSED(entry);
  return -ENOTSUP;
}

static inline uint16_t mt_tx_xdp_burst(struct mt_tx_xdp_entry* entry,
                                       struct rte_mbuf** tx_pkts, uint16_t nb_pkts) {
  MTL_MAY_UNUSED(entry);
  MTL_MAY_UNUSED(tx_pkts);
  MTL_MAY_UNUSED(nb_pkts);
  return 0;
}

static inline struct mt_rx_xdp_entry* mt_rx_xdp_get(struct mtl_main_impl* impl,
                                                    enum mtl_port port,
                                                    struct mt_rxq_flow* flow) {
  MTL_MAY_UNUSED(impl);
  MTL_MAY_UNUSED(port);
  MTL_MAY_UNUSED(flow);
  return NULL;
}

static inline int mt_rx_xdp_put(struct mt_rx_xdp_entry* entry) {
  MTL_MAY_UNUSED(entry);
  return -ENOTSUP;
}

static inline uint16_t mt_rx_xdp_burst(struct mt_rx_xdp_entry* entry,
                                       struct rte_mbuf** rx_pkts,
                                       const uint16_t nb_pkts) {
  MTL_MAY_UNUSED(entry);
  MTL_MAY_UNUSED(rx_pkts);
  MTL_MAY_UNUSED(nb_pkts);
  return 0;
}
#endif

static inline uint16_t mt_tx_xdp_queue_id(struct mt_tx_xdp_entry* entry) {
  return entry->queue_id;
}

static inline uint16_t mt_rx_xdp_queue_id(struct mt_rx_xdp_entry* entry) {
  return entry->queue_id;
}

#endif
