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

#include <SDL2/SDL_thread.h>
#include <pthread.h>

#include "app_base.h"
#include "log.h"

int st_app_player_uinit(struct st_app_context* ctx);
int st_app_player_init(struct st_app_context* ctx);

int st_app_attach_display(struct st_app_rx_video_session* video);
int st_app_dettach_display(struct st_app_rx_video_session* video);

int st_app_display_frame(struct st_display* display, uint8_t const* frame);
