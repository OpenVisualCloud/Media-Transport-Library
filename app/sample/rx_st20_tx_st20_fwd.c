/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "sample_util.h"

struct rx_st20_tx_st20_sample_ctx {
  st_handle st;
  int idx;
  st20_rx_handle rx_handle;
  st20_tx_handle tx_handle;

  bool stop;
  bool ready;
  pthread_t fwd_thread;

  int fb_fwd;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  size_t framebuff_size;
  uint16_t framebuff_cnt;
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_consumer_idx;
  struct st_rx_frame* framebuffs;

  uint16_t tx_framebuff_producer_idx;
  uint16_t tx_framebuff_consumer_idx;
  struct st_tx_frame* tx_framebuffs;

  bool zero_copy;

  /* logo */
  void* logo_buf;
  struct st_frame logo_meta;
};

static int st20_fwd_open_logo(struct st_sample_context* ctx,
                              struct rx_st20_tx_st20_sample_ctx* s, char* file) {
  FILE* fp_logo = st_fopen(file, "rb");
  if (!fp_logo) {
    err("%s, open %s fail\n", __func__, file);
    return -EIO;
  }

  size_t logo_size = st_frame_size(ctx->input_fmt, ctx->logo_width, ctx->logo_height);
  s->logo_buf = st_hp_malloc(s->st, logo_size, ST_PORT_P);
  if (!s->logo_buf) {
    err("%s, logo buf malloc fail\n", __func__);
    fclose(fp_logo);
    return -EIO;
  }

  size_t read = fread(s->logo_buf, 1, logo_size, fp_logo);
  if (read != logo_size) {
    err("%s, logo buf read fail\n", __func__);
    st_hp_free(s->st, s->logo_buf);
    s->logo_buf = NULL;
    fclose(fp_logo);
    return -EIO;
  }

  s->logo_meta.addr = s->logo_buf;
  s->logo_meta.fmt = ctx->input_fmt;
  s->logo_meta.width = ctx->logo_width;
  s->logo_meta.height = ctx->logo_height;

  fclose(fp_logo);
  return 0;
}

static int rx_st20_enqueue_frame(struct rx_st20_tx_st20_sample_ctx* s, void* frame,
                                 size_t size) {
  uint16_t producer_idx = s->framebuff_producer_idx;
  struct st_rx_frame* framebuff = &s->framebuffs[producer_idx];

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

static int rx_st20_frame_ready(void* priv, void* frame, struct st20_rx_frame_meta* meta) {
  struct rx_st20_tx_st20_sample_ctx* s = (struct rx_st20_tx_st20_sample_ctx*)priv;

  if (!s->ready) return -EIO;

  /* incomplete frame */
  if (!st_is_frame_complete(meta->status)) {
    st20_rx_put_framebuff(s->rx_handle, frame);
    return 0;
  }

  st_pthread_mutex_lock(&s->wake_mutex);
  /* rx framebuffer from lib */
  int ret = rx_st20_enqueue_frame(s, frame, meta->frame_total_size);
  if (ret < 0) {
    err("%s(%d), frame %p dropped\n", __func__, s->idx, frame);
    /* free the queue */
    st20_rx_put_framebuff(s->rx_handle, frame);
    st_pthread_mutex_unlock(&s->wake_mutex);
    return ret;
  }
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

static int tx_video_next_frame(void* priv, uint16_t* next_frame_idx,
                               struct st20_tx_frame_meta* meta) {
  struct rx_st20_tx_st20_sample_ctx* s = priv;
  int ret;
  uint16_t consumer_idx = s->tx_framebuff_consumer_idx;
  struct st_tx_frame* framebuff = &s->tx_framebuffs[consumer_idx];

  st_pthread_mutex_lock(&s->wake_mutex);
  if (ST_TX_FRAME_READY == framebuff->stat) {
    dbg("%s(%d), next frame idx %u\n", __func__, s->idx, consumer_idx);
    ret = 0;
    framebuff->stat = ST_TX_FRAME_IN_TRANSMITTING;
    *next_frame_idx = consumer_idx;
    /* point to next */
    consumer_idx++;
    if (consumer_idx >= s->framebuff_cnt) consumer_idx = 0;
    s->tx_framebuff_consumer_idx = consumer_idx;
  } else {
    /* not ready */
    ret = -EIO;
  }
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return ret;
}

static int tx_video_frame_done(void* priv, uint16_t frame_idx,
                               struct st20_tx_frame_meta* meta) {
  struct rx_st20_tx_st20_sample_ctx* s = priv;
  int ret;
  struct st_tx_frame* framebuff = &s->tx_framebuffs[frame_idx];

  if (s->zero_copy) { /* rx framebuffer put back to lib here */
    void* frame_addr = st20_tx_get_framebuffer(s->tx_handle, frame_idx);
    st20_rx_put_framebuff(s->rx_handle, frame_addr);
  }

  st_pthread_mutex_lock(&s->wake_mutex);
  if (ST_TX_FRAME_IN_TRANSMITTING == framebuff->stat) {
    ret = 0;
    framebuff->stat = ST_TX_FRAME_FREE;
    dbg("%s(%d), done_idx %u\n", __func__, s->idx, frame_idx);
  } else {
    ret = -EIO;
    err("%s(%d), err status %d for frame %u\n", __func__, s->idx, framebuff->stat,
        frame_idx);
  }
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return ret;
}

static void rx_fwd_consume_frame(struct rx_st20_tx_st20_sample_ctx* s, void* frame,
                                 size_t frame_size) {
  uint16_t producer_idx;
  struct st_tx_frame* framebuff;
  struct st_frame tx_frame;

  if (frame_size != s->framebuff_size) {
    err("%s(%d), mismatch frame size %ld %ld\n", __func__, s->idx, frame_size,
        s->framebuff_size);
    return;
  }

  producer_idx = s->tx_framebuff_producer_idx;
  framebuff = &s->tx_framebuffs[producer_idx];
  if (ST_TX_FRAME_FREE != framebuff->stat) {
    /* not in free */
    err("%s(%d), frame %u err state %d\n", __func__, s->idx, producer_idx,
        framebuff->stat);
    return;
  }

  if (s->zero_copy) {
    struct st20_ext_frame ext_frame;
    ext_frame.buf_addr = frame;
    ext_frame.buf_iova = st_hp_virt2iova(s->st, frame);
    ext_frame.buf_len = s->framebuff_size;
    st20_tx_set_ext_frame(s->tx_handle, producer_idx, &ext_frame);
  } else {
    void* frame_addr = st20_tx_get_framebuffer(s->tx_handle, producer_idx);
    st_memcpy(frame_addr, frame, s->framebuff_size);
  }

  if (s->logo_buf) {
    tx_frame.addr = frame;
    tx_frame.fmt = s->logo_meta.fmt;
    tx_frame.buffer_size = s->framebuff_size;
    tx_frame.data_size = s->framebuff_size;
    tx_frame.width = 1920;
    tx_frame.height = 1080;
    st_draw_logo(&tx_frame, &s->logo_meta, 16, 16);
  }
  framebuff->size = s->framebuff_size;
  framebuff->stat = ST_TX_FRAME_READY;
  /* point to next */
  producer_idx++;
  if (producer_idx >= s->framebuff_cnt) producer_idx = 0;
  s->tx_framebuff_producer_idx = producer_idx;

  s->fb_fwd++;
}

static void* fwd_thread(void* arg) {
  struct rx_st20_tx_st20_sample_ctx* s = arg;
  struct st_rx_frame* rx_framebuff;
  int consumer_idx;

  info("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    st_pthread_mutex_lock(&s->wake_mutex);
    consumer_idx = s->framebuff_consumer_idx;
    rx_framebuff = &s->framebuffs[consumer_idx];
    if (!rx_framebuff->frame) {
      /* no ready frame */
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }

    rx_fwd_consume_frame(s, rx_framebuff->frame, rx_framebuff->size);
    if (!s->zero_copy) /* rx framebuffer put back to lib here */
      st20_rx_put_framebuff(s->rx_handle, rx_framebuff->frame);
    /* else, put back after tx done */
    /* point to next */
    rx_framebuff->frame = NULL;
    consumer_idx++;
    if (consumer_idx >= s->framebuff_cnt) consumer_idx = 0;
    s->framebuff_consumer_idx = consumer_idx;
    st_pthread_mutex_unlock(&s->wake_mutex);
  }
  info("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

static int rx_st20_tx_st20_free_app(struct rx_st20_tx_st20_sample_ctx* app) {
  if (app->tx_handle) {
    st20_tx_free(app->tx_handle);
    app->tx_handle = NULL;
  }
  if (app->rx_handle) {
    st20_rx_free(app->rx_handle);
    app->rx_handle = NULL;
  }
  if (app->logo_buf) {
    st_hp_free(app->st, app->logo_buf);
    app->logo_buf = NULL;
  }
  st_pthread_mutex_destroy(&app->wake_mutex);
  st_pthread_cond_destroy(&app->wake_cond);
  if (app->framebuffs) {
    free(app->framebuffs);
    app->framebuffs = NULL;
  }
  if (app->tx_framebuffs) {
    free(app->tx_framebuffs);
    app->tx_framebuffs = NULL;
  }

  return 0;
}

int main(int argc, char** argv) {
  struct st_sample_context ctx;
  int ret;

  /* init sample(st) dev */
  ret = st_sample_fwd_init(&ctx, argc, argv);
  if (ret < 0) return ret;

  struct rx_st20_tx_st20_sample_ctx app;
  memset(&app, 0, sizeof(app));
  app.idx = 0;
  app.stop = false;
  app.st = ctx.st;
  st_pthread_mutex_init(&app.wake_mutex, NULL);
  st_pthread_cond_init(&app.wake_cond, NULL);

  app.framebuff_cnt = ctx.framebuff_cnt;
  app.framebuffs =
      (struct st_rx_frame*)malloc(sizeof(*app.framebuffs) * app.framebuff_cnt);
  if (!app.framebuffs) {
    err("%s, rx framebuffs ctx malloc fail\n", __func__);
    ret = -EIO;
    goto error;
  }
  for (uint16_t j = 0; j < app.framebuff_cnt; j++) app.framebuffs[j].frame = NULL;
  app.framebuff_producer_idx = 0;
  app.framebuff_consumer_idx = 0;

  app.tx_framebuffs =
      (struct st_tx_frame*)malloc(sizeof(*app.tx_framebuffs) * app.framebuff_cnt);
  if (!app.tx_framebuffs) {
    err("%s, tx framebuffs ctx malloc fail\n", __func__);
    ret = -EIO;
    goto error;
  }
  for (uint16_t j = 0; j < app.framebuff_cnt; j++) {
    app.tx_framebuffs[j].stat = ST_TX_FRAME_FREE;
  }
  app.zero_copy = true;

  struct st20_rx_ops ops_rx;
  memset(&ops_rx, 0, sizeof(ops_rx));
  ops_rx.name = "st20_fwd";
  ops_rx.priv = &app;
  ops_rx.num_port = 1;
  memcpy(ops_rx.sip_addr[ST_PORT_P], ctx.rx_sip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  strncpy(ops_rx.port[ST_PORT_P], ctx.param.port[ST_PORT_P], ST_PORT_MAX_LEN);
  ops_rx.udp_port[ST_PORT_P] = ctx.udp_port;  // user config the udp port.
  ops_rx.pacing = ST21_PACING_NARROW;
  ops_rx.type = ST20_TYPE_FRAME_LEVEL;
  ops_rx.width = ctx.width;
  ops_rx.height = ctx.height;
  ops_rx.fps = ctx.fps;
  ops_rx.fmt = ctx.fmt;
  ops_rx.framebuff_cnt = app.framebuff_cnt;
  ops_rx.payload_type = ctx.payload_type;
  ops_rx.flags = 0;
  ops_rx.notify_frame_ready = rx_st20_frame_ready;
  st20_rx_handle rx_handle = st20_rx_create(ctx.st, &ops_rx);
  if (!rx_handle) {
    err("%s, st20_rx_create fail\n", __func__);
    ret = -EIO;
    goto error;
  }
  app.rx_handle = rx_handle;

  struct st20_tx_ops ops_tx;
  memset(&ops_tx, 0, sizeof(ops_tx));
  ops_tx.name = "st20_fwd";
  ops_tx.priv = &app;
  ops_tx.num_port = 1;
  memcpy(ops_tx.dip_addr[ST_PORT_P], ctx.fwd_dip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  strncpy(ops_tx.port[ST_PORT_P], ctx.param.port[ST_PORT_P], ST_PORT_MAX_LEN);
  ops_tx.udp_port[ST_PORT_P] = ctx.udp_port;
  ops_tx.pacing = ST21_PACING_NARROW;
  ops_tx.type = ST20_TYPE_FRAME_LEVEL;
  ops_tx.width = ctx.width;
  ops_tx.height = ctx.height;
  ops_tx.fps = ctx.fps;
  ops_tx.fmt = ctx.fmt;
  ops_tx.payload_type = ctx.payload_type;
  if (app.zero_copy) ops_tx.flags |= ST20_TX_FLAG_EXT_FRAME;
  ops_tx.framebuff_cnt = app.framebuff_cnt;
  ops_tx.get_next_frame = tx_video_next_frame;
  ops_tx.notify_frame_done = tx_video_frame_done;
  st20_tx_handle tx_handle = st20_tx_create(ctx.st, &ops_tx);
  if (!tx_handle) {
    err("%s, st20_tx_create fail\n", __func__);
    ret = -EIO;
    goto error;
  }
  app.tx_handle = tx_handle;
  app.framebuff_size = st20_tx_get_framebuffer_size(tx_handle);

  st20_fwd_open_logo(&ctx, &app, ctx.logo_url);

  ret = pthread_create(&app.fwd_thread, NULL, fwd_thread, &app);
  if (ret < 0) {
    err("%s, fwd thread create fail\n", __func__);
    ret = -EIO;
    goto error;
  }

  app.ready = true;

  // start dev
  ret = st_start(ctx.st);

  while (!ctx.exit) {
    sleep(1);
  }

  // stop app thread
  app.stop = true;
  st_pthread_mutex_lock(&app.wake_mutex);
  st_pthread_cond_signal(&app.wake_cond);
  st_pthread_mutex_unlock(&app.wake_mutex);
  pthread_join(app.fwd_thread, NULL);
  info("%s, fb_fwd %d\n", __func__, app.fb_fwd);

  // stop dev
  ret = st_stop(ctx.st);

  // check result
  if (app.fb_fwd <= 0) {
    err("%s, error, no fwd frames %d\n", __func__, app.fb_fwd);
    ret = -EIO;
  }

error:
  // release session
  rx_st20_tx_st20_free_app(&app);

  /* release sample(st) dev */
  st_sample_uinit(&ctx);
  return ret;
}