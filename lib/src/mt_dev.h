/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_DEV_HEAD_H_
#define _MT_LIB_DEV_HEAD_H_

#include "mt_main.h"

/* default desc nb for tx and rx */
#define MT_DEV_RX_DESC (4096 / 2)
#define MT_DEV_TX_DESC (4096 / 8)

#define MT_DEV_STAT_INTERVAL_S (10) /* 10s */
#define MT_DEV_STAT_INTERVAL_US(s) (s * US_PER_S)
#define MT_DEV_STAT_M_UNIT (1000 * 1000)

#define MT_DEV_TIMEOUT_INFINITE (INT_MAX)
#define MT_DEV_TIMEOUT_ZERO (0)

#define MT_EAL_MAX_ARGS (32)

#define MT_TX_MEMPOOL_PREFIX "T_"
#define MT_RX_MEMPOOL_PREFIX "R_"

int mt_dev_get_socket(const char* port);

int mt_dev_init(struct mtl_init_params* p, struct mt_kport_info* kport_info);
int mt_dev_uinit(struct mtl_init_params* p);

int mt_dev_create(struct mtl_main_impl* impl);
int mt_dev_free(struct mtl_main_impl* impl);

int mt_dev_start(struct mtl_main_impl* impl);
int mt_dev_stop(struct mtl_main_impl* impl);

int mt_dev_dst_ip_mac(struct mtl_main_impl* impl, uint8_t dip[MTL_IP_ADDR_LEN],
                      struct rte_ether_addr* ea, enum mtl_port port, int timeout_ms);

struct mt_tx_queue* mt_dev_get_tx_queue(struct mtl_main_impl* impl, enum mtl_port port,
                                        struct mt_txq_flow* flow);
int mt_dev_put_tx_queue(struct mtl_main_impl* impl, struct mt_tx_queue* queue);
static inline uint16_t mt_dev_tx_queue_id(struct mt_tx_queue* queue) {
  return queue->queue_id;
}
int mt_dev_set_tx_bps(struct mtl_main_impl* impl, enum mtl_port port, uint16_t q,
                      uint64_t bytes_per_sec);
int mt_dev_flush_tx_queue(struct mtl_main_impl* impl, struct mt_tx_queue* queue,
                          struct rte_mbuf* pad);
int mt_dev_tx_done_cleanup(struct mtl_main_impl* impl, struct mt_tx_queue* queue);
static inline uint16_t mt_dev_tx_burst(struct mt_tx_queue* queue,
                                       struct rte_mbuf** tx_pkts, uint16_t nb_pkts) {
  return rte_eth_tx_burst(queue->port_id, queue->queue_id, tx_pkts, nb_pkts);
}
uint16_t mt_dev_tx_burst_busy(struct mtl_main_impl* impl, struct mt_tx_queue* queue,
                              struct rte_mbuf** tx_pkts, uint16_t nb_pkts,
                              int timeout_ms);

uint16_t mt_dev_tx_sys_queue_burst(struct mtl_main_impl* impl, enum mtl_port port,
                                   struct rte_mbuf** tx_pkts, uint16_t nb_pkts);

struct mt_rx_queue* mt_dev_get_rx_queue(struct mtl_main_impl* impl, enum mtl_port port,
                                        struct mt_rxq_flow* flow);
int mt_dev_put_rx_queue(struct mtl_main_impl* impl, struct mt_rx_queue* queue);
static inline uint16_t mt_dev_rx_queue_id(struct mt_rx_queue* queue) {
  return queue->queue_id;
}
static inline uint16_t mt_dev_rx_burst(struct mt_rx_queue* queue,
                                       struct rte_mbuf** rx_pkts,
                                       const uint16_t nb_pkts) {
  return rte_eth_rx_burst(queue->port_id, queue->queue_id, rx_pkts, nb_pkts);
}

int mt_dev_if_init(struct mtl_main_impl* impl);
int mt_dev_if_uinit(struct mtl_main_impl* impl);
int mt_dev_if_post_init(struct mtl_main_impl* impl);
int mt_dev_if_pre_uinit(struct mtl_main_impl* impl);

int mt_dev_put_lcore(struct mtl_main_impl* impl, unsigned int lcore);
int mt_dev_get_lcore(struct mtl_main_impl* impl, unsigned int* lcore);
bool mt_dev_lcore_valid(struct mtl_main_impl* impl, unsigned int lcore);

int mt_dev_tsc_done_action(struct mtl_main_impl* impl);

uint32_t mt_dev_softrss(uint32_t* input_tuple, uint32_t input_len);

uint16_t mt_dev_rss_hash_queue(struct mtl_main_impl* impl, enum mtl_port port,
                               uint32_t hash);

struct mt_rx_flow_rsp* mt_dev_create_rx_flow(struct mtl_main_impl* impl,
                                             enum mtl_port port, uint16_t q,
                                             struct mt_rxq_flow* flow);
int mt_dev_free_rx_flow(struct mtl_main_impl* impl, enum mtl_port port,
                        struct mt_rx_flow_rsp* rsp);

#endif
