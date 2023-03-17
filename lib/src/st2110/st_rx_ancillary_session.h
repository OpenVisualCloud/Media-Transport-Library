/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_RX_ANCILLARY_SESSION_HEAD_H_
#define _ST_LIB_RX_ANCILLARY_SESSION_HEAD_H_

#include "st_main.h"

#define ST_RX_ANCILLARY_BURST_SIZE (128)

int st_rx_ancillary_sessions_mgr_uinit(struct st_rx_ancillary_sessions_mgr* mgr);

void st_rx_ancillary_sessions_stat(struct mtl_main_impl* impl);

#endif