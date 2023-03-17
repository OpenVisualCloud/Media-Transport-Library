/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "../sample_util.h"

struct tv_sample_context {
  int idx;
  int fb_send;
  st20_tx_handle handle;
  struct st20_tx_ops ops;

  bool stop;
  pthread_t app_thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  int framebuff_size;
  uint16_t framebuff_cnt;
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_consumer_idx;
  struct st_tx_frame* framebuffs;

  mtl_dma_mem_handle dma_mem;
};

static int tx_video_next_frame(void* priv, uint16_t* next_frame_idx,
                               struct st20_tx_frame_meta* meta) {
  struct tv_sample_context* s = priv;
  int ret;
  uint16_t consumer_idx = s->framebuff_consumer_idx;
  struct st_tx_frame* framebuff = &s->framebuffs[consumer_idx];

  if (!s->handle) return -EIO; /* not ready */

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
  struct tv_sample_context* s = priv;
  int ret;
  struct st_tx_frame* framebuff = &s->framebuffs[frame_idx];

  if (!s->handle) return -EIO; /* not ready */

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

static void tx_video_build_frame(struct tv_sample_context* s, void* frame,
                                 size_t frame_size) {
  /* call the real build here, sample just sleep */
  st_usleep(10 * 1000);
}

static void* tx_video_frame_thread(void* arg) {
  struct tv_sample_context* s = arg;
  uint16_t producer_idx;
  struct st_tx_frame* framebuff;

  info("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    st_pthread_mutex_lock(&s->wake_mutex);
    producer_idx = s->framebuff_producer_idx;
    framebuff = &s->framebuffs[producer_idx];
    if (ST_TX_FRAME_FREE != framebuff->stat) {
      /* not in free */
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }
    st_pthread_mutex_unlock(&s->wake_mutex);
    if (s->ops.flags & ST20_TX_FLAG_EXT_FRAME) { /* ext frame mode */
      struct st20_ext_frame ext_frame;
      ext_frame.buf_addr =
          mtl_dma_mem_addr(s->dma_mem) + producer_idx * s->framebuff_size;
      ext_frame.buf_iova =
          mtl_dma_mem_iova(s->dma_mem) + producer_idx * s->framebuff_size;
      ext_frame.buf_len = s->framebuff_size;
      st20_tx_set_ext_frame(s->handle, producer_idx, &ext_frame);
    } else {
      void* frame_addr = st20_tx_get_framebuffer(s->handle, producer_idx);
      tx_video_build_frame(s, frame_addr, s->framebuff_size);
    }
    st_pthread_mutex_lock(&s->wake_mutex);
    framebuff->size = s->framebuff_size;
    framebuff->stat = ST_TX_FRAME_READY;
    /* point to next */
    producer_idx++;
    if (producer_idx >= s->framebuff_cnt) producer_idx = 0;
    s->framebuff_producer_idx = producer_idx;
    st_pthread_mutex_unlock(&s->wake_mutex);
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
  memset(tx_handle, 0, sizeof(tx_handle));
  struct tv_sample_context* app[session_num];
  memset(app, 0, sizeof(app));

  // create and register tx session
  for (int i = 0; i < session_num; i++) {
    app[i] = (struct tv_sample_context*)malloc(sizeof(struct tv_sample_context));
    if (!app[i]) {
      err("%s(%d), app context malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(*app[i]));
    st_pthread_mutex_init(&app[i]->wake_mutex, NULL);
    st_pthread_cond_init(&app[i]->wake_cond, NULL);
    app[i]->idx = i;
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
    if (ctx.ext_frame) ops_tx.flags |= ST20_TX_FLAG_EXT_FRAME;
    ops_tx.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port + i;  // udp port
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.type = ST20_TYPE_FRAME_LEVEL;
    ops_tx.width = ctx.width;
    ops_tx.height = ctx.height;
    ops_tx.fps = ctx.fps;
    ops_tx.fmt = ctx.fmt;
    ops_tx.payload_type = ctx.payload_type;
    ops_tx.framebuff_cnt = app[i]->framebuff_cnt;
    // app register non-block func, app could get a frame to send to lib
    ops_tx.get_next_frame = tx_video_next_frame;
    // app register non-block func, app could get the frame tx done
    ops_tx.notify_frame_done = tx_video_frame_done;
    tx_handle[i] = st20_tx_create(ctx.st, &ops_tx);
    if (!tx_handle[i]) {
      err("%s(%d), st20_tx_create fail\n", __func__, i);
      ret = -EIO;
      goto error;
    }
    app[i]->ops = ops_tx;
    app[i]->framebuff_size = st20_tx_get_framebuffer_size(tx_handle[i]);
    if (ops_tx.flags & ST20_TX_FLAG_EXT_FRAME) {
      /* how user allocate framebuffers and map to iova */
      /* the memory malloc layout:
      |____________________|////////// valid framebuffers ////////|____|___|
      |                    |<--------------- size --------------->|    |   |
      |                    |<---------------- iova_size -------------->|   |
      |<---------------------- alloc_size (pgsz multiple)----------------->|
      *alloc_addr          *addr(pg aligned)
      */
      size_t fb_size = app[i]->framebuff_size * app[i]->framebuff_cnt;
      /* alloc enough memory to hold framebuffers and map to iova */
      mtl_dma_mem_handle dma_mem = mtl_dma_mem_alloc(ctx.st, fb_size);
      if (!dma_mem) {
        err("%s(%d), dma mem alloc/map fail\n", __func__, i);
        ret = -EIO;
        goto error;
      }
      app[i]->dma_mem = dma_mem;
    }

    app[i]->handle = tx_handle[i];

    app[i]->stop = false;
    ret = pthread_create(&app[i]->app_thread, NULL, tx_video_frame_thread, app[i]);
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

    if (app[i]->dma_mem) mtl_dma_mem_free(ctx.st, app[i]->dma_mem);
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
