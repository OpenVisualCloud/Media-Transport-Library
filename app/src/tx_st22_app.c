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

#include "tx_st22_app.h"

static int app_tx_st22_build_rtp_packet(struct st22_app_tx_session* s,
                                        struct st_rfc3550_rtp_hdr* rtp,
                                        uint16_t* pkt_len) {
  uint8_t* frame = s->st22_frame_cursor;
  uint8_t* payload = (uint8_t*)rtp + sizeof(*rtp);

  /* copy base hdr */
  st_memcpy(rtp, &s->st22_rtp_base, sizeof(*rtp));
  /* update hdr */
  rtp->tmstamp = htonl(s->st22_rtp_tmstamp);
  rtp->seq_number = htons(s->st22_seq_id);
  s->st22_seq_id++;
  *pkt_len = s->rtp_pkt_size;

  /* copy payload */
  uint32_t offset = s->st22_pkt_idx * s->rtp_pd_size;
  st_memcpy(payload, frame + offset, s->rtp_pd_size);

  s->st22_pkt_idx++;
  if (s->st22_pkt_idx >= s->rtp_frame_total_pkts) {
    dbg("%s(%d), frame %d done\n", __func__, s->idx, s->st22_frame_idx);
    /* end of current frame */
    rtp->marker = 1;

    s->st22_pkt_idx = 0;
    s->st22_rtp_tmstamp++;
    s->st22_frame_idx++;

    s->st22_frame_cursor += s->st22_frame_size;
    if ((s->st22_frame_cursor + s->st22_frame_size) > s->st22_source_end)
      s->st22_frame_cursor = s->st22_source_begin;
  }

  return 0;
}

static int app_tx_st22_rtp_done(void* priv) {
  struct st22_app_tx_session* s = priv;

  pthread_mutex_lock(&s->st22_wake_mutex);
  pthread_cond_signal(&s->st22_wake_cond);
  pthread_mutex_unlock(&s->st22_wake_mutex);

  return 0;
}

static void* app_tx_st22_rtp_thread(void* arg) {
  struct st22_app_tx_session* s = arg;
  int idx = s->idx;
  void* mbuf;
  uint16_t mbuf_len = s->rtp_pkt_size;
  void* usrptr = NULL;

  info("%s(%d), start\n", __func__, idx);
  while (!s->st22_app_thread_stop) {
    /* get available buffer*/
    mbuf = st22_tx_get_mbuf(s->handle, &usrptr);
    if (!mbuf) {
      pthread_mutex_lock(&s->st22_wake_mutex);
      /* try again */
      mbuf = st22_tx_get_mbuf(s->handle, &usrptr);
      if (mbuf) {
        pthread_mutex_unlock(&s->st22_wake_mutex);
      } else {
        if (!s->st22_app_thread_stop)
          pthread_cond_wait(&s->st22_wake_cond, &s->st22_wake_mutex);
        pthread_mutex_unlock(&s->st22_wake_mutex);
        continue;
      }
    }

    app_tx_st22_build_rtp_packet(s, (struct st_rfc3550_rtp_hdr*)usrptr, &mbuf_len);
    st22_tx_put_mbuf(s->handle, mbuf, mbuf_len);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static void app_tx_st22_stop_source(struct st22_app_tx_session* s) {
  s->st22_app_thread_stop = true;
  /* wake up the thread */
  pthread_mutex_lock(&s->st22_wake_mutex);
  pthread_cond_signal(&s->st22_wake_cond);
  pthread_mutex_unlock(&s->st22_wake_mutex);
  if (s->st22_app_thread) {
    pthread_join(s->st22_app_thread, NULL);
    s->st22_app_thread = 0;
  }
}

static int app_tx_st22_start_source(struct st_app_context* ctx,
                                    struct st22_app_tx_session* s) {
  int ret = -EINVAL;

  ret = pthread_create(&s->st22_app_thread, NULL, app_tx_st22_rtp_thread, s);
  if (ret < 0) {
    err("%s, st22_app_thread create fail err = %d\n", __func__, ret);
    return ret;
  }
  s->st22_app_thread_stop = false;

  return 0;
}

static int app_tx_st22_close_source(struct st22_app_tx_session* s) {
  if (s->st22_source_fd >= 0) {
    munmap(s->st22_source_begin, s->st22_source_end - s->st22_source_begin);
    close(s->st22_source_fd);
    s->st22_source_fd = -1;
  }

  return 0;
}

static int app_tx_st22_open_source(struct st22_app_tx_session* s) {
  int fd;
  struct stat i;

  fd = open(s->st22_source_url, O_RDONLY);
  if (fd < 0) {
    err("%s, open %s fai\n", __func__, s->st22_source_url);
    return -EIO;
  }

  fstat(fd, &i);
  if (i.st_size < s->st22_frame_size) {
    err("%s, %s file size small then a frame %d\n", __func__, s->st22_source_url,
        s->st22_frame_size);
    close(fd);
    return -EIO;
  }

  uint8_t* m = mmap(NULL, i.st_size, PROT_READ, MAP_SHARED, fd, 0);
  if (MAP_FAILED == m) {
    err("%s, mmap %s fail\n", __func__, s->st22_source_url);
    close(fd);
    return -EIO;
  }

  s->st22_source_begin = m;
  s->st22_frame_cursor = m;
  s->st22_source_end = m + i.st_size;
  s->st22_source_fd = fd;

  return 0;
}

static int app_tx_st22_init_rtp(struct st22_app_tx_session* s) {
  struct st_rfc3550_rtp_hdr* rtp = &s->st22_rtp_base;

  memset(rtp, 0, sizeof(*rtp));
  rtp->version = 2;
  rtp->payload_type = 112;
  rtp->ssrc = htonl(s->idx + 0x123450);
  return 0;
}

static int app_tx_st22_handle_free(struct st22_app_tx_session* s) {
  int ret;
  int idx = s->idx;

  if (s->handle) {
    ret = st22_tx_free(s->handle);
    if (ret < 0) err("%s(%d), st22_tx_free fail %d\n", __func__, idx, ret);
    s->handle = NULL;
  }

  return 0;
}

static int app_tx_st22_uinit(struct st22_app_tx_session* s) {
  app_tx_st22_stop_source(s);
  app_tx_st22_handle_free(s);
  app_tx_st22_close_source(s);

  pthread_mutex_destroy(&s->st22_wake_mutex);
  pthread_cond_destroy(&s->st22_wake_cond);
  return 0;
}

static int app_tx_st22_init(struct st_app_context* ctx, struct st22_app_tx_session* s) {
  int idx = s->idx, ret;
  struct st22_tx_ops ops;
  char name[32];
  st22_tx_handle handle;
  memset(&ops, 0, sizeof(ops));

  s->rtp_pkt_size = ctx->st22_rtp_pkt_size;
  s->rtp_pd_size = s->rtp_pkt_size - sizeof(struct st_rfc3550_rtp_hdr);
  s->rtp_frame_total_pkts = ctx->st22_rtp_frame_total_pkts;
  s->width = 1920;
  s->height = 1080;
  s->st22_frame_size = s->rtp_frame_total_pkts * s->rtp_pd_size;
  memcpy(s->st22_source_url, ctx->tx_st22_url, ST_APP_URL_MAX_LEN);
  s->st22_source_fd = -1;

  snprintf(name, 32, "app_tx_st22_%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.num_port = ctx->para.num_ports;
  memcpy(ops.dip_addr[ST_PORT_P], ctx->tx_dip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  strncpy(ops.port[ST_PORT_P], ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
  ops.udp_port[ST_PORT_P] = 15000 + s->idx;
  if (ops.num_port > 1) {
    memcpy(ops.dip_addr[ST_PORT_R], ctx->tx_dip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops.port[ST_PORT_R], ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
    ops.udp_port[ST_PORT_R] = 15000 + s->idx;
  }
  ops.pacing = ST21_PACING_NARROW;
  ops.width = s->width;
  ops.height = s->height;
  ops.fps = ST_FPS_P59_94;
  ops.fmt = ST20_FMT_YUV_422_10BIT;
  ops.rtp_ring_size = 1024;
  ops.rtp_pkt_size = s->rtp_pkt_size;
  ops.rtp_frame_total_pkts = s->rtp_frame_total_pkts;
  ops.notify_rtp_done = app_tx_st22_rtp_done;
  ops.payload_type = 112;

  pthread_mutex_init(&s->st22_wake_mutex, NULL);
  pthread_cond_init(&s->st22_wake_cond, NULL);

  app_tx_st22_init_rtp(s);

  handle = st22_tx_create(ctx->st, &ops);
  if (!handle) {
    err("%s(%d), st22_tx_create fail\n", __func__, idx);
    app_tx_st22_uinit(s);
    return -EIO;
  }
  s->handle = handle;

  ret = app_tx_st22_open_source(s);
  if (ret < 0) {
    err("%s(%d), app_tx_st22_open_source fail %d\n", __func__, idx, ret);
    app_tx_st22_uinit(s);
    return ret;
  }

  ret = app_tx_st22_start_source(ctx, s);
  if (ret < 0) {
    err("%s(%d), app_tx_st22_start_source fail %d\n", __func__, idx, ret);
    app_tx_st22_uinit(s);
    return ret;
  }

  info("%s(%d), rtp size(%d:%d) total %d pkts\n", __func__, idx, s->rtp_pkt_size,
       s->rtp_pd_size, s->rtp_frame_total_pkts);
  return 0;
}

int st22_app_tx_sessions_init(struct st_app_context* ctx) {
  int ret, i;
  struct st22_app_tx_session* s;
  ctx->tx_st22_sessions = (struct st22_app_tx_session*)st_app_zmalloc(
      sizeof(struct st22_app_tx_session) * ctx->tx_st22_session_cnt);
  if (!ctx->tx_st22_sessions) return -ENOMEM;
  for (i = 0; i < ctx->tx_st22_session_cnt; i++) {
    s = &ctx->tx_st22_sessions[i];
    s->idx = i;
    ret = app_tx_st22_init(ctx, s);
    if (ret < 0) {
      err("%s(%d), app_tx_st22_init fail %d\n", __func__, i, ret);
      return ret;
    }
  }

  return 0;
}

int st22_app_tx_sessions_stop(struct st_app_context* ctx) {
  struct st22_app_tx_session* s;
  if (!ctx->tx_st22_sessions) return 0;
  for (int i = 0; i < ctx->tx_st22_session_cnt; i++) {
    s = &ctx->tx_st22_sessions[i];
    app_tx_st22_stop_source(s);
  }

  return 0;
}

int st22_app_tx_sessions_uinit(struct st_app_context* ctx) {
  int i;
  struct st22_app_tx_session* s;
  if (!ctx->tx_st22_sessions) return 0;
  for (i = 0; i < ctx->tx_st22_session_cnt; i++) {
    s = &ctx->tx_st22_sessions[i];
    s->idx = i;
    app_tx_st22_uinit(s);
  }
  st_app_free(ctx->tx_st22_sessions);

  return 0;
}
