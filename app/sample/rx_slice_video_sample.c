/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "sample_util.h"

struct rv_slice_sample_ctx {
  int idx;
  int fb_rec;
  int slice_rec;
  st20_rx_handle handle;

  bool stop;
  pthread_t app_thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  uint16_t framebuff_cnt;
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_consumer_idx;
  struct st_rx_frame* framebuffs;
};

static int rx_video_enqueue_frame(struct rv_slice_sample_ctx* s, void* frame,
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

static int rx_video_slice_ready(void* priv, void* frame,
                                struct st20_rx_slice_meta* meta) {
  struct rv_slice_sample_ctx* s = (struct rv_slice_sample_ctx*)priv;

  if (!s->handle) return -EIO;

  // frame_recv_lines in meta indicate the ready lines for current frame
  // add the slice handling logic here

  s->slice_rec++;
  return 0;
}

static int rx_video_frame_ready(void* priv, void* frame,
                                struct st20_rx_frame_meta* meta) {
  struct rv_slice_sample_ctx* s = (struct rv_slice_sample_ctx*)priv;

  if (!s->handle) return -EIO;

  /* incomplete frame */
  if (!st_is_frame_complete(meta->status)) {
    st20_rx_put_framebuff(s->handle, frame);
    return 0;
  }

  st_pthread_mutex_lock(&s->wake_mutex);
  int ret = rx_video_enqueue_frame(s, frame, meta->frame_total_size);
  if (ret < 0) {
    info("%s(%d), frame %p dropped\n", __func__, s->idx, frame);
    /* free the queue */
    st20_rx_put_framebuff(s->handle, frame);
    st_pthread_mutex_unlock(&s->wake_mutex);
    return ret;
  }
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

static void rx_video_consume_frame(struct rv_slice_sample_ctx* s, void* frame,
                                   size_t frame_size) {
  dbg("%s(%d), frame %p\n", __func__, s->idx, frame);

  /* call the real consumer here, sample just sleep */
  st_usleep(10 * 1000);
  s->fb_rec++;
}

static void* rx_video_frame_thread(void* arg) {
  struct rv_slice_sample_ctx* s = arg;
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

int main(int argc, char** argv) {
  struct st_sample_context ctx;
  int ret;

  /* init sample(st) dev */
  ret = st_sample_rx_init(&ctx, argc, argv);
  if (ret < 0) return ret;

  uint32_t session_num = ctx.sessions;
  st20_rx_handle rx_handle[session_num];
  struct rv_slice_sample_ctx* app[session_num];

  // create and register rx session
  for (int i = 0; i < session_num; i++) {
    app[i] = (struct rv_slice_sample_ctx*)malloc(sizeof(struct rv_slice_sample_ctx));
    if (!app[i]) {
      err("%s(%d), app context malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(struct rv_slice_sample_ctx));
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

    struct st20_rx_ops ops_rx;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20_test";
    ops_rx.priv = app[i];  // app handle register to lib
    ops_rx.num_port = 1;
    memcpy(ops_rx.sip_addr[MTL_PORT_P], ctx.rx_sip_addr[MTL_PORT_P], MTL_IP_ADDR_LEN);
    strncpy(ops_rx.port[MTL_PORT_P], ctx.param.port[MTL_PORT_P], MTL_PORT_MAX_LEN);
    ops_rx.udp_port[MTL_PORT_P] = ctx.udp_port + i;  // user config the udp port.
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.type = ST20_TYPE_SLICE_LEVEL;
    ops_rx.width = ctx.width;
    ops_rx.height = ctx.height;
    ops_rx.fps = ctx.fps;
    ops_rx.fmt = ctx.fmt;
    ops_rx.framebuff_cnt = app[i]->framebuff_cnt;
    ops_rx.payload_type = ctx.payload_type;
    ops_rx.flags = ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
    ops_rx.notify_slice_ready = rx_video_slice_ready;
    // app regist non-block func, app get a frame ready notification info by this cb
    ops_rx.notify_frame_ready = rx_video_frame_ready;
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

  // start dev
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
    pthread_join(app[i]->app_thread, NULL);
    info("%s(%d), received frames %d\n", __func__, i, app[i]->fb_rec);
  }

  // stop rx
  ret = mtl_stop(ctx.st);

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
    if (app[i]->framebuffs) free(app[i]->framebuffs);
    free(app[i]);
  }

  /* release sample(st) dev */
  st_sample_uinit(&ctx);
  return ret;
}
