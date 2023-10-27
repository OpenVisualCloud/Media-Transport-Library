/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "../sample/sample_util.h"

/* include "struct sockaddr_in" define before include mudp_api */
// clang-format off
#ifdef WINDOWSENV
#include <mtl/mudp_win.h>
#endif
#include <mtl/mudp_api.h>
// clang-format on

struct udp_client_sample_ctx {
  mtl_handle st;
  int idx;
  bool stop;
  pthread_t thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  mudp_handle socket;
  struct sockaddr_in serv_addr;

  int udp_len;

  int send_cnt;
  int recv_cnt;
  int recv_fail_cnt;
  int recv_err_cnt;
  uint64_t last_stat_time;

  int send_cnt_total;
  int recv_cnt_total;
};

static void* udp_client_thread(void* arg) {
  struct udp_client_sample_ctx* s = arg;
  mudp_handle socket = s->socket;

  ssize_t udp_len = s->udp_len;
  char send_buf[udp_len];
  for (ssize_t i = 0; i < udp_len; i++) {
    send_buf[i] = i;
  }
  char recv_buf[udp_len];
  char last_rx_idx = -1;
  int idx_pos = udp_len / 2;
  int send_idx = 0;

  info("%s(%d), start socket %p udp len %d\n", __func__, s->idx, socket, (int)udp_len);
  while (!s->stop) {
    send_buf[idx_pos] = send_idx++;
    ssize_t send =
        mudp_sendto(socket, send_buf, sizeof(send_buf), 0,
                    (const struct sockaddr*)&s->serv_addr, sizeof(s->serv_addr));
    if (send != udp_len) {
      err("%s(%d), only send %d bytes\n", __func__, s->idx, (int)send);
      continue;
    }
    s->send_cnt++;
    s->send_cnt_total++;

    ssize_t recv = mudp_recvfrom(socket, recv_buf, sizeof(recv_buf), 0, NULL, NULL);
    if (recv != udp_len) {
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
    dbg("%s(%d), recv reply %d bytes succ\n", __func__, s->idx, (int)udp_len);
    s->recv_cnt++;
    s->recv_cnt_total++;
  }
  info("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

static void* udp_client_transport_thread(void* arg) {
  struct udp_client_sample_ctx* s = arg;
  mudp_handle socket = s->socket;

  ssize_t udp_len = s->udp_len;
  char send_buf[udp_len];
  for (ssize_t i = 0; i < udp_len; i++) {
    send_buf[i] = i;
  }

  info("%s(%d), start socket %p, udp len %d\n", __func__, s->idx, socket, (int)udp_len);
  while (!s->stop) {
    ssize_t send =
        mudp_sendto(socket, send_buf, sizeof(send_buf), 0,
                    (const struct sockaddr*)&s->serv_addr, sizeof(s->serv_addr));
    if (send != udp_len) {
      err("%s(%d), only send %d bytes\n", __func__, s->idx, (int)send);
      continue;
    }
    s->send_cnt++;
    s->send_cnt_total++;
  }
  info("%s(%d), stop\n", __func__, s->idx);

  return NULL;
}

static void udp_client_status(struct udp_client_sample_ctx* s) {
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

int main(int argc, char** argv) {
  struct st_sample_context ctx;
  int ret;

  memset(&ctx, 0, sizeof(ctx));
  ret = sample_parse_args(&ctx, argc, argv, true, true, true);
  if (ret < 0) return ret;

  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s, mtl_init fail\n", __func__);
    return -EIO;
  }

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
    if (ctx.udp_len)
      app[i]->udp_len = ctx.udp_len;
    else
      app[i]->udp_len = 1024;
    st_pthread_mutex_init(&app[i]->wake_mutex, NULL);
    st_pthread_cond_init(&app[i]->wake_cond, NULL);

    mudp_init_sockaddr(&app[i]->serv_addr, ctx.tx_dip_addr[MTL_PORT_P], ctx.udp_port + i);

    app[i]->socket = mudp_socket(ctx.st, AF_INET, SOCK_DGRAM, 0);
    if (!app[i]->socket) {
      err("%s(%d), socket create fail\n", __func__, i);
      ret = -EIO;
      goto error;
    }
    if (ctx.udp_tx_bps) mudp_set_tx_rate(app[i]->socket, ctx.udp_tx_bps);
    if (ctx.has_tx_dst_mac[MTL_PORT_P])
      mudp_set_tx_mac(app[i]->socket, ctx.tx_dst_mac[MTL_PORT_P]);

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 1000;
    ret = mudp_setsockopt(app[i]->socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (ret < 0) {
      err("%s(%d), SO_RCVTIMEO fail %d\n", __func__, i, ret);
      goto error;
    }

    if ((ctx.udp_mode == SAMPLE_UDP_TRANSPORT) ||
        (ctx.udp_mode == SAMPLE_UDP_TRANSPORT_POLL) ||
        (ctx.udp_mode == SAMPLE_UDP_TRANSPORT_UNIFY_POLL))
      ret = pthread_create(&app[i]->thread, NULL, udp_client_transport_thread, app[i]);
    else
      ret = pthread_create(&app[i]->thread, NULL, udp_client_thread, app[i]);
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
        udp_client_status(app[i]);
      }
    }
  }

  // check result
  ret = 0;
  for (int i = 0; i < session_num; i++) {
    info("%s(%d), send_cnt_total %d\n", __func__, i, app[i]->send_cnt_total);
    if (app[i]->send_cnt_total <= 0) {
      ret += -EIO;
    }
    if (ctx.udp_mode == SAMPLE_UDP_DEFAULT) {
      info("%s(%d), recv_cnt_total %d\n", __func__, i, app[i]->recv_cnt_total);
      if (app[i]->recv_cnt_total <= 0) {
        ret += -EIO;
      }
    }
  }

error:
  for (int i = 0; i < session_num; i++) {
    if (!app[i]) continue;
    // stop app thread
    app[i]->stop = true;
    st_pthread_mutex_lock(&app[i]->wake_mutex);
    st_pthread_cond_signal(&app[i]->wake_cond);
    st_pthread_mutex_unlock(&app[i]->wake_mutex);
    if (app[i]->thread) pthread_join(app[i]->thread, NULL);

    if (app[i]->socket) mudp_close(app[i]->socket);
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
