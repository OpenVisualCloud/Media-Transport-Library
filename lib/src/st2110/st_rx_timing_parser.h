/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _ST_LIB_RX_EBU_HEAD_H_
#define _ST_LIB_RX_EBU_HEAD_H_

#include "st_main.h"

int rv_tp_init(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s);
int rv_tp_uinit(struct st_rx_video_session_impl* s);

void rv_tp_slot_init(struct st_rv_tp_slot* slot);

void rv_tp_on_packet(struct st_rx_video_session_impl* s, struct st_rv_tp_slot* slot,
                     uint32_t rtp_tmstamp, uint64_t pkt_time, int pkt_idx);

void rv_tp_slot_parse_result(struct st_rx_video_session_impl* s,
                             struct st_rv_tp_slot* slot);

void rv_tp_stat(struct st_rx_video_session_impl* s);

int ra_tp_init(struct mtl_main_impl* impl, struct st_rx_audio_session_impl* s);
int ra_tp_uinit(struct st_rx_audio_session_impl* s);

void ra_tp_on_packet(struct st_rx_audio_session_impl* s, struct st_ra_tp_slot* slot,
                     uint32_t rtp_tmstamp, uint64_t pkt_time);

void ra_tp_slot_parse_result(struct st_rx_audio_session_impl* s,
                             struct st_ra_tp_slot* slot);

void ra_tp_stat(struct st_rx_audio_session_impl* s);

void ra_tp_slot_init(struct st_ra_tp_slot* slot);

#endif