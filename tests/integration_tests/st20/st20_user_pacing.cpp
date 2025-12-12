/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

static void st20_tx_user_pacing_test(int width[], int height[], enum st20_fmt fmt[],
                                     bool user_pacing[], bool user_timestamp[],
                                     enum st_test_level level, int sessions = 1) {
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
  std::vector<double> rx_framerate;
  std::vector<double> tx_framerate;
  std::vector<std::thread> sha_thread_rx;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  rx_framerate.resize(sessions);
  tx_framerate.resize(sessions);
  sha_thread_rx.resize(sessions);

  St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle, nullptr,
                        &sha_thread_rx);
  guard.set_rx_ctx_cleanup(st20_rx_drain_bufq_put_framebuff);

  enum st_fps fps = ST_FPS_P59_94;

  for (int i = 0; i < sessions; i++) {
    if (user_pacing[i])
      expect_framerate[i] = st_frame_rate(fps) / 2;
    else
      expect_framerate[i] = st_frame_rate(fps);
    test_ctx_tx[i] = init_test_ctx(ctx, i, TEST_SHA_HIST_NUM, true);
    ASSERT_TRUE(test_ctx_tx[i] != NULL);
    test_ctx_tx[i]->user_pacing = user_pacing[i];
    test_ctx_tx[i]->user_timestamp = user_timestamp[i];
    test_ctx_tx[i]->frame_time = (double)NS_PER_S / st_frame_rate(fps);

    init_single_port_tx(ops_tx, test_ctx_tx[i], "st20_timestamp_test",
                        udp_port_for_idx(i));
    ops_tx.packing = ST20_PACKING_BPM;
    ops_tx.type = ST20_TYPE_FRAME_LEVEL;
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.interlaced = false;
    ops_tx.fps = fps;
    ops_tx.fmt = fmt[i];
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = tx_next_video_frame_timestamp;
    ops_tx.notify_frame_done = tx_notify_timestamp_frame_done;
    if (user_pacing[i]) ops_tx.flags |= ST20_TX_FLAG_USER_PACING;
    if (user_timestamp[i]) ops_tx.flags |= ST20_TX_FLAG_USER_TIMESTAMP;

    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    /* sha calculate */
    struct st20_pgroup st20_pg;
    st20_get_pgroup(ops_tx.fmt, &st20_pg);
    size_t frame_size = ops_tx.width * ops_tx.height * st20_pg.size / st20_pg.coverage;
    test_ctx_tx[i]->frame_size = frame_size;
    test_ctx_tx[i]->height = ops_tx.height;
    test_ctx_tx[i]->stride = ops_tx.width / st20_pg.coverage * st20_pg.size;
    uint8_t* fb;
    for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
      fb = (uint8_t*)st20_tx_get_framebuffer(tx_handle[i], frame);
      ASSERT_TRUE(fb != NULL);
      st_test_rand_data(fb, frame_size, frame);
      unsigned char* result = test_ctx_tx[i]->shas[frame];
      SHA256((unsigned char*)fb, frame_size, result);
      test_sha_dump("st20_rx", result);
    }
    test_ctx_tx[i]->handle = tx_handle[i];
  }

  for (int i = 0; i < sessions; i++) {
    test_ctx_rx[i] = init_test_ctx(ctx, i, 3, true);
    ASSERT_TRUE(test_ctx_rx[i] != NULL);
    test_ctx_rx[i]->user_pacing = user_pacing[i];
    test_ctx_rx[i]->user_timestamp = user_timestamp[i];

    init_single_port_rx(ops_rx, test_ctx_rx[i], "st20_timestamp_test",
                        udp_port_for_idx(i));
    ops_rx.type = ST20_TYPE_FRAME_LEVEL;
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.fps = fps;
    ops_rx.fmt = fmt[i];
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready = st20_digest_rx_frame_ready;

    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    test_ctx_rx[i]->frame_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->fb_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->width = ops_rx.width;
    st20_get_pgroup(ops_rx.fmt, &test_ctx_rx[i]->st20_pg);
    memcpy(test_ctx_rx[i]->shas, test_ctx_tx[i]->shas,
           TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
    test_ctx_rx[i]->handle = rx_handle[i];

    test_ctx_rx[i]->stop = false;
    sha_thread_rx[i] = std::thread(st20_digest_rx_frame_check, test_ctx_rx[i]);
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  guard.set_started(ret >= 0);
  sleep(ST20_TRAIN_TIME_S * sessions); /* time for train_pacing */
  sleep(10 * 1);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    rx_framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;
    time_sec = (double)(cur_time_ns - test_ctx_tx[i]->start_time) / NS_PER_S;
    tx_framerate[i] = test_ctx_tx[i]->fb_send / time_sec;
  }

  /* freeze counters before assertions */
  guard.stop();
  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    EXPECT_GT(test_ctx_rx[i]->check_sha_frame_cnt, 0);
    EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 2);
    EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);

    info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
         rx_framerate[i]);
    info("%s, session %d fb_send %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
         tx_framerate[i]);

    EXPECT_NEAR(tx_framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    EXPECT_NEAR(rx_framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
  }
}

TEST(St20_tx, tx_user_pacing) {
  int width[3] = {1280, 1920, 1280};
  int height[3] = {720, 1080, 720};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  bool user_pacing[3] = {false, true, true};
  bool user_timestamp[3] = {true, false, true};
  st20_tx_user_pacing_test(width, height, fmt, user_pacing, user_timestamp,
                           ST_TEST_LEVEL_MANDATORY, 3);
}