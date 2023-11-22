/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_AUDIO_TRANSMITTER_HEAD_H_
#define _ST_LIB_AUDIO_TRANSMITTER_HEAD_H_

#include "st_main.h"

int st_audio_transmitter_init(struct mtl_main_impl* impl, struct mtl_sch_impl* sch,
                              struct st_tx_audio_sessions_mgr* mgr,
                              struct st_audio_transmitter_impl* trs);
int st_audio_transmitter_uinit(struct st_audio_transmitter_impl* trs);

int st_audio_queue_fatal_error(struct mtl_main_impl* impl,
                               struct st_tx_audio_sessions_mgr* mgr, enum mtl_port port);

#endif
