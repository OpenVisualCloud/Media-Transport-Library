/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "convert_plugin_sample.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../log.h"
#include "../plugin_platform.h"

static int convert_frame(struct converter_session* s,
                         struct st20_convert_frame_meta* frame) {
  switch (frame->src->fmt) {
    case ST_FRAME_FMT_YUV422RFC4175PG2BE10:
      switch (frame->dst->fmt) {
        case ST_FRAME_FMT_V210:
          st20_rfc4175_422be10_to_v210(frame->src->addr[0], frame->dst->addr[0],
                                       frame->dst->width, frame->dst->height);
          break;
        case ST_FRAME_FMT_YUV422RFC4175PG2BE10:
          st_memcpy(frame->dst->addr[0], frame->src->addr[0], frame->dst->data_size);
          break;
        case ST_FRAME_FMT_YUV422PACKED8:
          st20_rfc4175_422be10_to_422le8(frame->src->addr[0], frame->dst->addr[0],
                                         frame->dst->width, frame->dst->height);
          break;
        default:
          break;
      }
      break;
    default:
      break;
  }
  s->frame_cnt++;
  return 0;
}

static void* convert_thread(void* arg) {
  struct converter_session* s = arg;
  st20p_convert_session session_p = s->session_p;
  struct st20_convert_frame_meta* frame;
  int result;

  info("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st20_converter_get_frame(session_p);
    if (!frame) { /* no frame */
      st_pthread_mutex_lock(&s->wake_mutex);
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }
    result = convert_frame(s, frame);
    st20_converter_put_frame(session_p, frame, result);
  }
  info("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

static st20_convert_priv converter_create_session(void* priv,
                                                  st20p_convert_session session_p,
                                                  struct st20_converter_create_req* req) {
  struct convert_ctx* ctx = priv;
  struct converter_session* session = NULL;
  int ret;

  for (int i = 0; i < MAX_COLOR_CONVERT_SESSIONS; i++) {
    if (ctx->converter_sessions[i]) continue;
    session = malloc(sizeof(*session));
    if (!session) return NULL;
    memset(session, 0, sizeof(*session));
    session->idx = i;
    st_pthread_mutex_init(&session->wake_mutex, NULL);
    st_pthread_cond_init(&session->wake_cond, NULL);

    session->req = *req;
    session->session_p = session_p;

    ret = pthread_create(&session->convert_thread, NULL, convert_thread, session);
    if (ret < 0) {
      info("%s(%d), thread create fail %d\n", __func__, i, ret);
      st_pthread_mutex_destroy(&session->wake_mutex);
      st_pthread_cond_destroy(&session->wake_cond);
      free(session);
      return NULL;
    }

    ctx->converter_sessions[i] = session;
    info("%s(%d), input fmt: %s, output fmt: %s\n", __func__, i,
         st_frame_fmt_name(req->input_fmt), st_frame_fmt_name(req->output_fmt));
    return session;
  }

  info("%s, all session slot are used\n", __func__);
  return NULL;
}

static int converter_free_session(void* priv, st20_convert_priv session) {
  struct convert_ctx* ctx = priv;
  struct converter_session* converter_session = session;
  int idx = converter_session->idx;

  converter_session->stop = true;
  st_pthread_mutex_lock(&converter_session->wake_mutex);
  st_pthread_cond_signal(&converter_session->wake_cond);
  st_pthread_mutex_unlock(&converter_session->wake_mutex);
  pthread_join(converter_session->convert_thread, NULL);

  st_pthread_mutex_destroy(&converter_session->wake_mutex);
  st_pthread_cond_destroy(&converter_session->wake_cond);

  info("%s(%d), total %d convert frames\n", __func__, idx, converter_session->frame_cnt);
  free(converter_session);
  ctx->converter_sessions[idx] = NULL;
  return 0;
}

static int converter_frame_available(void* priv) {
  struct converter_session* s = priv;

  dbg("%s(%d)\n", __func__, s->idx);
  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

st_plugin_priv st_plugin_create(st_handle st) {
  struct convert_ctx* ctx;

  ctx = malloc(sizeof(*ctx));
  if (!ctx) return NULL;
  memset(ctx, 0, sizeof(*ctx));

  struct st20_converter_dev c_dev;
  memset(&c_dev, 0, sizeof(c_dev));
  c_dev.name = "color_convert_sample";
  c_dev.priv = ctx;
  c_dev.target_device = ST_PLUGIN_DEVICE_CPU;
  c_dev.input_fmt_caps = ST_FMT_CAP_YUV422PLANAR10LE | ST_FMT_CAP_YUV422PACKED8 |
                         ST_FMT_CAP_V210 | ST_FMT_CAP_YUV422RFC4175PG2BE10;
  c_dev.output_fmt_caps = ST_FMT_CAP_YUV422PLANAR10LE | ST_FMT_CAP_YUV422PACKED8 |
                          ST_FMT_CAP_V210 | ST_FMT_CAP_YUV422RFC4175PG2BE10;
  c_dev.create_session = converter_create_session;
  c_dev.free_session = converter_free_session;
  c_dev.notify_frame_available = converter_frame_available;
  ctx->converter_dev_handle = st20_converter_register(st, &c_dev);
  if (!ctx->converter_dev_handle) {
    err("%s, converter register fail\n", __func__);
    free(ctx);
    return NULL;
  }

  info("%s, succ with converter sample plugin\n", __func__);
  return ctx;
}
int st_plugin_free(st_plugin_priv handle) {
  struct convert_ctx* ctx = handle;

  for (int i = 0; i < MAX_COLOR_CONVERT_SESSIONS; i++) {
    if (ctx->converter_sessions[i]) {
      free(ctx->converter_sessions[i]);
    }
  }

  free(ctx);

  info("%s, succ with converter sample plugin\n", __func__);
  return 0;
}

int st_plugin_get_meta(struct st_plugin_meta* meta) {
  meta->version = ST_PLUGIN_VERSION_V1;
  meta->magic = ST_PLUGIN_VERSION_V1_MAGIC;
  return 0;
}