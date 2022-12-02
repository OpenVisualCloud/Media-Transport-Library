/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "../sample/sample_util.h"

/* include "struct sockaddr_in" define before include mudp_api */
// clang-format off
#include <mtl/mudp_api.h>
// clang-format on

struct udp_server_sample_ctx {
  mtl_handle st;
  int idx;
  bool stop;
  pthread_t thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  mudp_handle socket;
  struct sockaddr_in client_addr;

  int send_cnt;
  int recv_cnt;
};

static void* udp_server_thread(void* arg) {
  struct udp_server_sample_ctx* s = arg;
  mudp_handle socket = s->socket;
  ssize_t udp_len = MUDP_MAX_BYTES;
  char buf[udp_len];

  info("%s(%d), start socket %p\n", __func__, s->idx, socket);
  while (!s->stop) {
    ssize_t recv = mudp_recvfrom(socket, buf, sizeof(buf), 0, NULL, NULL);
    if (recv < 0) {
      dbg("%s(%d), recv fail %d\n", __func__, s->idx, (int)recv);
      continue;
    }
    s->recv_cnt++;
    dbg("%s(%d), recv %d bytes\n", __func__, s->idx, (int)recv);
    ssize_t send =
        mudp_sendto(socket, buf, recv, 0, (const struct sockaddr*)&s->client_addr,
                    sizeof(s->client_addr));
    if (send != recv) {
      err("%s(%d), only send %d bytes\n", __func__, s->idx, (int)send);
      continue;
    }
    s->send_cnt++;
  }
  info("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

static void udp_server_status(struct udp_server_sample_ctx* s) {
  info("%s(%d), send %d pkts recv %d pkts\n", __func__, s->idx, s->send_cnt, s->recv_cnt);
  s->send_cnt = 0;
  s->recv_cnt = 0;
}

int main(int argc, char** argv) {
  struct st_sample_context ctx;
  int ret;

  memset(&ctx, 0, sizeof(ctx));
  ret = sample_parse_args(&ctx, argc, argv, false, true, true);
  if (ret < 0) return ret;

  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s, mtl_init fail\n", __func__);
    return -EIO;
  }

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
    mudp_init_sockaddr(&app[i]->client_addr, ctx.rx_sip_addr[MTL_PORT_P],
                       ctx.udp_port + i);
    ret = mudp_bind(app[i]->socket, (const struct sockaddr*)&app[i]->client_addr,
                    sizeof(app[i]->client_addr));
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

  int time_s = 0;
  while (!ctx.exit) {
    sleep(1);
    /* display server status every 10s */
    time_s++;
    if ((time_s % 10) == 0) {
      for (int i = 0; i < session_num; i++) {
        udp_server_status(app[i]);
      }
    }
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
