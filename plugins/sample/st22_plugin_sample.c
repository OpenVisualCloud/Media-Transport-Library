/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st22_plugin_sample.h"

#include <errno.h>
#include <inttypes.h>
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
  size_t codestream_size = s->req.max_codestream_size;

  if (frame->src->interlaced) {
    dbg("%s(%d), %s field\n", __func__, s->idx,
        frame->src->second_field ? "second" : "first");
  }

  /* call the real encode here, sample just copy and sleep */
  memcpy(frame->dst->addr[0], frame->src->addr[0], codestream_size);
  st_usleep(10 * 1000);
  /* data size indicate the encode stream size for current frame */
  frame->dst->data_size = codestream_size;

  s->frame_cnt++;
  dbg("%s(%d), succ\n", __func__, s->idx);
  return 0;
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
      info("%s(%d), no frame ready\n", __func__, s->idx);
      continue;
    }
    result = encode_frame(s, frame);
    st22_encoder_put_frame(session_p, frame, result);
  }
  info("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

static st22_encode_priv encoder_create_session(void* priv, st22p_encode_session session_p,
                                               struct st22_encoder_create_req* req) {
  struct st22_sample_ctx* ctx = priv;
  struct st22_encoder_session* session = NULL;
  int ret;

  for (int i = 0; i < MAX_SAMPLE_ENCODER_SESSIONS; i++) {
    if (ctx->encoder_sessions[i]) continue;
    session = malloc(sizeof(*session));
    if (!session) return NULL;
    memset(session, 0, sizeof(*session));
    session->idx = i;

    /* simply same codestream size */
    req->max_codestream_size = req->codestream_size;
    /* enable block get mode */
    req->resp_flag = ST22_ENCODER_RESP_FLAG_BLOCK_GET;

    session->req = *req;
    session->session_p = session_p;

    ret = pthread_create(&session->encode_thread, NULL, encode_thread, session);
    if (ret < 0) {
      err("%s(%d), thread create fail %d\n", __func__, i, ret);
      free(session);
      return NULL;
    }

    ctx->encoder_sessions[i] = session;
    info("%s(%d), input fmt: %s, output fmt: %s, scan: %s\n", __func__, i,
         st_frame_fmt_name(req->input_fmt), st_frame_fmt_name(req->output_fmt),
         req->interlaced ? "interlaced" : "progressive");
    info("%s(%d), max_codestream_size %" PRIu64 "\n", __func__, i,
         session->req.max_codestream_size);
    return session;
  }

  info("%s, all session slot are used\n", __func__);
  return NULL;
}

static int encoder_free_session(void* priv, st22_encode_priv session) {
  struct st22_sample_ctx* ctx = priv;
  struct st22_encoder_session* encoder_session = session;
  int idx = encoder_session->idx;

  encoder_session->stop = true;
  st22_encoder_wake_block(encoder_session->session_p);
  pthread_join(encoder_session->encode_thread, NULL);

  info("%s(%d), total %d encode frames\n", __func__, idx, encoder_session->frame_cnt);
  free(encoder_session);
  ctx->encoder_sessions[idx] = NULL;
  return 0;
}

static int encoder_frame_available(void* priv) {
  /* nothing to do, sample enable ST22_ENCODER_RESP_FLAG_BLOCK_GET */
  MTL_MAY_UNUSED(priv);
  return 0;
}

static int decode_frame(struct st22_decoder_session* s,
                        struct st22_decode_frame_meta* frame) {
  size_t codestream_size = frame->src->data_size;

  if (frame->src->interlaced) {
    dbg("%s(%d), %s field\n", __func__, s->idx,
        frame->src->second_field ? "second" : "first");
  }

  /* call the real decode here, sample just copy and sleep */
  memcpy(frame->dst->addr[0], frame->src->addr[0], codestream_size);
  st_usleep(10 * 1000);

  s->frame_cnt++;
  dbg("%s(%d), succ\n", __func__, s->idx);
  return 0;
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
      info("%s(%d), no frame ready\n", __func__, s->idx);
      continue;
    }
    result = decode_frame(s, frame);
    st22_decoder_put_frame(session_p, frame, result);
  }
  info("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

static st22_decode_priv decoder_create_session(void* priv, st22p_decode_session session_p,
                                               struct st22_decoder_create_req* req) {
  struct st22_sample_ctx* ctx = priv;
  struct st22_decoder_session* session = NULL;
  int ret;

  for (int i = 0; i < MAX_SAMPLE_DECODER_SESSIONS; i++) {
    if (ctx->decoder_sessions[i]) continue;
    session = malloc(sizeof(*session));
    if (!session) return NULL;
    memset(session, 0, sizeof(*session));
    session->idx = i;

    /* enable block get mode */
    req->resp_flag = ST22_DECODER_RESP_FLAG_BLOCK_GET;

    session->req = *req;
    session->session_p = session_p;

    ret = pthread_create(&session->decode_thread, NULL, decode_thread, session);
    if (ret < 0) {
      err("%s(%d), thread create fail %d\n", __func__, i, ret);
      free(session);
      return NULL;
    }

    ctx->decoder_sessions[i] = session;
    info("%s(%d), input fmt: %s, output fmt: %s, scan: %s\n", __func__, i,
         st_frame_fmt_name(req->input_fmt), st_frame_fmt_name(req->output_fmt),
         req->interlaced ? "interlaced" : "progressive");
    return session;
  }

  info("%s, all session slot are used\n", __func__);
  return NULL;
}

static int decoder_free_session(void* priv, st22_decode_priv session) {
  struct st22_sample_ctx* ctx = priv;
  struct st22_decoder_session* decoder_session = session;
  int idx = decoder_session->idx;

  decoder_session->stop = true;

  st22_decoder_wake_block(decoder_session->session_p);
  pthread_join(decoder_session->decode_thread, NULL);

  info("%s(%d), total %d decode frames\n", __func__, idx, decoder_session->frame_cnt);
  free(decoder_session);
  ctx->decoder_sessions[idx] = NULL;
  return 0;
}

static int decoder_frame_available(void* priv) {
  /* nothing to do, sample enable ST22_DECODER_RESP_FLAG_BLOCK_GET */
  MTL_MAY_UNUSED(priv);
  return 0;
}

st_plugin_priv st_plugin_create(mtl_handle st) {
  struct st22_sample_ctx* ctx;

  ctx = malloc(sizeof(*ctx));
  if (!ctx) return NULL;
  memset(ctx, 0, sizeof(*ctx));

  struct st22_decoder_dev d_dev;
  memset(&d_dev, 0, sizeof(d_dev));
  d_dev.name = "st22_decoder_sample";
  d_dev.priv = ctx;
  d_dev.target_device = ST_PLUGIN_DEVICE_CPU;
  d_dev.input_fmt_caps = ST_FMT_CAP_JPEGXS_CODESTREAM | ST_FMT_CAP_H264_CBR_CODESTREAM;
  d_dev.output_fmt_caps = ST_FMT_CAP_ARGB | ST_FMT_CAP_BGRA | ST_FMT_CAP_RGB8;
  d_dev.output_fmt_caps |= ST_FMT_CAP_YUV422PLANAR10LE | ST_FMT_CAP_YUV422PLANAR8 |
                           ST_FMT_CAP_V210 | ST_FMT_CAP_YUV422RFC4175PG2BE10;
  d_dev.create_session = decoder_create_session;
  d_dev.free_session = decoder_free_session;
  d_dev.notify_frame_available = decoder_frame_available;
  ctx->decoder_dev_handle = st22_decoder_register(st, &d_dev);
  if (!ctx->decoder_dev_handle) {
    err("%s, decoder register fail\n", __func__);
    free(ctx);
    return NULL;
  }

  struct st22_encoder_dev e_dev;
  memset(&e_dev, 0, sizeof(e_dev));
  e_dev.name = "st22_encoder_sample";
  e_dev.priv = ctx;
  e_dev.target_device = ST_PLUGIN_DEVICE_CPU;

  e_dev.input_fmt_caps = ST_FMT_CAP_ARGB | ST_FMT_CAP_BGRA | ST_FMT_CAP_RGB8;
  e_dev.input_fmt_caps |= ST_FMT_CAP_YUV422PLANAR10LE | ST_FMT_CAP_YUV422PLANAR8 |
                          ST_FMT_CAP_V210 | ST_FMT_CAP_YUV422RFC4175PG2BE10;
  e_dev.output_fmt_caps = ST_FMT_CAP_JPEGXS_CODESTREAM | ST_FMT_CAP_H264_CBR_CODESTREAM;
  e_dev.create_session = encoder_create_session;
  e_dev.free_session = encoder_free_session;
  e_dev.notify_frame_available = encoder_frame_available;
  ctx->encoder_dev_handle = st22_encoder_register(st, &e_dev);
  if (!ctx->encoder_dev_handle) {
    err("%s, encoder register fail\n", __func__);
    st22_decoder_unregister(ctx->decoder_dev_handle);
    free(ctx);
    return NULL;
  }

  info("%s, succ with st22 sample plugin\n", __func__);
  return ctx;
}

int st_plugin_free(st_plugin_priv handle) {
  struct st22_sample_ctx* ctx = handle;

  for (int i = 0; i < MAX_SAMPLE_DECODER_SESSIONS; i++) {
    if (ctx->decoder_sessions[i]) {
      free(ctx->decoder_sessions[i]);
    }
  }
  for (int i = 0; i < MAX_SAMPLE_ENCODER_SESSIONS; i++) {
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

  info("%s, succ with st22 sample plugin\n", __func__);
  return 0;
}

int st_plugin_get_meta(struct st_plugin_meta* meta) {
  meta->version = ST_PLUGIN_VERSION_V1;
  meta->magic = ST_PLUGIN_VERSION_V1_MAGIC;
  return 0;
}
