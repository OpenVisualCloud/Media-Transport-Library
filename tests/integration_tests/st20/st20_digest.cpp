/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

static void st20_rx_digest_bufq_cleanup(tests_context* ctx) {
  auto handle = (st20_rx_handle)ctx->handle;
  while (!ctx->buf_q.empty()) {
    void* frame = ctx->buf_q.front();
    ctx->buf_q.pop();
    if (!ctx->second_field_q.empty()) ctx->second_field_q.pop();
    if (handle) {
      st20_rx_put_framebuff(handle, frame);
    }
  }
}

static void st20_rx_digest_test(enum st20_type tx_type[], enum st20_type rx_type[],
                                enum st20_packing packing[], enum st_fps fps[],
                                int width[], int height[], bool interlaced[],
                                enum st20_fmt fmt[], bool check_fps,
                                enum st_test_level level, int sessions = 1,
                                bool out_of_order = false, bool hdr_split = false,
                                bool enable_rtcp = false) {
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

  bool has_dma = st_test_dma_available(ctx);

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st20_tx_handle> tx_handle;
  std::vector<st20_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread_tx;
  std::vector<std::thread> rtp_thread_rx;
  std::vector<std::thread> sha_check;
  int slices_per_frame = 32;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  rtp_thread_tx.resize(sessions);
  rtp_thread_rx.resize(sessions);
  sha_check.resize(sessions);

  St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle,
                        &rtp_thread_tx, &rtp_thread_rx);
  guard.add_thread_group(sha_check);
  guard.set_rx_ctx_cleanup(st20_rx_digest_bufq_cleanup);

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(fps[i]);

    test_ctx_tx[i] = init_test_ctx(ctx, i, TEST_SHA_HIST_NUM, true);
    ASSERT_TRUE(test_ctx_tx[i] != NULL);
    test_ctx_tx[i]->stop = false;

    init_single_port_tx(ops_tx, test_ctx_tx[i], "st20_digest_test",
                        udp_port_for_idx(i, hdr_split));
    ops_tx.packing = packing[i];
    ops_tx.type = tx_type[i];
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.interlaced = interlaced[i];
    ops_tx.fps = fps[i];
    ops_tx.fmt = fmt[i];
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = interlaced[i] ? tx_next_video_field : tx_next_video_frame;
    ops_tx.query_frame_lines_ready = tx_frame_lines_ready;
    if (tx_type[i] == ST20_TYPE_RTP_LEVEL) {
      rtp_tx_specific_init(&ops_tx, test_ctx_tx[i]);
    }
    if (enable_rtcp) {
      ops_tx.flags |= ST20_TX_FLAG_ENABLE_RTCP;
      ops_tx.rtcp.buffer_size = 1024;
    }

    // out of order
    if (out_of_order) {
      test_ctx_tx[i]->ooo_mapping =
          (int*)st_test_zmalloc(sizeof(int) * test_ctx_tx[i]->total_pkts_in_frame);
      ASSERT_TRUE(test_ctx_tx[i]->ooo_mapping != NULL);
      tx_video_build_ooo_mapping(test_ctx_tx[i]);
    }
    test_ctx_tx[i]->out_of_order_pkt = out_of_order;

    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    /* sha calculate */
    struct st20_pgroup st20_pg;
    st20_get_pgroup(ops_tx.fmt, &st20_pg);
    size_t frame_size = ops_tx.width * ops_tx.height * st20_pg.size / st20_pg.coverage;
    if (interlaced[i]) frame_size = frame_size >> 1;
    if (tx_type[i] == ST20_TYPE_FRAME_LEVEL) {
      EXPECT_EQ(st20_tx_get_framebuffer_size(tx_handle[i]), frame_size);
      EXPECT_EQ(st20_tx_get_framebuffer_count(tx_handle[i]), test_ctx_tx[i]->fb_cnt);
    }
    test_ctx_tx[i]->frame_size = frame_size;
    test_ctx_tx[i]->slice = (tx_type[i] == ST20_TYPE_SLICE_LEVEL);
    test_ctx_tx[i]->lines_per_slice = ops_tx.height / 30;
    test_ctx_tx[i]->height = ops_tx.height;
    test_ctx_tx[i]->stride = ops_tx.width / st20_pg.coverage * st20_pg.size;
    uint8_t* fb;
    for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
      if (tx_type[i] == ST20_TYPE_FRAME_LEVEL) {
        fb = (uint8_t*)st20_tx_get_framebuffer(tx_handle[i], frame);
      } else {
        test_ctx_tx[i]->frame_buf[frame] = (uint8_t*)st_test_zmalloc(frame_size);
        fb = test_ctx_tx[i]->frame_buf[frame];
      }
      ASSERT_TRUE(fb != NULL);
      st_test_rand_data(fb, frame_size, frame);
      unsigned char* result = test_ctx_tx[i]->shas[frame];
      SHA256((unsigned char*)fb, frame_size, result);
      test_sha_dump("st20_rx", result);
    }
    test_ctx_tx[i]->handle = tx_handle[i];
    if (tx_type[i] == ST20_TYPE_RTP_LEVEL) {
      rtp_thread_tx[i] = std::thread(tx_feed_packet, test_ctx_tx[i]);
    }
  }

  for (int i = 0; i < sessions; i++) {
    test_ctx_rx[i] = init_test_ctx(ctx, i, 3, true);
    ASSERT_TRUE(test_ctx_rx[i] != NULL);
    test_ctx_rx[i]->stop = false;

    init_single_port_rx(ops_rx, test_ctx_rx[i], "st20_digest_test",
                        udp_port_for_idx(i, hdr_split));
    ops_rx.type = rx_type[i];
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.fps = fps[i];
    ops_rx.fmt = fmt[i];
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.interlaced = interlaced[i];
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.slice_lines = height[i] / slices_per_frame;
    ops_rx.notify_frame_ready =
        interlaced[i] ? st20_digest_rx_field_ready : st20_digest_rx_frame_ready;
    ops_rx.notify_slice_ready = st20_digest_rx_slice_ready;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024 * 2;
    ops_rx.flags = ST20_RX_FLAG_DMA_OFFLOAD;
    if (hdr_split) ops_rx.flags |= ST20_RX_FLAG_HDR_SPLIT;
    if (enable_rtcp) {
      ops_rx.flags |= ST20_RX_FLAG_ENABLE_RTCP | ST20_RX_FLAG_SIMULATE_PKT_LOSS;
      ops_rx.rtcp.nack_interval_us = 250;
      ops_rx.rtcp.seq_bitmap_size = 32;
      ops_rx.rtcp.seq_skip_window = 10;
      ops_rx.rtcp.burst_loss_max = 32;
      ops_rx.rtcp.sim_loss_rate = 0.0001;
    }

    if (rx_type[i] == ST20_TYPE_SLICE_LEVEL) {
      /* set expect meta data to private */
      auto meta =
          (struct st20_rx_slice_meta*)st_test_zmalloc(sizeof(struct st20_rx_slice_meta));
      ASSERT_TRUE(meta != NULL);
      meta->width = ops_rx.width;
      meta->height = ops_rx.height;
      meta->fps = ops_rx.fps;
      meta->fmt = ops_rx.fmt;
      meta->frame_total_size = test_ctx_tx[i]->frame_size;
      meta->uframe_total_size = 0;
      meta->second_field = false;
      test_ctx_rx[i]->priv = meta;
      ops_rx.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
    }

    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    test_ctx_rx[i]->frame_time = (double)NS_PER_S / st_frame_rate(ops_rx.fps);
    dbg("%s(%d), frame_time %f\n", __func__, i, test_ctx_rx[i]->frame_time);
    test_ctx_rx[i]->frame_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->fb_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->width = ops_rx.width;
    st20_get_pgroup(ops_rx.fmt, &test_ctx_rx[i]->st20_pg);
    memcpy(test_ctx_rx[i]->shas, test_ctx_tx[i]->shas,
           TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
    test_ctx_rx[i]->total_pkts_in_frame = test_ctx_tx[i]->total_pkts_in_frame;
    test_ctx_rx[i]->handle = rx_handle[i];
    if (rx_type[i] == ST20_TYPE_RTP_LEVEL) {
      rtp_thread_rx[i] = std::thread(rx_get_packet, test_ctx_rx[i]);
      sha_check[i] = std::thread(sha_frame_check, test_ctx_rx[i]);
    } else {
      if (interlaced[i]) {
        rtp_thread_rx[i] = std::thread(st20_digest_rx_field_check, test_ctx_rx[i]);
      } else {
        rtp_thread_rx[i] = std::thread(st20_digest_rx_frame_check, test_ctx_rx[i]);
      }
    }

    bool dma_enabled = st20_rx_dma_enabled(rx_handle[i]);
    if (has_dma && (rx_type[i] != ST20_TYPE_RTP_LEVEL)) {
      EXPECT_TRUE(dma_enabled);
    } else {
      EXPECT_FALSE(dma_enabled);
    }
    struct st_queue_meta meta;
    ret = st20_rx_get_queue_meta(rx_handle[i], &meta);
    EXPECT_GE(ret, 0);
  }

  ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  guard.set_started(ret >= 0);
  sleep(ST20_TRAIN_TIME_S * sessions); /* time for train_pacing */
  sleep(10 * 1);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;
  }

  /* freeze counters and stop background work before assertions */
  guard.stop();
  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    EXPECT_GT(test_ctx_rx[i]->check_sha_frame_cnt, 0);
    if (rx_type[i] == ST20_TYPE_SLICE_LEVEL)
      EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 2 * 8);
    else
      EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 4);
    if (check_fps && !enable_rtcp) {
      EXPECT_LT(test_ctx_rx[i]->meta_timing_fail_cnt, 4);
      EXPECT_LT(test_ctx_tx[i]->tx_tmstamp_delta_fail_cnt, 4);
    }
    EXPECT_EQ(test_ctx_rx[i]->incomplete_slice_cnt, 0);
    if (rx_type[i] == ST20_TYPE_FRAME_LEVEL)
      EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);
    else
      EXPECT_LE(test_ctx_rx[i]->sha_fail_cnt, 2);
    info("%s, session %d fb_rec %d framerate %f fb_send %d\n", __func__, i,
         test_ctx_rx[i]->fb_rec, framerate[i], test_ctx_tx[i]->fb_send);
    if (rx_type[i] == ST20_TYPE_SLICE_LEVEL) {
      int expect_slice_cnt = test_ctx_rx[i]->fb_rec * slices_per_frame;
      if (interlaced[i]) expect_slice_cnt /= 2;
      EXPECT_NEAR(test_ctx_rx[i]->slice_cnt, expect_slice_cnt, expect_slice_cnt * 0.1);
    }
    if (check_fps) {
      EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    }
  }
}

TEST(St20_rx, digest_frame_1080p_fps59_94_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  bool interlaced[1] = {false};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, true,
                      ST_TEST_LEVEL_ALL);
}

TEST(St20_rx, digest20_field_1080p_fps59_94_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  bool interlaced[1] = {true};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, true,
                      ST_TEST_LEVEL_ALL);
}

TEST(St20_rx, digest_frame_720p_fps59_94_s1_gpm) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_GPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1280};
  int height[1] = {720};
  bool interlaced[1] = {false};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, true,
                      ST_TEST_LEVEL_ALL);
}

TEST(St20_rx, digest20_field_720p_fps59_94_s1_gpm) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_GPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1280};
  int height[1] = {720};
  bool interlaced[1] = {true};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, true,
                      ST_TEST_LEVEL_ALL);
}

TEST(St20_rx, digest_frame_720p_fps29_97_s1_bpm) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P29_97};
  int width[1] = {1280};
  int height[1] = {720};
  bool interlaced[1] = {false};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, true,
                      ST_TEST_LEVEL_ALL);
}

TEST(St20_rx, digest20_field_720p_fps29_97_s1_bpm) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P29_97};
  int width[1] = {1280};
  int height[1] = {720};
  bool interlaced[1] = {true};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, true,
                      ST_TEST_LEVEL_ALL);
}

TEST(St20_rx, digest_rtp_1080p_fps59_94_s1) {
  enum st20_type type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  bool interlaced[1] = {false};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, true,
                      ST_TEST_LEVEL_ALL);
}

TEST(St20_rx, digest_frame_720p_fps59_94_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P59_94, ST_FPS_P59_94};
  int width[3] = {1280, 1280, 1280};
  int height[3] = {720, 720, 720};
  bool interlaced[3] = {false, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_ALL, 3);
}

TEST(St20_rx, digest20_field_720p_fps59_94_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P59_94, ST_FPS_P59_94};
  int width[3] = {1280, 1280, 1280};
  int height[3] = {720, 720, 720};
  bool interlaced[3] = {true, false, true};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_ALL, 3);
}

TEST(St20_rx, digest_frame_1080p_fps_mix_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P60, ST_FPS_P30};
  int width[3] = {1920, 1920, 1920};
  int height[3] = {1080, 1080, 1080};
  bool interlaced[3] = {false, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_ALL, 3);
}

TEST(St20_rx, digest20_field_1080p_fps59_94_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P59_94, ST_FPS_P59_94};
  int width[3] = {1920, 1920, 1920};
  int height[3] = {1080, 1080, 1080};
  bool interlaced[3] = {true, true, true};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_ALL, 3);
}

TEST(St20_rx, digest_frame_1080p_fps59_94_s4_8bit) {
  enum st20_type type[4] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[4] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[4] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM_SL,
                                  ST20_PACKING_BPM, ST20_PACKING_GPM};
  enum st_fps fps[4] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P59_94, ST_FPS_P50};
  int width[4] = {1920, 1920, 1920, 1280};
  int height[4] = {1080, 1080, 1080, 720};
  bool interlaced[4] = {false, false, false, false};
  enum st20_fmt fmt[4] = {ST20_FMT_YUV_422_8BIT, ST20_FMT_YUV_420_8BIT,
                          ST20_FMT_YUV_444_8BIT, ST20_FMT_RGB_8BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_ALL, 4);
}

TEST(St20_rx, digest20_field_4320p_fps59_94_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920 * 4};
  int height[1] = {1080 * 4};
  bool interlaced[1] = {true};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_ALL);
}

TEST(St20_rx, digest_frame_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P29_97};
  int width[3] = {1920, 1080, 1920 * 2};
  int height[3] = {1080, 720, 1080 * 2};
  bool interlaced[3] = {false, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_MANDATORY, 3);
}

TEST(St20_rx, digest_frame_field_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P29_97};
  int width[3] = {1920, 1080, 1920 * 2};
  int height[3] = {1080, 720, 1080 * 2};
  bool interlaced[3] = {true, true, true};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_ALL, 3);
}

TEST(St20_rx, digest_frame_rtp_s3) {
  enum st20_type type[3] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_RTP_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P29_97};
  int width[3] = {1920, 1080, 1920 * 2};
  int height[3] = {1080, 720, 1080 * 2};
  bool interlaced[3] = {false, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_MANDATORY, 3);
}

TEST(St20_rx, digest_frame_s4_8bit) {
  enum st20_type type[4] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[4] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[4] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM_SL,
                                  ST20_PACKING_BPM, ST20_PACKING_GPM};
  enum st_fps fps[4] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P59_94, ST_FPS_P119_88};
  int width[4] = {1920, 1920, 1920, 1280};
  int height[4] = {1080, 1080, 1080, 720};
  bool interlaced[4] = {false, false, false, false};
  enum st20_fmt fmt[4] = {ST20_FMT_YUV_422_8BIT, ST20_FMT_YUV_420_8BIT,
                          ST20_FMT_YUV_444_8BIT, ST20_FMT_RGB_8BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_MANDATORY, 4);
}

TEST(St20_rx, digest_frame_s4_10bit) {
  enum st20_type type[4] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[4] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[4] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM, ST20_PACKING_BPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[4] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P59_94, ST_FPS_P50};
  int width[4] = {1920, 1920, 1920, 1280};
  int height[4] = {1080, 1080, 1080, 720};
  bool interlaced[4] = {false, false, false, false};
  enum st20_fmt fmt[4] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_420_10BIT,
                          ST20_FMT_YUV_444_10BIT, ST20_FMT_RGB_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_MANDATORY, 4);
}

TEST(St20_rx, digest_rtp_s3) {
  enum st20_type type[3] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_RTP_LEVEL,
                            ST20_TYPE_RTP_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_RTP_LEVEL,
                               ST20_TYPE_RTP_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P29_97};
  int width[3] = {1920, 1920, 1920};
  int height[3] = {1080, 1080, 1080};
  bool interlaced[3] = {false, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_MANDATORY, 3);
}

TEST(St20_rx, digest_ooo_frame_s3) {
  enum st20_type type[3] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_RTP_LEVEL,
                            ST20_TYPE_RTP_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P50, ST_FPS_P50, ST_FPS_P59_94};
  int width[3] = {1920, 1280, 1280};
  int height[3] = {1080, 720, 720};
  bool interlaced[3] = {false, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_MANDATORY, 3, true);
}

TEST(St20_rx, digest_tx_slice_s3) {
  enum st20_type type[3] = {ST20_TYPE_SLICE_LEVEL, ST20_TYPE_SLICE_LEVEL,
                            ST20_TYPE_SLICE_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P50, ST_FPS_P50, ST_FPS_P59_94};
  int width[3] = {1920, 1280, 1280};
  int height[3] = {1080, 720, 720};
  bool interlaced[3] = {false, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_ALL, 3, false);
}

TEST(St20_rx, digest_slice_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_SLICE_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_SLICE_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_SLICE_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P50, ST_FPS_P50, ST_FPS_P59_94};
  int width[3] = {1920, 1280, 1280};
  int height[3] = {1080, 720, 720};
  bool interlaced[3] = {false, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_MANDATORY, 3, false);
}

TEST(St20_rx, digest20_field_slice_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_SLICE_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_SLICE_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P50, ST_FPS_P50, ST_FPS_P59_94};
  int width[3] = {1920, 1280, 1280};
  int height[3] = {1080, 720, 720};
  bool interlaced[3] = {true, true, true};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_ALL, 3, false);
}

TEST(St20_rx, digest_ooo_slice_s3) {
  enum st20_type type[3] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_RTP_LEVEL,
                            ST20_TYPE_RTP_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_SLICE_LEVEL, ST20_TYPE_SLICE_LEVEL,
                               ST20_TYPE_SLICE_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P50, ST_FPS_P50, ST_FPS_P59_94};
  int width[3] = {1920, 1280, 1280};
  int height[3] = {1080, 720, 720};
  bool interlaced[3] = {false, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_MANDATORY, 3, true);
}

TEST(St20_rx, digest_frame_4320p_fps59_94_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920 * 4};
  int height[1] = {1080 * 4};
  bool interlaced[1] = {false};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_ALL);
}

TEST(St20_rx, digest_frame_4096_2160_fps59_94_12bit_yuv444_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {4096};
  int height[1] = {2160};
  bool interlaced[1] = {false};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_444_12BIT};
  st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_MANDATORY, 1);
}

TEST(St20_rx, digest_slice_4320p) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_SLICE_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920 * 4};
  int height[1] = {1080 * 4};
  bool interlaced[1] = {false};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  if (st_test_dma_available(st_test_ctx())) {
    st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt,
                        false, ST_TEST_LEVEL_ALL, 1);
  } else {
    info("%s, skip as no dma available\n", __func__);
  }
}

TEST(St20_rx, digest_ooo_slice_4320p) {
  enum st20_type type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_SLICE_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P25};
  int width[1] = {1920 * 4};
  int height[1] = {1080 * 4};
  bool interlaced[1] = {false};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  if (st_test_dma_available(st_test_ctx())) {
    st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt,
                        false, ST_TEST_LEVEL_MANDATORY, 1, true);
  } else {
    info("%s, skip as no dma available\n", __func__);
  }
}

TEST(St20_rx, digest_hdr_split) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920 * 1};
  int height[1] = {1080 * 1};
  bool interlaced[1] = {false};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  if (st_test_ctx()->hdr_split) {
    st20_rx_digest_test(type, rx_type, packing, fps, width, height, interlaced, fmt,
                        false, ST_TEST_LEVEL_MANDATORY, 1, false, true);
  } else {
    info("%s, skip as no dma available\n", __func__);
  }
}

TEST(St20_rx, digest_rtcp_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1920};
  int height[1] = {1080};
  bool interlaced[1] = {false};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  /* check fps */
  st20_rx_digest_test(type, type, packing, fps, width, height, interlaced, fmt, true,
                      ST_TEST_LEVEL_ALL, 1, false, false, true);
}

TEST(St20_rx, digest_rtcp_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P29_97};
  int width[3] = {1920, 1920, 1280};
  int height[3] = {1080, 1080, 720};
  bool interlaced[3] = {true, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  /* no fps check */
  st20_rx_digest_test(type, type, packing, fps, width, height, interlaced, fmt, false,
                      ST_TEST_LEVEL_MANDATORY, 3, false, false, true);
}
