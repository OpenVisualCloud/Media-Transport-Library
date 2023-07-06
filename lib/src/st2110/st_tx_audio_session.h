/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_TX_AUDIO_SESSION_HEAD_H_
#define _ST_LIB_TX_AUDIO_SESSION_HEAD_H_

#include "st_main.h"

#define ST_TX_AUDIO_PREFIX "TA_"

int st_tx_audio_sessions_sch_uinit(struct mtl_main_impl* impl, struct mt_sch_impl* sch);

#endif
