/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

struct MtlStopGuard {
  mtl_handle handle;
  bool started;

  explicit MtlStopGuard(mtl_handle h) : handle(h), started(false) {
  }
  ~MtlStopGuard() {
    if (started && handle) {
      mtl_stop(handle);
    }
  }
};

static void st20_rx_after_start_test(enum st20_type type[], enum st_fps fps[],
                                     int width[], int height[], enum st20_fmt fmt,
                                     int sessions, int repeat, enum st_test_level level) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops_tx;
  struct st20_rx_ops ops_rx;

  /* return if level small than global */
  if (level < ctx->level) return;

  if (ctx->para.num_ports < 2) {
    info(
        "%s, dual port should be enabled for tx test, one for tx and one for "
        "rx\n",
        __func__);
    throw std::runtime_error("Dual port not enabled");
  }

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st20_tx_handle> tx_handle;
  std::vector<st20_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread_tx;
  std::vector<std::thread> rtp_thread_rx;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  rtp_thread_tx.resize(sessions);
  rtp_thread_rx.resize(sessions);

  MtlStopGuard mtl_guard(m_handle);

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  mtl_guard.started = (ret >= 0);

  for (int r = 0; r < repeat; r++) {
    St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle,
                          &rtp_thread_tx, &rtp_thread_rx);

    for (int i = 0; i < sessions; i++) {
      expect_framerate[i] = st_frame_rate(fps[i]);

      test_ctx_tx[i] = init_test_ctx(ctx, i, 3);
      ASSERT_TRUE(test_ctx_tx[i] != NULL);
      test_ctx_tx[i]->stop = false;

      init_single_port_tx(ops_tx, test_ctx_tx[i], "st20_test", udp_port_for_idx(i));
      ops_tx.type = type[i];
      ops_tx.width = width[i];
      ops_tx.height = height[i];
      ops_tx.fps = fps[i];
      ops_tx.fmt = fmt;
      ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
      ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
      ops_tx.get_next_frame = tx_next_video_frame;
      if (type[i] == ST20_TYPE_RTP_LEVEL) {
        rtp_tx_specific_init(&ops_tx, test_ctx_tx[i]);
      }
      tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
      ASSERT_TRUE(tx_handle[i] != NULL);
      test_ctx_tx[i]->handle = tx_handle[i];
      if (type[i] == ST20_TYPE_RTP_LEVEL) {
        test_ctx_tx[i]->stop = false;
        rtp_thread_tx[i] = std::thread(tx_feed_packet, test_ctx_tx[i]);
      }
    }

    for (int i = 0; i < sessions; i++) {
      test_ctx_rx[i] = init_test_ctx(ctx, i, 3);
      ASSERT_TRUE(test_ctx_rx[i] != NULL);
      test_ctx_rx[i]->stop = false;

      init_single_port_rx(ops_rx, test_ctx_rx[i], "st20_test", udp_port_for_idx(i));
      ops_rx.type = type[i];
      ops_rx.width = width[i];
      ops_rx.height = height[i];
      ops_rx.fps = fps[i];
      ops_rx.fmt = fmt;
      ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
      ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
      ops_rx.notify_frame_ready = st20_rx_frame_ready;
      ops_rx.notify_rtp_ready = rx_rtp_ready;
      rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
      ASSERT_TRUE(rx_handle[i] != NULL);

      test_ctx_rx[i]->total_pkts_in_frame = test_ctx_tx[i]->total_pkts_in_frame;
      test_ctx_rx[i]->handle = rx_handle[i];
      if (type[i] == ST20_TYPE_RTP_LEVEL) {
        test_ctx_rx[i]->stop = false;
        rtp_thread_rx[i] = std::thread(rx_get_packet, test_ctx_rx[i]);
      }
    }

    sleep(10);

    for (int i = 0; i < sessions; i++) {
      uint64_t cur_time_ns = st_test_get_monotonic_time();
      double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
      framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;
    }

    guard.stop();

    for (int i = 0; i < sessions; i++) {
      EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
      info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
           framerate[i]);
      EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    }

    sleep(1);
  }
}

TEST(St20_rx, after_start_frame_720p_fps50_s1_r1) {
  enum st20_type type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1280};
  int height[1] = {720};
  st20_rx_after_start_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 1, 1,
                           ST_TEST_LEVEL_MANDATORY);
}

TEST(St20_rx, after_start_frame_720p_fps29_97_s1_r2) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P29_97};
  int width[1] = {1280};
  int height[1] = {720};
  st20_rx_after_start_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 1, 2,
                           ST_TEST_LEVEL_ALL);
}