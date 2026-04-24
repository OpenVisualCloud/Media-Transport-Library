/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "rx_st40p_app.h"

#include <inttypes.h>

#include "log.h"

#define ST40P_APP_RX_MAX_UDW_SIZE 2048
#define ST40P_APP_RX_RTP_RING_SIZE 2048

static void* app_rx_st40p_frame_thread(void* arg) {
  struct st_app_rx_st40p_session* s = arg;
  st40p_rx_handle handle = s->handle;
  struct st40_frame_info* frame_info;
  int idx = s->idx;

  info("%s(%d), start\n", __func__, idx);
  while (!s->st40p_app_thread_stop) {
    frame_info = st40p_rx_get_frame(handle);
    if (!frame_info) {
      st_pthread_mutex_lock(&s->st40p_wake_mutex);
      if (!s->st40p_app_thread_stop)
        st_pthread_cond_wait(&s->st40p_wake_cond, &s->st40p_wake_mutex);
      st_pthread_mutex_unlock(&s->st40p_wake_mutex);
      continue;
    }

    s->stat_frame_total_received++;
    if (!s->stat_frame_first_rx_time)
      s->stat_frame_first_rx_time = st_app_get_monotonic_time();
    if (frame_info->seq_discont) s->stat_frame_seq_discont++;
    if (!frame_info->rtp_marker) s->stat_frame_marker_missing++;
    s->stat_seq_lost_total += frame_info->seq_lost;

    dbg("%s(%d), frame %d, status %d, marker %d, seq_lost %u\n", __func__, idx,
        s->stat_frame_total_received, frame_info->status, frame_info->rtp_marker,
        frame_info->seq_lost);

    st40p_rx_put_frame(handle, frame_info);
  }
  info("%s(%d), stop\n", __func__, idx);
  return NULL;
}

static int app_rx_st40p_frame_available(void* priv) {
  struct st_app_rx_st40p_session* s = priv;

  st_pthread_mutex_lock(&s->st40p_wake_mutex);
  st_pthread_cond_signal(&s->st40p_wake_cond);
  st_pthread_mutex_unlock(&s->st40p_wake_mutex);
  return 0;
}

static int app_rx_st40p_uinit(struct st_app_rx_st40p_session* s) {
  int ret, idx = s->idx;

  s->st40p_app_thread_stop = true;
  if (s->st40p_app_thread) {
    if (s->handle) st40p_rx_wake_block(s->handle);
    st_pthread_mutex_lock(&s->st40p_wake_mutex);
    st_pthread_cond_signal(&s->st40p_wake_cond);
    st_pthread_mutex_unlock(&s->st40p_wake_mutex);
    info("%s(%d), wait app thread stop\n", __func__, idx);
    pthread_join(s->st40p_app_thread, NULL);
    s->st40p_app_thread = 0;
  }
  if (s->handle) {
    ret = st40p_rx_free(s->handle);
    if (ret < 0) err("%s(%d), st40p_rx_free fail %d\n", __func__, idx, ret);
    s->handle = NULL;
  }
  st_pthread_mutex_destroy(&s->st40p_wake_mutex);
  st_pthread_cond_destroy(&s->st40p_wake_cond);

  return 0;
}

static int app_rx_st40p_init(struct st_app_context* ctx, st_json_st40p_session_t* st40p,
                             struct st_app_rx_st40p_session* s) {
  int idx = s->idx, ret;
  struct st40p_rx_ops ops;
  char name[32];
  st40p_rx_handle handle;

  memset(&ops, 0, sizeof(ops));

  s->ctx = ctx;
  s->st = ctx->st;
  s->framebuff_cnt = 3;

  snprintf(name, sizeof(name), "app_rx_st40p_%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.port.num_port = st40p ? st40p->base.num_inf : ctx->para.num_ports;

  memcpy(ops.port.ip_addr[MTL_SESSION_PORT_P],
         st40p ? st_json_ip(ctx, &st40p->base, MTL_SESSION_PORT_P)
               : ctx->rx_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  memcpy(
      ops.port.mcast_sip_addr[MTL_SESSION_PORT_P],
      st40p ? st40p->base.mcast_src_ip[MTL_PORT_P] : ctx->rx_mcast_sip_addr[MTL_PORT_P],
      MTL_IP_ADDR_LEN);
  snprintf(
      ops.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
      st40p ? st40p->base.inf[MTL_SESSION_PORT_P]->name : ctx->para.port[MTL_PORT_P]);
  ops.port.udp_port[MTL_SESSION_PORT_P] =
      st40p ? st40p->base.udp_port : (10200 + idx * 2);

  if (ops.port.num_port > 1) {
    memcpy(ops.port.ip_addr[MTL_SESSION_PORT_R],
           st40p ? st_json_ip(ctx, &st40p->base, MTL_SESSION_PORT_R)
                 : ctx->rx_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    memcpy(
        ops.port.mcast_sip_addr[MTL_SESSION_PORT_R],
        st40p ? st40p->base.mcast_src_ip[MTL_PORT_R] : ctx->rx_mcast_sip_addr[MTL_PORT_R],
        MTL_IP_ADDR_LEN);
    snprintf(
        ops.port.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
        st40p ? st40p->base.inf[MTL_SESSION_PORT_R]->name : ctx->para.port[MTL_PORT_R]);
    ops.port.udp_port[MTL_SESSION_PORT_R] =
        st40p ? st40p->base.udp_port : (10200 + idx * 2);
  }

  ops.port.payload_type =
      st40p ? st40p->base.payload_type : ST_APP_PAYLOAD_TYPE_ANCILLARY;
  ops.interlaced = st40p ? st40p->info.interlaced : false;
  s->expect_fps = st_frame_rate(st40p ? st40p->info.fps : ST_FPS_P59_94);
  ops.framebuff_cnt = s->framebuff_cnt;
  ops.max_udw_buff_size = ST40P_APP_RX_MAX_UDW_SIZE;
  ops.rtp_ring_size = ST40P_APP_RX_RTP_RING_SIZE;
  ops.notify_frame_available = app_rx_st40p_frame_available;
  ops.flags = ST40P_RX_FLAG_BLOCK_GET;
  if (st40p && st40p->enable_rtcp) ops.flags |= ST40P_RX_FLAG_ENABLE_RTCP;

  st_pthread_mutex_init(&s->st40p_wake_mutex, NULL);
  st_pthread_cond_init(&s->st40p_wake_cond, NULL);

  handle = st40p_rx_create(ctx->st, &ops);
  if (!handle) {
    err("%s(%d), st40p_rx_create fail\n", __func__, idx);
    app_rx_st40p_uinit(s);
    return -EIO;
  }
  s->handle = handle;

  s->st40p_app_thread_stop = false;
  ret = pthread_create(&s->st40p_app_thread, NULL, app_rx_st40p_frame_thread, s);
  if (ret < 0) {
    err("%s(%d), thread create fail %d\n", __func__, idx, ret);
    app_rx_st40p_uinit(s);
    return -EIO;
  }

  char thread_name[32];
  snprintf(thread_name, sizeof(thread_name), "rx_st40p_%d", idx);
  mtl_thread_setname(s->st40p_app_thread, thread_name);

  return 0;
}

static int app_rx_st40p_result(struct st_app_rx_st40p_session* s) {
  int idx = s->idx;
  uint64_t cur_time_ns = st_app_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->stat_frame_first_rx_time) / NS_PER_S;
  double framerate = time_sec > 0 ? s->stat_frame_total_received / time_sec : 0;

  if (!s->stat_frame_total_received) return -EINVAL;

  notce(
      "%s(%d), %s, fps %f, %d frames received, seq_discont %d, marker_missing %d, "
      "seq_lost %u\n",
      __func__, idx,
      ST_APP_EXPECT_NEAR(framerate, s->expect_fps, s->expect_fps * 0.05) ? "OK"
                                                                         : "FAILED",
      framerate, s->stat_frame_total_received, s->stat_frame_seq_discont,
      s->stat_frame_marker_missing, s->stat_seq_lost_total);
  return 0;
}

int st_app_rx_st40p_sessions_init(struct st_app_context* ctx) {
  int ret, i;
  struct st_app_rx_st40p_session* s;

  if (!ctx->rx_st40p_session_cnt) return 0;

  ctx->rx_st40p_sessions = (struct st_app_rx_st40p_session*)st_app_zmalloc(
      sizeof(struct st_app_rx_st40p_session) * ctx->rx_st40p_session_cnt);
  if (!ctx->rx_st40p_sessions) return -ENOMEM;

  for (i = 0; i < ctx->rx_st40p_session_cnt; i++) {
    s = &ctx->rx_st40p_sessions[i];
    s->idx = i;

    ret = app_rx_st40p_init(
        ctx, ctx->json_ctx ? &ctx->json_ctx->rx_st40p_sessions[i] : NULL, s);
    if (ret < 0) {
      err("%s(%d), app_rx_st40p_init fail %d\n", __func__, i, ret);
      return ret;
    }
  }

  return 0;
}

int st_app_rx_st40p_sessions_uinit(struct st_app_context* ctx) {
  int i;
  struct st_app_rx_st40p_session* s;
  if (!ctx->rx_st40p_sessions) return 0;

  for (i = 0; i < ctx->rx_st40p_session_cnt; i++) {
    s = &ctx->rx_st40p_sessions[i];
    app_rx_st40p_uinit(s);
  }
  st_app_free(ctx->rx_st40p_sessions);
  ctx->rx_st40p_sessions = NULL;
  return 0;
}

int st_app_rx_st40p_sessions_result(struct st_app_context* ctx) {
  int i, ret = 0;
  struct st_app_rx_st40p_session* s;
  if (!ctx->rx_st40p_sessions) return 0;

  for (i = 0; i < ctx->rx_st40p_session_cnt; i++) {
    s = &ctx->rx_st40p_sessions[i];
    ret += app_rx_st40p_result(s);
  }
  return ret;
}
