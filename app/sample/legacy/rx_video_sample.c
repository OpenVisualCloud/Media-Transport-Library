/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "../sample_util.h"

struct rv_sample_context {
  int idx;
  int fb_rec;
  st20_rx_handle handle;

  bool stop;
  pthread_t app_thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  uint16_t framebuff_cnt;
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_consumer_idx;
  struct st_rx_frame *framebuffs;

  mtl_dma_mem_handle dma_mem;
  struct st20_ext_frame *ext_frames;
};

static int rx_video_enqueue_frame(struct rv_sample_context *s, void *frame, size_t size) {
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

static int rx_video_frame_ready(void *priv, void *frame,
                                struct st20_rx_frame_meta *meta) {
  struct rv_sample_context *s = (struct rv_sample_context *)priv;

  if (!s->handle) return -EIO;

  if (meta->user_meta) {
    const struct st_frame_user_meta *user_meta = meta->user_meta;
    if (meta->user_meta_size != sizeof(*user_meta)) {
      err("%s(%d), user_meta_size wrong\n", __func__, s->idx);
    }
    info("%s(%d), user_meta %d %s\n", __func__, s->idx, user_meta->idx, user_meta->dummy);
  }

  /* incomplete frame */
  if (!st_is_frame_complete(meta->status)) {
    st20_rx_put_framebuff(s->handle, frame);
    return 0;
  }

  st_pthread_mutex_lock(&s->wake_mutex);
  int ret = rx_video_enqueue_frame(s, frame, meta->frame_total_size);
  if (ret < 0) {
    err("%s(%d), frame %p dropped\n", __func__, s->idx, frame);
    /* free the queue */
    st20_rx_put_framebuff(s->handle, frame);
    st_pthread_mutex_unlock(&s->wake_mutex);
    return ret;
  }
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

static void rx_video_consume_frame(struct rv_sample_context *s, void *frame,
                                   size_t frame_size) {
  MTL_MAY_UNUSED(frame);
  MTL_MAY_UNUSED(frame_size);
  dbg("%s(%d), frame %p\n", __func__, s->idx, frame);

  /* call the real consumer here, sample just sleep */
  st_usleep(10 * 1000);
  s->fb_rec++;
}

static void *rx_video_frame_thread(void *arg) {
  struct rv_sample_context *s = arg;
  int idx = s->idx;
  int consumer_idx;
  struct st_rx_frame *framebuff;

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
    rx_video_consume_frame(s, framebuff->frame, framebuff->size);
    st20_rx_put_framebuff(s->handle, framebuff->frame);
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

int main(int argc, char **argv) {
  struct st_sample_context ctx;
  int ret;

  /* init sample(st) dev */
  memset(&ctx, 0, sizeof(ctx));
  ret = rx_sample_parse_args(&ctx, argc, argv);
  if (ret < 0) return ret;

  /* enable auto start/stop */
  ctx.param.flags |= MTL_FLAG_DEV_AUTO_START_STOP;
  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s: mtl_init fail\n", __func__);
    return -EIO;
  }

  uint32_t session_num = ctx.sessions;
  st20_rx_handle rx_handle[session_num];
  struct rv_sample_context *app[session_num];
  // create and register rx session
  for (int i = 0; i < session_num; i++) {
    app[i] = (struct rv_sample_context *)malloc(sizeof(struct rv_sample_context));
    if (!app[i]) {
      err("%s(%d), app context malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(struct rv_sample_context));
    app[i]->idx = i;
    app[i]->framebuff_cnt = ctx.framebuff_cnt;
    st_pthread_mutex_init(&app[i]->wake_mutex, NULL);
    st_pthread_cond_init(&app[i]->wake_cond, NULL);
    app[i]->framebuffs =
        (struct st_rx_frame *)malloc(sizeof(*app[i]->framebuffs) * app[i]->framebuff_cnt);
    if (!app[i]->framebuffs) {
      err("%s(%d), framebuffs ctx malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    for (uint16_t j = 0; j < app[i]->framebuff_cnt; j++)
      app[i]->framebuffs[j].frame = NULL;
    app[i]->framebuff_producer_idx = 0;
    app[i]->framebuff_consumer_idx = 0;

    struct st20_rx_ops ops_rx;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20_rx";
    ops_rx.priv = app[i];  // app handle register to lib
    ops_rx.num_port = 1;
    memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], ctx.rx_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx.param.port[MTL_PORT_P]);
    ops_rx.udp_port[MTL_SESSION_PORT_P] =
        ctx.udp_port + i * 2;  // user config the udp port.
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.type = ST20_TYPE_FRAME_LEVEL;
    ops_rx.width = ctx.width;
    ops_rx.height = ctx.height;
    ops_rx.fps = ctx.fps;
    ops_rx.interlaced = ctx.interlaced;
    ops_rx.fmt = ctx.fmt;
    ops_rx.framebuff_cnt = app[i]->framebuff_cnt;
    ops_rx.payload_type = ctx.payload_type;
    // app register non-block func, app get a frame ready notification info by this cb
    ops_rx.notify_frame_ready = rx_video_frame_ready;

    if (ops_rx.ext_frames) {
      app[i]->ext_frames = (struct st20_ext_frame *)malloc(sizeof(*app[i]->ext_frames) *
                                                           app[i]->framebuff_cnt);
      if (!app[i]->ext_frames) {
        err("%s(%d), ext_frames malloc fail\n", __func__, i);
        ret = -ENOMEM;
        goto error;
      }
      size_t framebuff_size = st20_frame_size(ops_rx.fmt, ops_rx.width, ops_rx.height);
      size_t fb_size = framebuff_size * app[i]->framebuff_cnt;
      /* alloc enough memory to hold framebuffers and map to iova */
      mtl_dma_mem_handle dma_mem = mtl_dma_mem_alloc(ctx.st, fb_size);
      if (!dma_mem) {
        err("%s(%d), dma mem alloc/map fail\n", __func__, i);
        ret = -ENOMEM;
        goto error;
      }
      app[i]->dma_mem = dma_mem;

      for (int j = 0; j < app[i]->framebuff_cnt; ++j) {
        app[i]->ext_frames[j].buf_addr = mtl_dma_mem_addr(dma_mem) + j * framebuff_size;
        app[i]->ext_frames[j].buf_iova = mtl_dma_mem_iova(dma_mem) + j * framebuff_size;
        app[i]->ext_frames[j].buf_len = framebuff_size;
      }
      ops_rx.ext_frames = app[i]->ext_frames;
    }

    rx_handle[i] = st20_rx_create(ctx.st, &ops_rx);
    if (!rx_handle[i]) {
      err("%s(%d), st20_rx_create fail\n", __func__, i);
      ret = -EIO;
      goto error;
    }
    app[i]->handle = rx_handle[i];
    app[i]->stop = false;

    ret = pthread_create(&app[i]->app_thread, NULL, rx_video_frame_thread, app[i]);
    if (ret < 0) {
      err("%s(%d), app_thread create fail %d\n", __func__, ret, i);
      ret = -EIO;
      goto error;
    }
  }

  while (!ctx.exit) {
    sleep(1);
  }

  // stop app thread
  for (int i = 0; i < session_num; i++) {
    app[i]->stop = true;
    st_pthread_mutex_lock(&app[i]->wake_mutex);
    st_pthread_cond_signal(&app[i]->wake_cond);
    st_pthread_mutex_unlock(&app[i]->wake_mutex);
    pthread_join(app[i]->app_thread, NULL);
    info("%s(%d), received frames %d\n", __func__, i, app[i]->fb_rec);
  }

  // check result
  for (int i = 0; i < session_num; i++) {
    if (app[i]->fb_rec <= 0) {
      err("%s(%d), error, no received frames %d\n", __func__, i, app[i]->fb_rec);
      ret = -EIO;
    }
  }

error:
  // release session
  for (int i = 0; i < session_num; i++) {
    if (!app[i]) continue;
    if (app[i]->handle) st20_rx_free(app[i]->handle);
    st_pthread_mutex_destroy(&app[i]->wake_mutex);
    st_pthread_cond_destroy(&app[i]->wake_cond);

    if (app[i]->dma_mem) mtl_dma_mem_free(ctx.st, app[i]->dma_mem);
    if (app[i]->framebuffs) free(app[i]->framebuffs);
    if (app[i]->ext_frames) free(app[i]->ext_frames);
    free(app[i]);
  }

  /* release sample(st) dev */
  if (ctx.st) {
    mtl_uninit(ctx.st);
    ctx.st = NULL;
  }
  return ret;
}
