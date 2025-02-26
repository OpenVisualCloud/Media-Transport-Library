/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST22_AVCODEC_PLUGIN_HEAD_H_
#define _ST22_AVCODEC_PLUGIN_HEAD_H_

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <mtl/st_pipeline_api.h>

#define MAX_ST22_AVCODEC_ENCODER_SESSIONS (8)
#define MAX_ST22_AVCODEC_DECODER_SESSIONS (8)

struct st22_avcodec_encoder_session {
  int idx;
  enum AVPixelFormat pix_fmt;

  struct st22_encoder_create_req req;
  st22p_encode_session session_p;
  bool stop;
  pthread_t encode_thread;

  int frame_cnt;

  /* AVCodec info */
  AVCodecContext* codec_ctx;
  AVFrame* codec_frame;
  AVPacket* codec_pkt;
};

struct st22_avcodec_decoder_session {
  int idx;
  enum AVPixelFormat pix_fmt;

  struct st22_decoder_create_req req;
  st22p_decode_session session_p;
  bool stop;
  pthread_t decode_thread;

  int frame_cnt;

  /* AVCodec info */
  AVCodecContext* codec_ctx;
  AVFrame* codec_frame;
  AVPacket* codec_pkt;
  AVCodecParserContext* codec_parser;
};

struct st22_avcodec_plugin_ctx {
  st22_encoder_dev_handle encoder_dev_handle;
  st22_decoder_dev_handle decoder_dev_handle;
  struct st22_avcodec_encoder_session*
      encoder_sessions[MAX_ST22_AVCODEC_ENCODER_SESSIONS];
  struct st22_avcodec_decoder_session*
      decoder_sessions[MAX_ST22_AVCODEC_DECODER_SESSIONS];
};

/* the APIs for plugin */
int st_plugin_get_meta(struct st_plugin_meta* meta);
st_plugin_priv st_plugin_create(mtl_handle st);
int st_plugin_free(st_plugin_priv handle);

#endif
