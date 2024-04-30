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

#include <arpa/inet.h>
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

#include "libavcodec/codec_desc.h"
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

#define MTL_RX_DEV_ARGS                                                                    \
  {"p_port",           "mtl p port",  OFFSET(devArgs.port[MTL_PORT_P]),                    \
   AV_OPT_TYPE_STRING, {.str = NULL}, .flags = DEC},                                       \
      {"p_sip",       "mtl local ip", OFFSET(devArgs.sip[MTL_PORT_P]), AV_OPT_TYPE_STRING, \
       {.str = NULL}, .flags = DEC},                                                       \
  {                                                                                        \
    "dma_dev", "mtl dma dev", OFFSET(devArgs.dma_dev), AV_OPT_TYPE_STRING,                 \
        {.str = NULL}, .flags = DEC                                                        \
  }

#define MTL_RX_PORT_ARGS                                                            \
  {"p_rx_ip",          "p rx ip",     OFFSET(portArgs.sip[MTL_SESSION_PORT_P]),     \
   AV_OPT_TYPE_STRING, {.str = NULL}, .flags = DEC},                                \
      {"udp_port",                                                                  \
       "UDP port",                                                                  \
       OFFSET(portArgs.udp_port),                                                   \
       AV_OPT_TYPE_INT,                                                             \
       {.i64 = 20000},                                                              \
       -1,                                                                          \
       INT_MAX,                                                                     \
       DEC},                                                                        \
  {                                                                                 \
    "payload_type", "payload type", OFFSET(portArgs.payload_type), AV_OPT_TYPE_INT, \
        {.i64 = 112}, -1, INT_MAX, DEC                                              \
  }

#define MTL_TX_DEV_ARGS                                                                    \
  {"p_port",           "mtl p port",  OFFSET(devArgs.port[MTL_PORT_P]),                    \
   AV_OPT_TYPE_STRING, {.str = NULL}, .flags = ENC},                                       \
      {"p_sip",       "mtl local ip", OFFSET(devArgs.sip[MTL_PORT_P]), AV_OPT_TYPE_STRING, \
       {.str = NULL}, .flags = ENC},                                                       \
  {                                                                                        \
    "dma_dev", "mtl dma dev", OFFSET(devArgs.dma_dev), AV_OPT_TYPE_STRING,                 \
        {.str = NULL}, .flags = ENC                                                        \
  }

#define MTL_TX_PORT_ARGS                                                            \
  {"p_tx_ip",          "p tx ip",     OFFSET(portArgs.dip[MTL_SESSION_PORT_P]),     \
   AV_OPT_TYPE_STRING, {.str = NULL}, .flags = ENC},                                \
      {"udp_port",                                                                  \
       "UDP port",                                                                  \
       OFFSET(portArgs.udp_port),                                                   \
       AV_OPT_TYPE_INT,                                                             \
       {.i64 = 20000},                                                              \
       -1,                                                                          \
       INT_MAX,                                                                     \
       ENC},                                                                        \
  {                                                                                 \
    "payload_type", "payload type", OFFSET(portArgs.payload_type), AV_OPT_TYPE_INT, \
        {.i64 = 112}, -1, INT_MAX, ENC                                              \
  }

typedef struct StDevArgs {
  char* port[MTL_PORT_MAX];
  char* sip[MTL_PORT_MAX];
  int tx_queues_cnt[MTL_PORT_MAX];
  int rx_queues_cnt[MTL_PORT_MAX];
  char* dma_dev;
} StDevArgs;

typedef struct StTxSessionPortArgs {
  char* dip[MTL_SESSION_PORT_MAX];
  char* port[MTL_SESSION_PORT_MAX];
  int udp_port;
  int payload_type;
} StTxSessionPortArgs;

typedef struct StRxSessionPortArgs {
  char* sip[MTL_SESSION_PORT_MAX];
  char* port[MTL_SESSION_PORT_MAX];
  int udp_port;
  int payload_type;
} StRxSessionPortArgs;

typedef struct StFpsDecs {
  enum st_fps st_fps;
  unsigned int min;
  unsigned int max;
} StFpsDecs;

enum st_fps framerate_to_st_fps(AVRational framerate);

mtl_handle mtl_dev_get(AVFormatContext* ctx, const struct StDevArgs* args, int* idx);
int mtl_instance_put(AVFormatContext* ctx, mtl_handle handle);

int mtl_parse_rx_port(AVFormatContext* ctx, const struct StDevArgs* devArgs,
                      const StRxSessionPortArgs* args, struct st_rx_port* port);
int mtl_parse_tx_port(AVFormatContext* ctx, const struct StDevArgs* devArgs,
                      const StTxSessionPortArgs* args, struct st_tx_port* port);
