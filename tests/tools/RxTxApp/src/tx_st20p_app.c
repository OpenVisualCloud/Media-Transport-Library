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

static int app_tx_st20p_notify_event(void* priv, enum st_event event, void* args) {
  struct st_app_tx_st20p_session* s = priv;
  if (event == ST_EVENT_VSYNC) {
    struct st10_vsync_meta* meta = args;
    info("%s(%d), epoch %" PRIu64 "\n", __func__, s->idx, meta->epoch);
  } else if (event == ST_EVENT_FATAL_ERROR) {
    err("%s(%d), ST_EVENT_FATAL_ERROR\n", __func__, s->idx);
    /* add a exist routine */
  } else if (event == ST_EVENT_RECOVERY_ERROR) {
    info("%s(%d), ST_EVENT_RECOVERY_ERROR\n", __func__, s->idx);
  }
  return 0;
}

static void app_tx_st20p_build_frame(struct st_app_tx_st20p_session* s,
                                     struct st_frame* frame, size_t frame_size) {
  uint8_t* src = s->st20p_frame_cursor;

  if (!s->ctx->tx_copy_once || !s->st20p_frames_copied) {
    mtl_memcpy(frame->addr[0], src, frame_size);
  }
  /* point to next frame */
  s->st20p_frame_cursor += frame_size;
  if (s->st20p_frame_cursor + frame_size > s->st20p_source_end) {
    s->st20p_frame_cursor = s->st20p_source_begin;
    s->st20p_frames_copied = true;
    /* mark file as complete for auto_stop feature */
    if (s->ctx->auto_stop && !s->tx_file_complete) {
      info("%s(%d), tx file complete\n", __func__, s->idx);
      s->tx_file_complete = true;
    }
  }

  app_tx_st20p_display_frame(s, frame);
}

static void* app_tx_st20p_frame_thread(void* arg) {
  struct st_app_tx_st20p_session* s = arg;
  st20p_tx_handle handle = s->handle;
  int idx = s->idx;
  struct st_frame* frame;
  uint8_t shas[SHA256_DIGEST_LENGTH];
  double frame_time;

  frame_time = s->expect_fps ? (NS_PER_S / s->expect_fps) : 0;

  info("%s(%d), start\n", __func__, idx);
  while (!s->st20p_app_thread_stop) {
    /* for auto_stop: stop sending after file is complete */
    if (s->ctx->auto_stop && s->tx_file_complete) {
      info("%s(%d), auto_stop: file complete, stopping tx\n", __func__, idx);
      break;
    }

    frame = st20p_tx_get_frame(handle);
    if (!frame) { /* no ready frame */
      warn("%s(%d), get frame time out\n", __func__, s->idx);
      continue;
    }
    app_tx_st20p_build_frame(s, frame, s->st20p_frame_size);
    if (s->sha_check) {
      st_sha256((unsigned char*)frame->addr[0], st_frame_plane_size(frame, 0), shas);
      frame->user_meta = shas;
      frame->user_meta_size = sizeof(shas);
    }

    if (s->user_time) {
      bool restart_base_time = !s->local_tai_base_time;
      frame->timestamp = st_app_user_time(s->ctx, s->user_time, s->frame_num, frame_time,
                                          restart_base_time);
      frame->tfmt = ST10_TIMESTAMP_FMT_TAI;
      s->frame_num++;
      s->local_tai_base_time = s->user_time->base_tai_time;
    }

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

  if (fstat(fd, &i) < 0) {
    err("%s, fstat %s fail\n", __func__, s->st20p_source_url);
    close(fd);
    return -EIO;
  }
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

static int app_tx_st20p_start_source(struct st_app_tx_st20p_session* s) {
  int ret = -EINVAL;
  int idx = s->idx;

  ret = pthread_create(&s->st20p_app_thread, NULL, app_tx_st20p_frame_thread, s);
  if (ret < 0) {
    err("%s(%d), thread create fail err = %d\n", __func__, idx, ret);
    return ret;
  }
  s->st20p_app_thread_stop = false;

  char thread_name[32];
  snprintf(thread_name, sizeof(thread_name), "tx_st20p_%d", idx);
  mtl_thread_setname(s->st20p_app_thread, thread_name);

  return 0;
}

static void app_tx_st20p_stop_source(struct st_app_tx_st20p_session* s) {
  s->st20p_app_thread_stop = true;
  if (s->st20p_app_thread) {
    info("%s(%d), wait app thread stop\n", __func__, s->idx);
    if (s->handle) st20p_tx_wake_block(s->handle);
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

  return 0;
}

static int app_tx_st20p_io_stat(struct st_app_tx_st20p_session* s) {
  int idx = s->idx;
  uint64_t cur_time = st_app_get_monotonic_time();
  double time_sec = (double)(cur_time - s->last_stat_time_ns) / NS_PER_S;
  double tx_rate_m, fps;
  int ret;
  struct st20_tx_user_stats stats;

  if (!s->handle) return 0;

  for (uint8_t port = 0; port < s->num_port; port++) {
    ret = st20p_tx_get_session_stats(s->handle, &stats);
    if (ret < 0) return ret;
    tx_rate_m = (double)stats.common.port[port].bytes * 8 / time_sec / MTL_STAT_M_UNIT;
    fps = (double)stats.common.port[port].frames / time_sec;

    info("%s(%d,%u), tx %f Mb/s fps %f\n", __func__, idx, port, tx_rate_m, fps);
  }
  st20p_tx_reset_session_stats(s->handle);

  s->last_stat_time_ns = cur_time;
  return 0;
}

static int app_tx_st20p_init(struct st_app_context* ctx, st_json_st20p_session_t* st20p,
                             struct st_app_tx_st20p_session* s) {
  int idx = s->idx, ret;
  struct st20p_tx_ops ops;
  char name[32];
  st20p_tx_handle handle;
  memset(&ops, 0, sizeof(ops));

  s->ctx = ctx;
  s->last_stat_time_ns = st_app_get_monotonic_time();
  s->sha_check = ctx->video_sha_check;

  snprintf(name, 32, "app_tx_st20p_%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.port.num_port = st20p ? st20p->base.num_inf : ctx->para.num_ports;
  memcpy(ops.port.dip_addr[MTL_SESSION_PORT_P],
         st20p ? st_json_ip(ctx, &st20p->base, MTL_SESSION_PORT_P)
               : ctx->tx_dip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(
      ops.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
      st20p ? st20p->base.inf[MTL_SESSION_PORT_P]->name : ctx->para.port[MTL_PORT_P]);
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
    snprintf(
        ops.port.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
        st20p ? st20p->base.inf[MTL_SESSION_PORT_R]->name : ctx->para.port[MTL_PORT_R]);
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
  ops.transport_pacing = st20p ? st20p->info.transport_pacing : ST21_PACING_NARROW;
  if (ctx->tx_pacing_type) /* override if args has pacing defined */
    ops.transport_pacing = ctx->tx_pacing_type;
  ops.transport_packing = st20p ? st20p->info.transport_packing : ST20_PACKING_BPM;
  ops.transport_fmt = st20p ? st20p->info.transport_format : ST20_FMT_YUV_422_10BIT;
  ops.device = st20p ? st20p->info.device : ST_PLUGIN_DEVICE_AUTO;
  ops.framebuff_cnt = 2;
  ops.flags |= ST20P_TX_FLAG_BLOCK_GET;
  ops.start_vrx = ctx->tx_start_vrx;
  ops.pad_interval = ctx->tx_pad_interval;
  ops.rtp_timestamp_delta_us = ctx->tx_ts_delta_us;
  ops.notify_event = app_tx_st20p_notify_event;
  if (ctx->tx_static_pad) ops.flags |= ST20P_TX_FLAG_ENABLE_STATIC_PAD_P;
  if (st20p && st20p->enable_rtcp) ops.flags |= ST20P_TX_FLAG_ENABLE_RTCP;

  if (st20p && (st20p->user_timestamp || st20p->user_pacing)) {
    if (st20p->user_pacing) {
      ops.flags |= ST20P_TX_FLAG_USER_PACING;
    }
    if (st20p->user_timestamp) {
      ops.flags |= ST20P_TX_FLAG_USER_TIMESTAMP;
    }
    /* use global user time */
    s->user_time = &ctx->user_time;
    s->frame_num = 0;
    s->local_tai_base_time = 0;
  }

  if (st20p && st20p->exact_user_pacing) {
    /* should be active only with user_pacing */
    ops.flags |= ST20P_TX_FLAG_EXACT_USER_PACING;
  }

  if (ctx->tx_exact_user_pacing) ops.flags |= ST20P_TX_FLAG_EXACT_USER_PACING;
  if (ctx->tx_ts_epoch) ops.flags |= ST20P_TX_FLAG_RTP_TIMESTAMP_EPOCH;
  if (ctx->tx_no_bulk) ops.flags |= ST20P_TX_FLAG_DISABLE_BULK;
  if (ctx->force_tx_video_numa >= 0) {
    ops.flags |= ST20P_TX_FLAG_FORCE_NUMA;
    ops.socket_id = ctx->force_tx_video_numa;
  }

  s->width = ops.width;
  s->height = ops.height;
  if (ops.interlaced) {
    s->height >>= 1;
  }
  s->num_port = ops.port.num_port;
  memcpy(s->st20p_source_url, st20p ? st20p->info.st20p_url : ctx->tx_st20p_url,
         ST_APP_URL_MAX_LEN);
  s->st = ctx->st;
  s->expect_fps = st_frame_rate(ops.fps);

  s->framebuff_cnt = ops.framebuff_cnt;
  s->st20p_source_fd = -1;

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
  ret = app_tx_st20p_start_source(s);
  if (ret < 0) {
    err("%s(%d), app_tx_st20p_start_source fail %d\n", __func__, idx, ret);
    app_tx_st20p_uinit(s);
    return ret;
  }

  if ((st20p && st20p->display) || ctx->tx_display) {
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

int st_app_tx_st20p_io_stat(struct st_app_context* ctx) {
  int i, ret = 0;
  struct st_app_tx_st20p_session* s;
  if (!ctx->tx_st20p_sessions) return 0;

  for (i = 0; i < ctx->tx_st20p_session_cnt; i++) {
    s = &ctx->tx_st20p_sessions[i];
    ret += app_tx_st20p_io_stat(s);
  }

  return ret;
}

bool st_app_tx_st20p_sessions_all_complete(struct st_app_context* ctx) {
  struct st_app_tx_st20p_session* s;
  if (!ctx->tx_st20p_sessions || ctx->tx_st20p_session_cnt == 0) return true;

  for (int i = 0; i < ctx->tx_st20p_session_cnt; i++) {
    s = &ctx->tx_st20p_sessions[i];
    if (!s->tx_file_complete) return false;
  }
  return true;
}
