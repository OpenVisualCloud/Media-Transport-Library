/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

namespace {

void st20_rx_fps_test(const std::vector<St20StreamSpec>& streams) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  const int sessions = static_cast<int>(streams.size());

  MTL_TEST_REQUIRE_PORTS(ctx, 2);

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

  int ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  guard.set_started(ret >= 0);

  /* Warmup=12 absorbs RL pacing; >=35 frames over >=1500 ms gives <=2.2% edge error. */
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

void st20_tx_fps_test(const std::vector<St20StreamSpec>& streams, bool ext_buf = false) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  const int sessions = static_cast<int>(streams.size());

  if (ext_buf) MTL_TEST_REQUIRE_IOVA_VA(ctx);

  std::vector<tests_context*> test_ctx(sessions);
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st20_tx_handle> handle(sessions);
  std::vector<st20_rx_handle> rx_handle;
  std::vector<std::thread> rtp_thread(sessions);

  St20DeinitGuard guard(m_handle, test_ctx, test_ctx_rx, handle, rx_handle, &rtp_thread,
                        nullptr);

  for (int i = 0; i < sessions; i++) {
    const auto& st = streams[i];
    test_ctx[i] = init_test_ctx(ctx, i, 3);
    test_ctx[i]->ready.store(false, std::memory_order_relaxed);

    struct st20_tx_ops ops;
    st20_tx_ops_init(test_ctx[i], &ops);
    ops.type = st.tx_type;
    ops.fps = st.fps;
    ops.width = st.width;
    ops.height = st.height;
    ops.fmt = st.fmt;
    ops.packing = ST20_PACKING_BPM;
    if (ext_buf) {
      ops.flags |= ST20_TX_FLAG_EXT_FRAME;
      ops.get_next_frame = tx_next_ext_video_frame;
      ops.notify_frame_done = tx_notify_ext_frame_done;
    } else {
      ops.notify_frame_done = tx_notify_frame_done_check_tmstamp;
    }
    if (st.tx_type == ST20_TYPE_RTP_LEVEL) {
      rtp_tx_specific_init(&ops, test_ctx[i]);
    }
    handle[i] = st20_tx_create(m_handle, &ops);
    ASSERT_TRUE(handle[i] != NULL);

    test_ctx[i]->frame_size = st20_tx_get_framebuffer_size(handle[i]);
    if (ext_buf) {
      ASSERT_TRUE(st20_alloc_ext_frames(m_handle, test_ctx[i], test_ctx[i]->frame_size));
    }
    st20_set_handle(test_ctx[i], handle[i]);
    test_ctx[i]->ready.store(true, std::memory_order_release);
    if (st.tx_type == ST20_TYPE_RTP_LEVEL) {
      rtp_thread[i] = std::thread(tx_feed_packet, test_ctx[i]);
    }
  }

  int ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  guard.set_started(ret >= 0);

  auto res = mtl_test::measure_rate_window(
      test_ctx, mtl_test::tx_frames, kSt20WarmupFrames, kSt20MinRateFrames,
      kSt20MinRateWindowMs, kSt20WarmupTimeoutMs, kSt20WindowTimeoutMs);
  EXPECT_TRUE(res.warmed_up);
  EXPECT_TRUE(res.complete);

  st20_stop_tx_ready(test_ctx);
  guard.release_sessions();

  for (int i = 0; i < sessions; i++) {
    const double expect_fps = st_frame_rate(streams[i].fps);
    EXPECT_GT(test_ctx[i]->fb_send, 0);
    if (!ext_buf && streams[i].tx_type != ST20_TYPE_RTP_LEVEL) {
      EXPECT_GT(mtl_test::tx_done_frames(test_ctx[i]), 0);
    }
    EXPECT_LE(test_ctx[i]->tx_tmstamp_delta_fail_cnt, 1);
    info("%s, session %d fb_send %d framerate %f\n", __func__, i, test_ctx[i]->fb_send,
         res.rate[i]);
    EXPECT_NEAR(res.rate[i], expect_fps, expect_fps * 0.1);
  }
}

}  // namespace

TEST(St20_tx, mix_s3) {
  st20_tx_fps_test({
      {ST20_TYPE_RTP_LEVEL, ST20_TYPE_RTP_LEVEL, ST20_PACKING_BPM, ST_FPS_P50, 1920,
       1080},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P60, 1280,
       720},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P30, 1280,
       720},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
       1280, 720},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P29_97,
       1920, 1080},
  });
}

TEST(St20_tx, ext_frame_mix_s3) {
  st20_tx_fps_test(
      {
          {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
           1280, 720},
          {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P50,
           1920, 1080},
          {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P29_97,
           3840, 2160},
      },
      /*ext_buf=*/true);
}

TEST(St20_rx, mix_s2) {
  st20_rx_fps_test({
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
       1280, 720},
      {ST20_TYPE_RTP_LEVEL, ST20_TYPE_RTP_LEVEL, ST20_PACKING_BPM, ST_FPS_P50, 1920,
       1080},
  });
}
