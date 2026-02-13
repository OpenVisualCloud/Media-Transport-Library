/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "app_base.h"
#include "log.h"
#include "player.h"

#ifndef _TX_APP_ST20P_HEAD_H_
#define _TX_APP_ST20P_HEAD_H_

int st_app_tx_st20p_sessions_init(struct st_app_context* ctx);

int st_app_tx_st20p_sessions_stop(struct st_app_context* ctx);

int st_app_tx_st20p_sessions_uinit(struct st_app_context* ctx);

int st_app_tx_st20p_io_stat(struct st_app_context* ctx);

bool st_app_tx_st20p_sessions_all_complete(struct st_app_context* ctx);

#endif
