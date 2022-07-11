/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_TX_AUDIO_SESSION_HEAD_H_
#define _ST_LIB_TX_AUDIO_SESSION_HEAD_H_

#include "st_main.h"

int st_tx_audio_sessions_mgr_init(struct st_main_impl* impl, struct st_sch_impl* sch,
                                  struct st_tx_audio_sessions_mgr* mgr);
int st_tx_audio_sessions_mgr_uinit(struct st_tx_audio_sessions_mgr* mgr);

struct st_tx_audio_session_impl* st_tx_audio_sessions_mgr_attach(
    struct st_tx_audio_sessions_mgr* mgr, struct st30_tx_ops* ops);
int st_tx_audio_sessions_mgr_detach(struct st_tx_audio_sessions_mgr* mgr,
                                    struct st_tx_audio_session_impl* s);

int st_tx_audio_sessions_mgr_update(struct st_tx_audio_sessions_mgr* mgr);

void st_tx_audio_sessions_stat(struct st_main_impl* impl);

#endif
