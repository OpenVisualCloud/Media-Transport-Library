/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "log.h"
#include "ufd_test.h"

struct loop_para {
  int sessions;
  uint16_t udp_port;
  int udp_len;
  int tx_pkts;
  int max_rx_timeout_pkts;
  int tx_sleep_us;
  int rx_timeout_us;

  bool dual_loop;
};

static int loop_para_init(struct loop_para* para) {
  para->sessions = 1;
  para->udp_port = 10000;
  para->udp_len = 1024;
  para->tx_pkts = 1024;
  para->max_rx_timeout_pkts = para->tx_pkts / 100;
  para->tx_sleep_us = 100;
  para->rx_timeout_us = 1000;
  para->dual_loop = false;
  return 0;
}

static int loop_sanity_test(struct utest_ctx* ctx, struct loop_para* para) {
  int sessions = para->sessions;
  uint16_t udp_port = para->udp_port;
  int udp_len = para->udp_len;
  bool dual_loop = para->dual_loop;

  int tx_fds[sessions];
  int rx_fds[sessions];
  int rx_timeout[sessions];
  struct sockaddr_in tx_addr[sessions];
  struct sockaddr_in rx_addr[sessions];
  int ret;
  struct mtl_init_params* p = &ctx->init_params.mt_params;

  for (int i = 0; i < sessions; i++) {
    tx_fds[i] = -1;
    rx_fds[i] = -1;
    rx_timeout[i] = 0;
    mufd_init_sockaddr(&tx_addr[i], p->sip_addr[MTL_PORT_P], udp_port + i);
    mufd_init_sockaddr(&rx_addr[i], p->sip_addr[MTL_PORT_R], udp_port + i);
  }

  for (int i = 0; i < sessions; i++) {
    ret = mufd_socket_port(AF_INET, SOCK_DGRAM, 0, MTL_PORT_P);
    EXPECT_GE(ret, 0);
    if (ret < 0) goto exit;
    tx_fds[i] = ret;

    if (dual_loop) {
      ret = mufd_bind(tx_fds[i], (const struct sockaddr*)&tx_addr[i], sizeof(tx_addr[i]));
      EXPECT_GE(ret, 0);
      if (ret < 0) goto exit;

      struct timeval tv;
      tv.tv_sec = 0;
      tv.tv_usec = para->rx_timeout_us;
      ret = mufd_setsockopt(tx_fds[i], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      EXPECT_GE(ret, 0);
      if (ret < 0) goto exit;
    }

    ret = mufd_socket_port(AF_INET, SOCK_DGRAM, 0, MTL_PORT_R);
    EXPECT_GE(ret, 0);
    if (ret < 0) goto exit;
    rx_fds[i] = ret;

    ret = mufd_bind(rx_fds[i], (const struct sockaddr*)&rx_addr[i], sizeof(rx_addr[i]));
    EXPECT_GE(ret, 0);
    if (ret < 0) goto exit;

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = para->rx_timeout_us;
    ret = mufd_setsockopt(rx_fds[i], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    EXPECT_GE(ret, 0);
    if (ret < 0) goto exit;
  }

  for (int loop = 0; loop < para->tx_pkts; loop++) {
    for (int i = 0; i < sessions; i++) {
      char send_buf[udp_len];
      char recv_buf[udp_len];
      int payload_len = udp_len - SHA256_DIGEST_LENGTH;

      st_test_rand_data((uint8_t*)send_buf, payload_len, 0);
      SHA256((unsigned char*)send_buf, payload_len,
             (unsigned char*)send_buf + payload_len);

      ssize_t send = mufd_sendto(tx_fds[i], send_buf, sizeof(send_buf), 0,
                                 (const struct sockaddr*)&rx_addr[i], sizeof(rx_addr[i]));
      EXPECT_EQ(send, sizeof(send_buf));
      st_usleep(para->tx_sleep_us);

      ssize_t recv = mufd_recvfrom(rx_fds[i], recv_buf, sizeof(recv_buf), 0, NULL, NULL);
      if (recv < 0) { /* timeout */
        rx_timeout[i]++;
        err("%s, recv fail at session %d pkt %d\n", __func__, i, loop);
        continue;
      }
      EXPECT_EQ(recv, sizeof(send_buf));
      /* check sha */
      unsigned char sha_result[SHA256_DIGEST_LENGTH];
      SHA256((unsigned char*)recv_buf, payload_len, sha_result);
      ret = memcmp(recv_buf + payload_len, sha_result, SHA256_DIGEST_LENGTH);
      EXPECT_EQ(ret, 0);
      // test_sha_dump("upd_loop_sha", sha_result);

      if (dual_loop) {
        send = mufd_sendto(rx_fds[i], send_buf, sizeof(send_buf), 0,
                           (const struct sockaddr*)&tx_addr[i], sizeof(tx_addr[i]));
        EXPECT_EQ(send, sizeof(send_buf));
        st_usleep(para->tx_sleep_us);

        recv = mufd_recvfrom(tx_fds[i], recv_buf, sizeof(recv_buf), 0, NULL, NULL);
        if (recv < 0) { /* timeout */
          rx_timeout[i]++;
          err("%s, back recv fail at session %d pkt %d\n", __func__, i, loop);
          continue;
        }
        EXPECT_EQ(recv, sizeof(send_buf));
        /* check sha */
        SHA256((unsigned char*)recv_buf, payload_len, sha_result);
        ret = memcmp(recv_buf + payload_len, sha_result, SHA256_DIGEST_LENGTH);
        EXPECT_EQ(ret, 0);
      }
    }
  }

  /* rx timeout max check */
  for (int i = 0; i < sessions; i++) {
    EXPECT_LT(rx_timeout[i], para->max_rx_timeout_pkts);
  }

exit:
  for (int i = 0; i < sessions; i++) {
    if (tx_fds[i] > 0) mufd_close(tx_fds[i]);
    if (rx_fds[i] > 0) mufd_close(rx_fds[i]);
  }
  return 0;
}

TEST(Loop, single) {
  struct utest_ctx* ctx = utest_get_ctx();
  struct loop_para para;

  loop_para_init(&para);
  loop_sanity_test(ctx, &para);
}

TEST(Loop, multi) {
  struct utest_ctx* ctx = utest_get_ctx();
  struct loop_para para;

  loop_para_init(&para);
  para.sessions = 5;
  para.tx_sleep_us = 100;
  loop_sanity_test(ctx, &para);
}

TEST(Loop, multi_no_sleep) {
  struct utest_ctx* ctx = utest_get_ctx();
  struct loop_para para;

  loop_para_init(&para);
  para.sessions = 10;
  para.tx_sleep_us = 0;
  loop_sanity_test(ctx, &para);
}

TEST(Loop, multi_shared_max) {
  struct utest_ctx* ctx = utest_get_ctx();
  struct loop_para para;

  if (!(ctx->init_params.mt_params.flags & MTL_FLAG_SHARED_QUEUE)) {
    err("%s, skip as it's not shared mode\n", __func__);
    return;
  }

  loop_para_init(&para);
  para.sessions = mufd_get_sessions_max_nb() / 2;
  para.tx_pkts = 32;
  para.max_rx_timeout_pkts = para.tx_pkts / 2;
  para.tx_sleep_us = 0;
  loop_sanity_test(ctx, &para);
}

TEST(Loop, dual_single) {
  struct utest_ctx* ctx = utest_get_ctx();
  struct loop_para para;

  loop_para_init(&para);
  para.dual_loop = true;
  loop_sanity_test(ctx, &para);
}

TEST(Loop, dual_multi) {
  struct utest_ctx* ctx = utest_get_ctx();
  struct loop_para para;

  loop_para_init(&para);
  para.dual_loop = true;
  para.sessions = 5;
  para.tx_sleep_us = 100;
  loop_sanity_test(ctx, &para);
}

TEST(Loop, dual_multi_no_sleep) {
  struct utest_ctx* ctx = utest_get_ctx();
  struct loop_para para;

  loop_para_init(&para);
  para.dual_loop = true;
  para.sessions = 10;
  para.tx_sleep_us = 0;
  loop_sanity_test(ctx, &para);
}

TEST(Loop, dual_multi_shared_max) {
  struct utest_ctx* ctx = utest_get_ctx();
  struct loop_para para;

  if (!(ctx->init_params.mt_params.flags & MTL_FLAG_SHARED_QUEUE)) {
    err("%s, skip as it's not shared mode\n", __func__);
    return;
  }

  loop_para_init(&para);
  para.dual_loop = true;
  para.sessions = mufd_get_sessions_max_nb() / 2;
  para.tx_pkts = 32;
  para.max_rx_timeout_pkts = para.tx_pkts / 2;
  para.tx_sleep_us = 0;
  loop_sanity_test(ctx, &para);
}
