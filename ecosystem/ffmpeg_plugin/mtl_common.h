/*
 * MTL common struct and functions
 * Copyright (c) 2024 Intel
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <mtl/st_pipeline_api.h>

// clang-format off
/* MTL FFMPEG version */
#include "libavdevice/version.h"
#if LIBAVDEVICE_VERSION_MAJOR <= 58
#define MTL_FFMPEG_4_4
#else
#define MTL_FFMPEG_6_1
#endif
// clang-format on

#include "libavformat/avformat.h"
#include "libavformat/internal.h"
#ifdef MTL_FFMPEG_6_1
#include "libavformat/mux.h"
#endif
#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/rational.h"

/* log define */
#ifdef DEBUG
#define dbg(ctx, ...)                       \
  do {                                      \
    av_log(ctx, AV_LOG_DEBUG, __VA_ARGS__); \
  } while (0)
#else
#define dbg(ctx, ...) \
  do {                \
  } while (0)
#endif
#define info(ctx, ...)                     \
  do {                                     \
    av_log(ctx, AV_LOG_INFO, __VA_ARGS__); \
  } while (0)
#define warn(ctx, ...)                        \
  do {                                        \
    av_log(ctx, AV_LOG_WARNING, __VA_ARGS__); \
  } while (0)
#define err(ctx, ...)                       \
  do {                                      \
    av_log(ctx, AV_LOG_ERROR, __VA_ARGS__); \
  } while (0)

#ifndef NS_PER_MS
#define NS_PER_MS (1000 * 1000)
#endif

typedef struct StFpsDecs {
  enum st_fps st_fps;
  unsigned int min;
  unsigned int max;
} StFpsDecs;

enum st_fps framerate_to_st_fps(AVRational framerate);

mtl_handle mtl_instance_get(char* port, char* local_addr, int enc_session_cnt,
                            int dec_session_cnt, char* dma_dev, int* idx);
int mtl_instance_put(mtl_handle handle);
