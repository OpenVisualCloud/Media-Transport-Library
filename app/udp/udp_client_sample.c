/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <mtl/mudp_api.h>
#include <openssl/sha.h>

#include "../sample/sample_util.h"

struct udp_client_sample_ctx {
  mtl_handle st;
  int idx;
  bool stop;
  pthread_t thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  mudp_handle socket;
  struct sockaddr_in serv_addr;
};

static void* udp_client_thread(void* arg) {
  struct udp_client_sample_ctx* s = arg;
  mudp_handle socket = s->socket;

  ssize_t udp_len = 1024;
  char buf[udp_len];
  for (ssize_t i = 0; i < udp_len; i++) {
    buf[i] = i;
  }

  info("%s(%d), start socket %p\n", __func__, s->idx, socket);
  while (!s->stop) {
    ssize_t send =
        mudp_sendto(socket, buf, sizeof(buf), 0, (const struct sockaddr*)&s->serv_addr,
                    sizeof(s->serv_addr));
    if (send != udp_len) {
      dbg("%s(%d), only send %d bytes\n", __func__, s->idx, (int)send);
    }
    // usleep(1000);
  }
  info("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

int main(int argc, char** argv) {
  struct st_sample_context ctx;
  int ret;

  memset(&ctx, 0, sizeof(ctx));
  ret = fwd_sample_parse_args(&ctx, argc, argv);
  if (ret < 0) return ret;

  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s: mtl_init fail\n", __func__);
    return -EIO;
  }

  struct sockaddr_in bind_addr;
  uint32_t session_num = ctx.sessions;
  struct udp_client_sample_ctx* app[session_num];
  memset(app, 0, sizeof(app));

  for (int i = 0; i < session_num; i++) {
    app[i] = malloc(sizeof(*app[i]));
    if (!app[i]) {
      err("%s(%d), app context malloc fail\n", __func__, i);
      ret = -ENOMEM;
      goto error;
    }
    memset(app[i], 0, sizeof(*app[i]));

    app[i]->idx = i;
    app[i]->stop = false;
    app[i]->st = ctx.st;
    st_pthread_mutex_init(&app[i]->wake_mutex, NULL);
    st_pthread_cond_init(&app[i]->wake_cond, NULL);

    mudp_init_sockaddr(&app[i]->serv_addr, ctx.tx_dip_addr[MTL_PORT_P], ctx.udp_port + i);
    mudp_init_sockaddr_any(&bind_addr, ctx.udp_port + i);

    app[i]->socket = mudp_socket(ctx.st, AF_INET, SOCK_DGRAM, 0);
    if (!app[i]->socket) {
      err("%s(%d), socket create fail\n", __func__, i);
      ret = -EIO;
      goto error;
    }

    ret =
        mudp_bind(app[i]->socket, (const struct sockaddr*)&bind_addr, sizeof(bind_addr));
    if (ret < 0) {
      err("%s(%d), bind fail %d\n", __func__, i, ret);
      goto error;
    }

    ret = pthread_create(&app[i]->thread, NULL, udp_client_thread, app[i]);
    if (ret < 0) {
      err("%s(%d), thread create fail %d\n", __func__, ret, i);
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
    pthread_join(app[i]->thread, NULL);
  }

error:
  for (int i = 0; i < session_num; i++) {
    if (app[i]) {
      if (app[i]->socket) mudp_close(app[i]->socket);
      st_pthread_mutex_destroy(&app[i]->wake_mutex);
      st_pthread_cond_destroy(&app[i]->wake_cond);
      free(app[i]);
    }
  }
  /* release sample(st) dev */
  if (ctx.st) {
    mtl_uninit(ctx.st);
    ctx.st = NULL;
  }
  return ret;
}
