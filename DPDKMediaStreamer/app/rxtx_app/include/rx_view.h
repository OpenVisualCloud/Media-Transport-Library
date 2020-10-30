/*
* Copyright 2020 Intel Corporation.
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

#ifndef _RX_VIEW_H
#define _RX_VIEW_H

#include "st_api.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>
#include <pthread.h>

struct view_info;
typedef struct view_info view_info_t;

extern st_status_t InitSDL(void);
st_status_t CreateView(view_info_t **view,const char *label, st21_buf_fmt_t bufFormat, int width, int height);
extern st_status_t EventLoop(void);
st_status_t ShowFrame(view_info_t *view, uint8_t const*frame, int interlaced);
extern void CloseViews(void);

#endif	//_RX_VIEW_H
