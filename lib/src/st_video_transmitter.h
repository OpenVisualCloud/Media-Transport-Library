/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_VIDEO_TRANSMITTER_HEAD_H_
#define _ST_LIB_VIDEO_TRANSMITTER_HEAD_H_

#include "st_main.h"

int st_video_transmitter_init(struct mtl_main_impl* impl, struct st_sch_impl* sch,
                              struct st_tx_video_sessions_mgr* mgr,
                              struct st_video_transmitter_impl* trs);
int st_video_transmitter_uinit(struct st_video_transmitter_impl* trs);

int st_video_reslove_pacing_tasklet(struct st_tx_video_session_impl* s,
                                    enum st_session_port port);

#endif
