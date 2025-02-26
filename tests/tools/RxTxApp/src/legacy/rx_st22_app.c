/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "rx_st22_app.h"

static int app_rx_st22_close_source(struct st22_app_rx_session *s) {
  if (s->st22_dst_fd >= 0) {
    munmap(s->st22_dst_begin, s->st22_dst_end - s->st22_dst_begin);
    close(s->st22_dst_fd);
    s->st22_dst_fd = -1;
  }

  return 0;
}

static int app_rx_st22_open_source(struct st22_app_rx_session *s) {
  int fd, ret, idx = s->idx;
  off_t f_size;

  /* user do not require fb save to file */
  if (s->st22_dst_fb_cnt <= 1) return 0;

  fd = st_open_mode(s->st22_dst_url, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    err("%s(%d), open %s fail\n", __func__, idx, s->st22_dst_url);
    return -EIO;
  }

  f_size = s->st22_dst_fb_cnt * s->bytes_per_frame;
  ret = ftruncate(fd, f_size);
  if (ret < 0) {
    err("%s(%d), ftruncate %s fail\n", __func__, idx, s->st22_dst_url);
    close(fd);
    return -EIO;
  }

  uint8_t *m = mmap(NULL, f_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
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

static int app_rx_st22_enqueue_frame(struct st22_app_rx_session *s, void *frame,
                                     size_t size) {
  uint16_t producer_idx = s->framebuff_producer_idx;
  struct st_rx_frame *framebuff = &s->framebuffs[producer_idx];

  if (framebuff->frame) {
    return -EBUSY;
  }

  dbg("%s(%d), frame idx %d\n", __func__, s->idx, producer_idx);
  framebuff->frame = frame;
  framebuff->size = size;
  /* point to next */
  producer_idx++;
  if (producer_idx >= s->framebuff_cnt) producer_idx = 0;
  s->framebuff_producer_idx = producer_idx;
  return 0;
}

static int app_rx_st22_frame_ready(void *priv, void *frame,
                                   struct st22_rx_frame_meta *meta) {
  struct st22_app_rx_session *s = (struct st22_app_rx_session *)priv;

  if (!s->handle) return -EIO;

  st_pthread_mutex_lock(&s->wake_mutex);
  int ret = app_rx_st22_enqueue_frame(s, frame, meta->frame_total_size);
  if (ret < 0) {
    err("%s(%d), frame %p dropped\n", __func__, s->idx, frame);
    /* free the queue */
    st22_rx_put_framebuff(s->handle, frame);
    st_pthread_mutex_unlock(&s->wake_mutex);
    return ret;
  }
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

static void app_rx_st22_decode_frame(struct st22_app_rx_session *s, void *codestream_addr,
                                     size_t codestream_size) {
  if (s->st22_dst_cursor + codestream_size > s->st22_dst_end)
    s->st22_dst_cursor = s->st22_dst_begin;

  mtl_memcpy(s->st22_dst_cursor, codestream_addr, codestream_size);
  s->st22_dst_cursor += codestream_size;

  s->fb_decoded++;
}

static void *app_rx_st22_decode_thread(void *arg) {
  struct st22_app_rx_session *s = arg;
  int idx = s->idx;
  int consumer_idx;
  struct st_rx_frame *framebuff;

  info("%s(%d), start\n", __func__, idx);
  while (!s->st22_app_thread_stop) {
    st_pthread_mutex_lock(&s->wake_mutex);
    consumer_idx = s->framebuff_consumer_idx;
    framebuff = &s->framebuffs[consumer_idx];
    if (!framebuff->frame) {
      /* no ready frame */
      if (!s->st22_app_thread_stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }
    st_pthread_mutex_unlock(&s->wake_mutex);

    dbg("%s(%d), frame idx %d\n", __func__, idx, consumer_idx);
    app_rx_st22_decode_frame(s, framebuff->frame, framebuff->size);
    st22_rx_put_framebuff(s->handle, framebuff->frame);
    /* point to next */
    st_pthread_mutex_lock(&s->wake_mutex);
    framebuff->frame = NULL;
    consumer_idx++;
    if (consumer_idx >= s->framebuff_cnt) consumer_idx = 0;
    s->framebuff_consumer_idx = consumer_idx;
    st_pthread_mutex_unlock(&s->wake_mutex);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static int app_rx_st22_uinit(struct st22_app_rx_session *s) {
  int ret, idx = s->idx;

  s->st22_app_thread_stop = true;
  if (s->st22_app_thread) {
    /* wake up the thread */
    st_pthread_mutex_lock(&s->wake_mutex);
    st_pthread_cond_signal(&s->wake_cond);
    st_pthread_mutex_unlock(&s->wake_mutex);
    info("%s(%d), wait app thread stop\n", __func__, idx);
    pthread_join(s->st22_app_thread, NULL);
  }

  st_pthread_mutex_destroy(&s->wake_mutex);
  st_pthread_cond_destroy(&s->wake_cond);

  if (s->handle) {
    ret = st22_rx_free(s->handle);
    if (ret < 0) err("%s(%d), st22_rx_free fail %d\n", __func__, idx, ret);
    s->handle = NULL;
  }
  app_rx_st22_close_source(s);

  if (s->framebuffs) {
    st_app_free(s->framebuffs);
    s->framebuffs = NULL;
  }

  return 0;
}

static int app_rx_st22_init(struct st_app_context *ctx, struct st22_app_rx_session *s,
                            int bpp) {
  int idx = s->idx, ret;
  struct st22_rx_ops ops;
  char name[32];
  st22_rx_handle handle;
  memset(&ops, 0, sizeof(ops));

  s->width = 1920;
  s->height = 1080;
  s->bpp = bpp;
  s->bytes_per_frame = s->width * s->height * bpp / 8;

  uint32_t soc = 0, b = 0, d = 0, f = 0;
  sscanf(ctx->para.port[MTL_PORT_P], "%x:%x:%x.%x", &soc, &b, &d, &f);
  snprintf(s->st22_dst_url, ST_APP_URL_MAX_LEN,
           "st22_app%d_%d_%d_%02x_%02x_%02x_%02x.raw", idx, s->width, s->height, soc, b,
           d, f);

  snprintf(name, 32, "app_rx_st22_%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.num_port = ctx->para.num_ports;
  memcpy(ops.ip_addr[MTL_SESSION_PORT_P], ctx->rx_ip_addr[MTL_PORT_P], MTL_IP_ADDR_LEN);
  memcpy(ops.mcast_sip_addr[MTL_SESSION_PORT_P], ctx->rx_mcast_sip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops.udp_port[MTL_SESSION_PORT_P] = 15000 + s->idx;
  if (ops.num_port > 1) {
    memcpy(ops.ip_addr[MTL_SESSION_PORT_R], ctx->rx_ip_addr[MTL_PORT_R], MTL_IP_ADDR_LEN);
    memcpy(ops.mcast_sip_addr[MTL_SESSION_PORT_R], ctx->rx_mcast_sip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops.udp_port[MTL_SESSION_PORT_R] = 15000 + s->idx;
  }
  ops.pacing = ST21_PACING_NARROW;
  ops.width = s->width;
  ops.height = s->height;
  ops.fps = ST_FPS_P59_94;
  ops.payload_type = ST_APP_PAYLOAD_TYPE_ST22;
  ops.type = ST22_TYPE_FRAME_LEVEL;
  ops.pack_type = ST22_PACK_CODESTREAM;
  ops.framebuff_cnt = 3;
  ops.framebuff_max_size = s->bytes_per_frame;
  ops.notify_frame_ready = app_rx_st22_frame_ready;

  s->framebuff_cnt = ops.framebuff_cnt;
  s->framebuff_producer_idx = 0;
  s->framebuff_consumer_idx = 0;
  s->framebuffs =
      (struct st_rx_frame *)st_app_zmalloc(sizeof(*s->framebuffs) * s->framebuff_cnt);
  if (!s->framebuffs) return -ENOMEM;
  for (uint16_t j = 0; j < s->framebuff_cnt; j++) {
    s->framebuffs[j].frame = NULL;
  }

  st_pthread_mutex_init(&s->wake_mutex, NULL);
  st_pthread_cond_init(&s->wake_cond, NULL);

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

  s->st22_app_thread_stop = false;
  ret = pthread_create(&s->st22_app_thread, NULL, app_rx_st22_decode_thread, s);
  if (ret < 0) {
    err("%s(%d), init thread fail %d\n", __func__, idx, ret);
    app_rx_st22_uinit(s);
    return -EIO;
  }

  char thread_name[32];
  snprintf(thread_name, sizeof(thread_name), "rx_st22_%d", idx);
  mtl_thread_setname(s->st22_app_thread, thread_name);

  return 0;
}

int st22_app_rx_sessions_init(struct st_app_context *ctx) {
  int ret, i;
  struct st22_app_rx_session *s;
  ctx->rx_st22_sessions = (struct st22_app_rx_session *)st_app_zmalloc(
      sizeof(struct st22_app_rx_session) * ctx->rx_st22_session_cnt);
  if (!ctx->rx_st22_sessions) return -ENOMEM;
  for (i = 0; i < ctx->rx_st22_session_cnt; i++) {
    s = &ctx->rx_st22_sessions[i];
    s->idx = i;
    s->st22_dst_fb_cnt = 3;
    s->st22_dst_fd = -1;

    ret = app_rx_st22_init(ctx, s, ctx->st22_bpp);
    if (ret < 0) {
      err("%s(%d), app_rx_st22_init fail %d\n", __func__, i, ret);
      return ret;
    }
  }

  return 0;
}

int st22_app_rx_sessions_uinit(struct st_app_context *ctx) {
  int i;
  struct st22_app_rx_session *s;
  if (!ctx->rx_st22_sessions) return 0;
  for (i = 0; i < ctx->rx_st22_session_cnt; i++) {
    s = &ctx->rx_st22_sessions[i];
    app_rx_st22_uinit(s);
  }
  st_app_free(ctx->rx_st22_sessions);

  return 0;
}
