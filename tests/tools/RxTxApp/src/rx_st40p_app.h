/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef _RX_APP_ST40P_HEAD_H_
#define _RX_APP_ST40P_HEAD_H_

#include "app_base.h"

int st_app_rx_st40p_sessions_init(struct st_app_context* ctx);
int st_app_rx_st40p_sessions_uinit(struct st_app_context* ctx);
int st_app_rx_st40p_sessions_result(struct st_app_context* ctx);

#endif
