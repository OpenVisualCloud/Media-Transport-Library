/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_DEV_HEAD_H_
#define _MT_LIB_DEV_HEAD_H_

#include "../mt_main.h"

/* default desc nb for tx and rx */
#define MT_DEV_RX_DESC (4096 / 2)
#define MT_DEV_TX_DESC (4096 / 8)

/* how many times we try to detect if the port is up if the flag "allow_down_init"
is NOT set */
#define MT_DEV_DETECT_PORT_UP_RETRY 3

#define MT_EAL_MAX_ARGS (32)

#define MT_TX_MEMPOOL_PREFIX "T_"
#define MT_RX_MEMPOOL_PREFIX "R_"

/* set to 1 to enable the simulated test */
#define MT_DEV_SIMULATE_MALICIOUS_PKT (0)

int mt_dev_get_socket_id(const char* port);

int mt_dev_init(struct mtl_init_params* p, struct mt_kport_info* kport_info);
int mt_dev_uinit(struct mtl_init_params* p);

int mt_dev_create(struct mtl_main_impl* impl);
int mt_dev_free(struct mtl_main_impl* impl);

int mt_dev_start(struct mtl_main_impl* impl);
int mt_dev_stop(struct mtl_main_impl* impl);

struct mt_tx_queue* mt_dev_get_tx_queue(struct mtl_main_impl* impl, enum mtl_port port,
                                        struct mt_txq_flow* flow);
int mt_dev_put_tx_queue(struct mtl_main_impl* impl, struct mt_tx_queue* queue);
static inline uint16_t mt_dev_tx_queue_id(struct mt_tx_queue* queue) {
  return queue->queue_id;
}
int mt_dev_tx_queue_fatal_error(struct mtl_main_impl* impl, struct mt_tx_queue* queue);
int mt_dev_set_tx_bps(struct mtl_main_impl* impl, struct mt_tx_queue* queue,
                      uint64_t bytes_per_sec);
int mt_dpdk_flush_tx_queue(struct mtl_main_impl* impl, struct mt_tx_queue* queue,
                           struct rte_mbuf* pad);
int mt_dev_tx_done_cleanup(struct mtl_main_impl* impl, struct mt_tx_queue* queue);
static inline uint16_t mt_dpdk_tx_burst(struct mt_tx_queue* queue,
                                        struct rte_mbuf** tx_pkts, uint16_t nb_pkts) {
  return rte_eth_tx_burst(queue->port_id, queue->queue_id, tx_pkts, nb_pkts);
}
uint16_t mt_dpdk_tx_burst_busy(struct mtl_main_impl* impl, struct mt_tx_queue* queue,
                               struct rte_mbuf** tx_pkts, uint16_t nb_pkts,
                               int timeout_ms);

struct mt_rx_queue* mt_dev_get_rx_queue(struct mtl_main_impl* impl, enum mtl_port port,
                                        struct mt_rxq_flow* flow);
int mt_dev_put_rx_queue(struct mtl_main_impl* impl, struct mt_rx_queue* queue);
static inline uint16_t mt_dev_rx_queue_id(struct mt_rx_queue* queue) {
  return queue->queue_id;
}
static inline uint16_t mt_dpdk_rx_burst(struct mt_rx_queue* queue,
                                        struct rte_mbuf** rx_pkts,
                                        const uint16_t nb_pkts) {
  return rte_eth_rx_burst(queue->port_id, queue->queue_id, rx_pkts, nb_pkts);
}

int mt_dev_if_init(struct mtl_main_impl* impl);
int mt_dev_setup_port (struct mtl_main_impl* impl, struct mt_interface* inf, enum mt_port_type port_type);
int mt_dev_if_uinit(struct mtl_main_impl* impl);
int mt_dev_if_pre_uinit(struct mtl_main_impl* impl);

int mt_dev_tsc_done_action(struct mtl_main_impl* impl);

uint16_t mt_dev_rss_hash_queue(struct mtl_main_impl* impl, enum mtl_port port,
                               uint32_t hash);

int mt_update_admin_port_stats(struct mtl_main_impl* impl);
int mt_read_admin_port_stats(struct mtl_main_impl* impl, enum mtl_port port,
                             struct mtl_port_status* stats);
int mt_reset_admin_port_stats(struct mtl_main_impl* impl);

#endif
