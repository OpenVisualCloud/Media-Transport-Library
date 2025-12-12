/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

static void st20_linesize_rx_ctx_cleanup(tests_context* ctx) {
  if (!ctx) return;
  st20_rx_drain_bufq_put_framebuff(ctx);
}

static void st20_linesize_digest_test(enum st20_packing packing[], enum st_fps fps[],
                                      int width[], int height[], int linesize[],
                                      bool interlaced[], enum st20_fmt fmt[],
                                      bool check_fps, enum st_test_level level,
                                      int sessions = 1, bool ext = false) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops_tx;
  struct st20_rx_ops ops_rx;

  /* return if level small than global */
  if (level < ctx->level) return;

  if (ext) {
    if (ctx->iova == MTL_IOVA_MODE_PA) {
      info("%s, skip ext test as it's PA iova mode\n", __func__);
      return;
    }
  }

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
  std::vector<std::thread> sha_check;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  sha_check.resize(sessions);

  St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle);
  guard.add_thread_group(sha_check);
  guard.set_rx_ctx_cleanup(st20_linesize_rx_ctx_cleanup);

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(fps[i]);

    test_ctx_tx[i] = init_test_ctx(ctx, i, TEST_SHA_HIST_NUM, true);
    ASSERT_TRUE(test_ctx_tx[i] != NULL);
    test_ctx_tx[i]->stop = false;

    init_single_port_tx(ops_tx, test_ctx_tx[i], "st20_linesize_digest_test",
                        udp_port_for_idx(i));
    ops_tx.packing = packing[i];
    ops_tx.type = ST20_TYPE_FRAME_LEVEL;
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.linesize = linesize[i];
    ops_tx.interlaced = interlaced[i];
    ops_tx.fps = fps[i];
    ops_tx.fmt = fmt[i];
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;

    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    if (ext) {
      ops_tx.flags |= ST20_TX_FLAG_EXT_FRAME;
      ops_tx.get_next_frame =
          interlaced[i] ? tx_next_ext_video_field : tx_next_ext_video_frame;
      ops_tx.notify_frame_done = tx_notify_ext_frame_done;
    } else {
      ops_tx.get_next_frame = interlaced[i] ? tx_next_video_field : tx_next_video_frame;
    }

    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    /* sha calculate */
    struct st20_pgroup st20_pg;
    st20_get_pgroup(ops_tx.fmt, &st20_pg);
    size_t frame_size = ops_tx.width * ops_tx.height * st20_pg.size / st20_pg.coverage;
    if (interlaced[i]) frame_size = frame_size >> 1;
    test_ctx_tx[i]->frame_size = frame_size;
    test_ctx_tx[i]->height = ops_tx.height;
    test_ctx_tx[i]->stride = ops_tx.width / st20_pg.coverage * st20_pg.size;

    size_t fb_size = frame_size;
    if (linesize[i] > test_ctx_tx[i]->stride) {
      test_ctx_tx[i]->stride = linesize[i];
      fb_size = (size_t)linesize[i] * height[i];
      if (interlaced[i]) fb_size = fb_size >> 1;
    }
    test_ctx_tx[i]->fb_size = fb_size;
    EXPECT_EQ(st20_tx_get_framebuffer_size(tx_handle[i]), fb_size);
    EXPECT_EQ(st20_tx_get_framebuffer_count(tx_handle[i]), test_ctx_tx[i]->fb_cnt);

    if (ext) {
      test_ctx_tx[i]->ext_frames = (struct st20_ext_frame*)malloc(
          sizeof(*test_ctx_tx[i]->ext_frames) * test_ctx_tx[i]->fb_cnt);
      size_t fbs_size = fb_size * test_ctx_tx[i]->fb_cnt;
      mtl_dma_mem_handle dma_mem = mtl_dma_mem_alloc(m_handle, fbs_size);
      ASSERT_TRUE(dma_mem != NULL);
      test_ctx_tx[i]->dma_mem = dma_mem;

      for (int j = 0; j < test_ctx_tx[i]->fb_cnt; j++) {
        test_ctx_tx[i]->ext_frames[j].buf_addr =
            (uint8_t*)mtl_dma_mem_addr(dma_mem) + j * fb_size;
        test_ctx_tx[i]->ext_frames[j].buf_iova = mtl_dma_mem_iova(dma_mem) + j * fb_size;
        test_ctx_tx[i]->ext_frames[j].buf_len = fb_size;
      }
    }

    uint8_t* fb;
    int total_lines = height[i];
    size_t bytes_per_line = (size_t)ops_tx.width / st20_pg.coverage * st20_pg.size;
    if (interlaced[i]) total_lines /= 2;
    for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
      if (ext) {
        fb = (uint8_t*)test_ctx_tx[i]->ext_frames[frame].buf_addr;
      } else {
        fb = (uint8_t*)st20_tx_get_framebuffer(tx_handle[i], frame);
      }

      ASSERT_TRUE(fb != NULL);

      for (int line = 0; line < total_lines; line++) {
        st_test_rand_data(fb + test_ctx_tx[i]->stride * line, bytes_per_line, frame);
      }
      unsigned char* result = test_ctx_tx[i]->shas[frame];
      SHA256((unsigned char*)fb, fb_size, result);
      test_sha_dump("st20_rx", result);
    }

    test_ctx_tx[i]->handle = tx_handle[i]; /* all ready now */
  }

  for (int i = 0; i < sessions; i++) {
    test_ctx_rx[i] = init_test_ctx(ctx, i, 3, true);
    ASSERT_TRUE(test_ctx_rx[i] != NULL);
    test_ctx_rx[i]->stop = false;

    test_ctx_rx[i]->fb_size = test_ctx_tx[i]->fb_size;
    test_ctx_rx[i]->frame_size = test_ctx_tx[i]->frame_size;

    if (ext) {
      test_ctx_rx[i]->ext_frames = (struct st20_ext_frame*)malloc(
          sizeof(*test_ctx_rx[i]->ext_frames) * test_ctx_rx[i]->fb_cnt);
      size_t fbs_size = test_ctx_rx[i]->fb_size * test_ctx_rx[i]->fb_cnt;
      mtl_dma_mem_handle dma_mem = mtl_dma_mem_alloc(m_handle, fbs_size);
      ASSERT_TRUE(dma_mem != NULL);
      test_ctx_rx[i]->dma_mem = dma_mem;

      for (int j = 0; j < test_ctx_rx[i]->fb_cnt; j++) {
        test_ctx_rx[i]->ext_frames[j].buf_addr =
            (uint8_t*)mtl_dma_mem_addr(dma_mem) + j * test_ctx_rx[i]->fb_size;
        test_ctx_rx[i]->ext_frames[j].buf_iova =
            mtl_dma_mem_iova(dma_mem) + j * test_ctx_rx[i]->fb_size;
        test_ctx_rx[i]->ext_frames[j].buf_len = test_ctx_rx[i]->fb_size;
      }
    }

    init_single_port_rx(ops_rx, test_ctx_rx[i], "st20_linesize_digest_test",
                        udp_port_for_idx(i));
    ops_rx.type = ST20_TYPE_FRAME_LEVEL;
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.linesize = linesize[i];
    ops_rx.fps = fps[i];
    ops_rx.fmt = fmt[i];
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.interlaced = interlaced[i];
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready =
        interlaced[i] ? st20_digest_rx_field_ready : st20_digest_rx_frame_ready;
    /* ST20_RX_FLAG_DMA_OFFLOAD set in helper */
    if (ext) ops_rx.ext_frames = test_ctx_rx[i]->ext_frames;

    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    test_ctx_rx[i]->width = ops_rx.width;
    test_ctx_rx[i]->height = ops_rx.height;
    st20_get_pgroup(ops_rx.fmt, &test_ctx_rx[i]->st20_pg);
    memcpy(test_ctx_rx[i]->shas, test_ctx_tx[i]->shas,
           TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
    test_ctx_rx[i]->total_pkts_in_frame = test_ctx_tx[i]->total_pkts_in_frame;
    test_ctx_rx[i]->handle = rx_handle[i];

    test_ctx_rx[i]->stop = false;
    if (interlaced[i]) {
      sha_check[i] = std::thread(st20_digest_rx_field_check, test_ctx_rx[i]);
    } else {
      sha_check[i] = std::thread(st20_digest_rx_frame_check, test_ctx_rx[i]);
    }

    bool dma_enabled = st20_rx_dma_enabled(rx_handle[i]);
    if (has_dma) {
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

  /* freeze counters before assertions */
  guard.stop();
  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    EXPECT_GT(test_ctx_rx[i]->check_sha_frame_cnt, 0);

    EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 2);
    EXPECT_EQ(test_ctx_rx[i]->incomplete_slice_cnt, 0);
    EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);
    info("%s, session %d fb_rec %d framerate %f fb_send %d\n", __func__, i,
         test_ctx_rx[i]->fb_rec, framerate[i], test_ctx_tx[i]->fb_send);
    if (check_fps) {
      EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    }
  }
}

TEST(St20_rx, linesize_digest_s3) {
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM_SL,
                                  ST20_PACKING_GPM_SL};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P50};
  int width[3] = {1280, 1920, 1920};
  int height[3] = {720, 1080, 1080};
  int linesize[3] = {4096, 5120, 8192};
  bool interlaced[3] = {false, true, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_linesize_digest_test(packing, fps, width, height, linesize, interlaced, fmt, true,
                            ST_TEST_LEVEL_MANDATORY, 3);
}

TEST(St20_rx, linesize_digest_crosslines_s3) {
  enum st20_packing packing[3] = {ST20_PACKING_BPM, ST20_PACKING_GPM, ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P50};
  int width[3] = {1280, 1920, 1920};
  int height[3] = {720, 1080, 1080};
  int linesize[3] = {4096, 5120, 8192};
  bool interlaced[3] = {true, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_linesize_digest_test(packing, fps, width, height, linesize, interlaced, fmt, true,
                            ST_TEST_LEVEL_MANDATORY, 3);
}

TEST(St20_rx, linesize_digest_ext_s3) {
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM_SL,
                                  ST20_PACKING_GPM_SL};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P50};
  int width[3] = {1280, 1920, 1920};
  int height[3] = {720, 1080, 1080};
  int linesize[3] = {4096, 5120, 8192};
  bool interlaced[3] = {true, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_linesize_digest_test(packing, fps, width, height, linesize, interlaced, fmt, true,
                            ST_TEST_LEVEL_MANDATORY, 3, true);
}