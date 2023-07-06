/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <pthread.h>

#include "app_base.h"
#include "log.h"

#ifndef _PLAYER_HEAD_H_
#define _PLAYER_HEAD_H_

#ifdef APP_HAS_SDL2
int st_app_player_uinit(struct st_app_context* ctx);
int st_app_player_init(struct st_app_context* ctx);

int st_app_init_display(struct st_display* d, char* name, int width, int height,
                        char* font);
int st_app_uinit_display(struct st_display* d);
#else
static inline int st_app_player_uinit(struct st_app_context* ctx) {
  warn("%s, not support as build without SDL2\n", __func__);
  return -ENOTSUP;
}
static inline int st_app_player_init(struct st_app_context* ctx) {
  warn("%s, not support as build without SDL2\n", __func__);
  return -ENOTSUP;
}
static inline int st_app_init_display(struct st_display* d, char* name, int width,
                                      int height, char* font) {
  warn("%s, not support as build without SDL2\n", __func__);
  return -ENOTSUP;
}
static inline int st_app_uinit_display(struct st_display* d) {
  warn("%s, not support as build without SDL2\n", __func__);
  return -ENOTSUP;
}
#endif

#endif