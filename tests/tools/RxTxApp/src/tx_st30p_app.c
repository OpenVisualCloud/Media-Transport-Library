/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#include "tx_st30p_app.h"

static void app_tx_st30p_build_frame(struct st_app_tx_st30p_session* s,
                                     struct st30_frame* frame, size_t frame_size) {
  uint8_t* src = s->st30p_frame_cursor;

  mtl_memcpy(frame->addr, src, frame_size);

  /* point to next frame */
  s->st30p_frame_cursor += frame_size;
  if (s->st30p_frame_cursor + frame_size > s->st30p_source_end) {
    s->st30p_frame_cursor = s->st30p_source_begin;
    s->st30p_frames_copied = true;
  }
}

static void* app_tx_st30p_frame_thread(void* arg) {
  struct st_app_tx_st30p_session* s = arg;
  st30p_tx_handle handle = s->handle;
  int idx = s->idx;
  struct st30_frame* frame;
  double frame_time;

  frame_time = s->expect_fps ? (NS_PER_S / s->expect_fps) : 0;

  info("%s(%d), start\n", __func__, idx);
  while (!s->st30p_app_thread_stop) {
    frame = st30p_tx_get_frame(handle);
    if (!frame) { /* no ready frame */
      warn("%s(%d), get frame time out\n", __func__, s->idx);
      continue;
    }
    app_tx_st30p_build_frame(s, frame, s->st30p_frame_size);

    if (s->user_time) {
      bool restart_base_time = !s->local_tai_base_time;

      frame->timestamp = st_app_user_time(s->ctx, s->user_time, s->frame_num, frame_time,
                                          restart_base_time);
      frame->tfmt = ST10_TIMESTAMP_FMT_TAI;
      s->frame_num++;
      s->local_tai_base_time = s->user_time->base_tai_time;
    }

    st30p_tx_put_frame(handle, frame);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static int app_tx_st30p_open_source(struct st_app_tx_st30p_session* s) {
  int fd;
  struct stat i;

  fd = st_open(s->st30p_source_url, O_RDONLY);
  if (fd < 0) {
    err("%s, open fail '%s'\n", __func__, s->st30p_source_url);
    return -EIO;
  }

  if (fstat(fd, &i) < 0) {
    err("%s, fstat %s fail\n", __func__, s->st30p_source_url);
    close(fd);
    return -EIO;
  }
  if (i.st_size < s->st30p_frame_size) {
    err("%s, %s file size small then a frame %d\n", __func__, s->st30p_source_url,
        s->st30p_frame_size);
    close(fd);
    return -EIO;
  }

  uint8_t* m = mmap(NULL, i.st_size, PROT_READ, MAP_SHARED, fd, 0);
  if (MAP_FAILED == m) {
    err("%s, mmap fail '%s'\n", __func__, s->st30p_source_url);
    close(fd);
    return -EIO;
  }

  s->st30p_source_begin = mtl_hp_malloc(s->st, i.st_size, MTL_PORT_P);
  if (!s->st30p_source_begin) {
    warn("%s, source malloc on hugepage fail\n", __func__);
    s->st30p_source_begin = m;
    s->st30p_frame_cursor = m;
    s->st30p_source_end = m + i.st_size;
    s->st30p_source_fd = fd;
  } else {
    s->st30p_frame_cursor = s->st30p_source_begin;
    mtl_memcpy(s->st30p_source_begin, m, i.st_size);
    s->st30p_source_end = s->st30p_source_begin + i.st_size;
    close(fd);
  }

  return 0;
}

static int app_tx_st30p_start_source(struct st_app_tx_st30p_session* s) {
  int ret = -EINVAL;
  int idx = s->idx;

  ret = pthread_create(&s->st30p_app_thread, NULL, app_tx_st30p_frame_thread, s);
  if (ret < 0) {
    err("%s(%d), thread create fail ret %d\n", __func__, idx, ret);
    return ret;
  }
  s->st30p_app_thread_stop = false;

  char thread_name[32];
  snprintf(thread_name, sizeof(thread_name), "tx_st30p_%d", idx);
  mtl_thread_setname(s->st30p_app_thread, thread_name);

  return 0;
}

static void app_tx_st30p_stop_source(struct st_app_tx_st30p_session* s) {
  s->st30p_app_thread_stop = true;
  if (s->st30p_app_thread) {
    info("%s(%d), wait app thread stop\n", __func__, s->idx);
    if (s->handle) st30p_tx_wake_block(s->handle);
    pthread_join(s->st30p_app_thread, NULL);
    s->st30p_app_thread = 0;
  }
}

static int app_tx_st30p_close_source(struct st_app_tx_st30p_session* s) {
  if (s->st30p_source_fd < 0 && s->st30p_source_begin) {
    mtl_hp_free(s->st, s->st30p_source_begin);
    s->st30p_source_begin = NULL;
  }
  if (s->st30p_source_fd >= 0) {
    munmap(s->st30p_source_begin, s->st30p_source_end - s->st30p_source_begin);
    close(s->st30p_source_fd);
    s->st30p_source_fd = -1;
  }

  return 0;
}

static int app_tx_st30p_handle_free(struct st_app_tx_st30p_session* s) {
  int ret;
  int idx = s->idx;

  if (s->handle) {
    ret = st30p_tx_free(s->handle);
    if (ret < 0) err("%s(%d), st30p_tx_free fail %d\n", __func__, idx, ret);
    s->handle = NULL;
  }

  return 0;
}

static int app_tx_st30p_uinit(struct st_app_tx_st30p_session* s) {
  app_tx_st30p_stop_source(s);
  app_tx_st30p_handle_free(s);
  app_tx_st30p_close_source(s);
  return 0;
}

static int app_tx_st30p_init(struct st_app_context* ctx, st_json_st30p_session_t* st30p,
                             struct st_app_tx_st30p_session* s) {
  int idx = s->idx, ret;
  struct st30p_tx_ops ops;
  char name[32];
  st30p_tx_handle handle;
  memset(&ops, 0, sizeof(ops));

  s->ctx = ctx;
  s->last_stat_time_ns = st_app_get_monotonic_time();

  snprintf(name, 32, "app_tx_st30p_%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.port.num_port = st30p ? st30p->base.num_inf : ctx->para.num_ports;
  memcpy(ops.port.dip_addr[MTL_SESSION_PORT_P],
         st30p ? st_json_ip(ctx, &st30p->base, MTL_SESSION_PORT_P)
               : ctx->tx_dip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(
      ops.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
      st30p ? st30p->base.inf[MTL_SESSION_PORT_P]->name : ctx->para.port[MTL_PORT_P]);
  ops.port.udp_port[MTL_SESSION_PORT_P] = st30p ? st30p->base.udp_port : (10000 + s->idx);
  if (ctx->has_tx_dst_mac[MTL_PORT_P]) {
    memcpy(&ops.tx_dst_mac[MTL_SESSION_PORT_P][0], ctx->tx_dst_mac[MTL_PORT_P],
           MTL_MAC_ADDR_LEN);
    ops.flags |= ST30P_TX_FLAG_USER_P_MAC;
  }
  if (ops.port.num_port > 1) {
    memcpy(ops.port.dip_addr[MTL_SESSION_PORT_R],
           st30p ? st_json_ip(ctx, &st30p->base, MTL_SESSION_PORT_R)
                 : ctx->tx_dip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(
        ops.port.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
        st30p ? st30p->base.inf[MTL_SESSION_PORT_R]->name : ctx->para.port[MTL_PORT_R]);
    ops.port.udp_port[MTL_SESSION_PORT_R] =
        st30p ? st30p->base.udp_port : (10000 + s->idx);
    if (ctx->has_tx_dst_mac[MTL_PORT_R]) {
      memcpy(&ops.tx_dst_mac[MTL_SESSION_PORT_R][0], ctx->tx_dst_mac[MTL_PORT_R],
             MTL_MAC_ADDR_LEN);
      ops.flags |= ST30P_TX_FLAG_USER_R_MAC;
    }
  }
  ops.port.payload_type = st30p ? st30p->base.payload_type : ST_APP_PAYLOAD_TYPE_AUDIO;
  ops.fmt = st30p ? st30p->info.audio_format : ST30_FMT_PCM24;
  ops.channel = st30p ? st30p->info.audio_channel : 2;
  ops.sampling = st30p ? st30p->info.audio_sampling : ST30_SAMPLING_48K;
  ops.ptime = st30p ? st30p->info.audio_ptime : ST30_PTIME_1MS;

  if (st30p && st30p->user_pacing) {
    s->packet_time = st30_get_packet_time(ops.ptime);
  } else {
    s->packet_time = ST_APP_TX_ST30P_DEFAULT_PACKET_TIME;
  }

  /* set frame size to 10ms time */
  int framebuff_size = st30_calculate_framebuff_size(
      ops.fmt, ops.ptime, ops.sampling, ops.channel, s->packet_time, &s->expect_fps);
  ops.framebuff_size = framebuff_size;
  ops.framebuff_cnt = 3;

  if (st30p && st30p->user_pacing) {
    ops.flags |= ST30P_TX_FLAG_USER_PACING;

    /* use global user time */
    s->user_time = &ctx->user_time;
    s->frame_num = 0;
    s->local_tai_base_time = 0;
  }

  ops.flags |= ST30P_TX_FLAG_BLOCK_GET;
  s->num_port = ops.port.num_port;
  memcpy(s->st30p_source_url, st30p ? st30p->info.audio_url : ctx->tx_audio_url,
         ST_APP_URL_MAX_LEN);
  s->st = ctx->st;

  s->framebuff_cnt = ops.framebuff_cnt;
  s->st30p_source_fd = -1;

  if (ctx->tx_audio_dedicate_queue) ops.flags |= ST30P_TX_FLAG_DEDICATE_QUEUE;

  if (ctx->force_tx_audio_numa >= 0) {
    ops.flags |= ST30P_TX_FLAG_FORCE_NUMA;
    ops.socket_id = ctx->force_tx_audio_numa;
  }

  handle = st30p_tx_create(ctx->st, &ops);
  if (!handle) {
    err("%s(%d), st30p_tx_create fail\n", __func__, idx);
    app_tx_st30p_uinit(s);
    return -EIO;
  }
  s->handle = handle;
  s->st30p_frame_size = st30p_tx_frame_size(handle);

  ret = app_tx_st30p_open_source(s);
  if (ret < 0) {
    err("%s(%d), app_tx_st30p_open_source fail %d\n", __func__, idx, ret);
    app_tx_st30p_uinit(s);
    return ret;
  }
  ret = app_tx_st30p_start_source(s);
  if (ret < 0) {
    err("%s(%d), app_tx_st30p_start_source fail %d\n", __func__, idx, ret);
    app_tx_st30p_uinit(s);
    return ret;
  }

  return 0;
}

int st_app_tx_st30p_sessions_init(struct st_app_context* ctx) {
  int ret, i;
  struct st_app_tx_st30p_session* s;
  ctx->tx_st30p_sessions = (struct st_app_tx_st30p_session*)st_app_zmalloc(
      sizeof(struct st_app_tx_st30p_session) * ctx->tx_st30p_session_cnt);
  if (!ctx->tx_st30p_sessions) return -ENOMEM;
  for (i = 0; i < ctx->tx_st30p_session_cnt; i++) {
    s = &ctx->tx_st30p_sessions[i];
    s->idx = i;
    ret = app_tx_st30p_init(
        ctx, ctx->json_ctx ? &ctx->json_ctx->tx_st30p_sessions[i] : NULL, s);
    if (ret < 0) {
      err("%s(%d), app_tx_st30p_init fail %d\n", __func__, i, ret);
      return ret;
    }
  }

  return 0;
}

int st_app_tx_st30p_sessions_stop(struct st_app_context* ctx) {
  struct st_app_tx_st30p_session* s;
  if (!ctx->tx_st30p_sessions) return 0;
  for (int i = 0; i < ctx->tx_st30p_session_cnt; i++) {
    s = &ctx->tx_st30p_sessions[i];
    app_tx_st30p_stop_source(s);
  }

  return 0;
}

int st_app_tx_st30p_sessions_uinit(struct st_app_context* ctx) {
  int i;
  struct st_app_tx_st30p_session* s;
  if (!ctx->tx_st30p_sessions) return 0;
  for (i = 0; i < ctx->tx_st30p_session_cnt; i++) {
    s = &ctx->tx_st30p_sessions[i];
    app_tx_st30p_uinit(s);
  }
  st_app_free(ctx->tx_st30p_sessions);

  return 0;
}
