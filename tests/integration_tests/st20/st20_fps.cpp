/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

namespace {

struct St20FpsStream {
  enum st20_type type;
  enum st_fps fps;
  int width;
  int height;
};

struct St20FpsScenario {
  std::vector<St20FpsStream> streams;
  enum st20_fmt fmt = ST20_FMT_YUV_422_10BIT;
  bool ext_buf = false;
};

void st20_rx_fps_test(const St20FpsScenario& s) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  const int sessions = static_cast<int>(s.streams.size());

  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  if (s.ext_buf) MTL_TEST_REQUIRE_IOVA_VA(ctx);

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

    if (s.ext_buf) {
      size_t frame_size = st20_frame_size(s.fmt, st.width, st.height);
      ASSERT_TRUE(st20_alloc_ext_frames(m_handle, test_ctx_rx[i], frame_size));
    }

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
    if (s.ext_buf) {
      ops_rx.ext_frames = test_ctx_rx[i]->ext_frames;
    }
    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    test_ctx_rx[i]->total_pkts_in_frame = test_ctx_tx[i]->total_pkts_in_frame;
    st20_set_handle(test_ctx_rx[i], rx_handle[i]);
    if (st.type == ST20_TYPE_RTP_LEVEL) {
      rtp_thread_rx[i] = std::thread(rx_get_packet, test_ctx_rx[i]);
    }
  }

  /* Publish TX handles and start TX feeds only after every RX session is ready. */
  for (int i = 0; i < sessions; i++) {
    st20_set_handle(test_ctx_tx[i], tx_handle[i]);
    test_ctx_tx[i]->ready.store(true, std::memory_order_release);
    if (s.streams[i].type == ST20_TYPE_RTP_LEVEL) {
      rtp_thread_tx[i] = std::thread(tx_feed_packet, test_ctx_tx[i]);
    }
  }

  int ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  guard.set_started(ret >= 0);

  /*
   * Measure steady-state RX FPS: warmup=12 absorbs RL pacing training, then
   * >=35 frames over >=1500 ms (<=2.2% error at 29.97 fps vs +/-10% tolerance).
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

void st20_tx_fps_test(const St20FpsScenario& s) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  const int sessions = static_cast<int>(s.streams.size());

  if (s.ext_buf) MTL_TEST_REQUIRE_IOVA_VA(ctx);

  std::vector<tests_context*> test_ctx(sessions);
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st20_tx_handle> handle(sessions);
  std::vector<st20_rx_handle> rx_handle;
  std::vector<double> expect_framerate(sessions);
  std::vector<std::thread> rtp_thread(sessions);

  St20DeinitGuard guard(m_handle, test_ctx, test_ctx_rx, handle, rx_handle, &rtp_thread,
                        nullptr);

  for (int i = 0; i < sessions; i++) {
    const auto& st = s.streams[i];
    expect_framerate[i] = st_frame_rate(st.fps);
    test_ctx[i] = init_test_ctx(ctx, i, 3);
    ASSERT_TRUE(test_ctx[i] != NULL);
    test_ctx[i]->ready.store(false, std::memory_order_relaxed);

    struct st20_tx_ops ops;
    st20_tx_ops_init(test_ctx[i], &ops);
    ops.type = st.type;
    ops.fps = st.fps;
    ops.width = st.width;
    ops.height = st.height;
    ops.fmt = s.fmt;
    ops.packing = ST20_PACKING_BPM;
    if (s.ext_buf) {
      ops.flags |= ST20_TX_FLAG_EXT_FRAME;
      ops.get_next_frame = tx_next_ext_video_frame;
      ops.notify_frame_done = tx_notify_ext_frame_done;
    } else {
      ops.notify_frame_done = tx_notify_frame_done_check_tmstamp;
    }
    if (st.type == ST20_TYPE_RTP_LEVEL) {
      rtp_tx_specific_init(&ops, test_ctx[i]);
    }
    handle[i] = st20_tx_create(m_handle, &ops);
    ASSERT_TRUE(handle[i] != NULL);

    size_t frame_size = st20_tx_get_framebuffer_size(handle[i]);
    test_ctx[i]->frame_size = frame_size;

    if (s.ext_buf) {
      ASSERT_TRUE(st20_alloc_ext_frames(m_handle, test_ctx[i], frame_size));
    }

    st20_set_handle(test_ctx[i], handle[i]);
    test_ctx[i]->ready.store(true, std::memory_order_release);
    if (st.type == ST20_TYPE_RTP_LEVEL) {
      test_ctx[i]->stop = false;
      rtp_thread[i] = std::thread(tx_feed_packet, test_ctx[i]);
    }
  }

  int ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  guard.set_started(ret >= 0);

  /*
   * Measure steady-state TX FPS: warmup=12 absorbs RL pacing training on 1 or 2
   * ports, then >=35 frames over >=1500 ms (<=2.2% error at 29.97 fps vs +/-10%).
   */
  auto res = mtl_test::measure_rate_window(
      test_ctx, mtl_test::tx_frames, kSt20WarmupFrames, kSt20MinRateFrames,
      kSt20MinRateWindowMs, kSt20WarmupTimeoutMs, kSt20WindowTimeoutMs);
  EXPECT_TRUE(res.warmed_up);
  EXPECT_TRUE(res.complete);

  for (int i = 0; i < sessions; i++)
    test_ctx[i]->ready.store(false, std::memory_order_release);
  guard.release_sessions();

  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx[i]->fb_send, 0);
    EXPECT_LE(test_ctx[i]->tx_tmstamp_delta_fail_cnt, 1);
    info("%s, session %d fb_send %d framerate %f\n", __func__, i, test_ctx[i]->fb_send,
         res.rate[i]);
    EXPECT_NEAR(res.rate[i], expect_framerate[i], expect_framerate[i] * 0.1);
  }
}

}  // namespace

TEST(St20_tx, mix_s3) {
  St20FpsScenario sc;
  sc.streams = {
      {ST20_TYPE_RTP_LEVEL, ST_FPS_P50, 1920, 1080},
      {ST20_TYPE_FRAME_LEVEL, ST_FPS_P59_94, 1280, 720},
      {ST20_TYPE_FRAME_LEVEL, ST_FPS_P29_97, 1920, 1080},
  };
  sc.fmt = ST20_FMT_YUV_422_10BIT;
  sc.ext_buf = false;
  st20_tx_fps_test(sc);
}

TEST(St20_tx, ext_frame_mix_s3) {
  St20FpsScenario sc;
  sc.streams = {
      {ST20_TYPE_FRAME_LEVEL, ST_FPS_P59_94, 1280, 720},
      {ST20_TYPE_FRAME_LEVEL, ST_FPS_P50, 1920, 1080},
      {ST20_TYPE_FRAME_LEVEL, ST_FPS_P29_97, 3840, 2160},
  };
  sc.fmt = ST20_FMT_YUV_422_10BIT;
  sc.ext_buf = true;
  st20_tx_fps_test(sc);
}

TEST(St20_tx, fps_30_60_s2) {
  St20FpsScenario sc;
  sc.streams = {
      {ST20_TYPE_FRAME_LEVEL, ST_FPS_P30, 1920, 1080},
      {ST20_TYPE_FRAME_LEVEL, ST_FPS_P60, 1920, 1080},
  };
  sc.fmt = ST20_FMT_YUV_422_10BIT;
  sc.ext_buf = false;
  st20_tx_fps_test(sc);
}

TEST(St20_rx, mix_s2) {
  St20FpsScenario sc;
  sc.streams = {
      {ST20_TYPE_FRAME_LEVEL, ST_FPS_P59_94, 1280, 720},
      {ST20_TYPE_RTP_LEVEL, ST_FPS_P50, 1920, 1080},
  };
  sc.fmt = ST20_FMT_YUV_422_10BIT;
  sc.ext_buf = false;
  st20_rx_fps_test(sc);
}

TEST(St20_rx, ext_frame_mix_s2) {
  St20FpsScenario sc;
  sc.streams = {
      {ST20_TYPE_FRAME_LEVEL, ST_FPS_P59_94, 1280, 720},
      {ST20_TYPE_FRAME_LEVEL, ST_FPS_P50, 1920, 1080},
  };
  sc.fmt = ST20_FMT_YUV_422_10BIT;
  sc.ext_buf = true;
  st20_rx_fps_test(sc);
}
