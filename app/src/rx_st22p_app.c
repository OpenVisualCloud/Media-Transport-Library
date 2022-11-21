/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "rx_st22p_app.h"

static int app_rx_st22p_frame_available(void* priv) {
  struct st_app_rx_st22p_session* s = priv;

  st_pthread_mutex_lock(&s->st22p_wake_mutex);
  st_pthread_cond_signal(&s->st22p_wake_cond);
  st_pthread_mutex_unlock(&s->st22p_wake_mutex);

  return 0;
}

static void app_rx_st22p_consume_frame(struct st_app_rx_st22p_session* s,
                                       struct st_frame* frame) {
  struct st_display* d = s->display;

  if (d && d->front_frame) {
    if (st_pthread_mutex_trylock(&d->display_frame_mutex) == 0) {
      if (frame->fmt == ST_FRAME_FMT_YUV422PACKED8)
        mtl_memcpy(d->front_frame, frame->addr[0], d->front_frame_size);
      else if (frame->fmt == ST_FRAME_FMT_YUV422RFC4175PG2BE10)
        st20_rfc4175_422be10_to_422le8(frame->addr[0], d->front_frame, s->width,
                                       s->height);
      else {
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

static void* app_rx_st22p_frame_thread(void* arg) {
  struct st_app_rx_st22p_session* s = arg;
  struct st_frame* frame;

  info("%s(%d), start\n", __func__, s->idx);
  while (!s->st22p_app_thread_stop) {
    frame = st22p_rx_get_frame(s->handle);
    if (!frame) { /* no frame */
      st_pthread_mutex_lock(&s->st22p_wake_mutex);
      if (!s->st22p_app_thread_stop)
        st_pthread_cond_wait(&s->st22p_wake_cond, &s->st22p_wake_mutex);
      st_pthread_mutex_unlock(&s->st22p_wake_mutex);
      continue;
    }

    s->stat_frame_received++;
    if (s->measure_latency) {
      uint64_t latency_ns;
      uint64_t ptp_ns = mtl_ptp_read_time(s->st);
      uint32_t sampling_rate = 90 * 1000;

      if (frame->tfmt == ST10_TIMESTAMP_FMT_MEDIA_CLK) {
        uint32_t latency_media_clk =
            st10_tai_to_media_clk(ptp_ns, sampling_rate) - frame->timestamp;
        latency_ns = st10_media_clk_to_ns(latency_media_clk, sampling_rate);
      } else {
        latency_ns = ptp_ns - frame->timestamp;
      }
      dbg("%s, latency_us %lu\n", __func__, latency_ns / 1000);
      s->stat_latency_us_sum += latency_ns / 1000;
    }

    app_rx_st22p_consume_frame(s, frame);
    s->stat_frame_total_received++;
    if (!s->stat_frame_frist_rx_time)
      s->stat_frame_frist_rx_time = st_app_get_monotonic_time();
    st22p_rx_put_frame(s->handle, frame);
  }
  info("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

static int app_rx_st22p_init_frame_thread(struct st_app_rx_st22p_session* s) {
  int ret, idx = s->idx;

  ret = pthread_create(&s->st22p_app_thread, NULL, app_rx_st22p_frame_thread, s);
  if (ret < 0) {
    err("%s(%d), st22p_app_thread create fail %d\n", __func__, ret, idx);
    return -EIO;
  }

  return 0;
}

static int app_rx_st22p_uinit(struct st_app_rx_st22p_session* s) {
  int ret, idx = s->idx;

  st_app_uinit_display(s->display);
  if (s->display) {
    st_app_free(s->display);
    s->display = NULL;
  }

  s->st22p_app_thread_stop = true;
  if (s->st22p_app_thread_stop) {
    /* wake up the thread */
    st_pthread_mutex_lock(&s->st22p_wake_mutex);
    st_pthread_cond_signal(&s->st22p_wake_cond);
    st_pthread_mutex_unlock(&s->st22p_wake_mutex);
    info("%s(%d), wait app thread stop\n", __func__, idx);
    pthread_join(s->st22p_app_thread, NULL);
  }

  st_pthread_mutex_destroy(&s->st22p_wake_mutex);
  st_pthread_cond_destroy(&s->st22p_wake_cond);

  if (s->handle) {
    ret = st22p_rx_free(s->handle);
    if (ret < 0) err("%s(%d), st20_rx_free fail %d\n", __func__, idx, ret);
    s->handle = NULL;
  }

  return 0;
}

static int app_rx_st22p_init(struct st_app_context* ctx,
                             struct st_json_st22p_session* st22p,
                             struct st_app_rx_st22p_session* s) {
  int idx = s->idx, ret;
  struct st22p_rx_ops ops;
  char name[32];
  st22p_rx_handle handle;
  memset(&ops, 0, sizeof(ops));

  snprintf(name, 32, "app_rx_st22p_%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.port.num_port = st22p ? st22p->base.num_inf : ctx->para.num_ports;
  memcpy(ops.port.sip_addr[MTL_PORT_P],
         st22p ? st22p->base.ip[MTL_PORT_P] : ctx->rx_sip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  strncpy(ops.port.port[MTL_PORT_P],
          st22p ? st22p->base.inf[MTL_PORT_P]->name : ctx->para.port[MTL_PORT_P],
          MTL_PORT_MAX_LEN);
  ops.port.udp_port[MTL_PORT_P] = st22p ? st22p->base.udp_port : (10000 + s->idx);
  if (ops.port.num_port > 1) {
    memcpy(ops.port.sip_addr[MTL_PORT_R],
           st22p ? st22p->base.ip[MTL_PORT_R] : ctx->rx_sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    strncpy(ops.port.port[MTL_PORT_R],
            st22p ? st22p->base.inf[MTL_PORT_R]->name : ctx->para.port[MTL_PORT_R],
            MTL_PORT_MAX_LEN);
    ops.port.udp_port[MTL_PORT_R] = st22p ? st22p->base.udp_port : (10000 + s->idx);
  }

  ops.width = st22p ? st22p->info.width : 1920;
  ops.height = st22p ? st22p->info.height : 1080;
  ops.fps = st22p ? st22p->info.fps : ST_FPS_P59_94;
  ops.output_fmt = st22p ? st22p->info.format : ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  ops.port.payload_type = st22p ? st22p->base.payload_type : ST_APP_PAYLOAD_TYPE_ST22;
  ops.pack_type = st22p ? st22p->info.pack_type : ST22_PACK_CODESTREAM;
  ops.codec = st22p ? st22p->info.codec : ST22_CODEC_JPEGXS;
  ops.device = st22p ? st22p->info.device : ST_PLUGIN_DEVICE_AUTO;
  ops.codec_thread_cnt = st22p ? st22p->info.codec_thread_count : 0;
  ops.max_codestream_size = 0;
  ops.notify_frame_available = app_rx_st22p_frame_available;
  ops.framebuff_cnt = s->framebuff_cnt;

  st_pthread_mutex_init(&s->st22p_wake_mutex, NULL);
  st_pthread_cond_init(&s->st22p_wake_cond, NULL);

  s->width = ops.width;
  s->height = ops.height;

  s->pcapng_max_pkts = ctx->pcapng_max_pkts;
  s->expect_fps = st_frame_rate(ops.fps);

  if (ctx->has_sdl && st22p && st22p->display) {
    struct st_display* d = st_app_zmalloc(sizeof(struct st_display));
    ret = st_app_init_display(d, name, s->width, s->height, ctx->ttf_file);
    if (ret < 0) {
      err("%s(%d), st_app_init_display fail %d\n", __func__, idx, ret);
      app_rx_st22p_uinit(s);
      return -EIO;
    }
    s->display = d;
  }

  s->measure_latency = st22p ? st22p->measure_latency : true;

  handle = st22p_rx_create(ctx->st, &ops);
  if (!handle) {
    err("%s(%d), st20_rx_create fail\n", __func__, idx);
    app_rx_st22p_uinit(s);
    return -EIO;
  }
  s->handle = handle;

  s->st22p_frame_size = st22p_rx_frame_size(handle);

  ret = app_rx_st22p_init_frame_thread(s);
  if (ret < 0) {
    err("%s(%d), app_rx_st22p_init_thread fail %d\n", __func__, idx, ret);
    app_rx_st22p_uinit(s);
    return -EIO;
  }

  s->stat_frame_received = 0;
  s->stat_last_time = st_app_get_monotonic_time();

  return 0;
}

static int app_rx_st22p_stat(struct st_app_rx_st22p_session* s) {
  uint64_t cur_time_ns = st_app_get_monotonic_time();
#ifdef DEBUG
  double time_sec = (double)(cur_time_ns - s->stat_last_time) / NS_PER_S;
  double framerate = s->stat_frame_received / time_sec;
  dbg("%s(%d), fps %f, %d frame received\n", __func__, s->idx, framerate,
      s->stat_frame_received);
#endif
  if (s->measure_latency && s->stat_frame_received) {
    double latency_ms = (double)s->stat_latency_us_sum / s->stat_frame_received / 1000;
    info("%s(%d), avrage latency %fms\n", __func__, s->idx, latency_ms);
    s->stat_latency_us_sum = 0;
  }
  s->stat_frame_received = 0;
  s->stat_last_time = cur_time_ns;

  return 0;
}

static int app_rx_st22p_result(struct st_app_rx_st22p_session* s) {
  int idx = s->idx;
  uint64_t cur_time_ns = st_app_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->stat_frame_frist_rx_time) / NS_PER_S;
  double framerate = s->stat_frame_total_received / time_sec;

  if (!s->stat_frame_total_received) return -EINVAL;

  critical("%s(%d), %s, fps %f, %d frame received\n", __func__, idx,
           ST_APP_EXPECT_NEAR(framerate, s->expect_fps, s->expect_fps * 0.05) ? "OK"
                                                                              : "FAILED",
           framerate, s->stat_frame_total_received);
  return 0;
}

static int app_rx_st22p_pcap(struct st_app_rx_st22p_session* s) {
  if (s->pcapng_max_pkts)
    st22p_rx_pcapng_dump(s->handle, s->pcapng_max_pkts, false, NULL);
  return 0;
}

int st_app_rx_st22p_sessions_init(struct st_app_context* ctx) {
  int ret = 0, i = 0;
  struct st_app_rx_st22p_session* s;

  dbg("%s(%d), rx_st22p_session_cnt %d\n", __func__, i, ctx->rx_st22p_session_cnt);
  ctx->rx_st22p_sessions = (struct st_app_rx_st22p_session*)st_app_zmalloc(
      sizeof(struct st_app_rx_st22p_session) * ctx->rx_st22p_session_cnt);
  if (!ctx->rx_st22p_sessions) return -ENOMEM;
  for (i = 0; i < ctx->rx_st22p_session_cnt; i++) {
    s = &ctx->rx_st22p_sessions[i];
    s->idx = i;
    s->st = ctx->st;
    s->framebuff_cnt = 3;

    ret = app_rx_st22p_init(
        ctx, ctx->json_ctx ? &ctx->json_ctx->rx_st22p_sessions[i] : NULL, s);
    if (ret < 0) {
      err("%s(%d), app_rx_st22p_init fail %d\n", __func__, i, ret);
      return ret;
    }
  }

  return 0;
}

int st_app_rx_st22p_sessions_uinit(struct st_app_context* ctx) {
  int i;
  struct st_app_rx_st22p_session* s;
  if (!ctx->rx_st22p_sessions) return 0;
  for (i = 0; i < ctx->rx_st22p_session_cnt; i++) {
    s = &ctx->rx_st22p_sessions[i];
    app_rx_st22p_uinit(s);
  }
  st_app_free(ctx->rx_st22p_sessions);

  return 0;
}

int st_app_rx_st22p_sessions_stat(struct st_app_context* ctx) {
  int i;
  struct st_app_rx_st22p_session* s;
  if (!ctx->rx_st22p_sessions) return 0;

  for (i = 0; i < ctx->rx_st22p_session_cnt; i++) {
    s = &ctx->rx_st22p_sessions[i];
    app_rx_st22p_stat(s);
  }

  return 0;
}

int st_app_rx_st22p_sessions_result(struct st_app_context* ctx) {
  int i, ret = 0;
  struct st_app_rx_st22p_session* s;

  if (!ctx->rx_st22p_sessions) return 0;

  for (i = 0; i < ctx->rx_st22p_session_cnt; i++) {
    s = &ctx->rx_st22p_sessions[i];
    ret += app_rx_st22p_result(s);
  }

  return ret;
}

int st_app_rx_st22p_sessions_pcap(struct st_app_context* ctx) {
  int i;
  struct st_app_rx_st22p_session* s;

  if (!ctx->rx_st22p_sessions) return 0;

  for (i = 0; i < ctx->rx_st22p_session_cnt; i++) {
    s = &ctx->rx_st22p_sessions[i];
    app_rx_st22p_pcap(s);
  }

  return 0;
}
