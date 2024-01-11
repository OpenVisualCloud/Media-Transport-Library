/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "../sample_util.h"

struct rx_st22_sample_ctx {
  int idx;
  int fb_decoded;
  st22_rx_handle handle;
  bool stop;
  pthread_t decode_thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;
  size_t bytes_per_frame;

  uint16_t framebuff_cnt;
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_consumer_idx;
  struct st_rx_frame* framebuffs;
};

static int rx_st22_enqueue_frame(struct rx_st22_sample_ctx* s, void* frame, size_t size) {
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

static int rx_st22_frame_ready(void* priv, void* frame, struct st22_rx_frame_meta* meta) {
  struct rx_st22_sample_ctx* s = (struct rx_st22_sample_ctx*)priv;

  if (!s->handle) return -EIO;

  st_pthread_mutex_lock(&s->wake_mutex);
  int ret = rx_st22_enqueue_frame(s, frame, meta->frame_total_size);
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

static void st22_decode_frame(struct rx_st22_sample_ctx* s, void* codestream_addr,
                              size_t codestream_size) {
  MTL_MAY_UNUSED(codestream_addr);
  MTL_MAY_UNUSED(codestream_size);

  dbg("%s(%d), frame %p\n", __func__, s->idx, frame);

  /* call the real decoding here, sample just sleep */
  st_usleep(10 * 1000);
  s->fb_decoded++;
}

static void* st22_decode_thread(void* arg) {
  struct rx_st22_sample_ctx* s = arg;
  int idx = s->idx;
  int consumer_idx;
  struct st_rx_frame* framebuff;

  info("%s(%d), start\n", __func__, idx);
  while (!s->stop) {
    st_pthread_mutex_lock(&s->wake_mutex);
    consumer_idx = s->framebuff_consumer_idx;
    framebuff = &s->framebuffs[consumer_idx];
    if (!framebuff->frame) {
      /* no ready frame */
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }
    st_pthread_mutex_unlock(&s->wake_mutex);

    dbg("%s(%d), frame idx %d\n", __func__, idx, consumer_idx);
    st22_decode_frame(s, framebuff->frame, framebuff->size);
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

int main(int argc, char** argv) {
  int bpp = 3; /* 3bit per pixel */
  struct st_sample_context ctx;
  int ret;

  /* init sample(st) dev */
  memset(&ctx, 0, sizeof(ctx));
  ret = rx_sample_parse_args(&ctx, argc, argv);
  if (ret < 0) return ret;

  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s: mtl_init fail\n", __func__);
    return -EIO;
  }

  uint32_t session_num = ctx.sessions;
  st22_rx_handle rx_handle[session_num];
  struct rx_st22_sample_ctx* app[session_num];

  // create and register rx session
  for (int i = 0; i < session_num; i++) {
    app[i] = (struct rx_st22_sample_ctx*)malloc(sizeof(struct rx_st22_sample_ctx));
    if (!app[i]) {
      err("%s(%d), app context malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(struct rx_st22_sample_ctx));
    app[i]->idx = i;
    st_pthread_mutex_init(&app[i]->wake_mutex, NULL);
    st_pthread_cond_init(&app[i]->wake_cond, NULL);
    app[i]->framebuff_cnt = ctx.framebuff_cnt;
    app[i]->framebuffs =
        (struct st_rx_frame*)malloc(sizeof(*app[i]->framebuffs) * app[i]->framebuff_cnt);
    if (!app[i]->framebuffs) {
      err("%s(%d), framebuffs ctx malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    for (uint16_t j = 0; j < app[i]->framebuff_cnt; j++)
      app[i]->framebuffs[j].frame = NULL;
    app[i]->framebuff_producer_idx = 0;
    app[i]->framebuff_consumer_idx = 0;

    struct st22_rx_ops ops_rx;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st22_test";
    ops_rx.priv = app[i];  // app handle register to lib
    ops_rx.num_port = 1;
    memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx.rx_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx.param.port[MTL_PORT_P]);
    // user could config the udp port in this interface.
    ops_rx.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port + i * 2;
    ops_rx.width = ctx.width;
    ops_rx.height = ctx.height;
    ops_rx.fps = ctx.fps;
    ops_rx.payload_type = ctx.payload_type;
    ops_rx.type = ST22_TYPE_FRAME_LEVEL;
    ops_rx.pack_type = ST22_PACK_CODESTREAM;

    app[i]->bytes_per_frame = ops_rx.width * ops_rx.height * bpp / 8;
    ops_rx.framebuff_cnt = app[i]->framebuff_cnt;
    /* set to the max size per frame if not CBR model */
    ops_rx.framebuff_max_size = app[i]->bytes_per_frame;
    ops_rx.notify_frame_ready = rx_st22_frame_ready;

    rx_handle[i] = st22_rx_create(ctx.st, &ops_rx);
    if (!rx_handle[i]) {
      err("%s(%d), st22_rx_create fail\n", __func__, i);
      ret = -EIO;
      goto error;
    }
    app[i]->handle = rx_handle[i];

    app[i]->stop = false;
    ret = pthread_create(&app[i]->decode_thread, NULL, st22_decode_thread, app[i]);
    if (ret < 0) {
      err("%s(%d), app_thread create fail %d\n", __func__, ret, i);
      ret = -EIO;
      goto error;
    }
  }

  // start rx
  ret = mtl_start(ctx.st);

  while (!ctx.exit) {
    sleep(1);
  }

  // stop app thread
  for (int i = 0; i < session_num; i++) {
    app[i]->stop = true;
    st_pthread_mutex_lock(&app[i]->wake_mutex);
    st_pthread_cond_signal(&app[i]->wake_cond);
    st_pthread_mutex_unlock(&app[i]->wake_mutex);
    pthread_join(app[i]->decode_thread, NULL);
    info("%s(%d), decoded frames %d\n", __func__, i, app[i]->fb_decoded);
  }

  // stop rx
  ret = mtl_stop(ctx.st);

  // check result
  for (int i = 0; i < session_num; i++) {
    if (app[i]->fb_decoded <= 0) {
      err("%s(%d), error, no decoded frames %d\n", __func__, i, app[i]->fb_decoded);
      ret = -EIO;
    }
  }

error:
  // release session
  for (int i = 0; i < session_num; i++) {
    if (!app[i]) continue;
    if (app[i]->handle) st22_rx_free(app[i]->handle);
    st_pthread_mutex_destroy(&app[i]->wake_mutex);
    st_pthread_cond_destroy(&app[i]->wake_cond);
    if (app[i]->framebuffs) free(app[i]->framebuffs);
    free(app[i]);
  }

  /* release sample(st) dev */
  if (ctx.st) {
    mtl_uninit(ctx.st);
    ctx.st = NULL;
  }
  return ret;
}
