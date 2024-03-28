/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "rx_audio_app.h"

static int app_rx_audio_close_dump(struct st_app_rx_audio_session* s) {
  if (s->st30_dump_fd >= 0) {
    munmap(s->st30_dump_begin, s->st30_dump_end - s->st30_dump_begin);
    close(s->st30_dump_fd);
    s->st30_dump_fd = -1;
  }

  return 0;
}

static int app_rx_audio_open_dump(struct st_app_rx_audio_session* s) {
  int fd, ret, idx = s->idx;
  off_t f_size;

  /* user do not require dump to file */
  if (s->st30_dump_time_s < 1) return 0;

  fd = st_open_mode(s->st30_dump_url, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    err("%s(%d), open %s fail\n", __func__, idx, s->st30_dump_url);
    return -EIO;
  }

  f_size = (off_t)s->expect_fps * s->st30_frame_size * s->st30_dump_time_s;
  ret = ftruncate(fd, f_size);
  if (ret < 0) {
    err("%s(%d), ftruncate %s fail\n", __func__, idx, s->st30_dump_url);
    close(fd);
    return -EIO;
  }

  uint8_t* m = mmap(NULL, f_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (MAP_FAILED == m) {
    err("%s(%d), mmap %s fail\n", __func__, idx, s->st30_dump_url);
    close(fd);
    return -EIO;
  }

  s->st30_dump_begin = m;
  s->st30_dump_cursor = m;
  s->st30_dump_end = m + f_size;
  s->st30_dump_fd = fd;
  info("%s(%d), save %ds data to file %s(%p,%" PRIu64 ")\n", __func__, idx,
       s->st30_dump_time_s, s->st30_dump_url, m, f_size);

  return 0;
}

static int app_rx_audio_close_source(struct st_app_rx_audio_session* session) {
  if (session->st30_ref_fd >= 0) {
    munmap(session->st30_ref_begin, session->st30_ref_end - session->st30_ref_begin);
    close(session->st30_ref_fd);
    session->st30_ref_fd = -1;
  }

  return 0;
}

static int app_rx_audio_open_ref(struct st_app_rx_audio_session* session) {
  int fd, idx = session->idx;
  struct stat i;

  fd = st_open(session->st30_ref_url, O_RDONLY);
  if (fd < 0) {
    info("%s(%d), open %s fail\n", __func__, idx, session->st30_ref_url);
    return 0;
  }

  if (fstat(fd, &i) < 0) {
    err("%s, fstat %s fail\n", __func__, session->st30_ref_url);
    close(fd);
    return -EIO;
  }
  uint8_t* m = mmap(NULL, i.st_size, PROT_READ, MAP_SHARED, fd, 0);
  if (MAP_FAILED == m) {
    err("%s(%d), mmap %s fail\n", __func__, idx, session->st30_ref_url);
    close(fd);
    return -EIO;
  }

  session->st30_ref_begin = m;
  session->st30_ref_cursor = m;
  session->st30_ref_end = m + i.st_size;
  session->st30_ref_fd = fd;

  info("%s, succ\n", __func__);
  return 0;
}

static int app_rx_audio_compare_with_ref(struct st_app_rx_audio_session* session,
                                         void* frame) {
  int ret = -1;
  bool rewind = false;
  int count = 0;
  uint8_t* old_ref = session->st30_ref_cursor;

  while (ret) {
    ret = memcmp(frame, session->st30_ref_cursor, session->st30_frame_size);
    /* calculate new reference frame */
    session->st30_ref_cursor += session->st30_frame_size;
    if ((session->st30_ref_cursor >= session->st30_ref_end) ||
        ((session->st30_ref_end - session->st30_ref_cursor) < session->st30_frame_size)) {
      session->st30_ref_cursor = session->st30_ref_begin;
    }

    if (ret) {
      if (!rewind) {
        info("%s(%d), bad audio...rewinding...\n", __func__, session->idx);
        rewind = true;
      }
      count++;
    }
    if (session->st30_ref_cursor == old_ref) {
      if (ret) {
        info("%s(%d), bad audio, stop referencing for current frame\n", __func__,
             session->idx);
        session->st30_ref_err++;
        if (session->st30_ref_err > 100) {
          err("%s(%d), too many bad audio err, stop referencing\n", __func__,
              session->idx);
          app_rx_audio_close_source(session);
        }
        return ret;
      } else {
        break;
      }
    }
  }
  if (rewind) {
    info("%s(%d) audio rewind %d\n", __func__, session->idx, count);
  }

  return 0;
}

static int app_rx_audio_handle_rtp(struct st_app_rx_audio_session* s,
                                   struct st_rfc3550_rtp_hdr* hdr) {
  /* only compare each packet with reference now */
  uint8_t* payload = (uint8_t*)hdr + sizeof(*hdr);

  s->stat_frame_total_received++;
  if (!s->stat_frame_first_rx_time)
    s->stat_frame_first_rx_time = st_app_get_monotonic_time();

  if (s->st30_ref_fd > 0) app_rx_audio_compare_with_ref(s, payload);
  return 0;
}

static void* app_rx_audio_rtp_thread(void* arg) {
  struct st_app_rx_audio_session* s = arg;
  int idx = s->idx;
  void* usrptr;
  uint16_t len;
  void* mbuf;
  struct st_rfc3550_rtp_hdr* hdr;

  info("%s(%d), start\n", __func__, idx);
  while (!s->st30_app_thread_stop) {
    mbuf = st30_rx_get_mbuf(s->handle, &usrptr, &len);
    if (!mbuf) {
      /* no buffer */
      st_pthread_mutex_lock(&s->st30_wake_mutex);
      if (!s->st30_app_thread_stop)
        st_pthread_cond_wait(&s->st30_wake_cond, &s->st30_wake_mutex);
      st_pthread_mutex_unlock(&s->st30_wake_mutex);
      continue;
    }

    /* get one packet */
    hdr = (struct st_rfc3550_rtp_hdr*)usrptr;
    app_rx_audio_handle_rtp(s, hdr);
    /* free to lib */
    st30_rx_put_mbuf(s->handle, mbuf);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static int app_rx_audio_init_rtp_thread(struct st_app_rx_audio_session* s) {
  int ret, idx = s->idx;

  ret = pthread_create(&s->st30_app_thread, NULL, app_rx_audio_rtp_thread, s);
  if (ret < 0) {
    err("%s(%d), st30_app_thread create fail %d\n", __func__, ret, idx);
    return -EIO;
  }

  char thread_name[32];
  snprintf(thread_name, sizeof(thread_name), "rx_audio_%d", idx);
  mtl_thread_setname(s->st30_app_thread, thread_name);

  return 0;
}

static int app_rx_audio_frame_ready(void* priv, void* frame,
                                    struct st30_rx_frame_meta* meta) {
  struct st_app_rx_audio_session* s = priv;
  MTL_MAY_UNUSED(meta);

  if (!s->handle) return -EIO;

  s->stat_frame_total_received++;
  if (!s->stat_frame_first_rx_time)
    s->stat_frame_first_rx_time = st_app_get_monotonic_time();

  if (s->st30_ref_fd > 0) app_rx_audio_compare_with_ref(s, frame);

  if (s->st30_dump_fd > 0) {
    if (s->st30_dump_cursor + s->st30_frame_size > s->st30_dump_end)
      s->st30_dump_cursor = s->st30_dump_begin;
    dbg("%s(%d), dst %p src %p size %d\n", __func__, s->idx, s->st30_dump_cursor, frame,
        s->st30_frame_size);
    mtl_memcpy(s->st30_dump_cursor, frame, s->st30_frame_size);
    s->st30_dump_cursor += s->st30_frame_size;
  }

  dbg("%s(%d), frame %p\n", __func__, s->idx, frame);
  st30_rx_put_framebuff(s->handle, frame);
  return 0;
}

static int app_rx_audio_rtp_ready(void* priv) {
  struct st_app_rx_audio_session* s = priv;

  st_pthread_mutex_lock(&s->st30_wake_mutex);
  st_pthread_cond_signal(&s->st30_wake_cond);
  st_pthread_mutex_unlock(&s->st30_wake_mutex);

  return 0;
}

static int app_rx_audio_timing_parser_result(void* priv, enum mtl_session_port port,
                                             struct st30_rx_tp_meta* tp) {
  struct st_app_rx_audio_session* s = priv;

  s->stat_compliant_result[tp->compliant]++;
  s->ipt_max = ST_MAX(s->ipt_max, tp->ipt_max);
  if (tp->compliant != ST_RX_TP_COMPLIANT_NARROW) {
    warn("%s(%d,%d), compliant %d, failed cause %s, pkts_cnt %u\n", __func__, s->idx,
         port, tp->compliant, tp->failed_cause, tp->pkts_cnt);
    warn("%s(%d,%d), tsdf %dus, ipt(ns) min %d max %d avg %f\n", __func__, s->idx, port,
         tp->tsdf, tp->ipt_min, tp->ipt_max, tp->ipt_avg);
    dbg("%s(%d,%d), dpvr(us) min %d max %d avg %f\n", __func__, s->idx, port,
        tp->dpvr_min, tp->dpvr_max, tp->dpvr_avg);
  }
  return 0;
}

static int app_rx_audio_uinit(struct st_app_rx_audio_session* s) {
  int ret, idx = s->idx;

  s->st30_app_thread_stop = true;
  if (s->st30_app_thread) {
    /* wake up the thread */
    st_pthread_mutex_lock(&s->st30_wake_mutex);
    st_pthread_cond_signal(&s->st30_wake_cond);
    st_pthread_mutex_unlock(&s->st30_wake_mutex);
    info("%s(%d), wait app thread stop\n", __func__, idx);
    pthread_join(s->st30_app_thread, NULL);
  }

  st_pthread_mutex_destroy(&s->st30_wake_mutex);
  st_pthread_cond_destroy(&s->st30_wake_cond);

  if (s->handle) {
    ret = st30_rx_free(s->handle);
    if (ret < 0) err("%s(%d), st30_rx_free fail %d\n", __func__, idx, ret);
    s->handle = NULL;
  }
  app_rx_audio_close_source(s);
  app_rx_audio_close_dump(s);

  return 0;
}

static int app_rx_audio_result(struct st_app_rx_audio_session* s) {
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

static int app_rx_audio_stat(struct st_app_rx_audio_session* s) {
  s->stat_dump_cnt++;
  if (s->enable_timing_parser_meta) {
    if ((s->stat_dump_cnt % 6) == 0) {
      /* report every 1 min */
      warn("%s(%d), COMPLIANT NARROW %d WIDE %d FAILED %d, ipt max %fus\n", __func__,
           s->idx, s->stat_compliant_result[ST_RX_TP_COMPLIANT_NARROW],
           s->stat_compliant_result[ST_RX_TP_COMPLIANT_WIDE],
           s->stat_compliant_result[ST_RX_TP_COMPLIANT_FAILED], (float)s->ipt_max / 1000);
      memset(s->stat_compliant_result, 0, sizeof(s->stat_compliant_result));
      s->ipt_max = 0;
    }
  }
  return 0;
}

static int app_rx_audio_init(struct st_app_context* ctx, st_json_audio_session_t* audio,
                             struct st_app_rx_audio_session* s) {
  int idx = s->idx, ret;
  struct st30_rx_ops ops;
  char name[32];
  st30_rx_handle handle;
  memset(&ops, 0, sizeof(ops));

  snprintf(name, 32, "app_rx_audio%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.num_port = audio ? audio->base.num_inf : ctx->para.num_ports;
  memcpy(ops.ip_addr[MTL_SESSION_PORT_P],
         audio ? st_json_ip(ctx, &audio->base, MTL_SESSION_PORT_P)
               : ctx->rx_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  memcpy(
      ops.mcast_sip_addr[MTL_SESSION_PORT_P],
      audio ? audio->base.mcast_src_ip[MTL_PORT_P] : ctx->rx_mcast_sip_addr[MTL_PORT_P],
      MTL_IP_ADDR_LEN);
  snprintf(
      ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
      audio ? audio->base.inf[MTL_SESSION_PORT_P]->name : ctx->para.port[MTL_PORT_P]);
  ops.udp_port[MTL_SESSION_PORT_P] = audio ? audio->base.udp_port : (10100 + s->idx);
  if (ops.num_port > 1) {
    memcpy(ops.ip_addr[MTL_SESSION_PORT_R],
           audio ? st_json_ip(ctx, &audio->base, MTL_SESSION_PORT_R)
                 : ctx->rx_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    memcpy(
        ops.mcast_sip_addr[MTL_SESSION_PORT_R],
        audio ? audio->base.mcast_src_ip[MTL_PORT_R] : ctx->rx_mcast_sip_addr[MTL_PORT_R],
        MTL_IP_ADDR_LEN);
    snprintf(ops.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
             audio ? audio->base.inf[MTL_PORT_R]->name : ctx->para.port[MTL_PORT_R]);
    ops.udp_port[MTL_SESSION_PORT_R] = audio ? audio->base.udp_port : (10100 + s->idx);
  }
  ops.notify_frame_ready = app_rx_audio_frame_ready;
  ops.notify_rtp_ready = app_rx_audio_rtp_ready;
  ops.type = audio ? audio->info.type : ST30_TYPE_FRAME_LEVEL;
  ops.fmt = audio ? audio->info.audio_format : ST30_FMT_PCM16;
  ops.payload_type = audio ? audio->base.payload_type : ST_APP_PAYLOAD_TYPE_AUDIO;
  ops.channel = audio ? audio->info.audio_channel : 2;
  ops.sampling = audio ? audio->info.audio_sampling : ST30_SAMPLING_48K;
  ops.ptime = audio ? audio->info.audio_ptime : ST30_PTIME_1MS;
  s->pkt_len = st30_get_packet_size(ops.fmt, ops.ptime, ops.sampling, ops.channel);
  if (s->pkt_len < 0) {
    err("%s(%d), st30_get_packet_size fail\n", __func__, idx);
    app_rx_audio_uinit(s);
    return -EIO;
  }
  int pkt_per_frame = 1;

  double pkt_time = st30_get_packet_time(ops.ptime);
  /* when ptime <= 1ms, set frame time to 1ms */
  if (pkt_time < NS_PER_MS) {
    pkt_per_frame = NS_PER_MS / pkt_time;
  }

  s->st30_frame_size = pkt_per_frame * s->pkt_len;
  s->expect_fps = (double)NS_PER_S / st30_get_packet_time(ops.ptime) / pkt_per_frame;

  ops.framebuff_size = s->st30_frame_size;
  ops.framebuff_cnt = s->framebuff_cnt;
  ops.rtp_ring_size = ctx->rx_audio_rtp_ring_size ? ctx->rx_audio_rtp_ring_size : 16;
  if (audio && audio->enable_rtcp) ops.flags |= ST30_RX_FLAG_ENABLE_RTCP;
  if (ctx->enable_timing_parser) ops.flags |= ST30_RX_FLAG_TIMING_PARSER_STAT;
  if (ctx->enable_timing_parser_meta) {
    ops.notify_timing_parser_result = app_rx_audio_timing_parser_result;
    ops.flags |= ST30_RX_FLAG_TIMING_PARSER_META;
    s->enable_timing_parser_meta = true;
  }

  st_pthread_mutex_init(&s->st30_wake_mutex, NULL);
  st_pthread_cond_init(&s->st30_wake_cond, NULL);

  snprintf(s->st30_ref_url, sizeof(s->st30_ref_url), "%s",
           audio ? audio->info.audio_url : "null");

  ret = app_rx_audio_open_ref(s);
  if (ret < 0) {
    err("%s(%d), app_rx_audio_open_ref fail %d\n", __func__, idx, ret);
    app_rx_audio_uinit(s);
    return -EIO;
  }

  /* dump */
  snprintf(s->st30_dump_url, ST_APP_URL_MAX_LEN, "st_audio_app%d_%d_%d_%u.pcm", idx,
           st30_get_sample_rate(ops.sampling), st30_get_sample_size(ops.fmt) * 8,
           ops.channel);
  ret = app_rx_audio_open_dump(s);
  if (ret < 0) {
    err("%s(%d), app_rx_audio_open_dump fail %d\n", __func__, idx, ret);
    app_rx_audio_uinit(s);
    return -EIO;
  }

  handle = st30_rx_create(ctx->st, &ops);
  if (!handle) {
    err("%s(%d), st30_rx_create fail\n", __func__, idx);
    return -EIO;
  }
  s->handle = handle;

  if (ops.type == ST30_TYPE_RTP_LEVEL) {
    ret = app_rx_audio_init_rtp_thread(s);
  }
  if (ret < 0) {
    err("%s(%d), app_rx_audio_init_thread fail %d, type %d\n", __func__, idx, ret,
        ops.type);
    app_rx_audio_uinit(s);
    return -EIO;
  }

  return 0;
}

int st_app_rx_audio_sessions_init(struct st_app_context* ctx) {
  int ret, i;
  struct st_app_rx_audio_session* s;
  ctx->rx_audio_sessions = (struct st_app_rx_audio_session*)st_app_zmalloc(
      sizeof(struct st_app_rx_audio_session) * ctx->rx_audio_session_cnt);
  if (!ctx->rx_audio_sessions) return -ENOMEM;
  for (i = 0; i < ctx->rx_audio_session_cnt; i++) {
    s = &ctx->rx_audio_sessions[i];
    s->idx = i;
    s->framebuff_cnt = 2;
    s->st30_ref_fd = -1;
    s->st30_dump_time_s = ctx->rx_audio_dump_time_s;
    s->st30_dump_fd = -1;

    ret = app_rx_audio_init(
        ctx, ctx->json_ctx ? &ctx->json_ctx->rx_audio_sessions[i] : NULL, s);
    if (ret < 0) {
      err("%s(%d), app_rx_audio_init fail %d\n", __func__, i, ret);
      return ret;
    }
  }

  return 0;
}

int st_app_rx_audio_sessions_uinit(struct st_app_context* ctx) {
  int i;
  struct st_app_rx_audio_session* s;
  if (!ctx->rx_audio_sessions) return 0;

  for (i = 0; i < ctx->rx_audio_session_cnt; i++) {
    s = &ctx->rx_audio_sessions[i];
    app_rx_audio_uinit(s);
  }
  st_app_free(ctx->rx_audio_sessions);

  return 0;
}

int st_app_rx_audio_sessions_result(struct st_app_context* ctx) {
  int i, ret = 0;
  struct st_app_rx_audio_session* s;

  if (!ctx->rx_audio_sessions) return 0;
  for (i = 0; i < ctx->rx_audio_session_cnt; i++) {
    s = &ctx->rx_audio_sessions[i];
    ret += app_rx_audio_result(s);
  }

  return ret;
}

int st_app_rx_audio_sessions_stat(struct st_app_context* ctx) {
  int i, ret = 0;
  struct st_app_rx_audio_session* s;

  if (!ctx->rx_audio_sessions) return 0;
  for (i = 0; i < ctx->rx_audio_session_cnt; i++) {
    s = &ctx->rx_audio_sessions[i];
    ret += app_rx_audio_stat(s);
  }

  return ret;
}
