/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

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
TEST(St20_tx, create_queue_cnt) {
  auto ctx = st_test_ctx();
  if (ctx->para.pmd[MTL_PORT_P] != MTL_PMD_DPDK_USER ||
      (ctx->para.flags & MTL_FLAG_SHARED_TX_QUEUE))
    GTEST_SKIP() << "needs one dedicated DPDK tx queue per session";

  /* the port holds queue_cnt session queues plus the system queue */
  const int want = ctx->para.tx_queues_cnt[MTL_PORT_P];
  auto sch_cnt = st_test_sch_cnt(ctx);
  auto test_ctx = mtl_test::make_context(ctx, 0, 2);
  ASSERT_TRUE(test_ctx != nullptr);
  struct st20_tx_ops ops;
  st20_tx_ops_init(test_ctx.get(), &ops);

  std::vector<mtl_test::ScopedHandle<st20_tx_handle, st20_tx_free>> handles;
  for (int i = 0; i < want; i++) {
    st20_tx_handle h = st20_tx_create(ctx->handle, &ops);
    if (!h) break;
    handles.emplace_back(h);
    ops.udp_port[MTL_SESSION_PORT_P]++;
    ops.udp_port[MTL_SESSION_PORT_R]++;
  }
  const int created = static_cast<int>(handles.size());
  handles.clear();
  EXPECT_EQ(sch_cnt, st_test_sch_cnt(ctx));

  EXPECT_EQ(created, want) << "a port with fewer tx queues than --queue_cnt is the "
                              "usual cause, see the MTL log";
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
/* RTP-level create takes rtp_pkt_size in [1:1452], so a 1396-byte ST 2022-6 pkt fits */
TEST(St20_tx, rtp_pkt_size) {
  uint16_t rtp_pkt_size = 0;
  expect_test_rtp_pkt_size(st20_tx, ST20_TYPE_RTP_LEVEL, rtp_pkt_size, false);
  /* the video TX limit: the 1494-byte TX frame minus the 42-byte Ethernet/IPv4/UDP hdr */
  rtp_pkt_size = 1452;
  expect_test_rtp_pkt_size(st20_tx, ST20_TYPE_RTP_LEVEL, rtp_pkt_size, true);
  rtp_pkt_size = 1452 + 1;
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
