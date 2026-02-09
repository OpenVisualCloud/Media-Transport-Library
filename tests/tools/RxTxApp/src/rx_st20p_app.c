/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "rx_st20p_app.h"

static void app_rx_st20p_consume_frame(struct st_app_rx_st20p_session* s,
                                       mtl_buffer_t* buf) {
  struct st_display* d = s->display;
  int idx = s->idx;

  if (s->st20p_destination_file && !s->rx_file_size_limit_reached) {
    /* check if writing this frame would exceed the file size limit */
    uint64_t max_size = s->ctx->rx_max_file_size;
    if (max_size > 0 && (s->rx_file_bytes_written + s->st20p_frame_size) > max_size) {
      info("%s(%d), rx_max_file_size limit reached: %" PRIu64
           " bytes written, limit %" PRIu64 "\n",
           __func__, idx, s->rx_file_bytes_written, max_size);
      s->rx_file_size_limit_reached = true;
    } else {
      if (!fwrite(buf->data, 1, s->st20p_frame_size, s->st20p_destination_file)) {
        err("%s(%d), failed to write frame to file %s\n", __func__, idx,
            s->st20p_destination_url);
      } else {
        s->rx_file_bytes_written += s->st20p_frame_size;
      }
    }
  }

  if (s->num_port > 1) {
    dbg("%s(%d): pkts_total %u, pkts per port P %u R %u\n", __func__, idx,
        buf->video.pkts_total, buf->video.pkts_recv[MTL_SESSION_PORT_P],
        buf->video.pkts_recv[MTL_SESSION_PORT_R]);
    if (buf->video.pkts_recv[MTL_SESSION_PORT_P] < (buf->video.pkts_total / 2))
      warn("%s(%d): P port only receive %u pkts while total pkts is %u\n", __func__, idx,
           buf->video.pkts_recv[MTL_SESSION_PORT_P], buf->video.pkts_total);
    if (buf->video.pkts_recv[MTL_SESSION_PORT_R] < (buf->video.pkts_total / 2))
      warn("%s(%d): R port only receive %u pkts while total pkts is %u\n", __func__, idx,
           buf->video.pkts_recv[MTL_SESSION_PORT_R], buf->video.pkts_total);
  }

  if (buf->video.interlaced) {
    dbg("%s(%d), %s field\n", __func__, s->idx,
        buf->video.second_field ? "second" : "first");
  }

  if (d && d->front_frame) {
    if (st_pthread_mutex_trylock(&d->display_frame_mutex) == 0) {
      if (buf->video.fmt == ST_FRAME_FMT_YUV422RFC4175PG2BE10) {
        st20_rfc4175_422be10_to_422le8(buf->data, d->front_frame, s->width, s->height);
      } else if (buf->video.fmt == ST_FRAME_FMT_UYVY) {
        mtl_memcpy(d->front_frame, buf->data, d->front_frame_size);
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
  mtl_buffer_t* buf;
  uint8_t shas[SHA256_DIGEST_LENGTH];
  int idx = s->idx;

  info("%s(%d), start\n", __func__, s->idx);
  while (!s->st20p_app_thread_stop) {
    int ret = mtl_session_buffer_get(s->session, &buf, -1); /* blocking */
    if (ret < 0) { /* no ready buffer or stopped */
      warn("%s(%d), get buffer time out\n", __func__, s->idx);
      /* track consecutive timeouts for auto_stop */
      if (s->ctx && s->ctx->auto_stop && s->rx_started) {
        s->rx_timeout_cnt++;
        if (s->rx_timeout_cnt >= 3) { /* 3 consecutive timeouts */
          info("%s(%d), auto_stop: rx timeout after receiving started\n", __func__, idx);
          s->rx_timeout_after_start = true;
          break;
        }
      }
      continue;
    }

    /* reset timeout counter on successful frame receive */
    s->rx_timeout_cnt = 0;
    /* mark as started for auto_stop */
    if (!s->rx_started) {
      s->rx_started = true;
      info("%s(%d), rx started\n", __func__, idx);
    }

    s->stat_frame_received++;
    if (s->measure_latency) {
      uint64_t latency_ns;
      uint64_t ptp_ns = mtl_ptp_read_time(s->st);
      uint32_t sampling_rate = 90 * 1000;

      if (buf->tfmt == ST10_TIMESTAMP_FMT_MEDIA_CLK) {
        uint32_t latency_media_clk =
            st10_tai_to_media_clk(ptp_ns, sampling_rate) - buf->timestamp;
        latency_ns = st10_media_clk_to_ns(latency_media_clk, sampling_rate);
      } else {
        latency_ns = ptp_ns - buf->timestamp;
      }
      dbg("%s, latency_us %" PRIu64 "\n", __func__, latency_ns / 1000);
      s->stat_latency_us_sum += latency_ns / 1000;
    }

    app_rx_st20p_consume_frame(s, buf);
    if (s->sha_check) {
      if (buf->user_meta_size != sizeof(shas)) {
        err("%s(%d), invalid user meta size %" PRId64 "\n", __func__, idx,
            buf->user_meta_size);
      } else {
        st_sha256((unsigned char*)buf->data, buf->data_size, shas);
        if (memcmp(shas, buf->user_meta, sizeof(shas))) {
          err("%s(%d), sha check fail for frame %p\n", __func__, idx, buf->data);
          st_sha_dump("user meta sha:", buf->user_meta);
          st_sha_dump("frame sha:", shas);
        }
      }
    }
    s->stat_frame_total_received++;
    if (!s->stat_frame_first_rx_time)
      s->stat_frame_first_rx_time = st_app_get_monotonic_time();
    s->stat_frame_last_rx_time = st_app_get_monotonic_time();
    mtl_session_buffer_put(s->session, buf);
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
    if (s->session) mtl_session_stop(s->session);
    if (s->st20p_app_thread) pthread_join(s->st20p_app_thread, NULL);
  }

  if (s->session) {
    ret = mtl_session_destroy(s->session);
    if (ret < 0) err("%s(%d), mtl_session_destroy fail %d\n", __func__, idx, ret);
    s->session = NULL;
  }

  if (s->st20p_destination_file) {
    fclose(s->st20p_destination_file);
    s->st20p_destination_file = NULL;
  }

  return 0;
}

static int app_rx_st20p_io_stat(struct st_app_rx_st20p_session* s) {
  int idx = s->idx;
  uint64_t cur_time = st_app_get_monotonic_time();
  double time_sec = (double)(cur_time - s->last_stat_time_ns) / NS_PER_S;
  double tx_rate_m, fps;
  int ret;
  struct st20_rx_user_stats stats;

  if (!s->session) return 0;

  for (uint8_t port = 0; port < s->num_port; port++) {
    ret = mtl_session_io_stats_get(s->session, &stats, sizeof(stats));

    if (ret < 0) return ret;
    tx_rate_m = (double)stats.common.port[port].bytes * 8 / time_sec / MTL_STAT_M_UNIT;
    fps = (double)stats.common.port[port].frames / time_sec;

    info("%s(%d,%u), rx %f Mb/s fps %f\n", __func__, idx, port, tx_rate_m, fps);
  }
  mtl_session_io_stats_reset(s->session);

  s->last_stat_time_ns = cur_time;
  return 0;
}

static int app_rx_st20p_init(struct st_app_context* ctx,
                             struct st_json_st20p_session* st20p,
                             struct st_app_rx_st20p_session* s) {
  int idx = s->idx, ret;
  mtl_video_config_t config;
  char name[32];
  mtl_session_t* session = NULL;
  memset(&config, 0, sizeof(config));

  s->ctx = ctx;
  s->last_stat_time_ns = st_app_get_monotonic_time();
  s->sha_check = ctx->video_sha_check;

  snprintf(name, 32, "app_rx_st20p_%d", idx);
  config.base.name = name;
  config.base.priv = s;
  config.base.direction = MTL_SESSION_RX;
  config.rx_port.num_port = st20p ? st20p->base.num_inf : ctx->para.num_ports;
  memcpy(config.rx_port.ip_addr[MTL_SESSION_PORT_P],
         st20p ? st_json_ip(ctx, &st20p->base, MTL_SESSION_PORT_P)
               : ctx->rx_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  memcpy(
      config.rx_port.mcast_sip_addr[MTL_SESSION_PORT_P],
      st20p ? st20p->base.mcast_src_ip[MTL_PORT_P] : ctx->rx_mcast_sip_addr[MTL_PORT_P],
      MTL_IP_ADDR_LEN);
  snprintf(
      config.rx_port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
      st20p ? st20p->base.inf[MTL_SESSION_PORT_P]->name : ctx->para.port[MTL_PORT_P]);
  config.rx_port.udp_port[MTL_SESSION_PORT_P] =
      st20p ? st20p->base.udp_port : (10000 + s->idx);
  if (config.rx_port.num_port > 1) {
    memcpy(config.rx_port.ip_addr[MTL_SESSION_PORT_R],
           st20p ? st_json_ip(ctx, &st20p->base, MTL_SESSION_PORT_R)
                 : ctx->rx_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    memcpy(
        config.rx_port.mcast_sip_addr[MTL_SESSION_PORT_R],
        st20p ? st20p->base.mcast_src_ip[MTL_PORT_R] : ctx->rx_mcast_sip_addr[MTL_PORT_R],
        MTL_IP_ADDR_LEN);
    snprintf(
        config.rx_port.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
        st20p ? st20p->base.inf[MTL_SESSION_PORT_R]->name : ctx->para.port[MTL_PORT_R]);
    config.rx_port.udp_port[MTL_SESSION_PORT_R] =
        st20p ? st20p->base.udp_port : (10000 + s->idx);
  }

  if (st20p && st20p->info.st20p_url[0] != '\0') {
    memcpy(s->st20p_destination_url, st20p->info.st20p_url, ST_APP_URL_MAX_LEN);
    s->st20p_destination_file = fopen(s->st20p_destination_url, "wb");

    if (!s->st20p_destination_file) {
      err("%s(%d), failed to open destination file %s\n", __func__, idx,
          s->st20p_destination_url);
      app_rx_st20p_uinit(s);
      return -EIO;
    }
  }

  config.width = st20p ? st20p->info.width : 1920;
  config.height = st20p ? st20p->info.height : 1080;
  config.fps = st20p ? st20p->info.fps : ST_FPS_P59_94;
  config.interlaced = st20p ? st20p->info.interlaced : false;
  config.frame_fmt = st20p ? st20p->info.format : ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  config.transport_fmt = st20p ? st20p->info.transport_format : ST20_FMT_YUV_422_10BIT;
  config.rx_port.payload_type =
      st20p ? st20p->base.payload_type : ST_APP_PAYLOAD_TYPE_VIDEO;
  config.plugin_device = st20p ? st20p->info.device : ST_PLUGIN_DEVICE_AUTO;
  config.base.flags |= MTL_SESSION_FLAG_BLOCK_GET;
  config.rx_burst_size = ctx->rx_burst_size;
  config.base.num_buffers = s->framebuff_cnt;
  /* always try to enable DMA offload */
  config.base.flags |= MTL_SESSION_FLAG_DMA_OFFLOAD;
  if (st20p && st20p->enable_rtcp) config.base.flags |= MTL_SESSION_FLAG_ENABLE_RTCP;
  if (ctx->enable_timing_parser) config.enable_timing_parser = true;
  if (ctx->rx_video_multi_thread)
    config.base.flags |= MTL_SESSION_FLAG_USE_MULTI_THREADS;
  if (ctx->enable_hdr_split) config.base.flags |= MTL_SESSION_FLAG_HDR_SPLIT;
  if (ctx->force_rx_video_numa >= 0) {
    config.base.flags |= MTL_SESSION_FLAG_FORCE_NUMA;
    config.base.socket_id = ctx->force_rx_video_numa;
  }

  s->width = config.width;
  s->height = config.height;
  if (config.interlaced) {
    s->height >>= 1;
  }
  s->num_port = config.rx_port.num_port;

  s->pcapng_max_pkts = ctx->pcapng_max_pkts;
  s->expect_fps = st_frame_rate(config.fps);

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

  ret = mtl_video_session_create(ctx->st, &config, &session);
  if (ret < 0) {
    err("%s(%d), mtl_video_session_create fail %d\n", __func__, idx, ret);
    app_rx_st20p_uinit(s);
    return -EIO;
  }
  s->session = session;

  s->st20p_frame_size = mtl_session_get_frame_size(session);

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
  uint64_t end_time_ns;
  /* for auto_stop: use last frame time to avoid counting timeout period in fps */
  if (s->rx_timeout_after_start && s->stat_frame_last_rx_time)
    end_time_ns = s->stat_frame_last_rx_time;
  else
    end_time_ns = st_app_get_monotonic_time();
  double time_sec = (double)(end_time_ns - s->stat_frame_first_rx_time) / NS_PER_S;
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
    mtl_session_pcap_dump(s->session, s->pcapng_max_pkts, false, NULL);
  return 0;
}

int st_app_rx_st20p_sessions_init(struct st_app_context* ctx) {
  int ret = 0, i = 0;
  struct st_app_rx_st20p_session* s;
  int fb_cnt = ctx->rx_video_fb_cnt;
  if (fb_cnt <= 0) fb_cnt = ST_APP_DEFAULT_FB_CNT;

  dbg("%s(%d), rx_st20p_session_cnt %d\n", __func__, i, ctx->rx_st20p_session_cnt);
  ctx->rx_st20p_sessions = (struct st_app_rx_st20p_session*)st_app_zmalloc(
      sizeof(struct st_app_rx_st20p_session) * ctx->rx_st20p_session_cnt);
  if (!ctx->rx_st20p_sessions) return -ENOMEM;
  for (i = 0; i < ctx->rx_st20p_session_cnt; i++) {
    s = &ctx->rx_st20p_sessions[i];
    s->idx = i;
    s->st = ctx->st;
    s->framebuff_cnt = fb_cnt;

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

bool st_app_rx_st20p_sessions_all_timeout(struct st_app_context* ctx) {
  struct st_app_rx_st20p_session* s;
  if (!ctx->rx_st20p_sessions || ctx->rx_st20p_session_cnt == 0) return true;

  for (int i = 0; i < ctx->rx_st20p_session_cnt; i++) {
    s = &ctx->rx_st20p_sessions[i];
    if (!s->rx_timeout_after_start) return false;
  }
  return true;
}
