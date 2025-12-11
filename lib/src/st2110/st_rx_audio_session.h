/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_RX_AUDIO_SESSION_HEAD_H_
#define _ST_LIB_RX_AUDIO_SESSION_HEAD_H_

#include "st_main.h"

#define ST_RX_AUDIO_BURST_SIZE (128)

#define ST_RX_AUDIO_PREFIX "RA_"

#ifdef MTL_ENABLE_FUZZING_ST30
int st_rx_audio_session_fuzz_handle_pkt(struct mtl_main_impl* impl,
                                        struct st_rx_audio_session_impl* s,
                                        struct rte_mbuf* mbuf,
                                        enum mtl_session_port s_port);
void st_rx_audio_session_fuzz_reset(struct st_rx_audio_session_impl* s);
#endif

int st_rx_audio_sessions_sch_uinit(struct mtl_sch_impl* sch);

#endif