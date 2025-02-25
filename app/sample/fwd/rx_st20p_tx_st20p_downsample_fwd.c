/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "../sample_util.h"

struct rx_st20p_tx_st20p_sample_ctx {
  mtl_handle st;
  int idx;
  st20p_rx_handle rx_handle;
  st20p_tx_handle tx_handle;

  bool stop;
  bool ready;
  pthread_t fwd_thread;

  int fb_fwd;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  struct st20_pgroup st20_pg;
};

static int tx_st20p_frame_available(void *priv) {
  struct rx_st20p_tx_st20p_sample_ctx *s = priv;

  if (!s->ready) return -EIO;

  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

static int rx_st20p_frame_available(void *priv) {
  struct rx_st20p_tx_st20p_sample_ctx *s = priv;

  if (!s->ready) return -EIO;

  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);

  return 0;
}

static void fwd_st20_consume_frame(struct rx_st20p_tx_st20p_sample_ctx *s,
                                   struct st_frame *frame) {
  st20p_tx_handle tx_handle = s->tx_handle;
  struct st_frame *tx_frame;

  while (!s->stop) {
    tx_frame = st20p_tx_get_frame(tx_handle);
    if (!tx_frame) { /* no frame */
      st_pthread_mutex_lock(&s->wake_mutex);
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }

    st_frame_downsample(frame, tx_frame, 0);
    st20p_tx_put_frame(tx_handle, tx_frame);

    s->fb_fwd++;
    return;
  }
}

static void *st20_fwd_st20_thread(void *arg) {
  struct rx_st20p_tx_st20p_sample_ctx *s = arg;
  st20p_rx_handle rx_handle = s->rx_handle;
  struct st_frame *frame;

  info("%s(%d), start\n", __func__, s->idx);
  while (!s->stop) {
    frame = st20p_rx_get_frame(rx_handle);
    if (!frame) { /* no frame */
      st_pthread_mutex_lock(&s->wake_mutex);
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }

    fwd_st20_consume_frame(s, frame);
    st20p_rx_put_frame(rx_handle, frame);
  }
  info("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

static int rx_st20p_tx_st20p_free_app(struct rx_st20p_tx_st20p_sample_ctx *app) {
  if (app->tx_handle) {
    st20p_tx_free(app->tx_handle);
    app->tx_handle = NULL;
  }
  if (app->rx_handle) {
    st20p_rx_free(app->rx_handle);
    app->rx_handle = NULL;
  }
  st_pthread_mutex_destroy(&app->wake_mutex);
  st_pthread_cond_destroy(&app->wake_cond);

  return 0;
}

int main(int argc, char **argv) {
  struct st_sample_context ctx;
  int ret;

  /* init sample(st) dev */
  memset(&ctx, 0, sizeof(ctx));
  ret = fwd_sample_parse_args(&ctx, argc, argv);
  if (ret < 0) return ret;

  /* enable auto start/stop */
  ctx.param.flags |= MTL_FLAG_DEV_AUTO_START_STOP;
  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s: mtl_init fail\n", __func__);
    return -EIO;
  }

  struct rx_st20p_tx_st20p_sample_ctx app;
  memset(&app, 0, sizeof(app));
  app.idx = 0;
  app.stop = false;
  app.st = ctx.st;
  st_pthread_mutex_init(&app.wake_mutex, NULL);
  st_pthread_cond_init(&app.wake_cond, NULL);

  st20_get_pgroup(ST20_FMT_YUV_422_10BIT, &app.st20_pg);

  struct st20p_rx_ops ops_rx;
  memset(&ops_rx, 0, sizeof(ops_rx));
  ops_rx.name = "st20p_test";
  ops_rx.priv = &app;  // app handle register to lib
  ops_rx.port.num_port = 1;
  memcpy(ops_rx.port.ip_addr[MTL_SESSION_PORT_P], ctx.rx_ip_addr[MTL_PORT_P],
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

  struct st20p_tx_ops ops_tx;
  memset(&ops_tx, 0, sizeof(ops_tx));
  ops_tx.name = "st20p_fwd";
  ops_tx.priv = &app;  // app handle register to lib
  ops_tx.port.num_port = 1;
  memcpy(ops_tx.port.dip_addr[MTL_SESSION_PORT_P], ctx.fwd_dip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops_tx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx.param.port[MTL_PORT_P]);
  ops_tx.port.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port;
  ops_tx.port.payload_type = ctx.payload_type;
  ops_tx.width = ctx.width / 2;
  ops_tx.height = ctx.height / 2;
  ops_tx.fps = ctx.fps;
  ops_tx.interlaced = ctx.interlaced;
  ops_tx.input_fmt = ctx.input_fmt;
  ops_tx.transport_fmt = ctx.fmt;
  ops_tx.device = ST_PLUGIN_DEVICE_AUTO;
  ops_tx.framebuff_cnt = ctx.framebuff_cnt;
  ops_tx.notify_frame_available = tx_st20p_frame_available;

  st20p_tx_handle tx_handle = st20p_tx_create(ctx.st, &ops_tx);
  if (!tx_handle) {
    err("%s, st20p_tx_create fail\n", __func__);
    ret = -EIO;
    goto error;
  }
  app.tx_handle = tx_handle;

  ret = pthread_create(&app.fwd_thread, NULL, st20_fwd_st20_thread, &app);
  if (ret < 0) {
    err("%s, thread create fail %d\n", __func__, ret);
    ret = -EIO;
    goto error;
  }

  app.ready = true;

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

  // check result
  if (app.fb_fwd <= 0) {
    err("%s, error, no fwd frames %d\n", __func__, app.fb_fwd);
    ret = -EIO;
  }

error:
  // release session
  rx_st20p_tx_st20p_free_app(&app);

  /* release sample(st) dev */
  if (ctx.st) {
    mtl_uninit(ctx.st);
    ctx.st = NULL;
  }
  return ret;
}
