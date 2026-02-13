/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

static void st20_rx_fps_test(enum st20_type type[], enum st_fps fps[], int width[],
                             int height[], enum st20_fmt fmt, enum st_test_level level,
                             int sessions = 1, bool ext_buf = false) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops_tx;
  struct st20_rx_ops ops_rx;
  if (ctx->para.num_ports < 2) {
    info(
        "%s, dual port should be enabled for tx test, one for tx and one for "
        "rx\n",
        __func__);
    throw std::runtime_error("Dual port not enabled");
  }

  /* return if level small than global */
  if (level < ctx->level) return;

  if (ext_buf) {
    if (ctx->iova == MTL_IOVA_MODE_PA) {
      info("%s, skip ext_buf test as it's PA iova mode\n", __func__);
      return;
    }
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

  St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle,
                        &rtp_thread_tx, &rtp_thread_rx);
  guard.set_ext_buf(ext_buf);

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
    test_ctx_rx[i]->ext_fb = NULL;
    test_ctx_rx[i]->ext_fb_malloc = NULL;
    test_ctx_rx[i]->ext_fb_iova = MTL_BAD_IOVA;
    test_ctx_rx[i]->ext_fb_iova_map_sz = 0;
    test_ctx_rx[i]->ext_frames = NULL;

    if (ext_buf) {
      test_ctx_rx[i]->ext_frames = (struct st20_ext_frame*)malloc(
          sizeof(*test_ctx_rx[i]->ext_frames) * test_ctx_rx[i]->fb_cnt);
      ASSERT_TRUE(test_ctx_rx[i]->ext_frames != NULL);
      size_t frame_size = st20_frame_size(fmt, width[i], height[i]);
      size_t pg_sz = mtl_page_size(m_handle);
      size_t fb_size = frame_size * test_ctx_rx[i]->fb_cnt;
      test_ctx_rx[i]->ext_fb_iova_map_sz =
          mtl_size_page_align(fb_size, pg_sz); /* align */
      size_t fb_size_malloc = test_ctx_rx[i]->ext_fb_iova_map_sz + pg_sz;
      test_ctx_rx[i]->ext_fb_malloc = st_test_zmalloc(fb_size_malloc);
      ASSERT_TRUE(test_ctx_rx[i]->ext_fb_malloc != NULL);
      test_ctx_rx[i]->ext_fb =
          (uint8_t*)MTL_ALIGN((uint64_t)test_ctx_rx[i]->ext_fb_malloc, pg_sz);
      test_ctx_rx[i]->ext_fb_iova = mtl_dma_map(m_handle, test_ctx_rx[i]->ext_fb,
                                                test_ctx_rx[i]->ext_fb_iova_map_sz);
      info("%s, session %d ext_fb %p\n", __func__, i, test_ctx_rx[i]->ext_fb);
      ASSERT_TRUE(test_ctx_rx[i]->ext_fb_iova != MTL_BAD_IOVA);

      for (int j = 0; j < test_ctx_rx[i]->fb_cnt; j++) {
        test_ctx_rx[i]->ext_frames[j].buf_addr = test_ctx_rx[i]->ext_fb + j * frame_size;
        test_ctx_rx[i]->ext_frames[j].buf_iova =
            test_ctx_rx[i]->ext_fb_iova + j * frame_size;
        test_ctx_rx[i]->ext_frames[j].buf_len = frame_size;
      }
    }

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
    if (ext_buf) {
      ops_rx.ext_frames = test_ctx_rx[i]->ext_frames;
    }
    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    test_ctx_rx[i]->total_pkts_in_frame = test_ctx_tx[i]->total_pkts_in_frame;
    test_ctx_rx[i]->handle = rx_handle[i];
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx_rx[i]->stop = false;
      rtp_thread_rx[i] = std::thread(rx_get_packet, test_ctx_rx[i]);
    }
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  guard.set_started(ret >= 0);
  sleep(ST20_TRAIN_TIME_S * sessions); /* time for train_pacing */
  sleep(10);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;
  }

  /* freeze counters before assertions */
  guard.stop();

  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
         framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
  }
}

static void st20_tx_fps_test(enum st20_type type[], enum st_fps fps[], int width[],
                             int height[], enum st20_fmt fmt, enum st_test_level level,
                             int sessions = 1, bool ext_buf = false) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops;

  std::vector<tests_context*> test_ctx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st20_tx_handle> handle;
  std::vector<st20_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread;

  /* return if level small than global */
  if (level < ctx->level) return;

  if (ext_buf) {
    if (ctx->iova == MTL_IOVA_MODE_PA) {
      info("%s, skip ext_buf test as it's PA iova mode\n", __func__);
      return;
    }
  }

  test_ctx.resize(sessions);
  handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  rtp_thread.resize(sessions);

  St20DeinitGuard guard(m_handle, test_ctx, test_ctx_rx, handle, rx_handle, &rtp_thread,
                        nullptr);
  guard.set_ext_buf(ext_buf);

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(fps[i]);
    test_ctx[i] = init_test_ctx(ctx, i, 3);
    ASSERT_TRUE(test_ctx[i] != NULL);
    st20_tx_ops_init(test_ctx[i], &ops);
    ops.type = type[i];
    ops.fps = fps[i];
    ops.width = width[i];
    ops.height = height[i];
    ops.fmt = fmt;
    ops.packing = ST20_PACKING_BPM;
    if (ext_buf) {
      ops.flags |= ST20_TX_FLAG_EXT_FRAME;
      ops.get_next_frame = tx_next_ext_video_frame;
      ops.notify_frame_done = tx_notify_ext_frame_done;
    } else {
      ops.notify_frame_done = tx_notify_frame_done_check_tmstamp;
    }
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      rtp_tx_specific_init(&ops, test_ctx[i]);
    }
    handle[i] = st20_tx_create(m_handle, &ops);
    ASSERT_TRUE(handle[i] != NULL);

    size_t frame_size = st20_tx_get_framebuffer_size(handle[i]);
    test_ctx[i]->frame_size = frame_size;

    if (ext_buf) {
      test_ctx[i]->ext_frames = (struct st20_ext_frame*)malloc(
          sizeof(*test_ctx[i]->ext_frames) * test_ctx[i]->fb_cnt);
      size_t pg_sz = mtl_page_size(m_handle);
      size_t fb_size = test_ctx[i]->frame_size * test_ctx[i]->fb_cnt;
      test_ctx[i]->ext_fb_iova_map_sz = mtl_size_page_align(fb_size, pg_sz); /* align */
      size_t fb_size_malloc = test_ctx[i]->ext_fb_iova_map_sz + pg_sz;
      test_ctx[i]->ext_fb_malloc = st_test_zmalloc(fb_size_malloc);
      ASSERT_TRUE(test_ctx[i]->ext_fb_malloc != NULL);
      test_ctx[i]->ext_fb =
          (uint8_t*)MTL_ALIGN((uint64_t)test_ctx[i]->ext_fb_malloc, pg_sz);
      test_ctx[i]->ext_fb_iova =
          mtl_dma_map(m_handle, test_ctx[i]->ext_fb, test_ctx[i]->ext_fb_iova_map_sz);
      info("%s, session %d ext_fb %p\n", __func__, i, test_ctx[i]->ext_fb);
      ASSERT_TRUE(test_ctx[i]->ext_fb_iova != MTL_BAD_IOVA);

      for (int j = 0; j < test_ctx[i]->fb_cnt; j++) {
        test_ctx[i]->ext_frames[j].buf_addr = test_ctx[i]->ext_fb + j * frame_size;
        test_ctx[i]->ext_frames[j].buf_iova = test_ctx[i]->ext_fb_iova + j * frame_size;
        test_ctx[i]->ext_frames[j].buf_len = frame_size;
      }
    }

    test_ctx[i]->handle = handle[i];
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx[i]->stop = false;
      rtp_thread[i] = std::thread(tx_feed_packet, test_ctx[i]);
    }
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  guard.set_started(ret >= 0);
  sleep(ST20_TRAIN_TIME_S * sessions); /* time for train_pacing */
  if (ctx->para.num_ports > 1)
    sleep(ST20_TRAIN_TIME_S * sessions); /* time for train_pacing */
  sleep(5);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx[i]->fb_send / time_sec;
  }

  /* freeze counters before assertions */
  guard.stop();
  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx[i]->fb_send, 0);
    EXPECT_LE(test_ctx[i]->tx_tmstamp_delta_fail_cnt, 1);
    info("%s, session %d fb_send %d framerate %f\n", __func__, i, test_ctx[i]->fb_send,
         framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
  }
}

TEST(St20_tx, rtp_1080p_fps59_94_s1) {
  enum st20_type type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL);
}
TEST(St20_tx, frame_1080p_fps29_97_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P29_97};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL);
}
TEST(St20_tx, frame_1080p_fps50_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL);
}
TEST(St20_tx, frame_1080p_fps30_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P30};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL);
}
TEST(St20_tx, frame_1080p_fps60_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P60};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL);
}
TEST(St20_tx, rtp_720p_fps50_s1) {
  enum st20_type type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1280};
  int height[1] = {720};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL);
}
TEST(St20_tx, frame_1080p_yuv422_8bit_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_8BIT, ST_TEST_LEVEL_ALL);
}
TEST(St20_tx, frame_1080p_yuv420_10bit_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_420_10BIT, ST_TEST_LEVEL_ALL);
}
TEST(St20_tx, mix_1080p_fps59_94_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_RTP_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P59_94, ST_FPS_P59_94};
  int width[3] = {1920, 1920, 1920};
  int height[3] = {1080, 1080, 1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL,
                   3);
}
TEST(St20_tx, mix_720p_fps29_97_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_RTP_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P29_97, ST_FPS_P29_97, ST_FPS_P29_97};
  int width[3] = {1280, 1280, 1280};
  int height[3] = {720, 720, 720};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL,
                   3);
}
TEST(St20_tx, mix_1080p_fps50_fps29_97) {
  enum st20_type type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P29_97};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL,
                   2);
}
TEST(St20_tx, mix_1080p_fps50_fps59_94) {
  enum st20_type type[2] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P59_94};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL,
                   2);
}
TEST(St20_tx, ext_frame_1080p_fps_mix_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P29_97, ST_FPS_P59_94, ST_FPS_P50};
  int width[3] = {1920, 1920, 1920};
  int height[3] = {1080, 1080, 1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL, 3,
                   true);
}

TEST(St20_rx, frame_1080p_fps50_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_420_10BIT, ST_TEST_LEVEL_ALL);
}
TEST(St20_rx, mix_1080p_fps50_s3) {
  enum st20_type type[3] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P50, ST_FPS_P50, ST_FPS_P50};
  int width[3] = {1920, 1920, 1920};
  int height[3] = {1080, 1080, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL,
                   3);
}
TEST(St20_rx, rtp_1080p_fps59_94_s1) {
  enum st20_type type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_420_10BIT, ST_TEST_LEVEL_ALL);
}
TEST(St20_rx, rtp_1080p_fps29_97_s1) {
  enum st20_type type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P29_97};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL);
}
TEST(St20_rx, frame_1080p_fps29_97_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P29_97, ST_FPS_P29_97, ST_FPS_P29_97};
  int width[3] = {1920, 1920, 1920};
  int height[3] = {1080, 1080, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL,
                   3);
}
TEST(St20_rx, mix_1080p_fps29_97_fp50) {
  enum st20_type type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P29_97, ST_FPS_P50};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL,
                   2);
}
TEST(St20_rx, mix_1080p_fps59_94_fp50) {
  enum st20_type type[2] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL,
                   2);
}
TEST(St20_rx, mix_1080p_fps29_97_720p_fp50) {
  enum st20_type type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P29_97, ST_FPS_P50};
  int width[2] = {1920, 1280};
  int height[2] = {1080, 720};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL,
                   2);
}
TEST(St20_rx, ext_frame_1080p_fps_mix_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P29_97, ST_FPS_P59_94, ST_FPS_P50};
  int width[3] = {1280, 1920, 1920};
  int height[3] = {720, 1080, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL, 3,
                   true);
}

TEST(St20_tx, mix_s3) {
  enum st20_type type[3] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P50, ST_FPS_P59_94, ST_FPS_P29_97};
  int width[3] = {1920, 1280, 1920};
  int height[3] = {1080, 720, 1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT,
                   ST_TEST_LEVEL_MANDATORY, 3);
}
TEST(St20_tx, ext_frame_mix_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P29_97};
  int width[3] = {1280, 1920, 3840};
  int height[3] = {720, 1080, 2160};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT,
                   ST_TEST_LEVEL_MANDATORY, 3, true);
}
TEST(St20_rx, frame_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P29_97};
  int width[3] = {1280, 1920, 1920};
  int height[3] = {720, 1080, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL,
                   3);
}
TEST(St20_rx, mix_s2) {
  enum st20_type type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1280, 1920};
  int height[2] = {720, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT,
                   ST_TEST_LEVEL_MANDATORY, 2);
}
TEST(St20_rx, frame_mix_4k_s2) {
  enum st20_type type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1280, 3840};
  int height[2] = {720, 2160};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, ST_TEST_LEVEL_ALL,
                   2);
}
TEST(St20_rx, ext_frame_mix_s2) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[3] = {1280, 1920};
  int height[3] = {720, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT,
                   ST_TEST_LEVEL_MANDATORY, 2, true);
}
