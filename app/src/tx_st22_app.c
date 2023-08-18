/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "tx_st22_app.h"

static int app_tx_st22_next_frame(void* priv, uint16_t* next_frame_idx,
                                  struct st22_tx_frame_meta* meta) {
  struct st22_app_tx_session* s = priv;
  int ret;
  uint16_t consumer_idx = s->framebuff_consumer_idx;
  struct st_tx_frame* framebuff = &s->framebuffs[consumer_idx];

  st_pthread_mutex_lock(&s->wake_mutex);
  if (ST_TX_FRAME_READY == framebuff->stat) {
    dbg("%s(%d), next frame idx %u\n", __func__, s->idx, consumer_idx);
    ret = 0;
    framebuff->stat = ST_TX_FRAME_IN_TRANSMITTING;
    *next_frame_idx = consumer_idx;
    meta->codestream_size = framebuff->size;
    /* point to next */
    consumer_idx++;
    if (consumer_idx >= s->framebuff_cnt) consumer_idx = 0;
    s->framebuff_consumer_idx = consumer_idx;
  } else {
    /* not ready */
    ret = -EIO;
    dbg("%s(%d), idx %u err stat %d\n", __func__, s->idx, consumer_idx, framebuff->stat);
  }
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return ret;
}

static int app_tx_st22_frame_done(void* priv, uint16_t frame_idx,
                                  struct st22_tx_frame_meta* meta) {
  struct st22_app_tx_session* s = priv;
  int ret;
  struct st_tx_frame* framebuff = &s->framebuffs[frame_idx];

  st_pthread_mutex_lock(&s->wake_mutex);
  if (ST_TX_FRAME_IN_TRANSMITTING == framebuff->stat) {
    ret = 0;
    framebuff->stat = ST_TX_FRAME_FREE;
    dbg("%s(%d), done_idx %u\n", __func__, s->idx, frame_idx);
    s->fb_send++;
  } else {
    ret = -EIO;
    err("%s(%d), err status %d for frame %u\n", __func__, s->idx, framebuff->stat,
        frame_idx);
  }
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return ret;
}

static void app_tx_st22_thread_bind(struct st22_app_tx_session* s) {
  if (s->lcore != -1) {
    mtl_bind_to_lcore(s->st, pthread_self(), s->lcore);
  }
}

static void app_tx_st22_check_lcore(struct st22_app_tx_session* s, bool rtp) {
  int sch_idx = st22_tx_get_sch_idx(s->handle);

  if (!s->ctx->app_thread && (s->handle_sch_idx != sch_idx)) {
    s->handle_sch_idx = sch_idx;
    unsigned int lcore;
    int ret = st_app_video_get_lcore(s->ctx, s->handle_sch_idx, rtp, &lcore);
    if ((ret >= 0) && (lcore != s->lcore)) {
      s->lcore = lcore;
      app_tx_st22_thread_bind(s);
      info("%s(%d), bind to new lcore %d\n", __func__, s->idx, lcore);
    }
  }
}

static void app_tx_st22_build_frame(struct st22_app_tx_session* s, void* codestream_addr,
                                    size_t max_codestream_size, size_t* codestream_size) {
  uint8_t* src = s->st22_frame_cursor;
  uint8_t* dst = codestream_addr;
  int framesize = s->bytes_per_frame;

  if (s->st22_frame_cursor + framesize > s->st22_source_end) {
    s->st22_frame_cursor = s->st22_source_begin;
    src = s->st22_frame_cursor;
  }
  /* call the real encoding here, sample just copy from the file */
  mtl_memcpy(dst, src, framesize);
  /* point to next frame */
  s->st22_frame_cursor += framesize;

  *codestream_size = framesize;
}

static void* app_tx_st22_frame_thread(void* arg) {
  struct st22_app_tx_session* s = arg;
  int idx = s->idx;
  uint16_t producer_idx;
  struct st_tx_frame* framebuff;

  app_tx_st22_thread_bind(s);

  info("%s(%d), start\n", __func__, idx);
  while (!s->st22_app_thread_stop) {
    st_pthread_mutex_lock(&s->wake_mutex);
    producer_idx = s->framebuff_producer_idx;
    framebuff = &s->framebuffs[producer_idx];
    if (ST_TX_FRAME_FREE != framebuff->stat) {
      /* not in free */
      if (!s->st22_app_thread_stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }
    st_pthread_mutex_unlock(&s->wake_mutex);

    app_tx_st22_check_lcore(s, false);

    void* frame_addr = st22_tx_get_fb_addr(s->handle, producer_idx);
    size_t max_framesize = s->bytes_per_frame;
    size_t codestream_size = s->bytes_per_frame;
    app_tx_st22_build_frame(s, frame_addr, max_framesize, &codestream_size);

    st_pthread_mutex_lock(&s->wake_mutex);
    framebuff->size = codestream_size;
    framebuff->stat = ST_TX_FRAME_READY;
    /* point to next */
    producer_idx++;
    if (producer_idx >= s->framebuff_cnt) producer_idx = 0;
    s->framebuff_producer_idx = producer_idx;
    st_pthread_mutex_unlock(&s->wake_mutex);
  }
  info("%s(%d), stop\n", __func__, idx);

  return NULL;
}

static void app_tx_st22_stop_source(struct st22_app_tx_session* s) {
  s->st22_app_thread_stop = true;
  /* wake up the thread */
  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);
  if (s->st22_app_thread) {
    pthread_join(s->st22_app_thread, NULL);
    s->st22_app_thread = 0;
  }
}

static int app_tx_st22_start_source(struct st_app_context* ctx,
                                    struct st22_app_tx_session* s) {
  int ret = -EINVAL;

  s->st22_app_thread_stop = false;
  ret = pthread_create(&s->st22_app_thread, NULL, app_tx_st22_frame_thread, s);
  if (ret < 0) {
    err("%s, st22_app_thread create fail err = %d\n", __func__, ret);
    return ret;
  }

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

  fd = st_open(s->st22_source_url, O_RDONLY);
  if (fd < 0) {
    err("%s, open %s fai\n", __func__, s->st22_source_url);
    return -EIO;
  }

  fstat(fd, &i);
  if (i.st_size < s->bytes_per_frame) {
    err("%s, %s file size small then a frame %" PRIu64 "\n", __func__, s->st22_source_url,
        s->bytes_per_frame);
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

  st_pthread_mutex_destroy(&s->wake_mutex);
  st_pthread_cond_destroy(&s->wake_cond);

  if (s->framebuffs) {
    st_app_free(s->framebuffs);
    s->framebuffs = NULL;
  }

  return 0;
}

static int app_tx_st22_init(struct st_app_context* ctx, struct st22_app_tx_session* s,
                            int bpp) {
  int idx = s->idx, ret;
  struct st22_tx_ops ops;
  char name[32];
  st22_tx_handle handle;
  memset(&ops, 0, sizeof(ops));

  s->width = 1920;
  s->height = 1080;
  s->bpp = bpp;
  s->bytes_per_frame = s->width * s->height * bpp / 8;
  memcpy(s->st22_source_url, ctx->tx_st22_url, ST_APP_URL_MAX_LEN);
  s->st22_source_fd = -1;
  s->st = ctx->st;
  s->ctx = ctx;

  snprintf(name, 32, "app_tx_st22_%d", idx);
  ops.name = name;
  ops.priv = s;
  ops.num_port = ctx->para.num_ports;
  memcpy(ops.dip_addr[MTL_SESSION_PORT_P], ctx->tx_dip_addr[MTL_PORT_P], MTL_IP_ADDR_LEN);
  snprintf(ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops.udp_port[MTL_SESSION_PORT_P] = 15000 + s->idx;
  if (ctx->has_tx_dst_mac[MTL_PORT_P]) {
    memcpy(&ops.tx_dst_mac[MTL_SESSION_PORT_P][0], ctx->tx_dst_mac[MTL_PORT_P],
           MTL_MAC_ADDR_LEN);
    ops.flags |= ST22_TX_FLAG_USER_P_MAC;
  }
  if (ops.num_port > 1) {
    memcpy(ops.dip_addr[MTL_SESSION_PORT_R], ctx->tx_dip_addr[MTL_PORT_R],
           MTL_IP_ADDR_LEN);
    snprintf(ops.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_R]);
    ops.udp_port[MTL_SESSION_PORT_R] = 15000 + s->idx;
    if (ctx->has_tx_dst_mac[MTL_PORT_R]) {
      memcpy(&ops.tx_dst_mac[MTL_SESSION_PORT_R][0], ctx->tx_dst_mac[MTL_PORT_R],
             MTL_MAC_ADDR_LEN);
      ops.flags |= ST22_TX_FLAG_USER_R_MAC;
    }
  }
  ops.pacing = ST21_PACING_NARROW;
  ops.width = s->width;
  ops.height = s->height;
  ops.fps = ST_FPS_P59_94;
  ops.payload_type = ST_APP_PAYLOAD_TYPE_ST22;
  ops.type = ST22_TYPE_FRAME_LEVEL;
  ops.framebuff_cnt = 3;
  ops.framebuff_max_size = s->bytes_per_frame;
  ops.get_next_frame = app_tx_st22_next_frame;
  ops.notify_frame_done = app_tx_st22_frame_done;

  s->framebuff_cnt = ops.framebuff_cnt;
  s->framebuff_producer_idx = 0;
  s->framebuff_consumer_idx = 0;
  s->framebuffs =
      (struct st_tx_frame*)st_app_zmalloc(sizeof(*s->framebuffs) * s->framebuff_cnt);
  if (!s->framebuffs) return -ENOMEM;
  for (uint16_t j = 0; j < s->framebuff_cnt; j++) {
    s->framebuffs[j].stat = ST_TX_FRAME_FREE;
  }

  st_pthread_mutex_init(&s->wake_mutex, NULL);
  st_pthread_cond_init(&s->wake_cond, NULL);

  handle = st22_tx_create(ctx->st, &ops);
  if (!handle) {
    err("%s(%d), st22_tx_create fail\n", __func__, idx);
    app_tx_st22_uinit(s);
    return -EIO;
  }
  s->handle = handle;
  s->type = ops.type;
  s->handle_sch_idx = st22_tx_get_sch_idx(handle);

  if (!ctx->app_thread) {
    unsigned int lcore;
    ret = st_app_video_get_lcore(ctx, s->handle_sch_idx, false, &lcore);
    if (ret >= 0) s->lcore = lcore;
  }

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

  info("%s(%d), bytes_per_frame %" PRIu64 "\n", __func__, idx, s->bytes_per_frame);
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
    s->lcore = -1;
    ret = app_tx_st22_init(ctx, s, ctx->st22_bpp);
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
