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

#ifndef _ST_LIB_RX_VIDEO_SESSION_HEAD_H_
#define _ST_LIB_RX_VIDEO_SESSION_HEAD_H_

#include "st_main.h"

#define ST_RX_VIDEO_BURTS_SIZE (128)

#define ST_RV_EBU_TSC_SYNC_MS (100) /* sync tsc with ptp period(ms) */
#define ST_RV_EBU_TSC_SYNC_NS (ST_RV_EBU_TSC_SYNC_MS * 1000 * 1000)

int st_rx_video_sessions_mgr_init(struct st_main_impl* impl, struct st_sch_impl* sch,
                                  struct st_rx_video_sessions_mgr* mgr);
int st_rx_video_sessions_mgr_uinit(struct st_rx_video_sessions_mgr* mgr);

struct st_rx_video_session_impl* st_rx_video_sessions_mgr_attach(
    struct st_rx_video_sessions_mgr* mgr, struct st20_rx_ops* ops);
int st_rx_video_sessions_mgr_detach(struct st_rx_video_sessions_mgr* mgr,
                                    struct st_rx_video_session_impl* s);

void st_rx_video_sessions_stat(struct st_main_impl* impl);

int st_rx_video_session_put_frame(struct st_rx_video_session_impl* s, void* frame);

int st_rx_video_sessions_mgr_update_src(struct st_rx_video_sessions_mgr* mgr,
                                        struct st_rx_video_session_impl* s,
                                        struct st_rx_source_info* src);

int st_rx_video_sessions_mgr_update(struct st_rx_video_sessions_mgr* mgr);

#endif
