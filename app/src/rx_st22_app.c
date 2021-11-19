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

#include "rx_st22_app.h"

static int app_rx_st22_handle_rtp(struct st22_app_rx_session* s,
                                  struct st22_rfc9143_rtp_hdr* hdr) {
  int idx = s->idx;
  uint32_t tmstamp = ntohl(hdr->tmstamp);
  uint8_t* frame;
  uint8_t* payload;

  dbg("%s(%d),tmstamp: 0x%x\n", __func__, idx, tmstamp);
  if (tmstamp != s->st22_last_tmstamp) {
    /* new frame received */
    s->st22_last_tmstamp = tmstamp;
    s->st22_pkt_idx = 0;

    s->st22_dst_cursor += s->st22_frame_size;
    if ((s->st22_dst_cursor + s->st22_frame_size) > s->st22_dst_end)
      s->st22_dst_cursor = s->st22_dst_begin;
  }

  if (s->st22_dst_fd < 0) return 0;

  frame = s->st22_dst_cursor;
  payload = (uint8_t*)hdr + sizeof(*hdr);
  /* copy the payload to target frame */
  uint32_t offset = s->st22_pkt_idx * s->rtp_pd_size;
  if ((offset + s->rtp_pd_size) > s->st22_frame_size) {
    err("%s(%d: invalid offset %u frame size %d\n", __func__, idx, offset,
        s->st22_frame_size);
    return -EIO;
  }
  st_memcpy(frame + offset, payload, s->rtp_pd_size);
  s->st22_pkt_idx++;
  return 0;
}

static void* app_rx_st22_rtp_thread(void* arg) {
  struct st22_app_rx_session* s = arg;
  int idx = s->idx;
  void* usrptr;
  uint16_t len;
  void* mbuf;
  struct st22_rfc9143_rtp_hdr* hdr;

  info("%s(%d), start\n", __func__, idx);
  while (!s->st22_app_thread_stop) {
    mbuf = st22_rx_get_mbuf(s->handle, &usrptr, &len);
    if (!mbuf) {
      /* no buffer */
      pthread_mutex_lock(&s->st22_wake_mutex);
      if (!s->st22_app_thread_stop)
        pthread_cond_wait(&s->st22_wake_cond, &s->st22_wake_mutex);
      pthread_mutex_unlock(&s->st22_wake_mutex);
      continue;
    }

    /* get one packet */
    hdr = (struct st22_rfc9143_rtp_hdr*)usrptr;
    app_rx_st22_handle_rtp(s, hdr);
    /* free to lib */
    st22_rx_put_mbuf(s->handle, mbuf);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static int app_rx_st22_close_source(struct st22_app_rx_session* s) {
  if (s->st22_dst_fd >= 0) {
    munmap(s->st22_dst_begin, s->st22_dst_end - s->st22_dst_begin);
    close(s->st22_dst_fd);
    s->st22_dst_fd = -1;
  }

  return 0;
}

static int app_rx_st22_open_source(struct st22_app_rx_session* s) {
  int fd, ret, idx = s->idx;
  off_t f_size;

  /* user do not require fb save to file */
  if (s->st22_dst_fb_cnt <= 1) return 0;

  fd = open(s->st22_dst_url, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    err("%s(%d), open %s fail\n", __func__, idx, s->st22_dst_url);
    return -EIO;
  }

  f_size = s->st22_dst_fb_cnt * s->st22_frame_size;
  ret = ftruncate(fd, f_size);
  if (ret < 0) {
    err("%s(%d), ftruncate %s fail\n", __func__, idx, s->st22_dst_url);
    close(fd);
    return -EIO;
  }

  uint8_t* m = mmap(NULL, f_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (MAP_FAILED == m) {
    err("%s(%d), mmap %s fail\n", __func__, idx, s->st22_dst_url);
    close(fd);
    return -EIO;
  }

  s->st22_dst_begin = m;
  s->st22_dst_cursor = m;
  s->st22_dst_end = m + f_size;
  s->st22_dst_fd = fd;
  info("%s(%d), save %d framebuffers to file %s(%p,%" PRIu64 ")\n", __func__, idx,
       s->st22_dst_fb_cnt, s->st22_dst_url, m, f_size);
  return 0;
}

static int app_rx_st22_init_rtp_thread(struct st22_app_rx_session* s) {
  int ret, idx = s->idx;

  ret = pthread_create(&s->st22_app_thread, NULL, app_rx_st22_rtp_thread, s);
  if (ret < 0) {
    err("%s(%d), st22_app_thread create fail %d\n", __func__, ret, idx);
    return -EIO;
  }

  return 0;
}

static int app_rx_st22_rtp_ready(void* priv) {
  struct st22_app_rx_session* s = priv;

  pthread_mutex_lock(&s->st22_wake_mutex);
  pthread_cond_signal(&s->st22_wake_cond);
  pthread_mutex_unlock(&s->st22_wake_mutex);

  return 0;
}

static int app_rx_st22_uinit(struct st22_app_rx_session* s) {
  int ret, idx = s->idx;

  s->st22_app_thread_stop = true;
  if (s->st22_app_thread) {
    /* wake up the thread */
    pthread_mutex_lock(&s->st22_wake_mutex);
    pthread_cond_signal(&s->st22_wake_cond);
    pthread_mutex_unlock(&s->st22_wake_mutex);
    info("%s(%d), wait app thread stop\n", __func__, idx);
    pthread_join(s->st22_app_thread, NULL);
  }

  pthread_mutex_destroy(&s->st22_wake_mutex);
  pthread_cond_destroy(&s->st22_wake_cond);

  if (s->handle) {
    ret = st22_rx_free(s->handle);
    if (ret < 0) err("%s(%d), st22_rx_free fail %d\n", __func__, idx, ret);
    s->handle = NULL;
  }
  app_rx_st22_close_source(s);

  return 0;
}

static int app_rx_st22_init(struct st_app_context* ctx, struct st22_app_rx_session* s) {
  int idx = s->idx, ret;
  struct st22_rx_ops ops;
  char name[32];
  st22_rx_handle handle;

  s->rtp_pkt_size = ctx->st22_rtp_pkt_size;
  s->rtp_pd_size = s->rtp_pkt_size - sizeof(struct st22_rfc9143_rtp_hdr);
  s->rtp_frame_total_pkts = ctx->st22_rtp_frame_total_pkts;
  s->width = 1920;
  s->height = 1080;
  s->st22_frame_size = s->rtp_frame_total_pkts * s->rtp_pd_size;

  uint32_t soc = 0, b = 0, d = 0, f = 0;
  sscanf(ctx->para.port[ST_PORT_P], "%x:%x:%x.%x", &soc, &b, &d, &f);
  snprintf(s->st22_dst_url, ST_APP_URL_MAX_LEN,
           "st22_app%d_%d_%d_%02x_%02x_%02x_%02x.raw", idx, s->width, s->height, soc, b,
           d, f);

  snprintf(name, 32, "app_rx_st22_%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.num_port = ctx->para.num_ports;
  memcpy(ops.sip_addr[ST_PORT_P], ctx->rx_sip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  strncpy(ops.port[ST_PORT_P], ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
  ops.udp_port[ST_PORT_P] = 15000 + s->idx;
  if (ops.num_port > 1) {
    memcpy(ops.sip_addr[ST_PORT_R], ctx->rx_sip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops.port[ST_PORT_R], ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
    ops.udp_port[ST_PORT_R] = 15000 + s->idx;
  }
  ops.pacing = ST21_PACING_NARROW;
  ops.width = s->width;
  ops.height = s->height;
  ops.fps = ST_FPS_P59_94;
  ops.fmt = ST20_FMT_YUV_422_10BIT;
  ops.notify_rtp_ready = app_rx_st22_rtp_ready;
  ops.rtp_ring_size = 1024;

  pthread_mutex_init(&s->st22_wake_mutex, NULL);
  pthread_cond_init(&s->st22_wake_cond, NULL);

  ret = app_rx_st22_open_source(s);
  if (ret < 0) {
    err("%s(%d), app_rx_st22_open_source fail %d\n", __func__, idx, ret);
    app_rx_st22_uinit(s);
    return -EIO;
  }

  handle = st22_rx_create(ctx->st, &ops);
  if (!handle) {
    err("%s(%d), st20_rx_create fail\n", __func__, idx);
    app_rx_st22_uinit(s);
    return -EIO;
  }
  s->handle = handle;

  ret = app_rx_st22_init_rtp_thread(s);
  if (ret < 0) {
    err("%s(%d), app_rx_st22_init_thread fail %d\n", __func__, idx, ret);
    app_rx_st22_uinit(s);
    return -EIO;
  }

  return 0;
}

int st22_app_rx_sessions_init(struct st_app_context* ctx) {
  int ret, i;
  struct st22_app_rx_session* s;

  for (i = 0; i < ctx->rx_st22_session_cnt; i++) {
    s = &ctx->rx_st22_sessions[i];
    s->idx = i;
    s->st22_dst_fb_cnt = 3;
    s->st22_dst_fd = -1;

    ret = app_rx_st22_init(ctx, s);
    if (ret < 0) {
      err("%s(%d), app_rx_st22_init fail %d\n", __func__, i, ret);
      return ret;
    }
  }

  return 0;
}

int st22_app_rx_sessions_uinit(struct st_app_context* ctx) {
  int i;
  struct st22_app_rx_session* s;

  for (i = 0; i < ctx->rx_st22_session_cnt; i++) {
    s = &ctx->rx_st22_sessions[i];
    app_rx_st22_uinit(s);
  }

  return 0;
}
