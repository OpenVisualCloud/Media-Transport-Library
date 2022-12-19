/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "../sample/sample_util.h"

/* include "struct sockaddr_in" define before include mudp_sockfd_api */
// clang-format off
#include <mtl/mudp_sockfd_api.h>
// clang-format on

struct ufd_server_sample_ctx {
  int idx;
  bool stop;
  pthread_t thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  int socket;
  struct sockaddr_in client_addr;

  int send_cnt;
  int recv_cnt;
};

struct ufd_server_samples_ctx {
  struct ufd_server_sample_ctx** apps;
  int apps_cnt;

  bool stop;
  pthread_t thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;
};

static void* ufd_server_thread(void* arg) {
  struct ufd_server_sample_ctx* s = arg;
  int socket = s->socket;
  ssize_t ufd_len = MUDP_MAX_BYTES;
  char buf[ufd_len];

  info("%s(%d), start socket %d\n", __func__, s->idx, socket);
  while (!s->stop) {
    ssize_t recv = mufd_recvfrom(socket, buf, sizeof(buf), 0, NULL, NULL);
    if (recv < 0) {
      dbg("%s(%d), recv fail %d\n", __func__, s->idx, (int)recv);
      continue;
    }
    s->recv_cnt++;
    dbg("%s(%d), recv %d bytes\n", __func__, s->idx, (int)recv);
    ssize_t send =
        mufd_sendto(socket, buf, recv, 0, (const struct sockaddr*)&s->client_addr,
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

static void* ufd_server_transport_thread(void* arg) {
  struct ufd_server_sample_ctx* s = arg;
  int socket = s->socket;
  ssize_t ufd_len = MUDP_MAX_BYTES;
  char buf[ufd_len];

  info("%s(%d), start socket %d\n", __func__, s->idx, socket);
  while (!s->stop) {
    ssize_t recv = mufd_recvfrom(socket, buf, sizeof(buf), 0, NULL, NULL);
    if (recv < 0) {
      dbg("%s(%d), recv fail %d\n", __func__, s->idx, (int)recv);
      continue;
    }
    s->recv_cnt++;
  }
  info("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

static void* ufd_server_transport_poll_thread(void* arg) {
  struct ufd_server_sample_ctx* s = arg;
  int socket = s->socket;
  ssize_t ufd_len = MUDP_MAX_BYTES;
  char buf[ufd_len];

  struct pollfd fds[1];
  memset(fds, 0, sizeof(fds));
  fds[0].fd = socket;
  fds[0].events = POLLIN;

  info("%s(%d), start socket %d\n", __func__, s->idx, socket);
  while (!s->stop) {
    int ret = mufd_poll(fds, 1, 100);
    if (ret <= 0) continue;
    ssize_t recv = mufd_recvfrom(socket, buf, sizeof(buf), 0, NULL, NULL);
    if (recv < 0) {
      err("%s(%d), recv fail %d\n", __func__, s->idx, (int)recv);
      continue;
    }
    s->recv_cnt++;
  }
  info("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

static void* ufd_servers_poll_thread(void* arg) {
  struct ufd_server_samples_ctx* ctxs = arg;
  struct ufd_server_sample_ctx* s = NULL;
  int apps_cnt = ctxs->apps_cnt;
  int socket;
  ssize_t ufd_len = MUDP_MAX_BYTES;
  char buf[ufd_len];

  struct pollfd fds[apps_cnt];
  memset(fds, 0, sizeof(fds));
  for (int i = 0; i < apps_cnt; i++) {
    s = ctxs->apps[i];
    fds[i].fd = s->socket;
    fds[i].events = POLLIN;
  }

  info("%s, start at %p\n", __func__, ctxs);
  while (!ctxs->stop) {
    int ret = mufd_poll(fds, apps_cnt, 100);
    if (ret <= 0) continue;
    for (int i = 0; i < apps_cnt; i++) {
      if (!fds[i].revents) continue; /* pkt not ready */
      s = ctxs->apps[i];
      socket = s->socket;
      ssize_t recv = mufd_recvfrom(socket, buf, sizeof(buf), 0, NULL, NULL);
      if (recv < 0) {
        err("%s(%d), recv fail %d\n", __func__, s->idx, (int)recv);
        continue;
      }
      s->recv_cnt++;
    }
  }
  info("%s, stop\n", __func__);

  return NULL;
}

static void ufd_server_status(struct ufd_server_sample_ctx* s) {
  info("%s(%d), send %d pkts recv %d pkts\n", __func__, s->idx, s->send_cnt, s->recv_cnt);
  s->send_cnt = 0;
  s->recv_cnt = 0;
}

static void ufd_server_sig_handler(int signo) {
  switch (signo) {
    case SIGINT: /* Interrupt from keyboard */
      mufd_abort();
      break;
  }

  return;
}

int main(int argc, char** argv) {
  struct st_sample_context ctx;
  struct ufd_server_samples_ctx ctxs;
  int ret;

  memset(&ctx, 0, sizeof(ctx));
  ret = sample_parse_args(&ctx, argc, argv, false, true, true);
  if (ret < 0) return ret;

  ctx.sig_handler = ufd_server_sig_handler;

  uint32_t session_num = ctx.sessions;
  struct ufd_server_sample_ctx* app[session_num];
  memset(app, 0, sizeof(app));

  ctxs.apps = NULL;
  ctxs.stop = false;
  st_pthread_mutex_init(&ctxs.wake_mutex, NULL);
  st_pthread_cond_init(&ctxs.wake_cond, NULL);
  if (ctx.udp_mode == SAMPLE_UDP_TRANSPORT_UNIFY_POLL) {
    ctxs.apps = malloc(sizeof(*ctxs.apps) * session_num);
    if (!ctxs.apps) {
      err("%s, app ctxs malloc fail\n", __func__);
      ret = -ENOMEM;
      goto error;
    }
    ctxs.apps_cnt = session_num;
  }

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
    st_pthread_mutex_init(&app[i]->wake_mutex, NULL);
    st_pthread_cond_init(&app[i]->wake_cond, NULL);

    app[i]->socket = mufd_socket(AF_INET, SOCK_DGRAM, 0);
    if (app[i]->socket < 0) {
      err("%s(%d), socket create fail %d\n", __func__, i, app[i]->socket);
      ret = -EIO;
      goto error;
    }
    if (ctx.udp_tx_bps) mufd_set_tx_rate(app[i]->socket, ctx.udp_tx_bps);
    mufd_init_sockaddr(&app[i]->client_addr, ctx.rx_sip_addr[MTL_PORT_P],
                       ctx.udp_port + i);
    ret = mufd_bind(app[i]->socket, (const struct sockaddr*)&app[i]->client_addr,
                    sizeof(app[i]->client_addr));
    if (ret < 0) {
      err("%s(%d), bind fail %d\n", __func__, i, ret);
      goto error;
    }

    if (ctx.udp_mode == SAMPLE_UDP_TRANSPORT) {
      ret = pthread_create(&app[i]->thread, NULL, ufd_server_transport_thread, app[i]);
    } else if (ctx.udp_mode == SAMPLE_UDP_TRANSPORT_POLL) {
      ret =
          pthread_create(&app[i]->thread, NULL, ufd_server_transport_poll_thread, app[i]);
    } else if (ctx.udp_mode == SAMPLE_UDP_TRANSPORT_UNIFY_POLL) {
      ctxs.apps[i] = app[i];
      if ((i + 1) == session_num) {
        ret = pthread_create(&ctxs.thread, NULL, ufd_servers_poll_thread, &ctxs);
      }
    } else {
      ret = pthread_create(&app[i]->thread, NULL, ufd_server_thread, app[i]);
    }
    if (ret < 0) {
      err("%s(%d), thread create fail %d\n", __func__, i, ret);
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
        ufd_server_status(app[i]);
      }
    }
  }

  if (ctx.udp_mode == SAMPLE_UDP_TRANSPORT_UNIFY_POLL) {
    ctxs.stop = true;
    dbg("%s(%d), stop ctxs thread\n", __func__, i);
    st_pthread_mutex_lock(&ctxs.wake_mutex);
    st_pthread_cond_signal(&ctxs.wake_cond);
    st_pthread_mutex_unlock(&ctxs.wake_mutex);
    pthread_join(ctxs.thread, NULL);
  } else {
    // stop app thread
    for (int i = 0; i < session_num; i++) {
      app[i]->stop = true;
      dbg("%s(%d), stop thread\n", __func__, i);
      st_pthread_mutex_lock(&app[i]->wake_mutex);
      st_pthread_cond_signal(&app[i]->wake_cond);
      st_pthread_mutex_unlock(&app[i]->wake_mutex);
      pthread_join(app[i]->thread, NULL);
    }
  }

error:
  for (int i = 0; i < session_num; i++) {
    if (app[i]) {
      if (app[i]->socket >= 0) mufd_close(app[i]->socket);
      st_pthread_mutex_destroy(&app[i]->wake_mutex);
      st_pthread_cond_destroy(&app[i]->wake_cond);
      free(app[i]);
    }
  }
  if (ctxs.apps) free(ctxs.apps);
  st_pthread_mutex_destroy(&ctxs.wake_mutex);
  st_pthread_cond_destroy(&ctxs.wake_cond);

  mufd_cleanup();
  return ret;
}
