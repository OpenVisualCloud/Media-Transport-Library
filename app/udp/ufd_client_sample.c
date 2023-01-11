/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "../sample/sample_util.h"

/* include "struct sockaddr_in" define before include mudp_sockfd_api */
// clang-format off
#include <mtl/mudp_sockfd_api.h>
// clang-format on

struct ufd_client_sample_ctx {
  int idx;
  bool stop;
  pthread_t thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  int socket;
  struct sockaddr_in serv_addr;

  int udp_len;

  int send_cnt;
  int recv_cnt;
  int recv_fail_cnt;
  int recv_err_cnt;
  uint64_t last_stat_time;
};

static void* ufd_client_thread(void* arg) {
  struct ufd_client_sample_ctx* s = arg;
  int socket = s->socket;

  ssize_t ufd_len = s->udp_len;
  char send_buf[ufd_len];
  for (ssize_t i = 0; i < ufd_len; i++) {
    send_buf[i] = i;
  }
  char recv_buf[ufd_len];
  char last_rx_idx = -1;
  int idx_pos = ufd_len / 2;
  int send_idx = 0;

  info("%s(%d), start socket %d ufd len %d\n", __func__, s->idx, socket, (int)ufd_len);
  while (!s->stop) {
    send_buf[idx_pos] = send_idx++;
    ssize_t send =
        mufd_sendto(socket, send_buf, sizeof(send_buf), 0,
                    (const struct sockaddr*)&s->serv_addr, sizeof(s->serv_addr));
    if (send != ufd_len) {
      err("%s(%d), only send %d bytes\n", __func__, s->idx, (int)send);
      continue;
    }
    s->send_cnt++;

    ssize_t recv = mufd_recvfrom(socket, recv_buf, sizeof(recv_buf), 0, NULL, NULL);
    if (recv != ufd_len) {
      dbg("%s(%d), only recv %d bytes\n", __func__, s->idx, (int)recv);
      s->recv_fail_cnt++;
      continue;
    }

    char expect_rx_idx = last_rx_idx + 1;
    last_rx_idx = recv_buf[idx_pos];
    if (last_rx_idx != expect_rx_idx) {
      err("%s(%d), idx mismatch, expect %u get %u\n", __func__, s->idx, expect_rx_idx,
          last_rx_idx);
      s->recv_err_cnt++;
      continue;
    }
    dbg("%s(%d), recv reply %d bytes succ\n", __func__, s->idx, (int)ufd_len);
    s->recv_cnt++;
  }
  info("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

static void* ufd_client_transport_thread(void* arg) {
  struct ufd_client_sample_ctx* s = arg;
  int socket = s->socket;

  ssize_t ufd_len = s->udp_len;
  char send_buf[ufd_len];
  for (ssize_t i = 0; i < ufd_len; i++) {
    send_buf[i] = i;
  }

  info("%s(%d), start socket %d ufd len %d\n", __func__, s->idx, socket, (int)ufd_len);
  while (!s->stop) {
    ssize_t send =
        mufd_sendto(socket, send_buf, sizeof(send_buf), 0,
                    (const struct sockaddr*)&s->serv_addr, sizeof(s->serv_addr));
    if (send != ufd_len) {
      err("%s(%d), only send %d bytes\n", __func__, s->idx, (int)send);
      continue;
    }
    s->send_cnt++;
  }
  info("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

static void ufd_client_status(struct ufd_client_sample_ctx* s) {
  uint64_t cur_ts = sample_get_monotonic_time();
  double time_sec = (double)(cur_ts - s->last_stat_time) / NS_PER_S;
  double bps = (double)s->send_cnt * s->udp_len * 8 / time_sec;
  double bps_g = bps / (1000 * 1000 * 1000);
  s->last_stat_time = cur_ts;

  info("%s(%d), send %d pkts(%fg/s) recv %d pkts\n", __func__, s->idx, s->send_cnt, bps_g,
       s->recv_cnt);
  s->send_cnt = 0;
  s->recv_cnt = 0;
  if (s->recv_fail_cnt) {
    info("%s(%d), fail recv %d pkts\n", __func__, s->idx, s->recv_fail_cnt);
    s->recv_fail_cnt = 0;
  }
  if (s->recv_err_cnt) {
    info("%s(%d), error recv %d pkts\n", __func__, s->idx, s->recv_err_cnt);
    s->recv_err_cnt = 0;
  }
}

static void ufd_client_sig_handler(int signo) {
  switch (signo) {
    case SIGINT: /* Interrupt from keyboard */
      mufd_abort();
      break;
  }

  return;
}

int main(int argc, char** argv) {
  struct st_sample_context ctx;
  int ret;

  memset(&ctx, 0, sizeof(ctx));
  ret = sample_parse_args(&ctx, argc, argv, true, false, true);
  if (ret < 0) return ret;

  ctx.sig_handler = ufd_client_sig_handler;

  uint32_t session_num = ctx.sessions;
  struct ufd_client_sample_ctx* app[session_num];
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
    if (ctx.udp_len)
      app[i]->udp_len = ctx.udp_len;
    else
      app[i]->udp_len = 1024;
    st_pthread_mutex_init(&app[i]->wake_mutex, NULL);
    st_pthread_cond_init(&app[i]->wake_cond, NULL);

    mufd_init_sockaddr(&app[i]->serv_addr, ctx.tx_dip_addr[MTL_PORT_P], ctx.udp_port + i);

    app[i]->socket = mufd_socket(AF_INET, SOCK_DGRAM, 0);
    if (app[i]->socket < 0) {
      err("%s(%d), socket create fail %d\n", __func__, i, app[i]->socket);
      ret = -EIO;
      goto error;
    }
    if (ctx.udp_tx_bps) mufd_set_tx_rate(app[i]->socket, ctx.udp_tx_bps);

    ret = mufd_bind(app[i]->socket, (const struct sockaddr*)&app[i]->serv_addr,
                    sizeof(app[i]->serv_addr));
    if (ret < 0) {
      err("%s(%d), bind fail %d\n", __func__, i, ret);
      goto error;
    }

    if ((ctx.udp_mode == SAMPLE_UDP_TRANSPORT) ||
        (ctx.udp_mode == SAMPLE_UDP_TRANSPORT_POLL) ||
        (ctx.udp_mode == SAMPLE_UDP_TRANSPORT_UNIFY_POLL))
      ret = pthread_create(&app[i]->thread, NULL, ufd_client_transport_thread, app[i]);
    else
      ret = pthread_create(&app[i]->thread, NULL, ufd_client_thread, app[i]);
    if (ret < 0) {
      err("%s(%d), thread create fail %d\n", __func__, ret, i);
      goto error;
    }
    app[i]->last_stat_time = sample_get_monotonic_time();
  }

  int time_s = 0;
  while (!ctx.exit) {
    sleep(1);
    /* display client status every 10s */
    time_s++;
    if ((time_s % 10) == 0) {
      for (int i = 0; i < session_num; i++) {
        ufd_client_status(app[i]);
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
      if (app[i]->socket >= 0) mufd_close(app[i]->socket);
      st_pthread_mutex_destroy(&app[i]->wake_mutex);
      st_pthread_cond_destroy(&app[i]->wake_cond);
      free(app[i]);
    }
  }

  return ret;
}
