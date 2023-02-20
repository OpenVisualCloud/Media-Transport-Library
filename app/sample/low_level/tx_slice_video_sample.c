/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "../sample_util.h"

struct tv_slice_sample_ctx {
  int idx;
  int fb_send;
  void* handle;

  bool stop;
  pthread_t app_thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  int framebuff_size;
  uint16_t framebuff_cnt;
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_consumer_idx;
  struct st_tx_frame* framebuffs;

  int lines_per_slice;
  int height;
};

static int tx_video_next_frame(void* priv, uint16_t* next_frame_idx,
                               struct st20_tx_frame_meta* meta) {
  struct tv_slice_sample_ctx* s = priv;
  int ret;
  uint16_t consumer_idx = s->framebuff_consumer_idx;
  struct st_tx_frame* framebuff = &s->framebuffs[consumer_idx];

  st_pthread_mutex_lock(&s->wake_mutex);
  if (ST_TX_FRAME_READY == framebuff->stat) {
    dbg("%s(%d), next frame idx %u\n", __func__, s->idx, consumer_idx);
    ret = 0;
    framebuff->stat = ST_TX_FRAME_IN_TRANSMITTING;
    *next_frame_idx = consumer_idx;
    /* point to next */
    consumer_idx++;
    if (consumer_idx >= s->framebuff_cnt) consumer_idx = 0;
    s->framebuff_consumer_idx = consumer_idx;
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
  struct tv_slice_sample_ctx* s = priv;
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

static int tx_video_frame_lines_ready(void* priv, uint16_t frame_idx,
                                      struct st20_tx_slice_meta* meta) {
  struct tv_slice_sample_ctx* s = priv;
  struct st_tx_frame* framebuff = &s->framebuffs[frame_idx];

  st_pthread_mutex_lock(&s->wake_mutex);
  framebuff->slice_trigger = true;
  meta->lines_ready = framebuff->lines_ready;
  dbg("%s(%d), frame %u ready %d lines\n", __func__, s->idx, frame_idx,
      framebuff->lines_ready);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

static void tx_video_build_slice(struct tv_slice_sample_ctx* s,
                                 struct st_tx_frame* framebuff, void* frame_addr) {
  int lines_build = 0;
  int slices = (s->height / s->lines_per_slice) + 1;

  /* simulate the timing */
  while (!framebuff->slice_trigger) {
    st_usleep(1);
  }
  lines_build += s->lines_per_slice;
  st_pthread_mutex_lock(&s->wake_mutex);
  framebuff->lines_ready = lines_build;
  st_pthread_mutex_unlock(&s->wake_mutex);

  while (lines_build < s->height) {
    /* call the real build here, sample just sleep */
    st_usleep(10 * 1000 / slices);

    st_pthread_mutex_lock(&s->wake_mutex);
    lines_build += s->lines_per_slice;
    if (lines_build > s->height) lines_build = s->height;
    framebuff->lines_ready = lines_build;
    st_pthread_mutex_unlock(&s->wake_mutex);
  }
}

static void* tx_video_slice_thread(void* arg) {
  struct tv_slice_sample_ctx* s = arg;
  uint16_t producer_idx;
  uint16_t consumer_idx;
  struct st_tx_frame* framebuff;

  dbg("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    st_pthread_mutex_lock(&s->wake_mutex);
    producer_idx = s->framebuff_producer_idx;
    consumer_idx = s->framebuff_consumer_idx;
    framebuff = &s->framebuffs[producer_idx];
    /* limit the producer to simulate the slice timing */
    if ((producer_idx != consumer_idx) || (ST_TX_FRAME_FREE != framebuff->stat)) {
      /* not in free */
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }

    dbg("%s(%d), producer_idx %d consumer_idx %d\n", __func__, s->idx, producer_idx,
        consumer_idx);
    void* frame_addr = st20_tx_get_framebuffer(s->handle, producer_idx);

    framebuff->size = s->framebuff_size;
    framebuff->lines_ready = 0;
    framebuff->slice_trigger = false;
    framebuff->stat = ST_TX_FRAME_READY;
    /* point to next */
    producer_idx++;
    if (producer_idx >= s->framebuff_cnt) producer_idx = 0;
    s->framebuff_producer_idx = producer_idx;
    st_pthread_mutex_unlock(&s->wake_mutex);

    tx_video_build_slice(s, framebuff, frame_addr);
  }
  info("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

int main(int argc, char** argv) {
  struct st_sample_context ctx;
  int ret;

  /* init sample(st) dev */
  memset(&ctx, 0, sizeof(ctx));
  ret = tx_sample_parse_args(&ctx, argc, argv);
  if (ret < 0) return ret;

  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s: mtl_init fail\n", __func__);
    return -EIO;
  }

  uint32_t session_num = ctx.sessions;
  st20_tx_handle tx_handle[session_num];
  struct tv_slice_sample_ctx* app[session_num];

  // create and register tx session
  for (int i = 0; i < session_num; i++) {
    app[i] = (struct tv_slice_sample_ctx*)malloc(sizeof(struct tv_slice_sample_ctx));
    if (!app[i]) {
      err("%s(%d), app context malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(struct tv_slice_sample_ctx));
    app[i]->idx = i;
    st_pthread_mutex_init(&app[i]->wake_mutex, NULL);
    st_pthread_cond_init(&app[i]->wake_cond, NULL);
    app[i]->framebuff_cnt = ctx.framebuff_cnt;
    app[i]->framebuffs =
        (struct st_tx_frame*)malloc(sizeof(*app[i]->framebuffs) * app[i]->framebuff_cnt);
    if (!app[i]->framebuffs) {
      err("%s(%d), framebuffs ctx malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    for (uint16_t j = 0; j < app[i]->framebuff_cnt; j++) {
      app[i]->framebuffs[j].stat = ST_TX_FRAME_FREE;
      app[i]->framebuffs[j].lines_ready = 0;
    }

    struct st20_tx_ops ops_tx;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_tx";
    ops_tx.priv = app[i];  // app handle register to lib
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx.tx_dip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    strncpy(ops_tx.port[MTL_SESSION_PORT_P], ctx.param.port[MTL_PORT_P],
            MTL_PORT_MAX_LEN);
    ops_tx.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port + i;  // udp port
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.type = ST20_TYPE_SLICE_LEVEL;
    ops_tx.width = ctx.width;
    ops_tx.height = ctx.height;
    ops_tx.fps = ctx.fps;
    ops_tx.fmt = ctx.fmt;
    ops_tx.payload_type = ctx.payload_type;
    ops_tx.framebuff_cnt = app[i]->framebuff_cnt;
    // app regist non-block func, app could get a frame to send to lib
    ops_tx.get_next_frame = tx_video_next_frame;
    // app regist non-block func, app could get the frame tx done
    ops_tx.notify_frame_done = tx_video_frame_done;
    ops_tx.query_frame_lines_ready = tx_video_frame_lines_ready;
    tx_handle[i] = st20_tx_create(ctx.st, &ops_tx);
    if (!tx_handle[i]) {
      err("%s(%d), st20_tx_create fail\n", __func__, i);
      ret = -EIO;
      goto error;
    }
    app[i]->handle = tx_handle[i];
    app[i]->stop = false;

    app[i]->framebuff_size = st20_tx_get_framebuffer_size(tx_handle[i]);
    app[i]->height = ops_tx.height;
    app[i]->lines_per_slice = app[i]->height / 30;
    ret = pthread_create(&app[i]->app_thread, NULL, tx_video_slice_thread, app[i]);
    if (ret < 0) {
      err("%s(%d), app_thread create fail %d\n", __func__, ret, i);
      ret = -EIO;
      goto error;
    }
  }

  // start tx
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
    info("%s(%d), sent frames %d\n", __func__, i, app[i]->fb_send);
  }

  // stop tx
  ret = mtl_stop(ctx.st);

  // check result
  for (int i = 0; i < session_num; i++) {
    if (app[i]->fb_send <= 0) {
      err("%s(%d), error, no sent frames %d\n", __func__, i, app[i]->fb_send);
      ret = -EIO;
    }
  }

error:
  // release session
  for (int i = 0; i < session_num; i++) {
    if (!app[i]) continue;
    if (app[i]->handle) st20_tx_free(app[i]->handle);
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
