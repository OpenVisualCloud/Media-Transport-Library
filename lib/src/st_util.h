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

#ifndef _ST_LIB_UTIL_HEAD_H_
#define _ST_LIB_UTIL_HEAD_H_

#include "st_main.h"

bool st_bitmap_test_and_set(uint8_t* bitmap, int idx);

int st_ring_dequeue_clean(struct rte_ring* ring);

void st_mbuf_sanity_check(struct rte_mbuf** mbufs, uint16_t nb, char* tag);

int st_pacing_train_result_add(struct st_main_impl* impl, enum st_port port,
                               uint64_t rl_bps, float pad_interval);

int st_pacing_train_result_search(struct st_main_impl* impl, enum st_port port,
                                  uint64_t rl_bps, float* pad_interval);

int st_build_port_map(struct st_main_impl* impl, char** ports, enum st_port* maps,
                      int num_ports);

/* logical session port to main(physical) port */
static inline enum st_port st_port_logic2phy(enum st_port* maps,
                                             enum st_session_port logic) {
  return maps[logic];
}

void st_video_rtp_dump(enum st_port port, int idx, char* tag,
                       struct st20_rfc4175_rtp_hdr* rtp);

void st_mbuf_dump(enum st_port port, int idx, char* tag, struct rte_mbuf* m);

void st_lcore_dump();

void st_eth_link_dump(uint16_t port_id);

/* 7 bits payload type define in RFC3550 */
static inline bool st_is_valid_payload_type(int payload_type) {
  if (payload_type > 0 && payload_type < 0x7F)
    return true;
  else
    return false;
}

void st_eth_macaddr_dump(enum st_port port, char* tag, struct rte_ether_addr* mac_addr);

static inline bool st_rx_seq_drop(uint16_t new_id, uint16_t old_id, uint16_t delta) {
  if ((new_id <= old_id) && ((old_id - new_id) < delta))
    return true;
  else
    return false;
}

struct rte_mbuf* st_build_pad(struct st_main_impl* impl, enum st_port port,
                              uint16_t port_id, uint16_t ether_type, uint16_t len);

#endif
