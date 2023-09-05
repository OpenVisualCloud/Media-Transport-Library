/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_RX_AUDIO_SESSION_HEAD_H_
#define _ST_LIB_RX_AUDIO_SESSION_HEAD_H_

#include "st_main.h"

#define ST_RX_AUDIO_BURST_SIZE (128)

#define ST_RX_AUDIO_PREFIX "RA_"

int st_rx_audio_sessions_sch_uinit(struct mt_sch_impl* sch);

#endif