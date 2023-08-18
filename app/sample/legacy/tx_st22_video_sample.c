/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "../sample_util.h"

struct tx_st22_sample_ctx {
  int idx;
  st22_tx_handle handle;
  size_t bytes_per_frame;

  uint16_t framebuff_cnt;
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_consumer_idx;
  struct st_tx_frame* framebuffs;

  bool stop;
  pthread_t encode_thread;

  int fb_send;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;
};

static int tx_st22_next_frame(void* priv, uint16_t* next_frame_idx,
                              struct st22_tx_frame_meta* meta) {
  struct tx_st22_sample_ctx* s = priv;
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
  }
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return ret;
}

static int tx_st22_frame_done(void* priv, uint16_t frame_idx,
                              struct st22_tx_frame_meta* meta) {
  struct tx_st22_sample_ctx* s = priv;
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

static void st22_encode_frame(struct tx_st22_sample_ctx* s, void* codestream_addr,
                              size_t max_codestream_size, size_t* codestream_size) {
  /* call the real encoding here, sample just sleep */
  st_usleep(10 * 1000);
  *codestream_size = s->bytes_per_frame;
}

static void* st22_encode_thread(void* arg) {
  struct tx_st22_sample_ctx* s = arg;
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

    void* frame_addr = st22_tx_get_fb_addr(s->handle, producer_idx);
    size_t max_framesize = s->bytes_per_frame;
    size_t codestream_size = s->bytes_per_frame;
    st22_encode_frame(s, frame_addr, max_framesize, &codestream_size);

    st_pthread_mutex_lock(&s->wake_mutex);
    framebuff->size = codestream_size;
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
  int bpp = 3;
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
  st22_tx_handle tx_handle[session_num];
  struct tx_st22_sample_ctx* app[session_num];

  // create and register tx session
  for (int i = 0; i < session_num; i++) {
    app[i] = (struct tx_st22_sample_ctx*)malloc(sizeof(struct tx_st22_sample_ctx));
    if (!app[i]) {
      err("%s(%d), app context malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(struct tx_st22_sample_ctx));
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
    }

    struct st22_tx_ops ops_tx;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st22_test";
    ops_tx.priv = app[i];  // app handle register to lib
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], ctx.tx_dip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx.param.port[MTL_PORT_P]);
    ops_tx.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port + i * 2;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.width = ctx.width;
    ops_tx.height = ctx.height;
    ops_tx.fps = ctx.fps;
    ops_tx.payload_type = ctx.payload_type;
    ops_tx.type = ST22_TYPE_FRAME_LEVEL;
    ops_tx.pack_type = ST22_PACK_CODESTREAM;

    app[i]->bytes_per_frame = ops_tx.width * ops_tx.height * bpp / 8;
    ops_tx.framebuff_cnt = app[i]->framebuff_cnt;
    /* set to the max size per frame if not CBR model */
    ops_tx.framebuff_max_size = app[i]->bytes_per_frame;
    ops_tx.get_next_frame = tx_st22_next_frame;
    ops_tx.notify_frame_done = tx_st22_frame_done;

    tx_handle[i] = st22_tx_create(ctx.st, &ops_tx);
    if (!tx_handle[i]) {
      err("%s(%d), st20_tx_create fail\n", __func__, i);
      ret = -EIO;
      goto error;
    }
    app[i]->handle = tx_handle[i];

    app[i]->stop = false;
    ret = pthread_create(&app[i]->encode_thread, NULL, st22_encode_thread, app[i]);
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
    pthread_join(app[i]->encode_thread, NULL);
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
    if (app[i]->handle) st22_tx_free(app[i]->handle);
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
