/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "sample_util.h"

struct st20r_sample_ctx {
  int idx;
  int fb_rec;
  int stat_fb_rec;
  st20r_rx_handle handle;

  bool stop;
  pthread_t app_thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  uint16_t framebuff_cnt;
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_consumer_idx;
  struct st_rx_frame* framebuffs;
};

static int rx_video_enqueue_frame(struct st20r_sample_ctx* s, void* frame, size_t size) {
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

static int rx_video_frame_ready(void* priv, void* frame,
                                struct st20_rx_frame_meta* meta) {
  struct st20r_sample_ctx* s = (struct st20r_sample_ctx*)priv;

  if (!s->handle) return -EIO;

  /* incomplete frame */
  if (!st_is_frame_complete(meta->status)) {
    st20r_rx_put_frame(s->handle, frame);
    return 0;
  }

  st_pthread_mutex_lock(&s->wake_mutex);
  int ret = rx_video_enqueue_frame(s, frame, meta->frame_total_size);
  if (ret < 0) {
    err("%s(%d), frame %p dropped\n", __func__, s->idx, frame);
    /* free the queue */
    st20r_rx_put_frame(s->handle, frame);
    st_pthread_mutex_unlock(&s->wake_mutex);
    return ret;
  }
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

static void rx_video_consume_frame(struct st20r_sample_ctx* s, void* frame,
                                   size_t frame_size) {
  dbg("%s(%d), frame %p\n", __func__, s->idx, frame);

  /* call the real consumer here, sample just sleep */
  st_usleep(10 * 1000);
  s->fb_rec++;
}

static void* rx_video_frame_thread(void* arg) {
  struct st20r_sample_ctx* s = arg;
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
    st20r_rx_put_frame(s->handle, framebuff->frame);
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
  st_sample_init(&ctx, argc, argv, true, false);
  ctx.param.num_ports = 2; /* force to 2 */
  ret = st_sample_start(&ctx);
  if (ret < 0) return ret;

  uint32_t session_num = ctx.sessions;
  st20r_rx_handle rx_handle[session_num];
  struct st20r_sample_ctx* app[session_num];
  uint64_t sart_time_ns;
  int loop = 0;

  // create and register rx session
  for (int i = 0; i < session_num; i++) {
    app[i] = (struct st20r_sample_ctx*)malloc(sizeof(struct st20r_sample_ctx));
    if (!app[i]) {
      err("%s(%d), app context malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(struct st20r_sample_ctx));
    app[i]->idx = i;
    app[i]->stop = false;
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

    struct st20r_rx_ops ops_rx;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20r_test";
    ops_rx.priv = app[i];  // app handle register to lib
    ops_rx.num_port = 2;
    memcpy(ops_rx.sip_addr[MTL_PORT_P], ctx.rx_sip_addr[MTL_PORT_P], MTL_IP_ADDR_LEN);
    memcpy(ops_rx.sip_addr[MTL_PORT_R], ctx.rx_sip_addr[MTL_PORT_R], MTL_IP_ADDR_LEN);
    strncpy(ops_rx.port[MTL_PORT_P], ctx.param.port[MTL_PORT_P], MTL_PORT_MAX_LEN);
    strncpy(ops_rx.port[MTL_PORT_R], ctx.param.port[MTL_PORT_R], MTL_PORT_MAX_LEN);
    ops_rx.udp_port[MTL_PORT_P] = ctx.udp_port + i;
    ops_rx.udp_port[MTL_PORT_R] = ctx.udp_port + i;
    ops_rx.width = ctx.width;
    ops_rx.height = ctx.height;
    ops_rx.fps = ctx.fps;
    ops_rx.fmt = ctx.fmt;
    ops_rx.framebuff_cnt = app[i]->framebuff_cnt;
    ops_rx.payload_type = ctx.payload_type;
    ops_rx.notify_frame_ready = rx_video_frame_ready;
    if (ctx.hdr_split) ops_rx.flags |= ST20R_RX_FLAG_HDR_SPLIT;

    rx_handle[i] = st20r_rx_create(ctx.st, &ops_rx);
    if (!rx_handle[i]) {
      err("%s(%d), rx create fail\n", __func__, i);
      ret = -EIO;
      goto error;
    }
    app[i]->handle = rx_handle[i];

    ret = pthread_create(&app[i]->app_thread, NULL, rx_video_frame_thread, app[i]);
    if (ret < 0) {
      err("%s(%d), app_thread create fail %d\n", __func__, ret, i);
      goto error;
    }
  }

  // start dev
  ret = mtl_start(ctx.st);

  // rx run
  sart_time_ns = mtl_ptp_read_time(ctx.st);
  while (!ctx.exit) {
    sleep(1);
    loop++;
    if (0 == (loop % 10)) {
      uint64_t end_time_ns = mtl_ptp_read_time(ctx.st);
      double time_sec = (double)(end_time_ns - sart_time_ns) / (1000 * 1000 * 1000);
      for (int i = 0; i < session_num; i++) {
        int fb_rec = app[i]->fb_rec - app[i]->stat_fb_rec;
        double framerate = fb_rec / time_sec;
        info("%s(%d), fps %f, %d frame received\n", __func__, i, framerate, fb_rec);
        app[i]->stat_fb_rec = app[i]->fb_rec;
      }
      sart_time_ns = end_time_ns;
    }
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
    if (app[i]->handle) st20r_rx_free(app[i]->handle);
    st_pthread_mutex_destroy(&app[i]->wake_mutex);
    st_pthread_cond_destroy(&app[i]->wake_cond);
    if (app[i]->framebuffs) free(app[i]->framebuffs);
    free(app[i]);
  }

  /* release sample(st) dev */
  st_sample_uinit(&ctx);
  return ret;
}
