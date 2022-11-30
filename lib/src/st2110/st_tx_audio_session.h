/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_TX_AUDIO_SESSION_HEAD_H_
#define _ST_LIB_TX_AUDIO_SESSION_HEAD_H_

#include "st_main.h"

int st_tx_audio_sessions_mgr_uinit(struct st_tx_audio_sessions_mgr* mgr);

void st_tx_audio_sessions_stat(struct mtl_main_impl* impl);

#endif
