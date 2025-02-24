/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#include "rx_st30p_app.h"

static void app_rx_st30p_consume_frame(struct st_app_rx_st30p_session *s,
                                       struct st30_frame *frame) {
  // int idx = s->idx;
  // todo
  MTL_MAY_UNUSED(s);
  MTL_MAY_UNUSED(frame);
}

static void *app_rx_st30p_frame_thread(void *arg) {
  struct st_app_rx_st30p_session *s = arg;
  struct st30_frame *frame;

  info("%s(%d), start\n", __func__, s->idx);
  while (!s->st30p_app_thread_stop) {
    frame = st30p_rx_get_frame(s->handle);
    if (!frame) { /* no ready frame */
      warn("%s(%d), get frame time out\n", __func__, s->idx);
      continue;
    }

    s->stat_frame_received++;
    app_rx_st30p_consume_frame(s, frame);
    s->stat_frame_total_received++;
    if (!s->stat_frame_first_rx_time)
      s->stat_frame_first_rx_time = st_app_get_monotonic_time();
    st30p_rx_put_frame(s->handle, frame);
  }
  info("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

static int app_rx_st30p_init_frame_thread(struct st_app_rx_st30p_session *s) {
  int ret, idx = s->idx;

  ret = pthread_create(&s->st30p_app_thread, NULL, app_rx_st30p_frame_thread, s);
  if (ret < 0) {
    err("%s(%d), st30p_app_thread create fail %d\n", __func__, ret, idx);
    return -EIO;
  }

  char thread_name[32];
  snprintf(thread_name, sizeof(thread_name), "rx_st30p_%d", idx);
  mtl_thread_setname(s->st30p_app_thread, thread_name);

  return 0;
}

static int app_rx_st30p_uinit(struct st_app_rx_st30p_session *s) {
  int ret, idx = s->idx;

  s->st30p_app_thread_stop = true;
  if (s->st30p_app_thread_stop) {
    /* wake up the thread */
    info("%s(%d), wait app thread stop\n", __func__, idx);
    if (s->handle) st30p_rx_wake_block(s->handle);
    pthread_join(s->st30p_app_thread, NULL);
  }

  if (s->handle) {
    ret = st30p_rx_free(s->handle);
    if (ret < 0) err("%s(%d), st30_rx_free fail %d\n", __func__, idx, ret);
    s->handle = NULL;
  }

  return 0;
}

static int app_rx_st30p_init(struct st_app_context *ctx,
                             struct st_json_st30p_session *st30p,
                             struct st_app_rx_st30p_session *s) {
  int idx = s->idx, ret;
  struct st30p_rx_ops ops;
  char name[32];
  st30p_rx_handle handle;
  memset(&ops, 0, sizeof(ops));

  s->last_stat_time_ns = st_app_get_monotonic_time();

  snprintf(name, 32, "app_rx_st30p_%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.port.num_port = st30p ? st30p->base.num_inf : ctx->para.num_ports;
  memcpy(ops.port.ip_addr[MTL_SESSION_PORT_P],
         st30p ? st_json_ip(ctx, &st30p->base, MTL_SESSION_PORT_P)
               : ctx->rx_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  memcpy(
      ops.port.mcast_sip_addr[MTL_SESSION_PORT_P],
      st30p ? st30p->base.mcast_src_ip[MTL_PORT_P] : ctx->rx_mcast_sip_addr[MTL_PORT_P],
      MTL_IP_ADDR_LEN);
  snprintf(
      ops.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
      st30p ? st30p->base.inf[MTL_SESSION_PORT_P]->name : ctx->para.port[MTL_PORT_P]);
  ops.port.udp_port[MTL_SESSION_PORT_P] = st30p ? st30p->base.udp_port : (10000 + s->idx);
  if (ops.port.num_port > 1) {
    memcpy(ops.port.ip_addr[MTL_SESSION_PORT_R],
           st30p ? st_json_ip(ctx, &st30p->base, MTL_SESSION_PORT_R)
                 : ctx->rx_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    memcpy(
        ops.port.mcast_sip_addr[MTL_SESSION_PORT_R],
        st30p ? st30p->base.mcast_src_ip[MTL_PORT_R] : ctx->rx_mcast_sip_addr[MTL_PORT_R],
        MTL_IP_ADDR_LEN);
    snprintf(
        ops.port.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
        st30p ? st30p->base.inf[MTL_SESSION_PORT_R]->name : ctx->para.port[MTL_PORT_R]);
    ops.port.udp_port[MTL_SESSION_PORT_R] =
        st30p ? st30p->base.udp_port : (10000 + s->idx);
  }
  ops.port.payload_type = st30p ? st30p->base.payload_type : ST_APP_PAYLOAD_TYPE_AUDIO;

  ops.fmt = st30p ? st30p->info.audio_format : ST30_FMT_PCM24;
  ops.channel = st30p ? st30p->info.audio_channel : 2;
  ops.sampling = st30p ? st30p->info.audio_sampling : ST30_SAMPLING_48K;
  ops.ptime = st30p ? st30p->info.audio_ptime : ST30_PTIME_1MS;
  /* set frame size to 10ms time */
  int framebuff_size = st30_calculate_framebuff_size(
      ops.fmt, ops.ptime, ops.sampling, ops.channel, 10 * NS_PER_MS, &s->expect_fps);
  ops.framebuff_size = framebuff_size;

  ops.flags |= ST30P_RX_FLAG_BLOCK_GET;
  ops.framebuff_cnt = 3;

  if (ctx->force_rx_audio_numa >= 0) {
    ops.flags |= ST30P_RX_FLAG_FORCE_NUMA;
    ops.socket_id = ctx->force_rx_audio_numa;
  }

  s->num_port = ops.port.num_port;

  handle = st30p_rx_create(ctx->st, &ops);
  if (!handle) {
    err("%s(%d), st30_rx_create fail\n", __func__, idx);
    app_rx_st30p_uinit(s);
    return -EIO;
  }
  s->handle = handle;

  s->st30p_frame_size = st30p_rx_frame_size(handle);

  ret = app_rx_st30p_init_frame_thread(s);
  if (ret < 0) {
    err("%s(%d), app_rx_st30p_init_thread fail %d\n", __func__, idx, ret);
    app_rx_st30p_uinit(s);
    return -EIO;
  }

  s->stat_frame_received = 0;
  s->stat_last_time = st_app_get_monotonic_time();

  return 0;
}

static int app_rx_st30p_stat(struct st_app_rx_st30p_session *s) {
  uint64_t cur_time_ns = st_app_get_monotonic_time();
#ifdef DEBUG
  double time_sec = (double)(cur_time_ns - s->stat_last_time) / NS_PER_S;
  double framerate = s->stat_frame_received / time_sec;
  dbg("%s(%d), fps %f, %d frame received\n", __func__, s->idx, framerate,
      s->stat_frame_received);
#endif
  s->stat_frame_received = 0;
  s->stat_last_time = cur_time_ns;

  return 0;
}

static int app_rx_st30p_result(struct st_app_rx_st30p_session *s) {
  int idx = s->idx;
  uint64_t cur_time_ns = st_app_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->stat_frame_first_rx_time) / NS_PER_S;
  double framerate = s->stat_frame_total_received / time_sec;

  if (!s->stat_frame_total_received) return -EINVAL;

  critical("%s(%d), %s, fps %f, %d frame received\n", __func__, idx,
           ST_APP_EXPECT_NEAR(framerate, s->expect_fps, s->expect_fps * 0.05) ? "OK"
                                                                              : "FAILED",
           framerate, s->stat_frame_total_received);
  return 0;
}

int st_app_rx_st30p_sessions_init(struct st_app_context *ctx) {
  int ret = 0, i = 0;
  struct st_app_rx_st30p_session *s;
  int fb_cnt = ctx->rx_video_fb_cnt;
  if (fb_cnt <= 0) fb_cnt = ST_APP_DEFAULT_FB_CNT;

  dbg("%s(%d), rx_st30p_session_cnt %d\n", __func__, i, ctx->rx_st30p_session_cnt);
  ctx->rx_st30p_sessions = (struct st_app_rx_st30p_session *)st_app_zmalloc(
      sizeof(struct st_app_rx_st30p_session) * ctx->rx_st30p_session_cnt);
  if (!ctx->rx_st30p_sessions) return -ENOMEM;
  for (i = 0; i < ctx->rx_st30p_session_cnt; i++) {
    s = &ctx->rx_st30p_sessions[i];
    s->idx = i;
    s->st = ctx->st;
    s->framebuff_cnt = fb_cnt;

    ret = app_rx_st30p_init(
        ctx, ctx->json_ctx ? &ctx->json_ctx->rx_st30p_sessions[i] : NULL, s);
    if (ret < 0) {
      err("%s(%d), app_rx_st30p_init fail %d\n", __func__, i, ret);
      return ret;
    }
  }

  return 0;
}

int st_app_rx_st30p_sessions_uinit(struct st_app_context *ctx) {
  int i;
  struct st_app_rx_st30p_session *s;
  if (!ctx->rx_st30p_sessions) return 0;
  for (i = 0; i < ctx->rx_st30p_session_cnt; i++) {
    s = &ctx->rx_st30p_sessions[i];
    app_rx_st30p_uinit(s);
  }
  st_app_free(ctx->rx_st30p_sessions);

  return 0;
}

int st_app_rx_st30p_sessions_stat(struct st_app_context *ctx) {
  int i;
  struct st_app_rx_st30p_session *s;
  if (!ctx->rx_st30p_sessions) return 0;

  for (i = 0; i < ctx->rx_st30p_session_cnt; i++) {
    s = &ctx->rx_st30p_sessions[i];
    app_rx_st30p_stat(s);
  }

  return 0;
}

int st_app_rx_st30p_sessions_result(struct st_app_context *ctx) {
  int i, ret = 0;
  struct st_app_rx_st30p_session *s;

  if (!ctx->rx_st30p_sessions) return 0;

  for (i = 0; i < ctx->rx_st30p_session_cnt; i++) {
    s = &ctx->rx_st30p_sessions[i];
    ret += app_rx_st30p_result(s);
  }

  return ret;
}
