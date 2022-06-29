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

#ifndef _PLAYER_HEAD_H_
#define _PLAYER_HEAD_H_

int st_app_player_uinit(struct st_app_context* ctx);
int st_app_player_init(struct st_app_context* ctx);

int st_app_init_display(struct st_display* d, int idx, int width, int height, char* font);
int st_app_uinit_display(struct st_display* d);

#endif