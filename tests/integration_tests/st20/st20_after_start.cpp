/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

namespace {

void st20_rx_after_start_test(const std::vector<St20StreamSpec>& streams, int repeat) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  const int sessions = static_cast<int>(streams.size());

  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  mtl_test::ScopedMtlStart mtl_guard(m_handle);
  EXPECT_GE(mtl_guard.start(), 0);

  for (int r = 0; r < repeat; r++) {
    std::vector<tests_context*> test_ctx_tx(sessions);
    std::vector<tests_context*> test_ctx_rx(sessions);
    std::vector<st20_tx_handle> tx_handle(sessions);
    std::vector<st20_rx_handle> rx_handle(sessions);
    std::vector<std::thread> rtp_thread_tx(sessions);
    std::vector<std::thread> rtp_thread_rx(sessions);

    St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle,
                          &rtp_thread_tx, &rtp_thread_rx);

    st20_setup_basic_loopback(ctx, streams, "st20_test", test_ctx_tx, test_ctx_rx,
                              tx_handle, rx_handle, rtp_thread_tx, rtp_thread_rx);
    if (::testing::Test::HasFatalFailure()) return;

    /* Warmup=12 absorbs startup; >=35 frames over >=1500 ms gives <=2.2% edge error. */
    auto res = st20_wait_rx_window(test_ctx_rx, true, false);
    EXPECT_TRUE(res.warmed_up);
    EXPECT_TRUE(res.complete);
    st20_stop_tx_ready(test_ctx_tx);
    guard.release_sessions();

    for (int i = 0; i < sessions; i++) {
      const double expect_fps = st_frame_rate(streams[i].fps);
      EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
      info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
           res.rate[i]);
      EXPECT_NEAR(res.rate[i], expect_fps, expect_fps * 0.1);
    }
  }
}

}  // namespace

TEST(St20_rx, after_start_frame_720p_fps50_s1_r1) {
  st20_rx_after_start_test({{ST20_TYPE_RTP_LEVEL, ST20_TYPE_RTP_LEVEL, ST20_PACKING_BPM,
                             ST_FPS_P50, 1280, 720}},
                           1);
}

TEST(St20_rx, after_start_frame_720p_fps29_97_s1_r2) {
  st20_rx_after_start_test({{ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                             ST20_PACKING_BPM, ST_FPS_P29_97, 1280, 720}},
                           2);
}
