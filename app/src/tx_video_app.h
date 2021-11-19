/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "app_base.h"
#include "log.h"

#ifndef _TX_APP_VIDEO_HEAD_H_
#define _TX_APP_VIDEO_HEAD_H_
int st_app_tx_video_sessions_init(struct st_app_context* ctx);

int st_app_tx_video_sessions_stop(struct st_app_context* ctx);

int st_app_tx_video_sessions_handle_uinit(struct st_app_context* ctx);

int st_app_tx_video_sessions_uinit(struct st_app_context* ctx);
#endif