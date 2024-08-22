/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "app_base.h"
#include "log.h"

#ifndef _RX_APP_FMD_HEAD_H_
#define _RX_APP_FMD_HEAD_H_

int st_app_rx_fmd_sessions_init(struct st_app_context* ctx);

int st_app_rx_fmd_sessions_uinit(struct st_app_context* ctx);

int st_app_rx_fmd_sessions_result(struct st_app_context* ctx);

#endif