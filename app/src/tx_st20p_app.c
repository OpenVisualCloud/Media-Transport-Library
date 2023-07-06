/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "tx_st20p_app.h"

static void app_tx_st20p_display_frame(struct st_app_tx_st20p_session* s,
                                       struct st_frame* frame) {
  struct st_display* d = s->display;

  if (d && d->front_frame) {
    if (st_pthread_mutex_trylock(&d->display_frame_mutex) == 0) {
      if (frame->fmt == ST_FRAME_FMT_YUV422RFC4175PG2BE10) {
        st20_rfc4175_422be10_to_422le8(frame->addr[0], d->front_frame, s->width,
                                       s->height);
      } else if (frame->fmt == ST_FRAME_FMT_UYVY) {
        mtl_memcpy(d->front_frame, frame->addr[0], d->front_frame_size);
      } else {
        st_pthread_mutex_unlock(&d->display_frame_mutex);
        return;
      }
      st_pthread_mutex_unlock(&d->display_frame_mutex);
      st_pthread_mutex_lock(&d->display_wake_mutex);
      st_pthread_cond_signal(&d->display_wake_cond);
      st_pthread_mutex_unlock(&d->display_wake_mutex);
    }
  }
}

static int app_tx_st20p_frame_available(void* priv) {
  struct st_app_tx_st20p_session* s = priv;

  st_pthread_mutex_lock(&s->st20p_wake_mutex);
  st_pthread_cond_signal(&s->st20p_wake_cond);
  st_pthread_mutex_unlock(&s->st20p_wake_mutex);

  return 0;
}

static void app_tx_st20p_build_frame(struct st_app_tx_st20p_session* s,
                                     struct st_frame* frame) {
  if (s->st20p_frame_cursor + s->st20p_frame_size > s->st20p_source_end) {
    s->st20p_frame_cursor = s->st20p_source_begin;
  }
  uint8_t* src = s->st20p_frame_cursor;

  mtl_memcpy(frame->addr[0], src, s->st20p_frame_size);
  /* point to next frame */
  s->st20p_frame_cursor += s->st20p_frame_size;

  app_tx_st20p_display_frame(s, frame);
}

static void* app_tx_st20p_frame_thread(void* arg) {
  struct st_app_tx_st20p_session* s = arg;
  st20p_tx_handle handle = s->handle;
  int idx = s->idx;
  struct st_frame* frame;

  info("%s(%d), start\n", __func__, idx);
  while (!s->st20p_app_thread_stop) {
    frame = st20p_tx_get_frame(handle);
    if (!frame) { /* no frame */
      st_pthread_mutex_lock(&s->st20p_wake_mutex);
      if (!s->st20p_app_thread_stop)
        st_pthread_cond_wait(&s->st20p_wake_cond, &s->st20p_wake_mutex);
      st_pthread_mutex_unlock(&s->st20p_wake_mutex);
      continue;
    }
    app_tx_st20p_build_frame(s, frame);
    st20p_tx_put_frame(handle, frame);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static int app_tx_st20p_open_source(struct st_app_tx_st20p_session* s) {
  int fd;
  struct stat i;

  fd = st_open(s->st20p_source_url, O_RDONLY);
  if (fd < 0) {
    err("%s, open fail '%s'\n", __func__, s->st20p_source_url);
    return -EIO;
  }

  fstat(fd, &i);
  if (i.st_size < s->st20p_frame_size) {
    err("%s, %s file size small then a frame %d\n", __func__, s->st20p_source_url,
        s->st20p_frame_size);
    close(fd);
    return -EIO;
  }

  uint8_t* m = mmap(NULL, i.st_size, PROT_READ, MAP_SHARED, fd, 0);
  if (MAP_FAILED == m) {
    err("%s, mmap fail '%s'\n", __func__, s->st20p_source_url);
    close(fd);
    return -EIO;
  }

  s->st20p_source_begin = mtl_hp_malloc(s->st, i.st_size, MTL_PORT_P);
  if (!s->st20p_source_begin) {
    warn("%s, source malloc on hugepage fail\n", __func__);
    s->st20p_source_begin = m;
    s->st20p_frame_cursor = m;
    s->st20p_source_end = m + i.st_size;
    s->st20p_source_fd = fd;
  } else {
    s->st20p_frame_cursor = s->st20p_source_begin;
    mtl_memcpy(s->st20p_source_begin, m, i.st_size);
    s->st20p_source_end = s->st20p_source_begin + i.st_size;
    close(fd);
  }

  return 0;
}

static int app_tx_st20p_start_source(struct st_app_context* ctx,
                                     struct st_app_tx_st20p_session* s) {
  int ret = -EINVAL;

  ret = pthread_create(&s->st20p_app_thread, NULL, app_tx_st20p_frame_thread, s);
  if (ret < 0) {
    err("%s, st20p_app_thread create fail err = %d\n", __func__, ret);
    return ret;
  }
  s->st20p_app_thread_stop = false;

  return 0;
}

static void app_tx_st20p_stop_source(struct st_app_tx_st20p_session* s) {
  s->st20p_app_thread_stop = true;
  /* wake up the thread */
  st_pthread_mutex_lock(&s->st20p_wake_mutex);
  st_pthread_cond_signal(&s->st20p_wake_cond);
  st_pthread_mutex_unlock(&s->st20p_wake_mutex);
  if (s->st20p_app_thread) {
    pthread_join(s->st20p_app_thread, NULL);
    s->st20p_app_thread = 0;
  }
}

static int app_tx_st20p_close_source(struct st_app_tx_st20p_session* s) {
  if (s->st20p_source_fd < 0 && s->st20p_source_begin) {
    mtl_hp_free(s->st, s->st20p_source_begin);
    s->st20p_source_begin = NULL;
  }
  if (s->st20p_source_fd >= 0) {
    munmap(s->st20p_source_begin, s->st20p_source_end - s->st20p_source_begin);
    close(s->st20p_source_fd);
    s->st20p_source_fd = -1;
  }

  return 0;
}

static int app_tx_st20p_handle_free(struct st_app_tx_st20p_session* s) {
  int ret;
  int idx = s->idx;

  if (s->handle) {
    ret = st20p_tx_free(s->handle);
    if (ret < 0) err("%s(%d), st20p_tx_free fail %d\n", __func__, idx, ret);
    s->handle = NULL;
  }

  return 0;
}

static int app_tx_st20p_uinit(struct st_app_tx_st20p_session* s) {
  app_tx_st20p_stop_source(s);
  app_tx_st20p_handle_free(s);
  app_tx_st20p_close_source(s);

  st_app_uinit_display(s->display);
  if (s->display) {
    st_app_free(s->display);
  }

  st_pthread_mutex_destroy(&s->st20p_wake_mutex);
  st_pthread_cond_destroy(&s->st20p_wake_cond);

  return 0;
}

static int app_tx_st20p_init(struct st_app_context* ctx, st_json_st20p_session_t* st20p,
                             struct st_app_tx_st20p_session* s) {
  int idx = s->idx, ret;
  struct st20p_tx_ops ops;
  char name[32];
  st20p_tx_handle handle;
  memset(&ops, 0, sizeof(ops));

  snprintf(name, 32, "app_tx_st20p_%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.port.num_port = st20p ? st20p->base.num_inf : ctx->para.num_ports;
  memcpy(ops.port.dip_addr[MTL_SESSION_PORT_P],
         st20p ? st_json_ip(ctx, &st20p->base, MTL_SESSION_PORT_P)
               : ctx->tx_dip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  strncpy(ops.port.port[MTL_SESSION_PORT_P],
          st20p ? st20p->base.inf[MTL_SESSION_PORT_P]->name : ctx->para.port[MTL_PORT_P],
          MTL_PORT_MAX_LEN);
  ops.port.udp_port[MTL_SESSION_PORT_P] = st20p ? st20p->base.udp_port : (10000 + s->idx);
  if (ctx->has_tx_dst_mac[MTL_PORT_P]) {
    memcpy(&ops.tx_dst_mac[MTL_SESSION_PORT_P][0], ctx->tx_dst_mac[MTL_PORT_P],
           MTL_MAC_ADDR_LEN);
    ops.flags |= ST20P_TX_FLAG_USER_P_MAC;
  }
  if (ops.port.num_port > 1) {
    memcpy(ops.port.dip_addr[MTL_SESSION_PORT_R],
           st20p ? st_json_ip(ctx, &st20p->base, MTL_SESSION_PORT_R)
                 : ctx->tx_dip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    strncpy(
        ops.port.port[MTL_SESSION_PORT_R],
        st20p ? st20p->base.inf[MTL_SESSION_PORT_R]->name : ctx->para.port[MTL_PORT_R],
        MTL_PORT_MAX_LEN);
    ops.port.udp_port[MTL_SESSION_PORT_R] =
        st20p ? st20p->base.udp_port : (10000 + s->idx);
    if (ctx->has_tx_dst_mac[MTL_PORT_R]) {
      memcpy(&ops.tx_dst_mac[MTL_SESSION_PORT_R][0], ctx->tx_dst_mac[MTL_PORT_R],
             MTL_MAC_ADDR_LEN);
      ops.flags |= ST20P_TX_FLAG_USER_R_MAC;
    }
  }
  ops.port.payload_type = st20p ? st20p->base.payload_type : ST_APP_PAYLOAD_TYPE_VIDEO;
  ops.width = st20p ? st20p->info.width : 1920;
  ops.height = st20p ? st20p->info.height : 1080;
  ops.fps = st20p ? st20p->info.fps : ST_FPS_P59_94;
  ops.interlaced = st20p ? st20p->info.interlaced : false;
  ops.input_fmt = st20p ? st20p->info.format : ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  ops.transport_fmt = st20p ? st20p->info.transport_format : ST20_FMT_YUV_422_10BIT;
  ops.device = st20p ? st20p->info.device : ST_PLUGIN_DEVICE_AUTO;
  ops.framebuff_cnt = 2;
  ops.notify_frame_available = app_tx_st20p_frame_available;
  ops.start_vrx = ctx->tx_start_vrx;
  ops.pad_interval = ctx->tx_pad_interval;
  if (ctx->tx_no_static_pad) ops.flags |= ST20P_TX_FLAG_DISABLE_STATIC_PAD_P;

  s->width = ops.width;
  s->height = ops.height;
  memcpy(s->st20p_source_url, st20p ? st20p->info.st20p_url : ctx->tx_st20p_url,
         ST_APP_URL_MAX_LEN);
  s->st = ctx->st;
  s->expect_fps = st_frame_rate(ops.fps);

  s->framebuff_cnt = ops.framebuff_cnt;
  s->st20p_source_fd = -1;

  st_pthread_mutex_init(&s->st20p_wake_mutex, NULL);
  st_pthread_cond_init(&s->st20p_wake_cond, NULL);

  handle = st20p_tx_create(ctx->st, &ops);
  if (!handle) {
    err("%s(%d), st20p_tx_create fail\n", __func__, idx);
    app_tx_st20p_uinit(s);
    return -EIO;
  }
  s->handle = handle;
  s->st20p_frame_size = st20p_tx_frame_size(handle);

  ret = app_tx_st20p_open_source(s);
  if (ret < 0) {
    err("%s(%d), app_tx_st20p_open_source fail %d\n", __func__, idx, ret);
    app_tx_st20p_uinit(s);
    return ret;
  }
  ret = app_tx_st20p_start_source(ctx, s);
  if (ret < 0) {
    err("%s(%d), app_tx_st20p_start_source fail %d\n", __func__, idx, ret);
    app_tx_st20p_uinit(s);
    return ret;
  }

  if (ctx->has_sdl && st20p && st20p->display) {
    struct st_display* d = st_app_zmalloc(sizeof(struct st_display));
    ret = st_app_init_display(d, name, s->width, s->height, ctx->ttf_file);
    if (ret < 0) {
      err("%s(%d), st_app_init_display fail %d\n", __func__, idx, ret);
      app_tx_st20p_uinit(s);
      return -EIO;
    }
    s->display = d;
  }

  return 0;
}

int st_app_tx_st20p_sessions_init(struct st_app_context* ctx) {
  int ret, i;
  struct st_app_tx_st20p_session* s;
  ctx->tx_st20p_sessions = (struct st_app_tx_st20p_session*)st_app_zmalloc(
      sizeof(struct st_app_tx_st20p_session) * ctx->tx_st20p_session_cnt);
  if (!ctx->tx_st20p_sessions) return -ENOMEM;
  for (i = 0; i < ctx->tx_st20p_session_cnt; i++) {
    s = &ctx->tx_st20p_sessions[i];
    s->idx = i;
    ret = app_tx_st20p_init(
        ctx, ctx->json_ctx ? &ctx->json_ctx->tx_st20p_sessions[i] : NULL, s);
    if (ret < 0) {
      err("%s(%d), app_tx_st20p_init fail %d\n", __func__, i, ret);
      return ret;
    }
  }

  return 0;
}

int st_app_tx_st20p_sessions_stop(struct st_app_context* ctx) {
  struct st_app_tx_st20p_session* s;
  if (!ctx->tx_st20p_sessions) return 0;
  for (int i = 0; i < ctx->tx_st20p_session_cnt; i++) {
    s = &ctx->tx_st20p_sessions[i];
    app_tx_st20p_stop_source(s);
  }

  return 0;
}

int st_app_tx_st20p_sessions_uinit(struct st_app_context* ctx) {
  int i;
  struct st_app_tx_st20p_session* s;
  if (!ctx->tx_st20p_sessions) return 0;
  for (i = 0; i < ctx->tx_st20p_session_cnt; i++) {
    s = &ctx->tx_st20p_sessions[i];
    app_tx_st20p_uinit(s);
  }
  st_app_free(ctx->tx_st20p_sessions);

  return 0;
}
