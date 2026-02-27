/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#ifndef _TX_APP_ST40P_HEAD_H_
#define _TX_APP_ST40P_HEAD_H_

#include "app_base.h"

int st_app_tx_st40p_sessions_init(struct st_app_context* ctx);
int st_app_tx_st40p_sessions_stop(struct st_app_context* ctx);
int st_app_tx_st40p_sessions_uinit(struct st_app_context* ctx);

#endif
