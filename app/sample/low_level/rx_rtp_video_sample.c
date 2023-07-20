/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "../sample_util.h"

struct rv_rtp_sample_ctx {
  int idx;
  int fb_rec;
  st20_rx_handle handle;
  bool stop;
  pthread_t app_thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;
};

static int rx_rtp_ready(void* priv) {
  struct rv_rtp_sample_ctx* s = (struct rv_rtp_sample_ctx*)priv;
  // wake up the app thread who is waiting for the rtp buf;
  st_pthread_mutex_lock(&s->wake_mutex);
  st_pthread_cond_signal(&s->wake_cond);
  st_pthread_mutex_unlock(&s->wake_mutex);
  return 0;
}

static void* app_rx_video_rtp_thread(void* arg) {
  struct rv_rtp_sample_ctx* s = arg;
  void* usrptr;
  uint16_t len;
  void* mbuf;
  struct st20_rfc4175_rtp_hdr* hdr;

  while (!s->stop) {
    mbuf = st20_rx_get_mbuf(s->handle, &usrptr, &len);
    if (!mbuf) {
      /* no buffer */
      st_pthread_mutex_lock(&s->wake_mutex);
      if (!s->stop) st_pthread_cond_wait(&s->wake_cond, &s->wake_mutex);
      st_pthread_mutex_unlock(&s->wake_mutex);
      continue;
    }

    /* get one packet */
    hdr = (struct st20_rfc4175_rtp_hdr*)usrptr;
    /* handle the rtp packet, should not handle the heavy work, if the st20_rx_get_mbuf is
     * not called timely, the rtp queue in the lib will be full and rtp will be enqueued
     * fail in the lib, packet will be dropped*/
    if (hdr->base.marker) s->fb_rec++;
    /* free to lib */
    st20_rx_put_mbuf(s->handle, mbuf);
  }

  return NULL;
}

int main(int argc, char** argv) {
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
  st20_rx_handle rx_handle[session_num];
  struct rv_rtp_sample_ctx* app[session_num];

  // create and register rx session
  for (int i = 0; i < session_num; i++) {
    app[i] = (struct rv_rtp_sample_ctx*)malloc(sizeof(struct rv_rtp_sample_ctx));
    if (!app[i]) {
      err("%s(%d), app context malloc fail\n", __func__, i);
      return -1;
    }
    memset(app[i], 0, sizeof(struct rv_rtp_sample_ctx));
    app[i]->idx = i;
    st_pthread_mutex_init(&app[i]->wake_mutex, NULL);
    st_pthread_cond_init(&app[i]->wake_cond, NULL);

    struct st20_rx_ops ops_rx;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20_test";
    ops_rx.priv = app[i];  // app handle register to lib
    ops_rx.num_port = 1;
    memcpy(ops_rx.sip_addr[MTL_SESSION_PORT_P], ctx.rx_sip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    strncpy(ops_rx.port[MTL_SESSION_PORT_P], ctx.param.port[MTL_PORT_P],
            MTL_PORT_MAX_LEN);
    ops_rx.udp_port[MTL_SESSION_PORT_P] = ctx.udp_port + i * 2;
    ops_rx.type = ST20_TYPE_RTP_LEVEL;
    ops_rx.width = ctx.width;
    ops_rx.height = ctx.height;
    ops_rx.fps = ctx.fps;
    ops_rx.fmt = ctx.fmt;
    ops_rx.payload_type = ctx.payload_type;
    ops_rx.rtp_ring_size = 1024;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    rx_handle[i] = st20_rx_create(ctx.st, &ops_rx);
    if (!rx_handle[i]) {
      err("%s(%d), ext_frames malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    app[i]->handle = rx_handle[i];
    ret = pthread_create(&app[i]->app_thread, NULL, app_rx_video_rtp_thread, app[i]);
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
    free(app[i]);
  }

  /* release sample(st) dev */
  if (ctx.st) {
    mtl_uninit(ctx.st);
    ctx.st = NULL;
  }
  return ret;
}
