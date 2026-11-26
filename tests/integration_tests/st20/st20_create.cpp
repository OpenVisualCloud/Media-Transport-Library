/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

namespace {

using ScopedSt20Tx = mtl_test::ScopedHandle<st20_tx_handle, st20_tx_free>;
using ScopedSt20Rx = mtl_test::ScopedHandle<st20_rx_handle, st20_rx_free>;

}  // namespace

TEST(St20_tx, create_free_mix) {
  mtl_test::verify_create_free<struct st20_tx_ops, st20_tx_handle, st20_tx_create,
                               st20_tx_free>(st20_tx_ops_init, st20_tx_assert_cnt, 2, 3,
                                             4);
}
TEST(St20_tx, create_free_max) {
  mtl_test::verify_create_free_max<struct st20_tx_ops, st20_tx_handle, st20_tx_create,
                                   st20_tx_free>(st20_tx_ops_init, st20_tx_assert_cnt,
                                                 TEST_CREATE_FREE_MAX);
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
  struct st20_tx_ops ops;
  st20_tx_ops_init(test_ctx.get(), &ops);

  std::vector<ScopedSt20Tx> handles;
  for (int i = 0; i < want; i++) {
    st20_tx_handle h = st20_tx_create(ctx->handle, &ops);
    if (!h) break;
    handles.emplace_back(h);
    ops.udp_port[MTL_SESSION_PORT_P]++;
    ops.udp_port[MTL_SESSION_PORT_R]++;
  }
  const int created = static_cast<int>(handles.size());
  EXPECT_EQ(created, want) << "a port with fewer tx queues than --queue_cnt is the "
                              "usual cause, see the MTL log";
  if (created == want) {
    ScopedSt20Tx overflow(st20_tx_create(ctx->handle, &ops));
    EXPECT_FALSE(static_cast<bool>(overflow));
  }
  handles.clear();
  st20_tx_assert_cnt(0);
  EXPECT_EQ(sch_cnt, st_test_sch_cnt(ctx));

  ScopedSt20Tx reused(st20_tx_create(ctx->handle, &ops));
  EXPECT_TRUE(static_cast<bool>(reused));
}
TEST(St20_tx, create_expect_fail) {
  mtl_test::verify_invalid_ports<struct st20_tx_ops, st20_tx_handle, st20_tx_create,
                                 st20_tx_free>(st20_tx_ops_init);
}
TEST(St20_tx, create_expect_fail_fb_cnt) {
  mtl_test::verify_invalid_fb_cnt<struct st20_tx_ops, st20_tx_handle, st20_tx_create,
                                  st20_tx_free>(st20_tx_ops_init, 1);
  mtl_test::verify_invalid_fb_cnt<struct st20_tx_ops, st20_tx_handle, st20_tx_create,
                                  st20_tx_free>(st20_tx_ops_init, ST20_FB_MAX_COUNT + 1);
}
TEST(St20_tx, create_expect_fail_ring_sz) {
  auto rtp_ops_init = [](tests_context* c, struct st20_tx_ops* ops) {
    st20_tx_ops_init(c, ops);
    ops->type = ST20_TYPE_RTP_LEVEL;
    ops->rtp_frame_total_pkts = 1024;
    ops->rtp_pkt_size = 1260;
  };
  {
    auto* ctx = st_test_ctx();
    auto test_ctx = mtl_test::make_context(ctx, 0, 0);
    struct st20_tx_ops ops;
    rtp_ops_init(test_ctx.get(), &ops);
    ops.num_port = 1;
    ops.rtp_ring_size = 128;
    ScopedSt20Tx valid(st20_tx_create(ctx->handle, &ops));
    ASSERT_TRUE(static_cast<bool>(valid));
  }
  mtl_test::verify_rtp_ring_invalid<struct st20_tx_ops, st20_tx_handle, st20_tx_create,
                                    st20_tx_free>(rtp_ops_init, ST20_TYPE_RTP_LEVEL, 0);
  mtl_test::verify_rtp_ring_invalid<struct st20_tx_ops, st20_tx_handle, st20_tx_create,
                                    st20_tx_free>(rtp_ops_init, ST20_TYPE_RTP_LEVEL,
                                                  128 + 1);
}
TEST(St20_tx, get_framebuffer) {
  mtl_test::verify_get_framebuffer<struct st20_tx_ops, st20_tx_handle, st20_tx_create,
                                   st20_tx_free>(st20_tx_ops_init,
                                                 st20_tx_get_framebuffer, 3);
  mtl_test::verify_get_framebuffer<struct st20_tx_ops, st20_tx_handle, st20_tx_create,
                                   st20_tx_free>(
      st20_tx_ops_init, st20_tx_get_framebuffer, ST20_FB_MAX_COUNT);
}
TEST(St20_tx, get_framebuffer_expect_fail) {
  mtl_test::verify_get_framebuffer_oob<struct st20_tx_ops, st20_tx_handle, st20_tx_create,
                                       st20_tx_free>(st20_tx_ops_init,
                                                     st20_tx_get_framebuffer, 3);
  mtl_test::verify_get_framebuffer_oob<struct st20_tx_ops, st20_tx_handle, st20_tx_create,
                                       st20_tx_free>(
      st20_tx_ops_init, st20_tx_get_framebuffer, ST20_FB_MAX_COUNT);
}
/* RTP-level create takes rtp_pkt_size in [1:1452], so a 1396-byte ST 2022-6 pkt fits */
TEST(St20_tx, rtp_pkt_size) {
  mtl_test::verify_rtp_pkt_size<struct st20_tx_ops, st20_tx_handle, st20_tx_create,
                                st20_tx_free>(st20_tx_ops_init, ST20_TYPE_RTP_LEVEL, 0,
                                              false, /*set_total_pkts=*/true);
  /* the video TX limit: the 1494-byte TX frame minus the 42-byte Ethernet/IPv4/UDP hdr */
  mtl_test::verify_rtp_pkt_size<struct st20_tx_ops, st20_tx_handle, st20_tx_create,
                                st20_tx_free>(st20_tx_ops_init, ST20_TYPE_RTP_LEVEL, 1452,
                                              true, /*set_total_pkts=*/true);
  mtl_test::verify_rtp_pkt_size<struct st20_tx_ops, st20_tx_handle, st20_tx_create,
                                st20_tx_free>(st20_tx_ops_init, ST20_TYPE_RTP_LEVEL,
                                              1452 + 1, false, /*set_total_pkts=*/true);
}

TEST(St20_tx, query_params) {
  auto ctx = st_test_ctx();
  const uint16_t fb_cnt = 3;
  auto test_ctx = mtl_test::make_context(ctx, 0, fb_cnt);

  struct st20_tx_ops ops;
  st20_tx_ops_init(test_ctx.get(), &ops);
  ops.num_port = 1;
  ScopedSt20Tx handle(st20_tx_create(ctx->handle, &ops));
  ASSERT_TRUE(static_cast<bool>(handle));

  const int sch_idx = st20_tx_get_sch_idx(handle.get());
  EXPECT_GE(sch_idx, 0);
  EXPECT_LT(sch_idx, st_test_sch_cnt(ctx));
  EXPECT_EQ(st20_tx_get_framebuffer_size(handle.get()),
            st20_frame_size(ops.fmt, ops.width, ops.height));
  EXPECT_EQ(st20_tx_get_framebuffer_count(handle.get()), fb_cnt);

  double tr_offset_ns = 0.0;
  double trs_ns = 0.0;
  uint32_t vrx_pkts = 0;
  EXPECT_EQ(st20_tx_get_pacing_params(handle.get(), &tr_offset_ns, &trs_ns, &vrx_pkts),
            0);
  EXPECT_GT(tr_offset_ns, trs_ns);
  EXPECT_GE(tr_offset_ns, 500e3);
  EXPECT_LE(tr_offset_ns, 800e3);
  EXPECT_GE(trs_ns, 1e3);
  EXPECT_LE(trs_ns, 15e3);
  EXPECT_GE(vrx_pkts, 4u);
  EXPECT_LE(vrx_pkts, 16u);
  const double expected_tr_offset_ns = tr_offset_ns;
  tr_offset_ns = -1.0;
  EXPECT_EQ(st20_tx_get_pacing_params(handle.get(), &tr_offset_ns, nullptr, nullptr), 0);
  EXPECT_DOUBLE_EQ(tr_offset_ns, expected_tr_offset_ns);
  EXPECT_LT(st20_tx_get_pacing_params(nullptr, &tr_offset_ns, &trs_ns, &vrx_pkts), 0);
}

TEST(St20_rx, create_free_mix) {
  mtl_test::verify_create_free<struct st20_rx_ops, st20_rx_handle, st20_rx_create,
                               st20_rx_free>(st20_rx_ops_init, st20_rx_assert_cnt, 2, 3,
                                             4);
}
TEST(St20_rx, create_free_max) {
  mtl_test::verify_create_free_max<struct st20_rx_ops, st20_rx_handle, st20_rx_create,
                                   st20_rx_free>(st20_rx_ops_init, st20_rx_assert_cnt,
                                                 TEST_CREATE_FREE_MAX);
}
TEST(St20_rx, create_expect_fail) {
  mtl_test::verify_invalid_ports<struct st20_rx_ops, st20_rx_handle, st20_rx_create,
                                 st20_rx_free>(st20_rx_ops_init);
}
TEST(St20_rx, create_expect_fail_fb_cnt) {
  mtl_test::verify_invalid_fb_cnt<struct st20_rx_ops, st20_rx_handle, st20_rx_create,
                                  st20_rx_free>(st20_rx_ops_init, 0);
  mtl_test::verify_invalid_fb_cnt<struct st20_rx_ops, st20_rx_handle, st20_rx_create,
                                  st20_rx_free>(st20_rx_ops_init, 1);
  mtl_test::verify_invalid_fb_cnt<struct st20_rx_ops, st20_rx_handle, st20_rx_create,
                                  st20_rx_free>(st20_rx_ops_init, ST20_FB_MAX_COUNT + 1);
}
TEST(St20_rx, create_expect_fail_ring_sz) {
  mtl_test::verify_rtp_ring_invalid<struct st20_rx_ops, st20_rx_handle, st20_rx_create,
                                    st20_rx_free>(st20_rx_ops_init, ST20_TYPE_RTP_LEVEL,
                                                  0);
  mtl_test::verify_rtp_ring_invalid<struct st20_rx_ops, st20_rx_handle, st20_rx_create,
                                    st20_rx_free>(st20_rx_ops_init, ST20_TYPE_RTP_LEVEL,
                                                  128 + 1);
}

TEST(St20_rx, query_params) {
  auto ctx = st_test_ctx();
  const uint16_t fb_cnt = 3;
  auto test_ctx = mtl_test::make_context(ctx, 0, fb_cnt);

  struct st20_rx_ops ops;
  st20_rx_ops_init(test_ctx.get(), &ops);
  ops.num_port = 1;
  ScopedSt20Rx handle(st20_rx_create(ctx->handle, &ops));
  ASSERT_TRUE(static_cast<bool>(handle));

  const int sch_idx = st20_rx_get_sch_idx(handle.get());
  EXPECT_GE(sch_idx, 0);
  EXPECT_LT(sch_idx, st_test_sch_cnt(ctx));
  EXPECT_EQ(st20_rx_get_framebuffer_size(handle.get()),
            st20_frame_size(ops.fmt, ops.width, ops.height));
  EXPECT_EQ(st20_rx_get_framebuffer_count(handle.get()), fb_cnt);

  struct st20_rx_tp_pass pass = {};
  EXPECT_LT(st20_rx_timing_parser_critical(handle.get(), &pass), 0);
}

TEST(St20_rx, session_stats_reset) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  auto tx_ctx = mtl_test::make_context(ctx, 0, 3);
  auto rx_ctx = mtl_test::make_context(ctx, 0, 3);
  const St20StreamSpec st{ST20_TYPE_FRAME_LEVEL,
                          ST20_TYPE_FRAME_LEVEL,
                          ST20_PACKING_BPM,
                          ST_FPS_P59_94,
                          1280,
                          720};

  struct st20_tx_ops ops_tx;
  struct st20_rx_ops ops_rx;
  st20_fill_loopback_tx_ops(ops_tx, tx_ctx.get(), st, "st20_stats_tx",
                            udp_port_for_idx(20));
  st20_fill_loopback_rx_ops(ops_rx, rx_ctx.get(), st, "st20_stats_rx",
                            udp_port_for_idx(20));

  ScopedSt20Rx rx_handle(st20_rx_create(m_handle, &ops_rx));
  ScopedSt20Tx tx_handle(st20_tx_create(m_handle, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(rx_handle));
  ASSERT_TRUE(static_cast<bool>(tx_handle));
  st20_set_handle(rx_ctx.get(), rx_handle.get());
  st20_set_handle(tx_ctx.get(), tx_handle.get());

  mtl_test::ScopedMtlStart mtl_run(m_handle);
  ASSERT_GE(mtl_run.start(), 0);
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return mtl_test::rx_frames(rx_ctx.get()) >= 5; }, kSt20WarmupTimeoutMs));
  tx_ctx->ready.store(false, std::memory_order_release);
  EXPECT_GE(mtl_run.stop(), 0);

  struct st20_tx_user_stats tx_stats = {};
  struct st20_rx_user_stats rx_stats = {};
  ASSERT_GE(st20_tx_get_session_stats(tx_handle.get(), &tx_stats), 0);
  EXPECT_GT(tx_stats.common.port[MTL_SESSION_PORT_P].frames, 0u);
  EXPECT_GT(tx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_GE(st20_tx_reset_session_stats(tx_handle.get()), 0);
  ASSERT_GE(st20_tx_get_session_stats(tx_handle.get(), &tx_stats), 0);
  EXPECT_EQ(tx_stats.common.port[MTL_SESSION_PORT_P].frames, 0u);
  EXPECT_EQ(tx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);

  /* Free TX before resetting RX stats so no packets arrive under auto-start mode. */
  tx_handle.reset();

  ASSERT_GE(st20_rx_get_session_stats(rx_handle.get(), &rx_stats), 0);
  EXPECT_GT(rx_stats.common.port[MTL_SESSION_PORT_P].bytes, 0u);
  EXPECT_GT(rx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_GT(rx_stats.common.stat_pkts_received, 0u);
  EXPECT_GT(rx_stats.stat_bytes_received, 0u);
  EXPECT_GE(st20_rx_reset_session_stats(rx_handle.get()), 0);
  ASSERT_GE(st20_rx_get_session_stats(rx_handle.get(), &rx_stats), 0);
  EXPECT_EQ(rx_stats.common.port[MTL_SESSION_PORT_P].bytes, 0u);
  EXPECT_EQ(rx_stats.common.port[MTL_SESSION_PORT_P].packets, 0u);
  EXPECT_EQ(rx_stats.common.stat_pkts_received, 0u);
  EXPECT_EQ(rx_stats.stat_bytes_received, 0u);

  EXPECT_LT(st20_tx_get_session_stats(nullptr, &tx_stats), 0);
  EXPECT_LT(st20_tx_reset_session_stats(nullptr), 0);
  EXPECT_LT(st20_rx_get_session_stats(nullptr, &rx_stats), 0);
  EXPECT_LT(st20_rx_reset_session_stats(nullptr), 0);
}

TEST(St20_rx, timing_parser_critical) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  auto tx_ctx = mtl_test::make_context(ctx, 0, 3);
  auto rx_ctx = mtl_test::make_context(ctx, 0, 3);
  const St20StreamSpec st{ST20_TYPE_FRAME_LEVEL,
                          ST20_TYPE_FRAME_LEVEL,
                          ST20_PACKING_BPM,
                          ST_FPS_P59_94,
                          1280,
                          720};

  struct st20_tx_ops ops_tx;
  struct st20_rx_ops ops_rx;
  st20_fill_loopback_tx_ops(ops_tx, tx_ctx.get(), st, "st20_tp_tx", udp_port_for_idx(21));
  st20_fill_loopback_rx_ops(ops_rx, rx_ctx.get(), st, "st20_tp_rx", udp_port_for_idx(21));
  ops_rx.flags |= ST20_RX_FLAG_TIMING_PARSER_META;

  ScopedSt20Rx rx_handle(st20_rx_create(m_handle, &ops_rx));
  ScopedSt20Tx tx_handle(st20_tx_create(m_handle, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(rx_handle));
  ASSERT_TRUE(static_cast<bool>(tx_handle));
  st20_set_handle(rx_ctx.get(), rx_handle.get());
  st20_set_handle(tx_ctx.get(), tx_handle.get());

  mtl_test::ScopedMtlStart mtl_run(m_handle);
  ASSERT_GE(mtl_run.start(), 0);
  /* Timing parser initializes after stream auto-detection on first received frames. */
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return mtl_test::rx_frames(rx_ctx.get()) >= 5; }, kSt20WarmupTimeoutMs));

  struct st20_rx_tp_pass pass = {};
  EXPECT_EQ(st20_rx_timing_parser_critical(rx_handle.get(), &pass), 0);
  EXPECT_GE(pass.cinst_max_narrow, 4);
  EXPECT_GE(pass.cinst_max_wide, 16);
  EXPECT_GE(pass.cinst_max_wide, pass.cinst_max_narrow);
  EXPECT_GE(pass.vrx_max_narrow, 8);
  EXPECT_GE(pass.vrx_max_wide, 720);
  EXPECT_GE(pass.vrx_max_wide, pass.vrx_max_narrow);
  EXPECT_GE(pass.tr_offset, 500000);
  EXPECT_LE(pass.tr_offset, 750000);
  EXPECT_GT(pass.latency_max, 0);
  EXPECT_EQ(pass.rtp_ts_delta_min, 1501);
  EXPECT_EQ(pass.rtp_ts_delta_max, 1502);
  EXPECT_GT(pass.rtp_offset_max, 0);

  tx_ctx->ready.store(false, std::memory_order_release);
  EXPECT_GE(mtl_run.stop(), 0);
}
