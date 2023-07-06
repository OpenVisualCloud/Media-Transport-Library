/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "../sample/sample_util.h"

/* include "struct sockaddr_in" define before include mudp_sockfd_api */
// clang-format off
#ifdef WINDOWSENV
#include <mtl/mudp_win.h>
#endif
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
  struct sockaddr_in bind_addr;

  int send_cnt;
  int recv_cnt;
  ssize_t recv_len;
  uint64_t last_stat_time;

  int send_cnt_total;
  int recv_cnt_total;
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
  struct sockaddr_in cli_addr;
  socklen_t cli_addr_len = sizeof(cli_addr);

  info("%s(%d), start socket %d\n", __func__, s->idx, socket);
  while (!s->stop) {
    ssize_t recv = mufd_recvfrom(socket, buf, sizeof(buf), 0, (struct sockaddr*)&cli_addr,
                                 &cli_addr_len);
    if (recv < 0) {
      dbg("%s(%d), recv fail %d\n", __func__, s->idx, (int)recv);
      continue;
    }
    s->recv_cnt++;
    s->recv_cnt_total++;
    s->recv_len += recv;
    dbg("%s(%d), recv %d bytes\n", __func__, s->idx, (int)recv);
    ssize_t send = mufd_sendto(socket, buf, recv, 0, (const struct sockaddr*)&cli_addr,
                               cli_addr_len);
    if (send != recv) {
      err("%s(%d), only send %d bytes\n", __func__, s->idx, (int)send);
      continue;
    }
    s->send_cnt++;
    s->send_cnt_total++;
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
    s->recv_cnt_total++;
    s->recv_len += recv;
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
    s->recv_cnt_total++;
    s->recv_len += recv;
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
      s->recv_cnt_total++;
      s->recv_len += recv;
    }
  }
  info("%s, stop\n", __func__);

  return NULL;
}

static void ufd_server_status(struct ufd_server_sample_ctx* s) {
  uint64_t cur_ts = sample_get_monotonic_time();
  double time_sec = (double)(cur_ts - s->last_stat_time) / NS_PER_S;
  double bps = (double)s->recv_len * 8 / time_sec;
  double bps_g = bps / (1000 * 1000 * 1000);
  s->last_stat_time = cur_ts;

  info("%s(%d), send %d pkts recv %d pkts(%fg/s)\n", __func__, s->idx, s->send_cnt,
       s->recv_cnt, bps_g);
  s->send_cnt = 0;
  s->recv_cnt = 0;
  s->recv_len = 0;
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

  ufd_override_check(&ctx);

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
    if (ctx.has_tx_dst_mac[MTL_PORT_P])
      mufd_set_tx_mac(app[i]->socket, ctx.tx_dst_mac[MTL_PORT_P]);
    mufd_init_sockaddr(&app[i]->client_addr, ctx.rx_sip_addr[MTL_PORT_P],
                       ctx.udp_port + i);
    bool mcast = mudp_is_multicast(&app[i]->client_addr);

    if (mcast) /* bind to any addr for mcast */
      mudp_init_sockaddr_any(&app[i]->bind_addr, ctx.udp_port + i);
    else
      mudp_init_sockaddr(&app[i]->bind_addr, ctx.param.sip_addr[MTL_PORT_P],
                         ctx.udp_port + i);
    ret = mufd_bind(app[i]->socket, (const struct sockaddr*)&app[i]->bind_addr,
                    sizeof(app[i]->bind_addr));
    if (ret < 0) {
      err("%s(%d), bind fail %d\n", __func__, i, ret);
      goto error;
    }

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 1000;
    ret = mufd_setsockopt(app[i]->socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (ret < 0) {
      err("%s(%d), SO_RCVTIMEO fail %d\n", __func__, i, ret);
      goto error;
    }

    if (mcast) {
      struct ip_mreq mreq;
      memset(&mreq, 0, sizeof(mreq));
      /* multicast addr */
      mreq.imr_multiaddr.s_addr = app[i]->client_addr.sin_addr.s_addr;
      /* local nic src ip */
      memcpy(&mreq.imr_interface.s_addr, ctx.param.sip_addr[MTL_PORT_P], MTL_IP_ADDR_LEN);
      ret = mufd_setsockopt(app[i]->socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
                            sizeof(mreq));
      if (ret < 0) {
        err("%s(%d), join multicast fail %d\n", __func__, i, ret);
        goto error;
      }
      info("%s(%d), join multicast succ\n", __func__, i);
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
    app[i]->last_stat_time = sample_get_monotonic_time();
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

  // check result
  ret = 0;
  for (int i = 0; i < session_num; i++) {
    info("%s(%d), recv_cnt_total %d\n", __func__, i, app[i]->recv_cnt_total);
    if (app[i]->recv_cnt_total <= 0) {
      ret += -EIO;
    }
    if (ctx.udp_mode == SAMPLE_UDP_DEFAULT) {
      info("%s(%d), send_cnt_total %d\n", __func__, i, app[i]->send_cnt_total);
      if (app[i]->send_cnt_total <= 0) {
        ret += -EIO;
      }
    }
  }

error:
  if (ctx.udp_mode == SAMPLE_UDP_TRANSPORT_UNIFY_POLL) {
    ctxs.stop = true;
    dbg("%s(%d), stop ctxs thread\n", __func__, i);
    st_pthread_mutex_lock(&ctxs.wake_mutex);
    st_pthread_cond_signal(&ctxs.wake_cond);
    st_pthread_mutex_unlock(&ctxs.wake_mutex);
    if (ctxs.thread) pthread_join(ctxs.thread, NULL);
  }

  for (int i = 0; i < session_num; i++) {
    if (!app[i]) continue;
    // stop app thread
    app[i]->stop = true;
    dbg("%s(%d), stop thread\n", __func__, i);
    st_pthread_mutex_lock(&app[i]->wake_mutex);
    st_pthread_cond_signal(&app[i]->wake_cond);
    st_pthread_mutex_unlock(&app[i]->wake_mutex);
    if (app[i]->thread) pthread_join(app[i]->thread, NULL);

    if (app[i]->socket >= 0) {
      bool mcast = mudp_is_multicast(&app[i]->client_addr);
      if (mcast) {
        struct ip_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        /* multicast addr */
        mreq.imr_multiaddr.s_addr = app[i]->client_addr.sin_addr.s_addr;
        /* local nic src ip */
        memcpy(&mreq.imr_interface.s_addr, ctx.param.sip_addr[MTL_PORT_P],
               MTL_IP_ADDR_LEN);
        mufd_setsockopt(app[i]->socket, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq,
                        sizeof(mreq));
      }
      mufd_close(app[i]->socket);
    }
    st_pthread_mutex_destroy(&app[i]->wake_mutex);
    st_pthread_cond_destroy(&app[i]->wake_cond);
    free(app[i]);
  }
  if (ctxs.apps) free(ctxs.apps);
  st_pthread_mutex_destroy(&ctxs.wake_mutex);
  st_pthread_cond_destroy(&ctxs.wake_cond);

  return ret;
}
