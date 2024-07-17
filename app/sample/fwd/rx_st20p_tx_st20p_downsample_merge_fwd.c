/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "../sample_util.h"

struct rx_ctx {
  st20p_rx_handle rx_handle;
  size_t fb_offset;
  pthread_cond_t rx_wake_cond;
  pthread_mutex_t rx_wake_mutex;
  void* app;
  int fb_rcv;
};

struct merge_fwd_sample_ctx {
  mtl_handle st;
  st20p_tx_handle tx_handle;
  struct rx_ctx rx[4];

  size_t fb_size;

  bool ready;
  bool stop;
  pthread_t fwd_thread;
  pthread_cond_t tx_wake_cond;
  pthread_mutex_t tx_wake_mutex;

  int fb_fwd;
  bool sync_tmstamp;
};

static int tx_st20p_frame_available(void* priv) {
  struct merge_fwd_sample_ctx* s = priv;

  if (!s->ready) return -EIO;

  st_pthread_mutex_lock(&s->tx_wake_mutex);
  st_pthread_cond_signal(&s->tx_wake_cond);
  st_pthread_mutex_unlock(&s->tx_wake_mutex);

  return 0;
}

static int rx_st20p_frame_available(void* priv) {
  struct rx_ctx* s = priv;
  struct merge_fwd_sample_ctx* app = s->app;

  if (!app->ready) return -EIO;

  st_pthread_mutex_lock(&s->rx_wake_mutex);
  st_pthread_cond_signal(&s->rx_wake_cond);
  st_pthread_mutex_unlock(&s->rx_wake_mutex);

  return 0;
}

static void* tx_st20p_fwd_thread(void* args) {
  struct merge_fwd_sample_ctx* s = args;
  st20p_tx_handle tx_handle = s->tx_handle;
  struct st_frame* frame;
  struct st_frame down_frame = {0}; /* empty temp frame*/

  /* in case when timestamp mismatch */
  struct st_frame* rx_restore_frame = NULL;
  int rx_restore_idx = -1;

loop_entry:
  while (!s->stop) {
    uint64_t tx_tmstamp = 0;
    frame = st20p_tx_get_frame(tx_handle);
    if (!frame) { /* no frame */
      st_pthread_mutex_lock(&s->tx_wake_mutex);
      if (!s->stop) st_pthread_cond_wait(&s->tx_wake_cond, &s->tx_wake_mutex);
      st_pthread_mutex_unlock(&s->tx_wake_mutex);
      continue;
    }
    /* set downsample frame */
    down_frame.linesize[0] = frame->linesize[0]; /* leave paddings for neighbor frame */
    down_frame.width = frame->width / 2;
    down_frame.height = frame->height / 2;
    down_frame.fmt = frame->fmt;

    for (int i = 0; i < 4; i++) {
      struct rx_ctx* rx = &s->rx[i];
      st20p_rx_handle rx_handle = rx->rx_handle;
      struct st_frame* rx_frame = NULL;

      while (!s->stop && !rx_frame) {
        if (rx_restore_frame && rx_restore_idx == i) {
          rx_frame = rx_restore_frame;
        } else {
          rx_frame = st20p_rx_get_frame(rx_handle);
          if (!rx_frame) { /* no frame */
            st_pthread_mutex_lock(&rx->rx_wake_mutex);
            if (!s->stop) st_pthread_cond_wait(&rx->rx_wake_cond, &rx->rx_wake_mutex);
            st_pthread_mutex_unlock(&rx->rx_wake_mutex);
            continue;
          }
        }
        if (s->sync_tmstamp) {
          uint64_t tmstamp = rx_frame->timestamp;
          if (!tx_tmstamp) tx_tmstamp = tmstamp;
          if (tx_tmstamp < tmstamp) {
            err("%s, newer timestamp occurs %" PRIu64 ", frame %" PRIu64
                " may have dropped packets\n",
                __func__, tmstamp, tx_tmstamp);
            rx_restore_frame = rx_frame;
            rx_restore_idx = i;
            st20p_tx_put_frame(tx_handle, frame);
            goto loop_entry; /* start new tx frame */
          } else if (tx_tmstamp > tmstamp) {
            warn("%s, clear outdated frame %" PRIu64 "\n", __func__, tmstamp);
            st20p_rx_put_frame(rx_handle, rx_frame);
            rx_frame = NULL;
            continue; /* continue while: get new rx frame */
          }
        }

        down_frame.addr[0] = frame->addr[0] + rx->fb_offset;
        st_frame_downsample(rx_frame, &down_frame, 0);

        st20p_rx_put_frame(rx_handle, rx_frame);
        rx->fb_rcv++;
      }
    }
    if (s->sync_tmstamp) {
      frame->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
      frame->timestamp = tx_tmstamp;

      /* reset status */
      rx_restore_frame = NULL;
      rx_restore_idx = -1;
    }

    st20p_tx_put_frame(tx_handle, frame);
    s->fb_fwd++;
  }

  return NULL;
}

static int split_fwd_sample_free_app(struct merge_fwd_sample_ctx* app) {
  for (int i = 0; i < 4; i++) {
    if (app->rx[i].rx_handle) {
      st20p_rx_free(app->rx[i].rx_handle);
      app->rx[i].rx_handle = NULL;
    }
    st_pthread_mutex_destroy(&app->rx[i].rx_wake_mutex);
    st_pthread_cond_destroy(&app->rx[i].rx_wake_cond);
  }
  if (app->tx_handle) {
    st20p_tx_free(app->tx_handle);
    app->tx_handle = NULL;
  }
  st_pthread_mutex_destroy(&app->tx_wake_mutex);
  st_pthread_cond_destroy(&app->tx_wake_cond);

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
  sample_rx_queue_cnt_set(&ctx, session_num);
  ctx.param.flags |=
      MTL_FLAG_RX_SEPARATE_VIDEO_LCORE; /* use separate lcores for tx and rx */

  /* enable auto start/stop */
  ctx.param.flags |= MTL_FLAG_DEV_AUTO_START_STOP;
  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s: mtl_init fail\n", __func__);
    return -EIO;
  }

  struct merge_fwd_sample_ctx app;
  memset(&app, 0, sizeof(app));
  app.st = ctx.st;
  st_pthread_mutex_init(&app.tx_wake_mutex, NULL);
  st_pthread_cond_init(&app.tx_wake_cond, NULL);
  app.sync_tmstamp = true; /* make sure frames to be merged have same timestamp, otherwise
                              disable this option */

  struct st20p_tx_ops ops_tx;
  memset(&ops_tx, 0, sizeof(ops_tx));
  ops_tx.name = "st20p_fwd";
  ops_tx.priv = &app;
  ops_tx.port.num_port = 1;
  memcpy(ops_tx.port.dip_addr[MTL_SESSION_PORT_P], ctx.fwd_dip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops_tx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx.param.port[MTL_PORT_P]);
  ops_tx.port.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port;
  ops_tx.port.payload_type = ctx.payload_type;
  ops_tx.width = ctx.width;
  ops_tx.height = ctx.height;
  ops_tx.fps = ctx.fps;
  ops_tx.interlaced = ctx.interlaced;
  ops_tx.input_fmt = ctx.input_fmt;
  ops_tx.transport_fmt = ctx.fmt;
  ops_tx.device = ST_PLUGIN_DEVICE_AUTO;
  ops_tx.framebuff_cnt = ctx.framebuff_cnt;
  if (app.sync_tmstamp) ops_tx.flags |= ST20P_TX_FLAG_USER_TIMESTAMP;
  ops_tx.notify_frame_available = tx_st20p_frame_available;
  st20p_tx_handle tx_handle = st20p_tx_create(ctx.st, &ops_tx);
  if (!tx_handle) {
    err("%s, st20p_tx_create fail\n", __func__);
    ret = -EIO;
    goto error;
  }
  app.tx_handle = tx_handle;

  for (int i = 0; i < 4; i++) {
    struct rx_ctx* rx = &app.rx[i];
    st_pthread_mutex_init(&rx->rx_wake_mutex, NULL);
    st_pthread_cond_init(&rx->rx_wake_cond, NULL);
    rx->app = &app;
    struct st20p_rx_ops ops_rx;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20p_rx";
    ops_rx.priv = rx;
    ops_rx.port.num_port = 1;
    memcpy(ops_rx.port.ip_addr[MTL_SESSION_PORT_P], ctx.rx_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx.param.port[MTL_PORT_P]);
    ops_rx.port.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port + i * 2;
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
    rx->rx_handle = rx_handle;
  }

  ret = pthread_create(&app.fwd_thread, NULL, tx_st20p_fwd_thread, &app);
  if (ret < 0) {
    err("%s, fwd thread create fail\n", __func__);
    ret = -EIO;
    goto error;
  }

  struct st20_pgroup st20_pg;
  st20_get_pgroup(ctx.fmt, &st20_pg);
  app.rx[0].fb_offset = 0;
  app.rx[1].fb_offset = (ctx.width / 2) * st20_pg.size / st20_pg.coverage;
  app.rx[2].fb_offset = (ctx.width / 2) * ctx.height * st20_pg.size / st20_pg.coverage;
  app.rx[3].fb_offset = app.rx[2].fb_offset + app.rx[1].fb_offset;
  app.fb_size = ctx.width * ctx.height * st20_pg.size / st20_pg.coverage;

  app.ready = true;

  while (!ctx.exit) {
    sleep(1);
  }

  // stop fwd thread
  app.stop = true;
  st_pthread_mutex_lock(&app.tx_wake_mutex);
  st_pthread_cond_signal(&app.tx_wake_cond);
  st_pthread_mutex_unlock(&app.tx_wake_mutex);
  for (int i = 0; i < 4; i++) {
    struct rx_ctx* rx = &app.rx[i];
    st_pthread_mutex_lock(&rx->rx_wake_mutex);
    st_pthread_cond_signal(&rx->rx_wake_cond);
    st_pthread_mutex_unlock(&rx->rx_wake_mutex);
  }
  pthread_join(app.fwd_thread, NULL);

  for (int i = 0; i < 4; i++) {
    info("%s, rx[%d] fb_received %d\n", __func__, i, app.rx[i].fb_rcv);
  }
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