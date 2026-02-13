/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "app_base.h"
#include "log.h"

#ifndef _RX_APP_ST40P_HEAD_H_
#define _RX_APP_ST40P_HEAD_H_

int st_app_rx_st40p_sessions_init(struct st_app_context* ctx);
int st_app_rx_st40p_sessions_uinit(struct st_app_context* ctx);
int st_app_rx_st40p_sessions_stat(struct st_app_context* ctx);
int st_app_rx_st40p_sessions_result(struct st_app_context* ctx);

#endif
