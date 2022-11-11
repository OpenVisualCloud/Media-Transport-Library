/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st22_ffmpeg_plugin.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../log.h"
#include "../plugin_platform.h"

static int encode_frame(struct st22_encoder_session* s,
                        struct st22_encode_frame_meta* frame) {
  int idx = s->idx;
  int f_idx = s->frame_idx;
  AVFrame* f = s->codec_frame;
  AVPacket* p = s->codec_pkt;
  AVCodecContext* ctx = s->codec_ctx;
  size_t res_size = s->req.width * s->req.height;
  size_t data_size = 0;
  void* src_addr = frame->src->addr[0];
  int ret;
  bool measure_time = false;
  uint64_t start_time = 0, end_time = 0;

  if (measure_time) {
    start_time = st_get_monotonic_time();
  }

  frame->dst->data_size = 0;

  /* prepare src */
  f->pict_type = AV_PICTURE_TYPE_I; /* all are i frame */
  f->pts = f_idx;
  /* only YUV422P now */
  st_memcpy(f->data[0], src_addr, res_size);
  src_addr += res_size;
  res_size /= 2;
  st_memcpy(f->data[1], src_addr, res_size);
  src_addr += res_size;
  st_memcpy(f->data[2], src_addr, res_size);
  src_addr += res_size;

  ret = avcodec_send_frame(ctx, f);
  s->frame_idx++;
  if (ret < 0) {
    err("%s(%d), send frame(%d) fail %s\n", __func__, idx, f_idx, av_err2str(ret));
    return ret;
  }

  while (ret >= 0) {
    ret = avcodec_receive_packet(ctx, p);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      dbg("%s(%d), receive packet fail %s on frame %d\n", __func__, idx, av_err2str(ret),
          f_idx);
      goto exit;
    } else if (ret < 0) {
      err("%s(%d), receive packet fail %s on frame %d\n", __func__, idx, av_err2str(ret),
          f_idx);
      return ret;
    }

    dbg("%s, receive packet %" PRId64 " size %d on frame %d\n", __func__, p->pts, p->size,
        f_idx);
    st_memcpy(frame->dst->addr[0] + data_size, p->data, p->size);
    data_size += p->size;
    av_packet_unref(p);
  }

exit:
  if (measure_time) {
    end_time = st_get_monotonic_time();
    info("%s(%d), consume time %" PRIu64 "us for frame %d\n", __func__, idx,
         (end_time - start_time) / 1000, f_idx);
  }
  s->frame_cnt++;
  frame->dst->data_size = data_size;
  return data_size > 0 ? 0 : -EIO;
}

static void* encode_thread(void* arg) {
  struct st22_encoder_session* s = arg;
  st22p_encode_session session_p = s->session_p;
  struct st22_encode_frame_meta* frame;
  int result;

  info("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st22_encoder_get_frame(session_p);
    if (!frame) { /* no frame */
      st_pthread_mutex_lock(&s->wake_mutex);
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }
    result = encode_frame(s, frame);
    st22_encoder_put_frame(session_p, frame, result);
  }
  info("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

static int encoder_uinit_session(struct st22_encoder_session* session) {
  int idx = session->idx;

  if (session->encode_thread) {
    info("%s(%d), stop thread\n", __func__, idx);
    session->stop = true;
    st_pthread_mutex_lock(&session->wake_mutex);
    st_pthread_cond_signal(&session->wake_cond);
    st_pthread_mutex_unlock(&session->wake_mutex);
    pthread_join(session->encode_thread, NULL);
    session->encode_thread = 0;
  }

  if (session->codec_ctx) {
    avcodec_free_context(&session->codec_ctx);
    session->codec_ctx = NULL;
  }

  if (session->codec_frame) {
    av_frame_free(&session->codec_frame);
    session->codec_frame = NULL;
  }

  if (session->codec_pkt) {
    av_packet_free(&session->codec_pkt);
    session->codec_pkt = NULL;
  }

  st_pthread_mutex_destroy(&session->wake_mutex);
  st_pthread_cond_destroy(&session->wake_cond);
  return 0;
}

static int encoder_init_session(struct st22_encoder_session* session,
                                struct st22_encoder_create_req* req) {
  int idx = session->idx;
  int ret;

  st_pthread_mutex_init(&session->wake_mutex, NULL);
  st_pthread_cond_init(&session->wake_cond, NULL);

  req->max_codestream_size = req->codestream_size;
  session->req = *req;

  AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
  if (!codec) {
    err("%s(%d), codec create fail\n", __func__, idx);
    encoder_uinit_session(session);
    return -EIO;
  }
  AVCodecContext* c = avcodec_alloc_context3(codec);
  if (!c) {
    err("%s(%d), codec ctx create fail\n", __func__, idx);
    encoder_uinit_session(session);
    return -EIO;
  }
  session->codec_ctx = c;
  /* init config */
  double fps = st_frame_rate(req->fps);
  /* bit per second */
  int64_t bit_rate = (req->codestream_size * 8) * fps;
  bit_rate = bit_rate * 7 / 10;
  bit_rate /= 10; /* temp for fps */
  c->bit_rate = bit_rate;
  c->rc_max_rate = bit_rate;
  c->rc_buffer_size = bit_rate * 3;
  c->width = req->width;
  c->height = req->height;
  c->time_base = (AVRational){1, fps};
  c->pix_fmt = AV_PIX_FMT_YUV422P;
  av_opt_set(c->priv_data, "fast", "preset", 0);
  av_opt_set(c->priv_data, "tune", "zerolatency", 0);
  av_opt_set(c->priv_data, "nal-hrd", "cbr", 0);

  ret = avcodec_open2(c, codec, NULL);
  if (ret < 0) {
    err("%s(%d), avcodec_open2 fail %d\n", __func__, idx, ret);
    encoder_uinit_session(session);
    return ret;
  }

  AVFrame* f = av_frame_alloc();
  if (!f) {
    err("%s(%d), frame alloc fail\n", __func__, idx);
    encoder_uinit_session(session);
    return -EIO;
  }
  session->codec_frame = f;
  f->format = c->pix_fmt;
  f->width = c->width;
  f->height = c->height;
  ret = av_frame_get_buffer(f, 0);
  if (ret < 0) {
    err("%s(%d), frame get fail\n", __func__, idx);
    encoder_uinit_session(session);
    return -EIO;
  }

  AVPacket* p = av_packet_alloc();
  if (!p) {
    err("%s(%d), pkt alloc fail\n", __func__, idx);
    encoder_uinit_session(session);
    return -EIO;
  }
  session->codec_pkt = p;

  ret = pthread_create(&session->encode_thread, NULL, encode_thread, session);
  if (ret < 0) {
    err("%s(%d), thread create fail %d\n", __func__, idx, ret);
    encoder_uinit_session(session);
    return ret;
  }

  return 0;
}

static st22_encode_priv encoder_create_session(void* priv, st22p_encode_session session_p,
                                               struct st22_encoder_create_req* req) {
  struct st22_ffmpeg_ctx* ctx = priv;
  struct st22_encoder_session* session = NULL;
  int ret;

  for (int i = 0; i < MAX_ST22_ENCODER_SESSIONS; i++) {
    if (ctx->encoder_sessions[i]) continue;
    session = malloc(sizeof(*session));
    if (!session) return NULL;
    memset(session, 0, sizeof(*session));
    session->idx = i;
    session->session_p = session_p;

    ret = encoder_init_session(session, req);
    if (ret < 0) {
      err("%s(%d), init session fail %d\n", __func__, i, ret);
      return NULL;
    }

    ctx->encoder_sessions[i] = session;
    info("%s(%d), input fmt: %s, output fmt: %s\n", __func__, i,
         st_frame_fmt_name(req->input_fmt), st_frame_fmt_name(req->output_fmt));
    info("%s(%d), max_codestream_size %ld\n", __func__, i,
         session->req.max_codestream_size);
    return session;
  }

  err("%s, all session slot are used\n", __func__);
  return NULL;
}

static int encoder_free_session(void* priv, st22_encode_priv session) {
  struct st22_ffmpeg_ctx* ctx = priv;
  struct st22_encoder_session* encoder_session = session;
  int idx = encoder_session->idx;

  info("%s(%d), total %d encode frames\n", __func__, idx, encoder_session->frame_cnt);

  encoder_uinit_session(encoder_session);

  free(encoder_session);
  ctx->encoder_sessions[idx] = NULL;
  return 0;
}

static int encoder_frame_available(void* priv) {
  struct st22_encoder_session* s = priv;

  dbg("%s(%d)\n", __func__, s->idx);
  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

static int decode_frame(struct st22_decoder_session* s,
                        struct st22_decode_frame_meta* frame) {
  int idx = s->idx;
  int f_idx = s->frame_idx;
  AVCodecContext* ctx = s->codec_ctx;
  AVFrame* f = s->codec_frame;
  AVPacket* p = s->codec_pkt;
  int ret;
  size_t src_size = frame->src->data_size;
  void* dst_addr = frame->dst->addr[0];
  size_t frame_size = 0;

  s->frame_idx++;

  av_packet_unref(p);
  p->data = frame->src->addr[0];
  p->size = src_size;
  ret = avcodec_send_packet(ctx, p);
  if (ret < 0) {
    err("%s(%d), send pkt(%d) fail %s\n", __func__, idx, f_idx, av_err2str(ret));
    return ret;
  }

  while (ret >= 0) {
    ret = avcodec_receive_frame(ctx, f);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      dbg("%s(%d), receive frame fail %s\n", __func__, idx, av_err2str(ret));
      goto exit;
    } else if (ret < 0) {
      err("%s(%d), receive packet fail %d on frame %d\n", __func__, idx, ret, f_idx);
      return ret;
    }

    dbg("%s, format(%dx%d@%d) on frame %d\n", __func__, f->width, f->height, f->format,
        f_idx);
    frame_size = f->width * f->height;
    /* only YUV422P now */
    st_memcpy(dst_addr, f->data[0], frame_size);
    dst_addr += frame_size;
    frame_size /= 2;
    st_memcpy(dst_addr, f->data[1], frame_size);
    dst_addr += frame_size;
    st_memcpy(dst_addr, f->data[2], frame_size);
    dst_addr += frame_size;

    av_frame_unref(f);
  }

exit:
  s->frame_cnt++;
  return frame_size > 0 ? 0 : -EIO;
}

static void* decode_thread(void* arg) {
  struct st22_decoder_session* s = arg;
  st22p_decode_session session_p = s->session_p;
  struct st22_decode_frame_meta* frame;
  int result;

  info("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st22_decoder_get_frame(session_p);
    if (!frame) { /* no frame */
      st_pthread_mutex_lock(&s->wake_mutex);
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }
    result = decode_frame(s, frame);
    st22_decoder_put_frame(session_p, frame, result);
  }
  info("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

static int decoder_uinit_session(struct st22_decoder_session* session) {
  int idx = session->idx;

  if (session->decode_thread) {
    info("%s(%d), stop thread\n", __func__, idx);
    session->stop = true;
    st_pthread_mutex_lock(&session->wake_mutex);
    st_pthread_cond_signal(&session->wake_cond);
    st_pthread_mutex_unlock(&session->wake_mutex);
    pthread_join(session->decode_thread, NULL);
    session->decode_thread = 0;
  }

  if (session->codec_parser) {
    av_parser_close(session->codec_parser);
    session->codec_parser = NULL;
  }

  if (session->codec_ctx) {
    avcodec_free_context(&session->codec_ctx);
    session->codec_ctx = NULL;
  }

  if (session->codec_frame) {
    av_frame_free(&session->codec_frame);
    session->codec_frame = NULL;
  }

  if (session->codec_pkt) {
    av_packet_free(&session->codec_pkt);
    session->codec_pkt = NULL;
  }

  st_pthread_mutex_destroy(&session->wake_mutex);
  st_pthread_cond_destroy(&session->wake_cond);
  return 0;
}

static int decoder_init_session(struct st22_decoder_session* session,
                                struct st22_decoder_create_req* req) {
  int idx = session->idx;
  int ret;

  st_pthread_mutex_init(&session->wake_mutex, NULL);
  st_pthread_cond_init(&session->wake_cond, NULL);

  session->req = *req;

  AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!codec) {
    err("%s(%d), codec create fail\n", __func__, idx);
    decoder_uinit_session(session);
    return -EIO;
  }

  AVCodecParserContext* parser = av_parser_init(codec->id);
  if (!parser) {
    err("%s(%d), parser create fail\n", __func__, idx);
    decoder_uinit_session(session);
    return -EIO;
  }
  session->codec_parser = parser;

  AVCodecContext* c = avcodec_alloc_context3(codec);
  if (!c) {
    err("%s(%d), codec ctx create fail\n", __func__, idx);
    decoder_uinit_session(session);
    return -EIO;
  }
  session->codec_ctx = c;
  /* init config */
  c->width = req->width;
  c->height = req->height;
  c->time_base = (AVRational){1, 60};
  c->framerate = (AVRational){60, 1};
  c->pix_fmt = AV_PIX_FMT_YUV422P;

  ret = avcodec_open2(c, codec, NULL);
  if (ret < 0) {
    err("%s(%d), avcodec_open2 fail %d\n", __func__, idx, ret);
    decoder_uinit_session(session);
    return ret;
  }

  AVFrame* f = av_frame_alloc();
  if (!f) {
    err("%s(%d), frame alloc fail\n", __func__, idx);
    decoder_uinit_session(session);
    return -EIO;
  }
  session->codec_frame = f;

  AVPacket* p = av_packet_alloc();
  if (!p) {
    err("%s(%d), pkt alloc fail\n", __func__, idx);
    decoder_uinit_session(session);
    return -EIO;
  }
  session->codec_pkt = p;

  ret = pthread_create(&session->decode_thread, NULL, decode_thread, session);
  if (ret < 0) {
    err("%s(%d), thread create fail %d\n", __func__, idx, ret);
    decoder_uinit_session(session);
    return ret;
  }

  return 0;
}

static st22_decode_priv decoder_create_session(void* priv, st22p_decode_session session_p,
                                               struct st22_decoder_create_req* req) {
  struct st22_ffmpeg_ctx* ctx = priv;
  struct st22_decoder_session* session = NULL;
  int ret;

  for (int i = 0; i < MAX_ST22_DECODER_SESSIONS; i++) {
    if (ctx->decoder_sessions[i]) continue;
    session = malloc(sizeof(*session));
    if (!session) return NULL;
    memset(session, 0, sizeof(*session));
    session->idx = i;
    session->session_p = session_p;

    ret = decoder_init_session(session, req);
    if (ret < 0) {
      err("%s(%d), init session fail %d\n", __func__, i, ret);
      return NULL;
    }

    ctx->decoder_sessions[i] = session;
    info("%s(%d), input fmt: %s, output fmt: %s\n", __func__, i,
         st_frame_fmt_name(req->input_fmt), st_frame_fmt_name(req->output_fmt));
    return session;
  }

  info("%s, all session slot are used\n", __func__);
  return NULL;
}

static int decoder_free_session(void* priv, st22_decode_priv session) {
  struct st22_ffmpeg_ctx* ctx = priv;
  struct st22_decoder_session* decoder_session = session;
  int idx = decoder_session->idx;

  decoder_session->stop = true;
  st_pthread_mutex_lock(&decoder_session->wake_mutex);
  st_pthread_cond_signal(&decoder_session->wake_cond);
  st_pthread_mutex_unlock(&decoder_session->wake_mutex);
  pthread_join(decoder_session->decode_thread, NULL);

  st_pthread_mutex_destroy(&decoder_session->wake_mutex);
  st_pthread_cond_destroy(&decoder_session->wake_cond);

  info("%s(%d), total %d decode frames\n", __func__, idx, decoder_session->frame_cnt);
  free(decoder_session);
  ctx->decoder_sessions[idx] = NULL;
  return 0;
}

static int decoder_frame_available(void* priv) {
  struct st22_decoder_session* s = priv;

  dbg("%s(%d)\n", __func__, s->idx);
  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

st_plugin_priv st_plugin_create(st_handle st) {
  struct st22_ffmpeg_ctx* ctx;

  ctx = malloc(sizeof(*ctx));
  if (!ctx) return NULL;
  memset(ctx, 0, sizeof(*ctx));

  struct st22_decoder_dev d_dev;
  memset(&d_dev, 0, sizeof(d_dev));
  d_dev.name = "st22_ffmpeg_plugin_decoder";
  d_dev.priv = ctx;
  d_dev.target_device = ST_PLUGIN_DEVICE_CPU;
  d_dev.input_fmt_caps = ST_FMT_CAP_H264_CBR_CODESTREAM;
  d_dev.output_fmt_caps = ST_FMT_CAP_YUV422PLANAR8;
  d_dev.create_session = decoder_create_session;
  d_dev.free_session = decoder_free_session;
  d_dev.notify_frame_available = decoder_frame_available;
  ctx->decoder_dev_handle = st22_decoder_register(st, &d_dev);
  if (!ctx->decoder_dev_handle) {
    info("%s, decoder register fail\n", __func__);
    free(ctx);
    return NULL;
  }

  struct st22_encoder_dev e_dev;
  memset(&e_dev, 0, sizeof(e_dev));
  e_dev.name = "st22_ffmpeg_plugin_encoder";
  e_dev.priv = ctx;
  e_dev.target_device = ST_PLUGIN_DEVICE_CPU;
  e_dev.input_fmt_caps = ST_FMT_CAP_YUV422PLANAR8;
  e_dev.output_fmt_caps = ST_FMT_CAP_H264_CBR_CODESTREAM;
  e_dev.create_session = encoder_create_session;
  e_dev.free_session = encoder_free_session;
  e_dev.notify_frame_available = encoder_frame_available;
  ctx->encoder_dev_handle = st22_encoder_register(st, &e_dev);
  if (!ctx->encoder_dev_handle) {
    info("%s, encoder register fail\n", __func__);
    st22_decoder_unregister(ctx->decoder_dev_handle);
    free(ctx);
    return NULL;
  }

  info("%s, succ with st22 ffmpeg plugin\n", __func__);
  return ctx;
}

int st_plugin_free(st_plugin_priv handle) {
  struct st22_ffmpeg_ctx* ctx = handle;

  for (int i = 0; i < MAX_ST22_DECODER_SESSIONS; i++) {
    if (ctx->decoder_sessions[i]) {
      free(ctx->decoder_sessions[i]);
    }
  }
  for (int i = 0; i < MAX_ST22_ENCODER_SESSIONS; i++) {
    if (ctx->encoder_sessions[i]) {
      free(ctx->encoder_sessions[i]);
    }
  }
  if (ctx->decoder_dev_handle) {
    st22_decoder_unregister(ctx->decoder_dev_handle);
    ctx->decoder_dev_handle = NULL;
  }
  if (ctx->encoder_dev_handle) {
    st22_encoder_unregister(ctx->encoder_dev_handle);
    ctx->encoder_dev_handle = NULL;
  }
  free(ctx);

  info("%s, succ with st22 ffmpeg plugin\n", __func__);
  return 0;
}

int st_plugin_get_meta(struct st_plugin_meta* meta) {
  meta->version = ST_PLUGIN_VERSION_V1;
  meta->magic = ST_PLUGIN_VERSION_V1_MAGIC;
  return 0;
}
