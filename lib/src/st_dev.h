/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

#ifndef _ST_LIB_DEV_HEAD_H_
#define _ST_LIB_DEV_HEAD_H_

#include "st_main.h"

#define ST_DEV_RX_RING_SIZE (4096)
#define ST_DEV_TX_RING_SIZE (4096)

#define ST_DEV_STAT_INTERVAL_S (10) /* 10s */
#define ST_DEV_STAT_INTERVAL_US(s) (s * US_PER_S)
#define ST_DEV_STAT_M_UNIT (1000 * 1000)

#define ST_EAL_MAX_ARGS (32)

/* flow conf for rx queue */
struct st_dev_flow {
  uint8_t dip_addr[ST_IP_ADDR_LEN]; /* rx destination IP */
  uint8_t sip_addr[ST_IP_ADDR_LEN]; /* source IP */
  bool port_flow;                   /* if apply port flow */
  uint16_t dst_port;                /* udp destination port */
  uint16_t src_port;                /* udp source port */
};

int st_dev_get_socket(const char* port);

int st_dev_init(struct st_init_params* p);
int st_dev_uinit(struct st_init_params* p);

int st_dev_create(struct st_main_impl* impl);
int st_dev_free(struct st_main_impl* impl);

int st_dev_start(struct st_main_impl* impl);
int st_dev_stop(struct st_main_impl* impl);

struct st_sch_impl* st_dev_get_sch(struct st_main_impl* impl, int quota_mbs);
int st_dev_put_sch(struct st_sch_impl* sch, int quota_mbs);
int st_dev_start_sch(struct st_main_impl* impl, struct st_sch_impl* sch);

/* ip from 224.x.x.x to 239.x.x.x */
static inline uint64_t st_is_multicast_ip(uint8_t ip[ST_IP_ADDR_LEN]) {
  if (ip[0] >= 224 && ip[0] <= 239)
    return true;
  else
    return false;
}

int st_dev_dst_ip_mac(struct st_main_impl* impl, uint8_t dip[ST_IP_ADDR_LEN],
                      struct rte_ether_addr* ea, enum st_port port);

int st_dev_request_tx_queue(struct st_main_impl* impl, enum st_port port,
                            uint16_t* queue_id, uint64_t bytes_per_sec);
int st_dev_request_rx_queue(struct st_main_impl* impl, enum st_port port,
                            uint16_t* queue_id, struct st_dev_flow* flow);
int st_dev_free_tx_queue(struct st_main_impl* impl, enum st_port port, uint16_t queue_id);
int st_dev_free_rx_queue(struct st_main_impl* impl, enum st_port port, uint16_t queue_id);

int st_dev_if_init(struct st_main_impl* impl);
int st_dev_if_uinit(struct st_main_impl* impl);

int st_dev_put_lcore(struct st_main_impl* impl, unsigned int lcore);
int st_dev_get_lcore(struct st_main_impl* impl, unsigned int* lcore);

#endif
