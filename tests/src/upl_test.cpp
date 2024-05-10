/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include "upl_test.h"

#include <getopt.h>

#include "log.h"

static struct uplt_ctx* g_uplt_ctx;

struct uplt_ctx* uplt_get_ctx(void) {
  return g_uplt_ctx;
}

static void uplt_ctx_init(struct uplt_ctx* ctx) {
  inet_pton(AF_INET, "192.168.89.80", ctx->sip_addr[UPLT_PORT_P]);
  inet_pton(AF_INET, "192.168.89.81", ctx->sip_addr[UPLT_PORT_R]);

  uint8_t* p_ip;
  srand(st_test_get_monotonic_time());
  p_ip = ctx->mcast_ip_addr;
  p_ip[0] = 239;
  p_ip[1] = 187;
  p_ip[2] = rand() % 0xFF;
  p_ip[3] = rand() % 0xFF;
}

static void uplt_ctx_uinit(struct uplt_ctx* ctx) {
  st_test_free(ctx);
}

static int uplt_set_port(int port) {
  char port_u[16];
  snprintf(port_u, sizeof(port_u), "%d", port);

  setenv("MUFD_PORT", port_u, 1);
  return 0;
}

int uplt_socket_port(int domain, int type, int protocol, int port) {
  uplt_set_port(port);
  return socket(domain, type, protocol);
}

static void socket_single_test(int port) {
  int ret = uplt_socket_port(AF_INET, SOCK_DGRAM, 0, port);
  EXPECT_GE(ret, 0);
  if (ret < 0) return;

  int fd = ret;
  ret = close(fd);
  EXPECT_GE(ret, 0);
}

TEST(Api, socket_single) {
  socket_single_test(UPLT_PORT_P);
}
TEST(Api, socket_single_r) {
  socket_single_test(UPLT_PORT_R);
}
TEST(Api, socket_single_port_max) {
  socket_single_test(32);
}

static int check_r_port_alive(struct uplt_ctx* ctx) {
  int tx_fd = -1;
  int rx_fd = -1;
  int ret = -EIO;
  struct sockaddr_in rx_addr;
  size_t payload_len = 1024;
  char* send_buf = new char[payload_len];
  char* recv_buf = new char[payload_len];
  st_test_rand_data((uint8_t*)send_buf, payload_len, 0);
  /* max timeout 3 min */
  int sleep_ms = 10;
  int max_retry = 1000 / sleep_ms * 60 * 3;
  int retry = 0;

  uplt_init_sockaddr(&rx_addr, ctx->sip_addr[1], 20000);

  ret = uplt_socket_port(AF_INET, SOCK_DGRAM, 0, UPLT_PORT_P);
  if (ret < 0) goto out;
  tx_fd = ret;

  ret = uplt_socket_port(AF_INET, SOCK_DGRAM, 0, UPLT_PORT_R);
  if (ret < 0) goto out;
  rx_fd = ret;

  ret = bind(rx_fd, (const struct sockaddr*)&rx_addr, sizeof(rx_addr));
  if (ret < 0) goto out;

  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 1000;
  ret = setsockopt(rx_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  if (ret < 0) goto out;

  info("%s, start to rx port status\n", __func__);
  while (retry < max_retry) {
    if (sendto(tx_fd, send_buf, payload_len, 0, (const struct sockaddr*)&rx_addr,
               sizeof(rx_addr)) < 0)
      continue;
    ssize_t recv = recvfrom(rx_fd, recv_buf, payload_len, 0, NULL, NULL);
    if (recv > 0) {
      info("%s, rx port alive at %d\n", __func__, retry);
      ret = 0;
      break;
    }
    retry++;
    st_usleep(sleep_ms * 1000);
  }

out:
  if (tx_fd > 0) close(tx_fd);
  if (rx_fd > 0) close(rx_fd);
  delete[] send_buf;
  delete[] recv_buf;
  return ret;
}

enum uplt_args_cmd {
  UPLT_ARG_UNKNOWN = 0,
  UPLT_ARG_P_SIP = 0x100, /* start from end of ascii */
  UPLT_ARG_R_SIP,
};

static struct option uplt_args_options[] = {
    {"p_sip", required_argument, 0, UPLT_ARG_P_SIP},
    {"r_sip", required_argument, 0, UPLT_ARG_R_SIP},
    {0, 0, 0, 0}};

static int uplt_parse_args(struct uplt_ctx* ctx, int argc, char** argv) {
  int cmd = -1, opt_idx = 0;

  while (1) {
    cmd = getopt_long_only(argc, argv, "hv", uplt_args_options, &opt_idx);
    if (cmd == -1) break;
    dbg("%s, cmd %d %s\n", __func__, cmd, optarg);

    switch (cmd) {
      case UPLT_ARG_P_SIP:
        inet_pton(AF_INET, optarg, ctx->sip_addr[0]);
        break;
      case UPLT_ARG_R_SIP:
        inet_pton(AF_INET, optarg, ctx->sip_addr[1]);
        break;
      default:
        break;
    }
  };

  return 0;
}

GTEST_API_ int main(int argc, char** argv) {
  struct uplt_ctx* ctx;
  int ret;
  bool link_flap_wa = true;

  testing::InitGoogleTest(&argc, argv);

  ctx = (struct uplt_ctx*)st_test_zmalloc(sizeof(*ctx));
  if (!ctx) {
    err("%s, ctx alloc fail\n", __func__);
    return -ENOMEM;
  }

  g_uplt_ctx = ctx;
  uplt_ctx_init(ctx);
  uplt_parse_args(ctx, argc, argv);

  uint64_t start_time_ns = st_test_get_monotonic_time();

  /* before test we should make sure the rx port is ready */
  ret = check_r_port_alive(ctx);

  if (ret >= 0) ret = RUN_ALL_TESTS();

  uint64_t end_time_ns = st_test_get_monotonic_time();
  int time_s = (end_time_ns - start_time_ns) / NS_PER_S;
  int time_least = 10;
  if (link_flap_wa && (time_s < time_least)) {
    /* wa for linkFlapErrDisabled in the hub */
    info("%s, sleep %ds before disable the port\n", __func__, time_least - time_s);
    sleep(time_least - time_s);
  }

  uplt_ctx_uinit(ctx);
  return ret;
}
