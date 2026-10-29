/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <algorithm>

#include "st20_common.h"
#include "tests.hpp"

TEST(St20_tx, create_free_single) {
  create_free_test(st20_tx, 0, 1, 1);
}
TEST(St20_tx, create_free_multi) {
  create_free_test(st20_tx, 0, 1, 6);
}
TEST(St20_tx, create_free_mix) {
  create_free_test(st20_tx, 2, 3, 4);
}
TEST(St20_tx, create_free_max) {
  create_free_max(st20_tx, TEST_CREATE_FREE_MAX);
}
TEST(St20_tx, create_expect_fail) {
  expect_fail_test(st20_tx);
}
TEST(St20_tx, create_expect_fail_fb_cnt) {
  uint16_t fbcnt = 1;
  expect_fail_test_fb_cnt(st20_tx, fbcnt);
  fbcnt = ST20_FB_MAX_COUNT + 1;
  expect_fail_test_fb_cnt(st20_tx, fbcnt);
}
TEST(St20_tx, create_expect_fail_ring_sz) {
  uint16_t ring_size = 0;
  expect_fail_test_rtp_ring(st20_tx, ST20_TYPE_RTP_LEVEL, ring_size);
  ring_size = 128 + 1;
  expect_fail_test_rtp_ring(st20_tx, ST20_TYPE_RTP_LEVEL, ring_size);
}
TEST(St20_tx, get_framebuffer) {
  uint16_t fbcnt = 3;
  test_get_framebuffer(st20_tx, fbcnt);
  fbcnt = ST20_FB_MAX_COUNT;
  test_get_framebuffer(st20_tx, fbcnt);
}
TEST(St20_tx, get_framebuffer_expect_fail) {
  uint16_t fbcnt = 3;
  expect_fail_test_get_framebuffer(st20_tx, fbcnt);
  fbcnt = ST20_FB_MAX_COUNT;
  expect_fail_test_get_framebuffer(st20_tx, fbcnt);
}
TEST(St20_tx, rtp_pkt_size) {
  uint16_t rtp_pkt_size = 0;
  expect_test_rtp_pkt_size(st20_tx, ST20_TYPE_RTP_LEVEL, rtp_pkt_size, false);
  rtp_pkt_size = MTL_PKT_MAX_RTP_BYTES;
  expect_test_rtp_pkt_size(st20_tx, ST20_TYPE_RTP_LEVEL, rtp_pkt_size, true);
  rtp_pkt_size = MTL_PKT_MAX_RTP_BYTES + 1;
  expect_test_rtp_pkt_size(st20_tx, ST20_TYPE_RTP_LEVEL, rtp_pkt_size, false);
}

TEST(St20_rx, create_free_single) {
  create_free_test(st20_rx, 0, 1, 1);
}
TEST(St20_rx, create_free_multi) {
  create_free_test(st20_rx, 0, 1, 6);
}
TEST(St20_rx, create_free_mix) {
  create_free_test(st20_rx, 2, 3, 4);
}
TEST(St20_rx, create_free_max) {
  create_free_max(st20_rx, TEST_CREATE_FREE_MAX);
}
TEST(St20_rx, create_expect_fail) {
  expect_fail_test(st20_rx);
}
TEST(St20_rx, create_expect_fail_fb_cnt) {
  uint16_t fbcnt = 0;
  expect_fail_test_fb_cnt(st20_rx, fbcnt);
  fbcnt = ST20_FB_MAX_COUNT + 1;
  expect_fail_test_fb_cnt(st20_rx, fbcnt);
}
TEST(St20_rx, create_expect_fail_ring_sz) {
  uint16_t ring_size = 0;
  expect_fail_test_rtp_ring(st20_rx, ST20_TYPE_RTP_LEVEL, ring_size);
  ring_size = 128 + 1;
  expect_fail_test_rtp_ring(st20_rx, ST20_TYPE_RTP_LEVEL, ring_size);
}

/* At the default quota and with no DMA, one scheduler holds per_sch 1080p59 RX sessions
 * of type and the next takes another. The sessions alternate over the ports, all on port
 * P's NUMA node, so the RX queues of one port do not cap the count. */
static void st20_rx_no_dma_sch_pack_test(enum st20_type type, int per_sch) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  int num_ports = ctx->same_dual_port ? 1 : ctx->para.num_ports;
  int rx_queues = 0;
  for (int i = 0; i < num_ports; i++) rx_queues += ctx->para.rx_queues_cnt[i];
  if (ctx->para.data_quota_mbs_per_sch) {
    info("%s, skip as a user scheduler quota is set\n", __func__);
    return;
  }
  if (rx_queues <= per_sch) {
    info("%s, skip as %d rx queues are fewer than %d sessions\n", __func__, rx_queues,
         per_sch + 1);
    return;
  }
  int socket = mtl_get_numa_id(m_handle, MTL_PORT_P);
  ASSERT_GE(socket, 0);
  int sch_cnt = st_test_sch_cnt(ctx);

  auto test_ctx = new tests_context();
  ASSERT_TRUE(test_ctx != NULL);
  test_ctx->ctx = ctx;
  test_ctx->fb_cnt = 2;
  struct st20_rx_ops ops;
  st20_rx_ops_init(test_ctx, &ops);
  ops.type = type;
  ops.num_port = 1;
  ops.flags |= ST20_RX_FLAG_FORCE_NUMA;
  ops.socket_id = socket;
  ops.notify_slice_ready = [](void*, void*, struct st20_rx_slice_meta*) { return 0; };
  /* slice mode refuses a session without it */
  if (type == ST20_TYPE_SLICE_LEVEL) ops.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;

  std::vector<st20_rx_handle> handles;
  std::vector<int> schs;
  for (int i = 0; i <= per_sch; i++) {
    int port = i % num_ports;
    memcpy(ops.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[port], MTL_IP_ADDR_LEN);
    snprintf(ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s", ctx->para.port[port]);
    ops.udp_port[MTL_SESSION_PORT_P] = udp_port_for_idx(i);
    st20_rx_handle handle = st20_rx_create(m_handle, &ops);
    if (!handle) break;
    handles.push_back(handle);
    schs.push_back(st20_rx_get_sch_idx(handle));
  }
  for (auto it = handles.rbegin(); it != handles.rend(); ++it)
    EXPECT_GE(st20_rx_free(*it), 0);
  delete test_ctx;

  ASSERT_EQ((int)schs.size(), per_sch + 1);
  EXPECT_EQ(std::count(schs.begin(), schs.end() - 1, schs[0]), per_sch);
  EXPECT_NE(schs[per_sch], schs[0]);
  EXPECT_EQ(sch_cnt, st_test_sch_cnt(ctx));
}
TEST(St20_rx, no_dma_frame_sessions_per_sch) {
  st20_rx_no_dma_sch_pack_test(ST20_TYPE_FRAME_LEVEL, 11);
}
TEST(St20_rx, no_dma_slice_sessions_per_sch) {
  st20_rx_no_dma_sch_pack_test(ST20_TYPE_SLICE_LEVEL, 8);
}
