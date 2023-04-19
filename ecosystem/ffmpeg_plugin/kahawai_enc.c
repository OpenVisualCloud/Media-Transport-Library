/*
 * Kahawai raw video muxer
 * Copyright (c) 2023 Intel
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

#include <mtl/st_convert_api.h>
#include <mtl/st_pipeline_api.h>

#include "kahawai_common.h"
#include "libavformat/avformat.h"
#include "libavformat/internal.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include <unistd.h>

typedef struct KahawaiMuxerContext {
  const AVClass *class; /**< Class for private options. */

  char *port;
  char *local_addr;
  char *dst_addr;
  int udp_port;
  int fb_cnt;
  int session_cnt;

  int width;
  int height;
  enum AVPixelFormat pixel_format;
  AVRational framerate;
  mtl_handle dev_handle;
  st20p_tx_handle tx_handle;

  pthread_cond_t get_frame_cond;
  pthread_mutex_t get_frame_mutex;
  int64_t frame_tx_completed;
  bool tx_completed;

  int64_t frame_counter;
  struct st_frame *frame;
  size_t output_frame_size;
} KahawaiMuxerContext;

extern unsigned int active_session_cnt;

static int tx_st20p_frame_available(void *priv) {
  KahawaiMuxerContext *s = priv;

  pthread_mutex_lock(&(s->get_frame_mutex));
  pthread_cond_signal(&(s->get_frame_cond));
  pthread_mutex_unlock(&(s->get_frame_mutex));

  return 0;
}

static int tx_st20p_frame_done(void *priv, struct st_frame *frame) {
  KahawaiMuxerContext *s = priv;

  if (ST_FRAME_STATUS_COMPLETE == frame->status) {
    s->frame_tx_completed++;
    /* s->frame_counter is the number of frames sent to MTL,
      -1 here because some times we cannot get all notifications from MTL
    */
    if (s->frame_tx_completed == (s->frame_counter - 1)) {
      s->frame_tx_completed = 0;
      s->frame_counter = 0;
      s->tx_completed = true;
    }
  }

  return 0;
}

static int kahawai_write_header(AVFormatContext *ctx) {
  KahawaiMuxerContext *s = ctx->priv_data;
  AVStream *st = NULL;

  int packet_size = 0;

  int ret = 0;

  // struct mtl_init_params param;
  struct st20p_tx_ops ops_tx;

  av_log(ctx, AV_LOG_VERBOSE, "kahawai_write_header triggered\n");

  memset(&ops_tx, 0, sizeof(ops_tx));

  if ((NULL == s->port) || (strlen(s->port) > MTL_PORT_MAX_LEN)) {
    av_log(ctx, AV_LOG_ERROR, "Invalid port info\n");
    return AVERROR(EINVAL);
  }
  ops_tx.port.num_port = 1;
  strncpy(ops_tx.port.port[MTL_PORT_P], s->port, MTL_PORT_MAX_LEN);

  if (NULL == s->dst_addr) {
    av_log(ctx, AV_LOG_ERROR, "Invalid destination IP address\n");
    return AVERROR(EINVAL);
  } else if (sscanf(s->dst_addr, "%hhu.%hhu.%hhu.%hhu",
                    &ops_tx.port.dip_addr[MTL_PORT_P][0],
                    &ops_tx.port.dip_addr[MTL_PORT_P][1],
                    &ops_tx.port.dip_addr[MTL_PORT_P][2],
                    &ops_tx.port.dip_addr[MTL_PORT_P][3]) != MTL_IP_ADDR_LEN) {
    av_log(ctx, AV_LOG_ERROR, "Failed to parse destination IP address: %s\n",
           s->dst_addr);
    return AVERROR(EINVAL);
  }

  if ((s->udp_port < 0) || (s->udp_port > 0xFFFF)) {
    av_log(ctx, AV_LOG_ERROR, "Invalid UDP port: %d\n", s->udp_port);
    return AVERROR(EINVAL);
  }
  ops_tx.port.udp_port[MTL_PORT_P] = s->udp_port;

  ops_tx.width = s->width = ctx->streams[0]->codecpar->width;
  ops_tx.height = s->height = ctx->streams[0]->codecpar->height;

  s->pixel_format = ctx->streams[0]->codecpar->format;
  if (s->pixel_format == AV_PIX_FMT_NONE) {
    av_log(ctx, AV_LOG_ERROR, "No such pixel format: %s.\n",
           av_pix_fmt_desc_get(s->pixel_format));
    return AVERROR(EINVAL);
  } else if (s->pixel_format != AV_PIX_FMT_RGB24) {
    av_log(ctx, AV_LOG_ERROR, "Only rgb24 are supported\n");
    return AVERROR(EINVAL);
  }

  ops_tx.transport_fmt = ST20_FMT_RGB_8BIT;
  ops_tx.input_fmt = ST_FRAME_FMT_RGB8;

  s->framerate = ctx->streams[0]->avg_frame_rate;
  if (ops_tx.fps = get_fps_table(s->framerate) == ST_FPS_MAX) {
    av_log(ctx, AV_LOG_ERROR, "Frame rate %f is not supported\n",
           av_q2d(s->framerate));
    return AVERROR(EINVAL);
  }

  // Create device
  if (!kahawai_get_handle()) {
    s->dev_handle = kahawai_init(s->port, s->local_addr, s->udp_port,
                                 s->session_cnt, 0, NULL);
    if (!s->dev_handle) {
      av_log(ctx, AV_LOG_ERROR, "mtl_init failed\n");
      return AVERROR(EIO);
    }
    kahawai_set_handle(s->dev_handle);
    av_log(ctx, AV_LOG_VERBOSE, "mtl_init finished: st_handle 0x%" PRIx64 "\n",
           (unsigned long)kahawai_get_handle());
  } else {
    s->dev_handle = kahawai_get_handle();
    av_log(ctx, AV_LOG_VERBOSE, "use shared st_handle 0x%" PRIx64 "\n",
           (unsigned long)kahawai_get_handle());
  }
  ++active_session_cnt;

  ops_tx.name = "st20p";
  ops_tx.priv = s;                // Handle of priv_data registered to lib
  ops_tx.port.payload_type = 112; // TX_ST20_PAYLOAD_TYPE
  ops_tx.device = ST_PLUGIN_DEVICE_AUTO;
  ops_tx.notify_frame_available = tx_st20p_frame_available;
  ops_tx.notify_frame_done = tx_st20p_frame_done;
  ops_tx.framebuff_cnt = s->fb_cnt;

  pthread_mutex_init(&(s->get_frame_mutex), NULL);
  pthread_cond_init(&(s->get_frame_cond), NULL);

  av_log(ctx, AV_LOG_VERBOSE, "st20p_tx_create st_handle 0x%" PRIx64 "\n",
         (unsigned long)s->dev_handle);
  av_log(ctx, AV_LOG_VERBOSE, "udp_port %d\n", s->udp_port);

  s->tx_handle = st20p_tx_create(s->dev_handle, &ops_tx);
  if (!s->tx_handle) {
    av_log(ctx, AV_LOG_ERROR, "st20p_tx_create failed\n");
    return AVERROR(EIO);
  }

  s->output_frame_size = st20p_tx_frame_size(s->tx_handle);

  if (s->output_frame_size <= 0) {
    av_log(ctx, AV_LOG_ERROR, "st20p_tx_frame_size failed\n");
    return AVERROR(EINVAL);
  }

  av_log(ctx, AV_LOG_VERBOSE, "st20p_tx_create finished\n");

  s->frame_counter = 0;
  s->frame = NULL;

  s->frame_tx_completed = 0;
  s->tx_completed = false;
  return 0;
}

static int kahawai_write_packet(AVFormatContext *ctx, AVPacket *pkt) {
  KahawaiMuxerContext *s = ctx->priv_data;
  int ret = 0;

  int i = 0, j = 0, h = 0;
  av_log(ctx, AV_LOG_VERBOSE, "kahawai_write_packet triggered\n");
  s->frame = st20p_tx_get_frame(s->tx_handle);
  if (!s->frame) {
    pthread_mutex_lock(&(s->get_frame_mutex));
    pthread_cond_wait(&(s->get_frame_cond), &(s->get_frame_mutex));
    pthread_mutex_unlock(&(s->get_frame_mutex));

    s->frame = st20p_tx_get_frame(s->tx_handle);
    if (!s->frame) {
      av_log(ctx, AV_LOG_ERROR, "st20p_tx_get_frame failed\n");
      return AVERROR(EIO);
    }
  }
  av_log(ctx, AV_LOG_VERBOSE, "st20p_tx_get_frame: 0x%" PRIx64 "\n",
         (unsigned long)(s->frame->addr[0]));

  if (s->frame->data_size != s->output_frame_size) {
    av_log(ctx, AV_LOG_ERROR,
           "Unexpected frame size received: %" PRIu64 " (%" PRIu64
           " expected)\n",
           s->frame->data_size, s->output_frame_size);
    return AVERROR(EIO);
  }

  uint8_t *data[4];
  int linesize[4];
  av_image_fill_arrays(data, linesize, pkt->data, s->pixel_format, s->width,
                       s->height, 1);
  av_image_copy(s->frame->addr, s->frame->linesize, data, linesize,
                s->pixel_format, s->width, s->height);

  st20p_tx_put_frame(s->tx_handle, s->frame);
  av_log(ctx, AV_LOG_VERBOSE, "st20p_tx_put_frame: 0x%" PRIx64 "\n",
         (unsigned long)(s->frame->addr[0]));

  return 0;
}

static int kahawai_write_trailer(AVFormatContext *ctx) {
  KahawaiMuxerContext *s = ctx->priv_data;
  int i;

  av_log(ctx, AV_LOG_VERBOSE, "kahawai_write_trailer triggered\n");

  for (i = 0; s->tx_completed != true && i < 100; i++) {
    usleep(10000);
  }

  s->frame_counter = 0;
  s->frame_tx_completed = 0;
  s->tx_completed = false;

  if (s->frame) {
    av_log(ctx, AV_LOG_VERBOSE, "Put a frame: 0x%" PRIx64 "\n",
           (unsigned long)(s->frame->addr[0]));
    st20p_tx_put_frame(s->tx_handle, s->frame);
    s->frame = NULL;
  }

  if (s->tx_handle) {
    st20p_tx_free(s->tx_handle);
    s->tx_handle = NULL;
  }
  av_log(ctx, AV_LOG_VERBOSE, "st20p_tx_free finished\n");

  pthread_mutex_destroy(&s->get_frame_mutex);
  pthread_cond_destroy(&s->get_frame_cond);

  // Destroy device
  if (--active_session_cnt == 0) {
    if (kahawai_get_handle()) {
      mtl_uninit(kahawai_get_handle());
      kahawai_set_handle(NULL);
      av_log(ctx, AV_LOG_VERBOSE, "mtl_uninit finished\n");
    } else {
      av_log(ctx, AV_LOG_ERROR, "missing st_handle\n");
    }
  } else {
    av_log(ctx, AV_LOG_VERBOSE, "no need to do st_uninit yet\n");
  }
  s->dev_handle = NULL;

  return 0;
}

#define OFFSET(x) offsetof(KahawaiMuxerContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM
static const AVOption kahawai_options[] = {
    {"port",
     "ST port",
     OFFSET(port),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     .flags = ENC},
    {"local_addr",
     "Local IP address",
     OFFSET(local_addr),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     .flags = ENC},
    {"dst_addr",
     "Destination IP address",
     OFFSET(dst_addr),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     .flags = ENC},
    {"udp_port",
     "UDP port",
     OFFSET(udp_port),
     AV_OPT_TYPE_INT,
     {.i64 = -1},
     -1,
     INT_MAX,
     ENC},
    {"fb_cnt",
     "Frame buffer count",
     OFFSET(fb_cnt),
     AV_OPT_TYPE_INT,
     {.i64 = 8},
     3,
     8,
     ENC},
    {"total_sessions",
     "Total sessions count",
     OFFSET(session_cnt),
     AV_OPT_TYPE_INT,
     {.i64 = 1},
     1,
     INT_MAX,
     ENC},
    {NULL},
};

static const AVClass kahawai_muxer_class = {
    .class_name = "kahawai Muxer",
    .item_name = av_default_item_name,
    .option = kahawai_options,
    .version = LIBAVUTIL_VERSION_INT,
    .category = AV_CLASS_CATEGORY_DEVICE_OUTPUT,
};

AVOutputFormat ff_kahawai_muxer = {
    .name = "kahawai_mux",
    .long_name = NULL_IF_CONFIG_SMALL("kahawai output device"),
    .priv_data_size = sizeof(KahawaiMuxerContext),
    .write_header = kahawai_write_header,
    .write_packet = kahawai_write_packet,
    .write_trailer = kahawai_write_trailer,
    .video_codec = AV_CODEC_ID_RAWVIDEO,
    .flags = AVFMT_NOFILE,
    .control_message = NULL,
    .priv_class = &kahawai_muxer_class,
};
