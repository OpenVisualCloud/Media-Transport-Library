/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

namespace {

struct St20AfterStartStream {
  enum st20_type type;
  enum st_fps fps;
  int width;
  int height;
};

struct St20AfterStartScenario {
  std::vector<St20AfterStartStream> streams;
  enum st20_fmt fmt = ST20_FMT_YUV_422_10BIT;
  int repeat = 1;
  enum st_test_level level = ST_TEST_LEVEL_ALL;
};

void st20_rx_after_start_test(const St20AfterStartScenario& s) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  const int sessions = static_cast<int>(s.streams.size());

  MTL_TEST_REQUIRE_LEVEL(ctx, s.level);
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  mtl_test::ScopedMtlStart mtl_guard(m_handle);
  EXPECT_GE(mtl_guard.start(), 0);

  for (int r = 0; r < s.repeat; r++) {
    std::vector<tests_context*> test_ctx_tx(sessions);
    std::vector<tests_context*> test_ctx_rx(sessions);
    std::vector<st20_tx_handle> tx_handle(sessions);
    std::vector<st20_rx_handle> rx_handle(sessions);
    std::vector<double> expect_framerate(sessions);
    std::vector<std::thread> rtp_thread_tx(sessions);
    std::vector<std::thread> rtp_thread_rx(sessions);

    St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle,
                          &rtp_thread_tx, &rtp_thread_rx);

    for (int i = 0; i < sessions; i++) {
      const auto& st = s.streams[i];
      expect_framerate[i] = st_frame_rate(st.fps);

      test_ctx_tx[i] = init_test_ctx(ctx, i, 3);
      ASSERT_TRUE(test_ctx_tx[i] != NULL);
      test_ctx_tx[i]->ready.store(false, std::memory_order_relaxed);
      test_ctx_tx[i]->stop = false;

      struct st20_tx_ops ops_tx;
      init_single_port_tx(ops_tx, test_ctx_tx[i], "st20_test", udp_port_for_idx(i));
      ops_tx.type = st.type;
      ops_tx.width = st.width;
      ops_tx.height = st.height;
      ops_tx.fps = st.fps;
      ops_tx.fmt = s.fmt;
      ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
      ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
      ops_tx.get_next_frame = tx_next_video_frame;
      if (st.type == ST20_TYPE_RTP_LEVEL) {
        rtp_tx_specific_init(&ops_tx, test_ctx_tx[i]);
      }
      tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
      ASSERT_TRUE(tx_handle[i] != NULL);
    }

    for (int i = 0; i < sessions; i++) {
      const auto& st = s.streams[i];
      test_ctx_rx[i] = init_test_ctx(ctx, i, 3);
      ASSERT_TRUE(test_ctx_rx[i] != NULL);
      test_ctx_rx[i]->stop = false;

      struct st20_rx_ops ops_rx;
      init_single_port_rx(ops_rx, test_ctx_rx[i], "st20_test", udp_port_for_idx(i));
      ops_rx.type = st.type;
      ops_rx.width = st.width;
      ops_rx.height = st.height;
      ops_rx.fps = st.fps;
      ops_rx.fmt = s.fmt;
      ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
      ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
      ops_rx.notify_frame_ready = st20_rx_frame_ready;
      ops_rx.notify_rtp_ready = rx_rtp_ready;
      rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
      ASSERT_TRUE(rx_handle[i] != NULL);

      test_ctx_rx[i]->total_pkts_in_frame = test_ctx_tx[i]->total_pkts_in_frame;
      st20_set_handle(test_ctx_rx[i], rx_handle[i]);
      if (st.type == ST20_TYPE_RTP_LEVEL) {
        rtp_thread_rx[i] = std::thread(rx_get_packet, test_ctx_rx[i]);
      }
    }

    /* MTL is already running; publish TX handles and release ready gate after RX setup.
     */
    for (int i = 0; i < sessions; i++) {
      st20_set_handle(test_ctx_tx[i], tx_handle[i]);
      test_ctx_tx[i]->ready.store(true, std::memory_order_release);
      if (s.streams[i].type == ST20_TYPE_RTP_LEVEL) {
        rtp_thread_tx[i] = std::thread(tx_feed_packet, test_ctx_tx[i]);
      }
    }

    /*
     * Measure steady-state RX FPS: warmup=3 absorbs pacing startup, >=25 frames
     * over >=1000 ms gives <=3.3% quantization error at 29.97 fps vs +/-10%.
     */
    auto res = st20_wait_rx_window(test_ctx_rx, true, false);
    EXPECT_TRUE(res.warmed_up);
    EXPECT_TRUE(res.complete);
    for (int i = 0; i < sessions; i++)
      test_ctx_tx[i]->ready.store(false, std::memory_order_release);
    guard.release_sessions();

    for (int i = 0; i < sessions; i++) {
      EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
      info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
           res.rate[i]);
      EXPECT_NEAR(res.rate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    }
  }
}

}  // namespace

TEST(St20_rx, after_start_frame_720p_fps50_s1_r1) {
  st20_rx_after_start_test({
      {{ST20_TYPE_RTP_LEVEL, ST_FPS_P50, 1280, 720}},
      ST20_FMT_YUV_422_10BIT,
      1,
      ST_TEST_LEVEL_MANDATORY,
  });
}

TEST(St20_rx, after_start_frame_720p_fps29_97_s1_r2) {
  st20_rx_after_start_test({
      {{ST20_TYPE_FRAME_LEVEL, ST_FPS_P29_97, 1280, 720}},
      ST20_FMT_YUV_422_10BIT,
      2,
      ST_TEST_LEVEL_ALL,
  });
}
