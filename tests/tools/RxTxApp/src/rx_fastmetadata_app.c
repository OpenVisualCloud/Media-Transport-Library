/* SPDX-License-Identifier: BSD-3-Clause
 * SPDX-FileCopyrightText: Copyright (c) 2024 Intel Corporation
 */

#include "rx_fastmetadata_app.h"

static void app_rx_fmd_close_source(struct st_app_rx_fmd_session* session) {
  if (session->st41_ref_fd >= 0) {
    munmap(session->st41_ref_begin, session->st41_ref_end - session->st41_ref_begin);
    close(session->st41_ref_fd);
    session->st41_ref_fd = -1;
  }
}

static int app_rx_fmd_open_ref(struct st_app_rx_fmd_session* session) {
  int fd, idx = session->idx;
  struct stat i;
  session->st41_ref_fd = -1;

  fd = st_open(session->st41_ref_url, O_RDONLY);
  if (fd < 0) {
    info("%s(%d), open file '%s' fail.\n", __func__, idx, session->st41_ref_url);
    return -EIO;
  }

  if (fstat(fd, &i) < 0) {
    err("%s, fstat '%s' fail.\n", __func__, session->st41_ref_url);
    close(fd);
    return -EIO;
  }

  uint8_t* m = mmap(NULL, i.st_size, PROT_READ, MAP_SHARED, fd, 0);
  if (MAP_FAILED == m) {
    err("%s(%d), mmap '%s' fail.\n", __func__, idx, session->st41_ref_url);
    close(fd);
    return -EIO;
  }

  session->st41_ref_begin = m;
  session->st41_ref_cursor = m;
  session->st41_ref_end = m + i.st_size;
  session->st41_ref_fd = fd;

  info("%s, opening file '%s' success.\n", __func__, session->st41_ref_url);
  return 0;
}

static int app_rx_fmd_compare_with_ref(struct st_app_rx_fmd_session* session, void* frame,
                                       int frame_size) {
  int ret = -1;
  uint32_t last_zeros = 0; /* 4 bytes with 0 */
  uint32_t st41_ref_remaining_length = session->st41_ref_end - session->st41_ref_cursor;

  if (frame_size <= st41_ref_remaining_length) {
    ret = memcmp(frame, session->st41_ref_cursor, frame_size);
    if (ret) {
      session->errors_count++;
      err("%s() FAIL: reference file comparison with frame.\n", __func__);
    } else {
      dbg("%s() PASS: reference file comparison with frame.\n", __func__);
    }
  } else {
    if (frame_size - st41_ref_remaining_length > 3) {
      ret = -1;
      session->errors_count++;
      err("%s() FAIL: frame_size > ref_remaining_length by %d.\n", __func__,
          frame_size - st41_ref_remaining_length);
    } else {
      ret = memcmp(frame, session->st41_ref_cursor, st41_ref_remaining_length);
      if (ret) {
        session->errors_count++;
        err("%s() FAIL: reference file comparison with ending frame.\n", __func__);
      } else {
        dbg("%s() PASS: reference file comparison with ending frame.\n", __func__);

        /* Verify last 0-3 bytes of frame (filled with zero's) */
        ret = memcmp(&(((uint8_t*)frame)[st41_ref_remaining_length]), (void*)&last_zeros,
                     frame_size - st41_ref_remaining_length);
        if (ret) {
          session->errors_count++;
          err("%s() FAIL: frame comparison with ending zeros.\n", __func__);
        } else {
          dbg("%s() PASS: frame comparison with ending zeros.\n", __func__);
        }
      }
    }
  }

#ifdef DEBUG /* print out of frame and reference file if error */
  if (ret) {
    err("%s() FRAME START>>", __func__);
    for (int i = 0; i < frame_size; i++) {
      err("%c", ((char*)frame)[i]);
    }
    err("<<END FRAME.\n\n");

    err("%s() REFERENCE START>>", __func__);
    for (int i = 0; i < frame_size; i++) {
      err("%c", ((char*)(session->st41_ref_cursor))[i]);
    }
    err("<<END REFERENCE.\n\n");
  }
#endif /* DEBUG */

  /* calculate new reference frame */
  session->st41_ref_cursor += frame_size;
  if (session->st41_ref_cursor >= session->st41_ref_end)
    session->st41_ref_cursor = session->st41_ref_begin;

  return ret;
}

static void app_rx_fmd_handle_rtp(struct st_app_rx_fmd_session* s, void* usrptr) {
  struct st41_rtp_hdr* hdr = (struct st41_rtp_hdr*)usrptr;
  void* payload = (void*)(&hdr[1]);

  hdr->swaped_st41_hdr_chunk = ntohl(hdr->swaped_st41_hdr_chunk);

  /* Testing data_item_type */
  if (s->st41_dit != 0xffffffff) {
    if (hdr->st41_hdr_chunk.data_item_type != s->st41_dit) {
      s->errors_count++;
      err("%s(%d) FAIL: hdr->st41_hdr_chunk.data_item_type=%u, expected to be %u.\n",
          __func__, s->idx, hdr->st41_hdr_chunk.data_item_type, s->st41_dit);
    } else {
      dbg("%s(%d) PASS: hdr->st41_hdr_chunk.data_item_type=%u, expected to be %u.\n",
          __func__, s->idx, hdr->st41_hdr_chunk.data_item_type, s->st41_dit);
    }
  }

  /* Testing data_item K-bit */
  if (s->st41_k_bit != 0xff) {
    if (hdr->st41_hdr_chunk.data_item_k_bit != s->st41_k_bit) {
      s->errors_count++;
      err("%s(%d) FAIL: hdr->st41_hdr_chunk.data_item_k_bit=%u, expected to be %u.\n",
          __func__, s->idx, hdr->st41_hdr_chunk.data_item_k_bit, s->st41_k_bit);
    } else {
      dbg("%s(%d) PASS: hdr->st41_hdr_chunk.data_item_k_bit=%u, expected to be %u.\n",
          __func__, s->idx, hdr->st41_hdr_chunk.data_item_k_bit, s->st41_k_bit);
    }
  }

  s->stat_frame_total_received++;
  if (!s->stat_frame_first_rx_time)
    s->stat_frame_first_rx_time = st_app_get_monotonic_time();

  /* Compare each packet with reference (part by part) */
  if (s->st41_ref_fd > 0) {
    app_rx_fmd_compare_with_ref(s, payload, hdr->st41_hdr_chunk.data_item_length * 4);
    /* (field hdr->st41_hdr_chunk.data_item_length is expressed in 4-byte words, thus
     * multiplying by 4) */
  }

  hdr->swaped_st41_hdr_chunk = htonl(hdr->swaped_st41_hdr_chunk);
}

static void* app_rx_fmd_read_thread(void* arg) {
  struct st_app_rx_fmd_session* s = arg;
  int idx = s->idx;
  void* usrptr;
  uint16_t len;
  void* mbuf;

  info("%s(%d), start\n", __func__, idx);
  while (!s->st41_app_thread_stop) {
    mbuf = st41_rx_get_mbuf(s->handle, &usrptr, &len);
    if (!mbuf) {
      /* no buffer */
      st_pthread_mutex_lock(&s->st41_wake_mutex);
      if (!s->st41_app_thread_stop)
        st_pthread_cond_wait(&s->st41_wake_cond, &s->st41_wake_mutex);
      st_pthread_mutex_unlock(&s->st41_wake_mutex);
      continue;
    }
    /* parse the packet */
    app_rx_fmd_handle_rtp(s, usrptr);
    st41_rx_put_mbuf(s->handle, mbuf);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static int app_rx_fmd_rtp_ready(void* priv) {
  struct st_app_rx_fmd_session* s = priv;

  st_pthread_mutex_lock(&s->st41_wake_mutex);
  st_pthread_cond_signal(&s->st41_wake_cond);
  st_pthread_mutex_unlock(&s->st41_wake_mutex);
  return 0;
}

static int app_rx_fmd_uinit(struct st_app_rx_fmd_session* s) {
  int ret, idx = s->idx;
  s->st41_app_thread_stop = true;
  if (s->st41_app_thread) {
    /* wake up the thread */
    st_pthread_mutex_lock(&s->st41_wake_mutex);
    st_pthread_cond_signal(&s->st41_wake_cond);
    st_pthread_mutex_unlock(&s->st41_wake_mutex);
    info("%s(%d), wait app thread stop\n", __func__, idx);
    pthread_join(s->st41_app_thread, NULL);
  }
  if (s->handle) {
    ret = st41_rx_free(s->handle);
    if (ret < 0) err("%s(%d), st30_rx_free fail %d\n", __func__, idx, ret);
    s->handle = NULL;
  }
  st_pthread_mutex_destroy(&s->st41_wake_mutex);
  st_pthread_cond_destroy(&s->st41_wake_cond);

  app_rx_fmd_close_source(s);

  return 0;
}

static int app_rx_fmd_init(struct st_app_context* ctx,
                           st_json_fastmetadata_session_t* fmd,
                           struct st_app_rx_fmd_session* s) {
  int idx = s->idx, ret;
  struct st41_rx_ops ops;
  char name[32];
  st41_rx_handle handle;
  memset(&ops, 0, sizeof(ops));

  snprintf(name, 32, "app_rx_fmd%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.num_port = fmd ? fmd->base.num_inf : ctx->para.num_ports;
  memcpy(
      ops.ip_addr[MTL_SESSION_PORT_P],
      fmd ? st_json_ip(ctx, &fmd->base, MTL_SESSION_PORT_P) : ctx->rx_ip_addr[MTL_PORT_P],
      MTL_IP_ADDR_LEN);
  memcpy(ops.mcast_sip_addr[MTL_SESSION_PORT_P],
         fmd ? fmd->base.mcast_src_ip[MTL_PORT_P] : ctx->rx_mcast_sip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           fmd ? fmd->base.inf[MTL_SESSION_PORT_P]->name : ctx->para.port[MTL_PORT_P]);
  ops.udp_port[MTL_SESSION_PORT_P] = fmd ? fmd->base.udp_port : (10200 + s->idx);
  if (ops.num_port > 1) {
    memcpy(ops.ip_addr[MTL_SESSION_PORT_R],
           fmd ? st_json_ip(ctx, &fmd->base, MTL_SESSION_PORT_R)
               : ctx->rx_ip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    memcpy(ops.mcast_sip_addr[MTL_SESSION_PORT_R],
           fmd ? fmd->base.mcast_src_ip[MTL_PORT_R] : ctx->rx_mcast_sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
             fmd ? fmd->base.inf[MTL_SESSION_PORT_R]->name : ctx->para.port[MTL_PORT_R]);
    ops.udp_port[MTL_SESSION_PORT_R] = fmd ? fmd->base.udp_port : (10200 + s->idx);
  }
  ops.rtp_ring_size = 1024;
  ops.payload_type = fmd ? fmd->base.payload_type : ST_APP_PAYLOAD_TYPE_FASTMETADATA;
  s->st41_dit = fmd ? fmd->info.fmd_dit : 0xffffffff;
  s->st41_k_bit = fmd ? fmd->info.fmd_k_bit : 0xff;
  ops.interlaced = fmd ? fmd->info.interlaced : false;
  ops.notify_rtp_ready = app_rx_fmd_rtp_ready;
  if (fmd && fmd->enable_rtcp) ops.flags |= ST41_RX_FLAG_ENABLE_RTCP;
  st_pthread_mutex_init(&s->st41_wake_mutex, NULL);
  st_pthread_cond_init(&s->st41_wake_cond, NULL);

  s->errors_count = 0;
  s->st41_ref_fd = -1;
  if (fmd) {
    if (strcmp(fmd->info.fmd_url, "")) {
      snprintf(s->st41_ref_url, sizeof(s->st41_ref_url), "%s", fmd->info.fmd_url);

      ret = app_rx_fmd_open_ref(s);
      if (ret < 0) {
        err("%s(%d), app_rx_fmd_open_ref fail %d.\n", __func__, idx, ret);
        app_rx_fmd_uinit(s);
        return -EIO;
      }
    }
  }

  handle = st41_rx_create(ctx->st, &ops);
  if (!handle) {
    err("%s(%d), st41_rx_create fail\n", __func__, idx);
    return -EIO;
  }
  s->handle = handle;

  ret = pthread_create(&s->st41_app_thread, NULL, app_rx_fmd_read_thread, s);
  if (ret < 0) {
    err("%s, st41_app_thread create fail %d\n", __func__, ret);
    return -EIO;
  }

  char thread_name[32];
  snprintf(thread_name, sizeof(thread_name), "rx_fmd_%d", idx);
  mtl_thread_setname(s->st41_app_thread, thread_name);

  return 0;
}

static bool app_rx_fmd_fps_check(double framerate) {
  double expect;

  for (enum st_fps fps = 0; fps < ST_FPS_MAX; fps++) {
    expect = st_frame_rate(fps);
    if (ST_APP_EXPECT_NEAR(framerate, expect, expect * 0.05)) return true;
  }

  return false;
}

static int app_rx_fmd_result(struct st_app_rx_fmd_session* s) {
  int idx = s->idx;
  uint64_t cur_time_ns = st_app_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->stat_frame_first_rx_time) / NS_PER_S;
  double framerate = s->stat_frame_total_received / time_sec;

  if (!s->stat_frame_total_received) return -EINVAL;

  critical("%s(%d), %s, fps %f, %d frame received, %d counted errors.\n", __func__, idx,
           (app_rx_fmd_fps_check(framerate) && (s->errors_count == 0)) ? "OK" : "FAILED",
           framerate, s->stat_frame_total_received, s->errors_count);
  return 0;
}

int st_app_rx_fmd_sessions_init(struct st_app_context* ctx) {
  int ret, i;
  struct st_app_rx_fmd_session* s;
  ctx->rx_fmd_sessions = (struct st_app_rx_fmd_session*)st_app_zmalloc(
      sizeof(struct st_app_rx_fmd_session) * ctx->rx_fmd_session_cnt);
  if (!ctx->rx_fmd_sessions) return -ENOMEM;
  for (i = 0; i < ctx->rx_fmd_session_cnt; i++) {
    s = &ctx->rx_fmd_sessions[i];
    s->idx = i;

    ret = app_rx_fmd_init(ctx, ctx->json_ctx ? &ctx->json_ctx->rx_fmd_sessions[i] : NULL,
                          s);
    if (ret < 0) {
      err("%s(%d), app_rx_fmd_session_init fail %d\n", __func__, i, ret);
      return ret;
    }
  }

  return 0;
}

int st_app_rx_fmd_sessions_uinit(struct st_app_context* ctx) {
  int i;
  struct st_app_rx_fmd_session* s;
  if (!ctx->rx_fmd_sessions) return 0;
  for (i = 0; i < ctx->rx_fmd_session_cnt; i++) {
    s = &ctx->rx_fmd_sessions[i];
    app_rx_fmd_uinit(s);
  }
  st_app_free(ctx->rx_fmd_sessions);
  return 0;
}

int st_app_rx_fmd_sessions_result(struct st_app_context* ctx) {
  int i, ret = 0;
  struct st_app_rx_fmd_session* s;
  if (!ctx->rx_fmd_sessions) return 0;

  for (i = 0; i < ctx->rx_fmd_session_cnt; i++) {
    s = &ctx->rx_fmd_sessions[i];
    ret += app_rx_fmd_result(s);
  }

  return ret;
}
