/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _ST_LIB_RX_EBU_HEAD_H_
#define _ST_LIB_RX_EBU_HEAD_H_

#include "st_main.h"

int rv_ebu_init(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s);
int rv_ebu_uinit(struct st_rx_video_session_impl* s);

void rv_ebu_slot_init(struct st_rv_ebu_slot* slot);

void rv_ebu_on_packet(struct st_rx_video_session_impl* s, struct st_rv_ebu_slot* slot,
                      uint32_t rtp_tmstamp, uint64_t pkt_time, int pkt_idx);

void rv_ebu_slot_parse_result(struct st_rx_video_session_impl* s,
                              struct st_rv_ebu_slot* slot);

void rv_ebu_stat(struct st_rx_video_session_impl* s);

#endif