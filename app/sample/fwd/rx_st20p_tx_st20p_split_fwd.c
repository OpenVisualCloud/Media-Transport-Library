/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "../sample_util.h"

struct tx_ctx {
  st20p_tx_handle tx_handle;
  size_t fb_offset;
  pthread_cond_t tx_wake_cond;
  pthread_mutex_t tx_wake_mutex;
  void* app;
};

struct split_fwd_sample_ctx {
  mtl_handle st;
  st20p_rx_handle rx_handle;
  struct tx_ctx tx[4];
  size_t fb_size;

  bool ready;
  bool stop;
  pthread_t fwd_thread;
  pthread_cond_t rx_wake_cond;
  pthread_mutex_t rx_wake_mutex;

  int fb_fwd;
};

static int tx_st20p_frame_available(void* priv) {
  struct tx_ctx* s = priv;
  struct split_fwd_sample_ctx* app = s->app;

  if (!app->ready) return -EIO;

  st_pthread_mutex_lock(&s->tx_wake_mutex);
  st_pthread_cond_signal(&s->tx_wake_cond);
  st_pthread_mutex_unlock(&s->tx_wake_mutex);

  return 0;
}

static int rx_st20p_frame_available(void* priv) {
  struct split_fwd_sample_ctx* s = priv;

  if (!s->ready) return -EIO;

  st_pthread_mutex_lock(&s->rx_wake_mutex);
  st_pthread_cond_signal(&s->rx_wake_cond);
  st_pthread_mutex_unlock(&s->rx_wake_mutex);

  return 0;
}

static void* tx_st20p_fwd_thread(void* args) {
  struct split_fwd_sample_ctx* s = args;
  st20p_rx_handle rx_handle = s->rx_handle;
  struct st_frame* frame;

  while (!s->stop) {
    frame = st20p_rx_get_frame(rx_handle);
    if (!frame) { /* no frame */
      st_pthread_mutex_lock(&s->rx_wake_mutex);
      if (!s->stop) st_pthread_cond_wait(&s->rx_wake_cond, &s->rx_wake_mutex);
      st_pthread_mutex_unlock(&s->rx_wake_mutex);
      continue;
    }

    for (int i = 0; i < 4; i++) {
      struct tx_ctx* tx = &s->tx[i];
      st20p_tx_handle tx_handle = tx->tx_handle;
      struct st_frame* tx_frame = NULL;

      while (!s->stop && !tx_frame) {
        tx_frame = st20p_tx_get_frame(tx_handle);
        if (!tx_frame) { /* no frame */
          st_pthread_mutex_lock(&tx->tx_wake_mutex);
          if (!s->stop) st_pthread_cond_wait(&tx->tx_wake_cond, &tx->tx_wake_mutex);
          st_pthread_mutex_unlock(&tx->tx_wake_mutex);
          continue;
        }
        uint8_t* src = frame->addr[0] + tx->fb_offset;
        uint8_t* dst = tx_frame->addr[0];
        uint32_t src_linesize = frame->linesize[0];
        uint32_t dst_linesize = tx_frame->linesize[0];
        for (int line = 0; line < tx_frame->height; line++) {
          mtl_memcpy(dst, src, dst_linesize);
          src += src_linesize;
          dst += dst_linesize;
        }

        tx_frame->tfmt = frame->tfmt;
        tx_frame->timestamp = frame->timestamp;

        st20p_tx_put_frame(tx_handle, tx_frame);
      }
    }

    st20p_rx_put_frame(rx_handle, frame);
  }

  return NULL;
}

static int split_fwd_sample_free_app(struct split_fwd_sample_ctx* app) {
  for (int i = 0; i < 4; i++) {
    if (app->tx[i].tx_handle) {
      st20p_tx_free(app->tx[i].tx_handle);
      app->tx[i].tx_handle = NULL;
    }
    st_pthread_mutex_destroy(&app->tx[i].tx_wake_mutex);
    st_pthread_cond_destroy(&app->tx[i].tx_wake_cond);
  }
  if (app->rx_handle) {
    st20p_rx_free(app->rx_handle);
    app->rx_handle = NULL;
  }
  st_pthread_mutex_destroy(&app->rx_wake_mutex);
  st_pthread_cond_destroy(&app->rx_wake_cond);

  return 0;
}

int main(int argc, char** argv) {
  int session_num = 4;
  struct st_sample_context ctx;
  int ret;

  /* init sample(st) dev */
  memset(&ctx, 0, sizeof(ctx));
  sample_parse_args(&ctx, argc, argv, true, false, false);
  ctx.sessions = session_num;
  sample_tx_queue_cnt_set(&ctx, session_num);
  ctx.param.flags |=
      MTL_FLAG_RX_SEPARATE_VIDEO_LCORE; /* use separate lcores for tx and rx */

  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s: mtl_init fail\n", __func__);
    return -EIO;
  }

  struct split_fwd_sample_ctx app;
  memset(&app, 0, sizeof(app));
  app.st = ctx.st;
  st_pthread_mutex_init(&app.rx_wake_mutex, NULL);
  st_pthread_cond_init(&app.rx_wake_cond, NULL);

  struct st20p_rx_ops ops_rx;
  memset(&ops_rx, 0, sizeof(ops_rx));
  ops_rx.name = "st20p_rx";
  ops_rx.priv = &app;  // app handle register to lib
  ops_rx.port.num_port = 1;
  memcpy(ops_rx.port.sip_addr[MTL_SESSION_PORT_P], ctx.rx_sip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops_rx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx.param.port[MTL_PORT_P]);
  ops_rx.port.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port;
  ops_rx.port.payload_type = ctx.payload_type;
  ops_rx.width = ctx.width;
  ops_rx.height = ctx.height;
  ops_rx.fps = ctx.fps;
  ops_rx.interlaced = ctx.interlaced;
  ops_rx.transport_fmt = ctx.fmt;
  ops_rx.output_fmt = ctx.output_fmt;
  ops_rx.device = ST_PLUGIN_DEVICE_AUTO;
  ops_rx.framebuff_cnt = ctx.framebuff_cnt;
  ops_rx.notify_frame_available = rx_st20p_frame_available;

  st20p_rx_handle rx_handle = st20p_rx_create(ctx.st, &ops_rx);
  if (!rx_handle) {
    err("%s, st20p_rx_create fail\n", __func__);
    ret = -EIO;
    goto error;
  }
  app.rx_handle = rx_handle;

  for (int i = 0; i < 4; i++) {
    struct tx_ctx* tx = &app.tx[i];
    tx->app = &app;
    st_pthread_mutex_init(&tx->tx_wake_mutex, NULL);
    st_pthread_cond_init(&tx->tx_wake_cond, NULL);
    struct st20p_tx_ops ops_tx;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20p_fwd";
    ops_tx.priv = tx;
    ops_tx.port.num_port = 1;
    memcpy(ops_tx.port.dip_addr[MTL_SESSION_PORT_P], ctx.fwd_dip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx.param.port[MTL_PORT_P]);
    ops_tx.port.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port + i * 2;
    ops_tx.port.payload_type = ctx.payload_type;
    ops_tx.width = ctx.width / 2;
    ops_tx.height = ctx.height / 2;
    ops_tx.fps = ctx.fps;
    ops_tx.interlaced = ctx.interlaced;
    ops_tx.input_fmt = ctx.input_fmt;
    ops_tx.transport_fmt = ctx.fmt;
    ops_tx.device = ST_PLUGIN_DEVICE_AUTO;
    ops_tx.framebuff_cnt = ctx.framebuff_cnt;
    ops_tx.flags |= ST20P_TX_FLAG_USER_TIMESTAMP;
    ops_tx.notify_frame_available = tx_st20p_frame_available;
    st20p_tx_handle tx_handle = st20p_tx_create(ctx.st, &ops_tx);
    if (!tx_handle) {
      err("%s, st20p_tx_create fail\n", __func__);
      ret = -EIO;
      goto error;
    }
    tx->tx_handle = tx_handle;
  }

  ret = pthread_create(&app.fwd_thread, NULL, tx_st20p_fwd_thread, &app);
  if (ret < 0) {
    err("%s, fwd thread create fail\n", __func__);
    ret = -EIO;
    goto error;
  }

  struct st20_pgroup st20_pg;
  st20_get_pgroup(ctx.fmt, &st20_pg);
  app.tx[0].fb_offset = 0;
  app.tx[1].fb_offset = (ctx.width / 2) * st20_pg.size / st20_pg.coverage;
  app.tx[2].fb_offset = (ctx.width / 2) * ctx.height * st20_pg.size / st20_pg.coverage;
  app.tx[3].fb_offset = app.tx[2].fb_offset + app.tx[1].fb_offset;
  app.fb_size = ctx.width * ctx.height * st20_pg.size / st20_pg.coverage;

  app.ready = true;

  // start dev
  ret = mtl_start(ctx.st);

  while (!ctx.exit) {
    sleep(1);
  }

  // stop fwd thread
  app.stop = true;
  st_pthread_mutex_lock(&app.rx_wake_mutex);
  st_pthread_cond_signal(&app.rx_wake_cond);
  st_pthread_mutex_unlock(&app.rx_wake_mutex);
  for (int i = 0; i < 4; i++) {
    struct tx_ctx* tx = &app.tx[i];
    st_pthread_mutex_lock(&tx->tx_wake_mutex);
    st_pthread_cond_signal(&tx->tx_wake_cond);
    st_pthread_mutex_unlock(&tx->tx_wake_mutex);
  }
  pthread_join(app.fwd_thread, NULL);

  // stop dev
  ret = mtl_stop(ctx.st);
  info("%s, fb_fwd %d\n", __func__, app.fb_fwd);
  app.ready = false;

  // check result
  if (app.fb_fwd <= 0) {
    err("%s, error, no fwd frames %d\n", __func__, app.fb_fwd);
    ret = -EIO;
  }

error:
  // release session
  split_fwd_sample_free_app(&app);

  /* release sample(st) dev */
  if (ctx.st) {
    mtl_uninit(ctx.st);
    ctx.st = NULL;
  }
  return ret;
}