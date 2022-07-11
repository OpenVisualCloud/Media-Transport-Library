/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "tx_st22p_app.h"

static int app_tx_st22p_frame_available(void* priv) {
  struct st_app_tx_st22p_session* s = priv;

  st_pthread_mutex_lock(&s->st22p_wake_mutex);
  st_pthread_cond_signal(&s->st22p_wake_cond);
  st_pthread_mutex_unlock(&s->st22p_wake_mutex);

  return 0;
}

static void app_tx_st22p_build_frame(struct st_app_tx_st22p_session* s,
                                     struct st_frame_meta* frame) {
  if (s->st22p_frame_cursor + s->st22p_frame_size > s->st22p_source_end) {
    s->st22p_frame_cursor = s->st22p_source_begin;
  }
  uint8_t* src = s->st22p_frame_cursor;

  st_memcpy(frame->addr, src, s->st22p_frame_size);
  /* point to next frame */
  s->st22p_frame_cursor += s->st22p_frame_size;
}

static void* app_tx_st22p_frame_thread(void* arg) {
  struct st_app_tx_st22p_session* s = arg;
  st22p_tx_handle handle = s->handle;
  int idx = s->idx;
  struct st_frame_meta* frame;

  info("%s(%d), start\n", __func__, idx);
  while (!s->st22p_app_thread_stop) {
    frame = st22p_tx_get_frame(handle);
    if (!frame) { /* no frame */
      st_pthread_mutex_lock(&s->st22p_wake_mutex);
      if (!s->st22p_app_thread_stop)
        st_pthread_cond_wait(&s->st22p_wake_cond, &s->st22p_wake_mutex);
      st_pthread_mutex_unlock(&s->st22p_wake_mutex);
      continue;
    }
    app_tx_st22p_build_frame(s, frame);
    st22p_tx_put_frame(handle, frame);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static int app_tx_st22p_open_source(struct st_app_tx_st22p_session* s) {
  int fd;
  struct stat i;

  fd = st_open(s->st22p_source_url, O_RDONLY);
  if (fd < 0) {
    err("%s, open fail '%s'\n", __func__, s->st22p_source_url);
    return -EIO;
  }

  fstat(fd, &i);
  if (i.st_size < s->st22p_frame_size) {
    err("%s, %s file size small then a frame %d\n", __func__, s->st22p_source_url,
        s->st22p_frame_size);
    close(fd);
    return -EIO;
  }

  uint8_t* m = mmap(NULL, i.st_size, PROT_READ, MAP_SHARED, fd, 0);
  if (MAP_FAILED == m) {
    err("%s, mmap fail '%s'\n", __func__, s->st22p_source_url);
    close(fd);
    return -EIO;
  }

  s->st22p_source_begin = st_hp_malloc(s->st, i.st_size, ST_PORT_P);
  if (!s->st22p_source_begin) {
    warn("%s, source malloc on hugepage fail\n", __func__);
    s->st22p_source_begin = m;
    s->st22p_frame_cursor = m;
    s->st22p_source_end = m + i.st_size;
    s->st22p_source_fd = fd;
  } else {
    s->st22p_frame_cursor = s->st22p_source_begin;
    st_memcpy(s->st22p_source_begin, m, i.st_size);
    s->st22p_source_end = s->st22p_source_begin + i.st_size;
    close(fd);
  }

  return 0;
}

static int app_tx_st22p_start_source(struct st_app_context* ctx,
                                     struct st_app_tx_st22p_session* s) {
  int ret = -EINVAL;

  ret = pthread_create(&s->st22p_app_thread, NULL, app_tx_st22p_frame_thread, s);
  if (ret < 0) {
    err("%s, st22p_app_thread create fail err = %d\n", __func__, ret);
    return ret;
  }
  s->st22p_app_thread_stop = false;

  return 0;
}

static void app_tx_st22p_stop_source(struct st_app_tx_st22p_session* s) {
  s->st22p_app_thread_stop = true;
  /* wake up the thread */
  st_pthread_mutex_lock(&s->st22p_wake_mutex);
  st_pthread_cond_signal(&s->st22p_wake_cond);
  st_pthread_mutex_unlock(&s->st22p_wake_mutex);
  if (s->st22p_app_thread) {
    pthread_join(s->st22p_app_thread, NULL);
    s->st22p_app_thread = 0;
  }
}

static int app_tx_st22p_close_source(struct st_app_tx_st22p_session* s) {
  if (s->st22p_source_fd < 0 && s->st22p_source_begin) {
    st_hp_free(s->st, s->st22p_source_begin);
    s->st22p_source_begin = NULL;
  }
  if (s->st22p_source_fd >= 0) {
    munmap(s->st22p_source_begin, s->st22p_source_end - s->st22p_source_begin);
    close(s->st22p_source_fd);
    s->st22p_source_fd = -1;
  }

  return 0;
}

static int app_tx_st22p_handle_free(struct st_app_tx_st22p_session* s) {
  int ret;
  int idx = s->idx;

  if (s->handle) {
    ret = st22p_tx_free(s->handle);
    if (ret < 0) err("%s(%d), st22p_tx_free fail %d\n", __func__, idx, ret);
    s->handle = NULL;
  }

  return 0;
}

static int app_tx_st22p_uinit(struct st_app_tx_st22p_session* s) {
  app_tx_st22p_stop_source(s);
  app_tx_st22p_handle_free(s);
  app_tx_st22p_close_source(s);

  st_pthread_mutex_destroy(&s->st22p_wake_mutex);
  st_pthread_cond_destroy(&s->st22p_wake_cond);

  return 0;
}

static int app_tx_st22p_init(struct st_app_context* ctx,
                             st_json_tx_st22p_session_t* video,
                             struct st_app_tx_st22p_session* s) {
  int idx = s->idx, ret;
  struct st22p_tx_ops ops;
  char name[32];
  st22p_tx_handle handle;
  memset(&ops, 0, sizeof(ops));

  snprintf(name, 32, "app_tx_st22p_%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.port.num_port = video ? video->num_inf : ctx->para.num_ports;
  memcpy(ops.port.dip_addr[ST_PORT_P],
         video ? video->dip[ST_PORT_P] : ctx->tx_dip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  strncpy(ops.port.port[ST_PORT_P],
          video ? video->inf[ST_PORT_P]->name : ctx->para.port[ST_PORT_P],
          ST_PORT_MAX_LEN);
  ops.port.udp_port[ST_PORT_P] = video ? video->udp_port : (10000 + s->idx);
  if (ops.port.num_port > 1) {
    memcpy(ops.port.dip_addr[ST_PORT_R],
           video ? video->dip[ST_PORT_R] : ctx->tx_dip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops.port.port[ST_PORT_R],
            video ? video->inf[ST_PORT_R]->name : ctx->para.port[ST_PORT_R],
            ST_PORT_MAX_LEN);
    ops.port.udp_port[ST_PORT_R] = video ? video->udp_port : (10000 + s->idx);
  }
  ops.port.payload_type = video ? video->payload_type : ST_APP_PAYLOAD_TYPE_ST22;
  ops.width = video ? video->width : 1920;
  ops.height = video ? video->height : 1080;
  ops.fps = video ? video->fps : ST_FPS_P59_94;
  ops.input_fmt = video ? video->format : ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  ops.pack_type = video ? video->pack_type : ST22_PACK_CODESTREAM;
  ops.codec = video ? video->codec : ST22_CODEC_JPEGXS;
  ops.device = video ? video->device : ST_PLUGIN_DEVICE_AUTO;
  ops.quality = video ? video->quality : ST22_QUALITY_MODE_SPEED;
  ops.codec_thread_cnt = video ? video->codec_thread_count : 0;
  ops.codestream_size = ops.width * ops.height * 3 / 8;
  ops.framebuff_cnt = 2;
  ops.notify_frame_available = app_tx_st22p_frame_available;

  s->width = ops.width;
  s->height = ops.height;
  memcpy(s->st22p_source_url, video ? video->st22p_url : ctx->tx_st22p_url,
         ST_APP_URL_MAX_LEN);
  s->st = ctx->st;
  s->expect_fps = st_frame_rate(ops.fps);

  s->framebuff_cnt = ops.framebuff_cnt;
  s->st22p_source_fd = -1;

  st_pthread_mutex_init(&s->st22p_wake_mutex, NULL);
  st_pthread_cond_init(&s->st22p_wake_cond, NULL);

  handle = st22p_tx_create(ctx->st, &ops);
  if (!handle) {
    err("%s(%d), st22p_tx_create fail\n", __func__, idx);
    app_tx_st22p_uinit(s);
    return -EIO;
  }
  s->handle = handle;
  s->st22p_frame_size = st22p_tx_frame_size(handle);

  ret = app_tx_st22p_open_source(s);
  if (ret < 0) {
    err("%s(%d), app_tx_st22p_open_source fail %d\n", __func__, idx, ret);
    app_tx_st22p_uinit(s);
    return ret;
  }
  ret = app_tx_st22p_start_source(ctx, s);
  if (ret < 0) {
    err("%s(%d), app_tx_st22p_start_source fail %d\n", __func__, idx, ret);
    app_tx_st22p_uinit(s);
    return ret;
  }

  return 0;
}

int st_app_tx_st22p_sessions_init(struct st_app_context* ctx) {
  int ret, i;
  struct st_app_tx_st22p_session* s;
  ctx->tx_st22p_sessions = (struct st_app_tx_st22p_session*)st_app_zmalloc(
      sizeof(struct st_app_tx_st22p_session) * ctx->tx_st22p_session_cnt);
  if (!ctx->tx_st22p_sessions) return -ENOMEM;
  for (i = 0; i < ctx->tx_st22p_session_cnt; i++) {
    s = &ctx->tx_st22p_sessions[i];
    s->idx = i;
    ret = app_tx_st22p_init(ctx, ctx->json_ctx ? &ctx->json_ctx->tx_st22p[i] : NULL, s);
    if (ret < 0) {
      err("%s(%d), app_tx_st22p_init fail %d\n", __func__, i, ret);
      return ret;
    }
  }

  return 0;
}

int st_app_tx_st22p_sessions_stop(struct st_app_context* ctx) {
  struct st_app_tx_st22p_session* s;
  if (!ctx->tx_st22p_sessions) return 0;
  for (int i = 0; i < ctx->tx_st22p_session_cnt; i++) {
    s = &ctx->tx_st22p_sessions[i];
    app_tx_st22p_stop_source(s);
  }

  return 0;
}

int st_app_tx_st22p_sessions_uinit(struct st_app_context* ctx) {
  int i;
  struct st_app_tx_st22p_session* s;
  if (!ctx->tx_st22p_sessions) return 0;
  for (i = 0; i < ctx->tx_st22p_session_cnt; i++) {
    s = &ctx->tx_st22p_sessions[i];
    app_tx_st22p_uinit(s);
  }
  st_app_free(ctx->tx_st22p_sessions);

  return 0;
}
