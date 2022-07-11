/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
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