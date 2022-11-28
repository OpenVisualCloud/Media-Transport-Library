/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <mtl/mudp_api.h>
#include <openssl/sha.h>

#include "../sample/sample_util.h"

struct udp_server_sample_ctx {
  mtl_handle st;
  int idx;
  bool stop;
  pthread_t thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  mudp_handle socket;
};

static void* udp_server_thread(void* arg) {
  struct udp_server_sample_ctx* s = arg;
  mudp_handle socket = s->socket;

  info("%s(%d), start socket %p\n", __func__, s->idx, socket);
  while (!s->stop) {
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

  struct sockaddr_in serv_addr;
  uint32_t session_num = ctx.sessions;
  struct udp_server_sample_ctx* app[session_num];
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

    app[i]->socket = mudp_socket(ctx.st, AF_INET, SOCK_DGRAM, 0);
    if (!app[i]->socket) {
      err("%s(%d), socket create fail\n", __func__, i);
      ret = -EIO;
      goto error;
    }
    mudp_init_sockaddr_any(&serv_addr, ctx.udp_port);
    ret =
        mudp_bind(app[i]->socket, (const struct sockaddr*)&serv_addr, sizeof(serv_addr));
    if (ret < 0) {
      err("%s(%d), bind fail %d\n", __func__, i, ret);
      goto error;
    }

    ret = pthread_create(&app[i]->thread, NULL, udp_server_thread, app[i]);
    if (ret < 0) {
      err("%s(%d), thread create fail %d\n", __func__, ret, i);
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
    pthread_join(app[i]->thread, NULL);
  }

  // stop dev
  ret = mtl_stop(ctx.st);

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
