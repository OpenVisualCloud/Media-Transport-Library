/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_RX_AUDIO_SESSION_HEAD_H_
#define _ST_LIB_RX_AUDIO_SESSION_HEAD_H_

#include "st_main.h"

#define ST_RX_AUDIO_BURTS_SIZE (128)

int st_rx_audio_sessions_mgr_uinit(struct st_rx_audio_sessions_mgr* mgr);

void st_rx_audio_sessions_stat(struct st_main_impl* impl);

#endif