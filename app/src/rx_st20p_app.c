/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "rx_st20p_app.h"

static void app_rx_st20p_consume_frame(struct st_app_rx_st20p_session* s,
                                       struct st_frame* frame) {
  struct st_display* d = s->display;
  int idx = s->idx;

  if (s->num_port > 1) {
    dbg("%s(%d): pkts_total %u, pkts per port P %u R %u\n", __func__, idx,
        frame->pkts_total, frame->pkts_recv[MTL_SESSION_PORT_P],
        frame->pkts_recv[MTL_SESSION_PORT_R]);
    if (frame->pkts_recv[MTL_SESSION_PORT_P] < (frame->pkts_total / 2))
      warn("%s(%d): P port only receive %u pkts while total pkts is %u\n", __func__, idx,
           frame->pkts_recv[MTL_SESSION_PORT_P], frame->pkts_total);
    if (frame->pkts_recv[MTL_SESSION_PORT_R] < (frame->pkts_total / 2))
      warn("%s(%d): R port only receive %u pkts while total pkts is %u\n", __func__, idx,
           frame->pkts_recv[MTL_SESSION_PORT_R], frame->pkts_total);
  }

  if (frame->interlaced) {
    dbg("%s(%d), %s field\n", __func__, s->idx, frame->second_field ? "second" : "first");
  }

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

static void* app_rx_st20p_frame_thread(void* arg) {
  struct st_app_rx_st20p_session* s = arg;
  struct st_frame* frame;
  uint8_t shas[SHA256_DIGEST_LENGTH];
  int idx = s->idx;

  info("%s(%d), start\n", __func__, s->idx);
  while (!s->st20p_app_thread_stop) {
    frame = st20p_rx_get_frame(s->handle);
    if (!frame) { /* no ready frame */
      warn("%s(%d), get frame time out\n", __func__, s->idx);
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
      dbg("%s, latency_us %" PRIu64 "\n", __func__, latency_ns / 1000);
      s->stat_latency_us_sum += latency_ns / 1000;
    }

    app_rx_st20p_consume_frame(s, frame);
    if (s->sha_check) {
      if (frame->user_meta_size != sizeof(shas)) {
        err("%s(%d), invalid user meta size %" PRId64 "\n", __func__, idx,
            frame->user_meta_size);
      } else {
        st_sha256((unsigned char*)frame->addr[0], st_frame_plane_size(frame, 0), shas);
        if (memcmp(shas, frame->user_meta, sizeof(shas))) {
          err("%s(%d), sha check fail for frame %p\n", __func__, idx, frame->addr);
          st_sha_dump("user meta sha:", frame->user_meta);
          st_sha_dump("frame sha:", shas);
        }
      }
    }
    s->stat_frame_total_received++;
    if (!s->stat_frame_first_rx_time)
      s->stat_frame_first_rx_time = st_app_get_monotonic_time();
    st20p_rx_put_frame(s->handle, frame);
  }
  info("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

static int app_rx_st20p_init_frame_thread(struct st_app_rx_st20p_session* s) {
  int ret, idx = s->idx;

  ret = pthread_create(&s->st20p_app_thread, NULL, app_rx_st20p_frame_thread, s);
  if (ret < 0) {
    err("%s(%d), st20p_app_thread create fail %d\n", __func__, ret, idx);
    return -EIO;
  }

  char thread_name[32];
  snprintf(thread_name, sizeof(thread_name), "rx_st20p_%d", idx);
  mtl_thread_setname(s->st20p_app_thread, thread_name);

  return 0;
}

static int app_rx_st20p_uinit(struct st_app_rx_st20p_session* s) {
  int ret, idx = s->idx;

  st_app_uinit_display(s->display);
  if (s->display) {
    st_app_free(s->display);
    s->display = NULL;
  }

  s->st20p_app_thread_stop = true;
  if (s->st20p_app_thread_stop) {
    /* wake up the thread */
    info("%s(%d), wait app thread stop\n", __func__, idx);
    if (s->handle) st20p_rx_wake_block(s->handle);
    pthread_join(s->st20p_app_thread, NULL);
  }

  if (s->handle) {
    ret = st20p_rx_free(s->handle);
    if (ret < 0) err("%s(%d), st20_rx_free fail %d\n", __func__, idx, ret);
    s->handle = NULL;
  }

  return 0;
}

static int app_rx_st20p_io_stat(struct st_app_rx_st20p_session* s) {
  int idx = s->idx;
  uint64_t cur_time = st_app_get_monotonic_time();
  double time_sec = (double)(cur_time - s->last_stat_time_ns) / NS_PER_S;
  double tx_rate_m, fps;
  int ret;
  struct st20_rx_port_status stats;

  if (!s->handle) return 0;

  for (uint8_t port = 0; port < s->num_port; port++) {
    ret = st20p_rx_get_port_stats(s->handle, port, &stats);
    if (ret < 0) return ret;
    tx_rate_m = (double)stats.bytes * 8 / time_sec / MTL_STAT_M_UNIT;
    fps = (double)stats.frames / time_sec;

    info("%s(%d,%u), rx %f Mb/s fps %f\n", __func__, idx, port, tx_rate_m, fps);
    st20p_rx_reset_port_stats(s->handle, port);
  }

  s->last_stat_time_ns = cur_time;
  return 0;
}

static int app_rx_st20p_init(struct st_app_context* ctx,
                             struct st_json_st20p_session* st20p,
                             struct st_app_rx_st20p_session* s) {
  int idx = s->idx, ret;
  struct st20p_rx_ops ops;
  char name[32];
  st20p_rx_handle handle;
  memset(&ops, 0, sizeof(ops));

  s->last_stat_time_ns = st_app_get_monotonic_time();
  s->sha_check = ctx->video_sha_check;

  snprintf(name, 32, "app_rx_st20p_%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.port.num_port = st20p ? st20p->base.num_inf : ctx->para.num_ports;
  memcpy(ops.port.sip_addr[MTL_SESSION_PORT_P],
         st20p ? st_json_ip(ctx, &st20p->base, MTL_SESSION_PORT_P)
               : ctx->rx_sip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  memcpy(
      ops.port.mcast_sip_addr[MTL_SESSION_PORT_P],
      st20p ? st20p->base.mcast_src_ip[MTL_PORT_P] : ctx->rx_mcast_sip_addr[MTL_PORT_P],
      MTL_IP_ADDR_LEN);
  snprintf(
      ops.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
      st20p ? st20p->base.inf[MTL_SESSION_PORT_P]->name : ctx->para.port[MTL_PORT_P]);
  ops.port.udp_port[MTL_SESSION_PORT_P] = st20p ? st20p->base.udp_port : (10000 + s->idx);
  if (ops.port.num_port > 1) {
    memcpy(ops.port.sip_addr[MTL_SESSION_PORT_R],
           st20p ? st_json_ip(ctx, &st20p->base, MTL_SESSION_PORT_R)
                 : ctx->rx_sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    memcpy(
        ops.port.mcast_sip_addr[MTL_SESSION_PORT_R],
        st20p ? st20p->base.mcast_src_ip[MTL_PORT_R] : ctx->rx_mcast_sip_addr[MTL_PORT_R],
        MTL_IP_ADDR_LEN);
    snprintf(
        ops.port.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
        st20p ? st20p->base.inf[MTL_SESSION_PORT_R]->name : ctx->para.port[MTL_PORT_R]);
    ops.port.udp_port[MTL_SESSION_PORT_R] =
        st20p ? st20p->base.udp_port : (10000 + s->idx);
  }

  ops.width = st20p ? st20p->info.width : 1920;
  ops.height = st20p ? st20p->info.height : 1080;
  ops.fps = st20p ? st20p->info.fps : ST_FPS_P59_94;
  ops.interlaced = st20p ? st20p->info.interlaced : false;
  ops.output_fmt = st20p ? st20p->info.format : ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  ops.transport_fmt = st20p ? st20p->info.transport_format : ST20_FMT_YUV_422_10BIT;
  ops.port.payload_type = st20p ? st20p->base.payload_type : ST_APP_PAYLOAD_TYPE_VIDEO;
  ops.device = st20p ? st20p->info.device : ST_PLUGIN_DEVICE_AUTO;
  ops.flags |= ST20P_RX_FLAG_BLOCK_GET;
  ops.framebuff_cnt = s->framebuff_cnt;
  /* always try to enable DMA offload */
  ops.flags |= ST20P_RX_FLAG_DMA_OFFLOAD;
  if (st20p && st20p->enable_rtcp) ops.flags |= ST20P_RX_FLAG_ENABLE_RTCP;
  if (ctx->enable_timing_parser) ops.flags |= ST20P_RX_FLAG_TIMING_PARSER_STAT;

  s->width = ops.width;
  s->height = ops.height;
  if (ops.interlaced) {
    s->height >>= 1;
  }
  s->num_port = ops.port.num_port;

  s->pcapng_max_pkts = ctx->pcapng_max_pkts;
  s->expect_fps = st_frame_rate(ops.fps);

  if ((st20p && st20p->display) || ctx->rx_display) {
    struct st_display* d = st_app_zmalloc(sizeof(struct st_display));
    ret = st_app_init_display(d, name, s->width, s->height, ctx->ttf_file);
    if (ret < 0) {
      err("%s(%d), st_app_init_display fail %d\n", __func__, idx, ret);
      app_rx_st20p_uinit(s);
      return -EIO;
    }
    s->display = d;
  }

  s->measure_latency = st20p ? st20p->measure_latency : true;

  handle = st20p_rx_create(ctx->st, &ops);
  if (!handle) {
    err("%s(%d), st20_rx_create fail\n", __func__, idx);
    app_rx_st20p_uinit(s);
    return -EIO;
  }
  s->handle = handle;

  s->st20p_frame_size = st20p_rx_frame_size(handle);

  ret = app_rx_st20p_init_frame_thread(s);
  if (ret < 0) {
    err("%s(%d), app_rx_st20p_init_thread fail %d\n", __func__, idx, ret);
    app_rx_st20p_uinit(s);
    return -EIO;
  }

  s->stat_frame_received = 0;
  s->stat_last_time = st_app_get_monotonic_time();

  return 0;
}

static int app_rx_st20p_stat(struct st_app_rx_st20p_session* s) {
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

static int app_rx_st20p_result(struct st_app_rx_st20p_session* s) {
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

static int app_rx_st20p_pcap(struct st_app_rx_st20p_session* s) {
  if (s->pcapng_max_pkts)
    st20p_rx_pcapng_dump(s->handle, s->pcapng_max_pkts, false, NULL);
  return 0;
}

int st_app_rx_st20p_sessions_init(struct st_app_context* ctx) {
  int ret = 0, i = 0;
  struct st_app_rx_st20p_session* s;

  dbg("%s(%d), rx_st20p_session_cnt %d\n", __func__, i, ctx->rx_st20p_session_cnt);
  ctx->rx_st20p_sessions = (struct st_app_rx_st20p_session*)st_app_zmalloc(
      sizeof(struct st_app_rx_st20p_session) * ctx->rx_st20p_session_cnt);
  if (!ctx->rx_st20p_sessions) return -ENOMEM;
  for (i = 0; i < ctx->rx_st20p_session_cnt; i++) {
    s = &ctx->rx_st20p_sessions[i];
    s->idx = i;
    s->st = ctx->st;
    s->framebuff_cnt = 3;

    ret = app_rx_st20p_init(
        ctx, ctx->json_ctx ? &ctx->json_ctx->rx_st20p_sessions[i] : NULL, s);
    if (ret < 0) {
      err("%s(%d), app_rx_st20p_init fail %d\n", __func__, i, ret);
      return ret;
    }
  }

  return 0;
}

int st_app_rx_st20p_sessions_uinit(struct st_app_context* ctx) {
  int i;
  struct st_app_rx_st20p_session* s;
  if (!ctx->rx_st20p_sessions) return 0;
  for (i = 0; i < ctx->rx_st20p_session_cnt; i++) {
    s = &ctx->rx_st20p_sessions[i];
    app_rx_st20p_uinit(s);
  }
  st_app_free(ctx->rx_st20p_sessions);

  return 0;
}

int st_app_rx_st20p_sessions_stat(struct st_app_context* ctx) {
  int i;
  struct st_app_rx_st20p_session* s;
  if (!ctx->rx_st20p_sessions) return 0;

  for (i = 0; i < ctx->rx_st20p_session_cnt; i++) {
    s = &ctx->rx_st20p_sessions[i];
    app_rx_st20p_stat(s);
  }

  return 0;
}

int st_app_rx_st20p_sessions_result(struct st_app_context* ctx) {
  int i, ret = 0;
  struct st_app_rx_st20p_session* s;

  if (!ctx->rx_st20p_sessions) return 0;

  for (i = 0; i < ctx->rx_st20p_session_cnt; i++) {
    s = &ctx->rx_st20p_sessions[i];
    ret += app_rx_st20p_result(s);
  }

  return ret;
}

int st_app_rx_st20p_sessions_pcap(struct st_app_context* ctx) {
  int i;
  struct st_app_rx_st20p_session* s;

  if (!ctx->rx_st20p_sessions) return 0;

  for (i = 0; i < ctx->rx_st20p_session_cnt; i++) {
    s = &ctx->rx_st20p_sessions[i];
    app_rx_st20p_pcap(s);
  }

  return 0;
}

int st_app_rx_st20p_io_stat(struct st_app_context* ctx) {
  int i, ret = 0;
  struct st_app_rx_st20p_session* s;
  if (!ctx->rx_st20p_sessions) return 0;

  for (i = 0; i < ctx->rx_st20p_session_cnt; i++) {
    s = &ctx->rx_st20p_sessions[i];
    ret += app_rx_st20p_io_stat(s);
  }

  return ret;
}
