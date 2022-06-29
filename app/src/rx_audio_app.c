/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

#include "rx_audio_app.h"

static int app_rx_audio_close_source(struct st_app_rx_audio_session* session) {
  if (session->st30_ref_fd >= 0) {
    munmap(session->st30_ref_begin, session->st30_ref_end - session->st30_ref_begin);
    close(session->st30_ref_fd);
    session->st30_ref_fd = -1;
  }

  return 0;
}

static int app_rx_audio_open_source(struct st_app_rx_audio_session* session) {
  int fd, idx = session->idx;
  struct stat i;

  fd = st_open(session->st30_ref_url, O_RDONLY);
  if (fd < 0) {
    info("%s(%d), open %s fail\n", __func__, idx, session->st30_ref_url);
    return 0;
  }

  fstat(fd, &i);
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
        info("%s bad audio...rewinding...\n", __func__);
        rewind = true;
      }
      count++;
    }
    if (session->st30_ref_cursor == old_ref) {
      if (ret) {
        err("%s, bad audio reference file, stop referencing\n", __func__);
        app_rx_audio_close_source(session);
        return ret;
      } else {
        break;
      }
    }
  }
  if (rewind) {
    info("%s audio rewind %d\n", __func__, count);
  }

  return 0;
}

static int app_rx_audio_handle_rtp(struct st_app_rx_audio_session* s,
                                   struct st_rfc3550_rtp_hdr* hdr) {
  /* only compare each packet with reference now */
  uint8_t* payload = (uint8_t*)hdr + sizeof(*hdr);

  s->stat_frame_total_received++;
  if (!s->stat_frame_frist_rx_time)
    s->stat_frame_frist_rx_time = st_app_get_monotonic_time();

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

  return 0;
}

static int app_rx_audio_frame_done(void* priv, void* frame,
                                   struct st30_frame_meta* meta) {
  struct st_app_rx_audio_session* s = priv;

  if (!s->handle) return -EIO;

  s->stat_frame_total_received++;
  if (!s->stat_frame_frist_rx_time)
    s->stat_frame_frist_rx_time = st_app_get_monotonic_time();

  if (s->st30_ref_fd > 0) app_rx_audio_compare_with_ref(s, frame);

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

  return 0;
}

static int app_rx_audio_result(struct st_app_rx_audio_session* s) {
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

static int app_rx_audio_init(struct st_app_context* ctx,
                             st_json_rx_audio_session_t* audio,
                             struct st_app_rx_audio_session* s) {
  int idx = s->idx, ret;
  struct st30_rx_ops ops;
  char name[32];
  st30_rx_handle handle;
  memset(&ops, 0, sizeof(ops));

  snprintf(name, 32, "app_rx_audio%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.num_port = audio ? audio->num_inf : ctx->para.num_ports;
  memcpy(ops.sip_addr[ST_PORT_P],
         audio ? audio->ip[ST_PORT_P] : ctx->rx_sip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  strncpy(ops.port[ST_PORT_P],
          audio ? audio->inf[ST_PORT_P]->name : ctx->para.port[ST_PORT_P],
          ST_PORT_MAX_LEN);
  ops.udp_port[ST_PORT_P] = audio ? audio->udp_port : (10100 + s->idx);
  if (ops.num_port > 1) {
    memcpy(ops.sip_addr[ST_PORT_R],
           audio ? audio->ip[ST_PORT_R] : ctx->rx_sip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops.port[ST_PORT_R],
            audio ? audio->inf[ST_PORT_R]->name : ctx->para.port[ST_PORT_R],
            ST_PORT_MAX_LEN);
    ops.udp_port[ST_PORT_R] = audio ? audio->udp_port : (10100 + s->idx);
  }
  ops.notify_frame_ready = app_rx_audio_frame_done;
  ops.notify_rtp_ready = app_rx_audio_rtp_ready;
  ops.type = audio ? audio->type : ST30_TYPE_FRAME_LEVEL;
  ops.fmt = audio ? audio->audio_format : ST30_FMT_PCM16;
  ops.payload_type = audio ? audio->payload_type : ST_APP_PAYLOAD_TYPE_AUDIO;
  ops.channel = audio ? audio->audio_channel : 2;
  ops.sampling = audio ? audio->audio_sampling : ST30_SAMPLING_48K;
  ops.ptime = audio ? audio->audio_ptime : ST30_PTIME_1MS;
  ops.sample_size = st30_get_sample_size(ops.fmt);
  ops.sample_num = st30_get_sample_num(ops.ptime, ops.sampling);
  s->pkt_len = ops.sample_size * ops.sample_num * ops.channel;
  /* frame time to 1ms */
  s->st30_frame_size =
      ops.sample_size * st30_get_sample_num(ST30_PTIME_1MS, ops.sampling) * ops.channel;
  s->expect_fps = 1000.0;
  ops.framebuff_size = s->st30_frame_size;
  ops.framebuff_cnt = s->framebuff_cnt;
  ops.rtp_ring_size = ctx->rx_audio_rtp_ring_size ? ctx->rx_audio_rtp_ring_size : 16;

  st_pthread_mutex_init(&s->st30_wake_mutex, NULL);
  st_pthread_cond_init(&s->st30_wake_cond, NULL);

  strncpy(s->st30_ref_url, audio ? audio->audio_url : "null", sizeof(s->st30_ref_url));

  ret = app_rx_audio_open_source(s);
  if (ret < 0) {
    err("%s(%d), app_rx_audio_open_source fail %d\n", __func__, idx, ret);
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

    ret = app_rx_audio_init(ctx, ctx->json_ctx ? &ctx->json_ctx->rx_audio[i] : NULL, s);
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

  return 0;
}
