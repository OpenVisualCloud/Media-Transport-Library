/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_RX_ANCILLARY_SESSION_HEAD_H_
#define _ST_LIB_RX_ANCILLARY_SESSION_HEAD_H_

#include "st_main.h"

#define ST_RX_ANCILLARY_BURST_SIZE (128)

#define ST_RX_ANCILLARY_PREFIX "RC_"

#ifdef MTL_ENABLE_FUZZING_ST40
int st_rx_ancillary_session_fuzz_handle_pkt(struct mtl_main_impl* impl,
                                            struct st_rx_ancillary_session_impl* s,
                                            struct rte_mbuf* mbuf,
                                            enum mtl_session_port s_port);
void st_rx_ancillary_session_fuzz_reset(struct st_rx_ancillary_session_impl* s);
#endif

int st_rx_ancillary_sessions_sch_uinit(struct mtl_sch_impl* sch);

#endif