/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_DEV_HEAD_H_
#define _MT_LIB_DEV_HEAD_H_

#include "mt_main.h"

#define MT_DEV_RX_DESC (4096 / 2)
#define MT_DEV_TX_DESC (4096 / 8)

#define MT_DEV_STAT_INTERVAL_S (10) /* 10s */
#define MT_DEV_STAT_INTERVAL_US(s) (s * US_PER_S)
#define MT_DEV_STAT_M_UNIT (1000 * 1000)

#define MT_EAL_MAX_ARGS (32)

int mt_dev_get_socket(const char* port);

int mt_dev_init(struct mtl_init_params* p, struct mt_kport_info* kport_info);
int mt_dev_uinit(struct mtl_init_params* p);

int mt_dev_create(struct mtl_main_impl* impl);
int mt_dev_free(struct mtl_main_impl* impl);

int mt_dev_start(struct mtl_main_impl* impl);
int mt_dev_stop(struct mtl_main_impl* impl);

int mt_dev_dst_ip_mac(struct mtl_main_impl* impl, uint8_t dip[MTL_IP_ADDR_LEN],
                      struct rte_ether_addr* ea, enum mtl_port port);

int mt_dev_request_tx_queue(struct mtl_main_impl* impl, enum mtl_port port,
                            uint16_t* queue_id, uint64_t bytes_per_sec);
int mt_dev_request_rx_queue(struct mtl_main_impl* impl, enum mtl_port port,
                            uint16_t* queue_id, struct mt_rx_flow* flow);
int mt_dev_free_tx_queue(struct mtl_main_impl* impl, enum mtl_port port,
                         uint16_t queue_id);
int mt_dev_free_rx_queue(struct mtl_main_impl* impl, enum mtl_port port,
                         uint16_t queue_id);
int mt_dev_flush_tx_queue(struct mtl_main_impl* impl, enum mtl_port port,
                          uint16_t queue_id, struct rte_mbuf* pad);

uint16_t mt_dev_tx_sys_queue_burst(struct mtl_main_impl* impl, enum mtl_port port,
                                   struct rte_mbuf** tx_pkts, uint16_t nb_pkts);

int mt_dev_if_init(struct mtl_main_impl* impl);
int mt_dev_if_uinit(struct mtl_main_impl* impl);

int mt_dev_put_lcore(struct mtl_main_impl* impl, unsigned int lcore);
int mt_dev_get_lcore(struct mtl_main_impl* impl, unsigned int* lcore);
bool mt_dev_lcore_valid(struct mtl_main_impl* impl, unsigned int lcore);

#endif
