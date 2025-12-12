/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

static int rx_query_ext_frame(void* priv, st20_ext_frame* ext_frame,
                              struct st20_rx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;
  if (!ctx->handle) return -EIO; /* not ready */
  int i = ctx->ext_idx;

  /* check ext_fb_in_use */
  if (ctx->ext_fb_in_use[i]) {
    err("%s(%d), ext frame %d in use\n", __func__, ctx->idx, i);
    return -EIO;
  }
  ext_frame->buf_addr = ctx->ext_frames[i].buf_addr;
  ext_frame->buf_iova = ctx->ext_frames[i].buf_iova;
  ext_frame->buf_len = ctx->ext_frames[i].buf_len;

  dbg("%s(%d), set ext frame %d(%p) to use\n", __func__, ctx->idx, i,
      ext_frame->buf_addr);
  ctx->ext_fb_in_use[i] = true;

  ext_frame->opaque = &ctx->ext_fb_in_use[i];

  if (++ctx->ext_idx >= ctx->fb_cnt) ctx->ext_idx = 0;
  return 0;
}

static void st20_tx_ext_frame_rx_digest_test(enum st20_packing packing[],
                                             enum st_fps fps[], int width[], int height[],
                                             bool interlaced[], enum st20_fmt fmt[],
                                             bool check_fps, enum st_test_level level,
                                             int sessions = 1, bool dynamic = false) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops_tx;
  struct st20_rx_ops ops_rx;

  /* return if level small than global */
  if (level < ctx->level) return;

  if (ctx->iova == MTL_IOVA_MODE_PA) {
    info("%s, skip as it's PA iova mode\n", __func__);
    return;
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
  guard.set_ext_buf(true);
  guard.set_rx_ctx_cleanup(st20_rx_drain_bufq_put_framebuff);

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(fps[i]);

    test_ctx_tx[i] = init_test_ctx(ctx, i, TEST_SHA_HIST_NUM, true);
    ASSERT_TRUE(test_ctx_tx[i] != NULL);
    test_ctx_tx[i]->stop = false;

    init_single_port_tx(ops_tx, test_ctx_tx[i], "st20_ext_frame_digest_test",
                        udp_port_for_idx(i));
    ops_tx.packing = packing[i];
    ops_tx.type = ST20_TYPE_FRAME_LEVEL;
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.interlaced = interlaced[i];
    ops_tx.fps = fps[i];
    ops_tx.fmt = fmt[i];
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_tx.flags |= ST20_TX_FLAG_EXT_FRAME;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame =
        interlaced[i] ? tx_next_ext_video_field : tx_next_ext_video_frame;
    ops_tx.notify_frame_done = tx_notify_ext_frame_done;

    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    /* sha calculate */
    struct st20_pgroup st20_pg;
    st20_get_pgroup(ops_tx.fmt, &st20_pg);
    size_t frame_size = ops_tx.width * ops_tx.height * st20_pg.size / st20_pg.coverage;
    if (interlaced[i]) frame_size = frame_size >> 1;
    EXPECT_EQ(st20_tx_get_framebuffer_size(tx_handle[i]), frame_size);
    EXPECT_EQ(st20_tx_get_framebuffer_count(tx_handle[i]), test_ctx_tx[i]->fb_cnt);

    test_ctx_tx[i]->frame_size = frame_size;
    test_ctx_tx[i]->height = ops_tx.height;
    test_ctx_tx[i]->stride = ops_tx.width / st20_pg.coverage * st20_pg.size;

    test_ctx_tx[i]->ext_frames = (struct st20_ext_frame*)malloc(
        sizeof(*test_ctx_tx[i]->ext_frames) * test_ctx_tx[i]->fb_cnt);
    size_t pg_sz = mtl_page_size(m_handle);
    size_t fb_size = test_ctx_tx[i]->frame_size * test_ctx_tx[i]->fb_cnt;
    test_ctx_tx[i]->ext_fb_iova_map_sz = mtl_size_page_align(fb_size, pg_sz); /* align */
    size_t fb_size_malloc = test_ctx_tx[i]->ext_fb_iova_map_sz + pg_sz;
    test_ctx_tx[i]->ext_fb_malloc = st_test_zmalloc(fb_size_malloc);
    ASSERT_TRUE(test_ctx_tx[i]->ext_fb_malloc != NULL);
    test_ctx_tx[i]->ext_fb =
        (uint8_t*)MTL_ALIGN((uint64_t)test_ctx_tx[i]->ext_fb_malloc, pg_sz);
    test_ctx_tx[i]->ext_fb_iova =
        mtl_dma_map(m_handle, test_ctx_tx[i]->ext_fb, test_ctx_tx[i]->ext_fb_iova_map_sz);
    ASSERT_TRUE(test_ctx_tx[i]->ext_fb_iova != MTL_BAD_IOVA);
    info("%s, session %d ext_fb %p\n", __func__, i, test_ctx_tx[i]->ext_fb);

    for (int j = 0; j < test_ctx_tx[i]->fb_cnt; j++) {
      test_ctx_tx[i]->ext_frames[j].buf_addr = test_ctx_tx[i]->ext_fb + j * frame_size;
      test_ctx_tx[i]->ext_frames[j].buf_iova =
          test_ctx_tx[i]->ext_fb_iova + j * frame_size;
      test_ctx_tx[i]->ext_frames[j].buf_len = frame_size;
    }

    uint8_t* fb;
    for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
      fb = (uint8_t*)test_ctx_tx[i]->ext_fb + frame * frame_size;

      ASSERT_TRUE(fb != NULL);
      st_test_rand_data(fb, frame_size, frame);
      unsigned char* result = test_ctx_tx[i]->shas[frame];
      SHA256((unsigned char*)fb, frame_size, result);
      test_sha_dump("st20_rx", result);
    }

    test_ctx_tx[i]->handle = tx_handle[i]; /* all ready now */
  }

  for (int i = 0; i < sessions; i++) {
    test_ctx_rx[i] = init_test_ctx(ctx, i, 3, true);
    ASSERT_TRUE(test_ctx_rx[i] != NULL);
    test_ctx_rx[i]->stop = false;

    test_ctx_rx[i]->ext_frames = (struct st20_ext_frame*)malloc(
        sizeof(*test_ctx_rx[i]->ext_frames) * test_ctx_rx[i]->fb_cnt);
    size_t frame_size = st20_frame_size(fmt[i], width[i], height[i]);
    size_t pg_sz = mtl_page_size(m_handle);
    size_t fb_size = frame_size * test_ctx_rx[i]->fb_cnt;
    test_ctx_rx[i]->ext_fb_iova_map_sz = mtl_size_page_align(fb_size, pg_sz); /* align */
    size_t fb_size_malloc = test_ctx_rx[i]->ext_fb_iova_map_sz + pg_sz;
    test_ctx_rx[i]->ext_fb_malloc = st_test_zmalloc(fb_size_malloc);
    ASSERT_TRUE(test_ctx_rx[i]->ext_fb_malloc != NULL);
    test_ctx_rx[i]->ext_fb =
        (uint8_t*)MTL_ALIGN((uint64_t)test_ctx_rx[i]->ext_fb_malloc, pg_sz);
    test_ctx_rx[i]->ext_fb_iova =
        mtl_dma_map(m_handle, test_ctx_rx[i]->ext_fb, test_ctx_rx[i]->ext_fb_iova_map_sz);
    info("%s, session %d ext_fb %p\n", __func__, i, test_ctx_rx[i]->ext_fb);
    ASSERT_TRUE(test_ctx_rx[i]->ext_fb_iova != MTL_BAD_IOVA);

    for (int j = 0; j < test_ctx_rx[i]->fb_cnt; j++) {
      test_ctx_rx[i]->ext_frames[j].buf_addr = test_ctx_rx[i]->ext_fb + j * frame_size;
      test_ctx_rx[i]->ext_frames[j].buf_iova =
          test_ctx_rx[i]->ext_fb_iova + j * frame_size;
      test_ctx_rx[i]->ext_frames[j].buf_len = frame_size;
    }

    init_single_port_rx(ops_rx, test_ctx_rx[i], "st20_ext_frame_digest_test",
                        udp_port_for_idx(i));
    ops_rx.type = ST20_TYPE_FRAME_LEVEL;
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.fps = fps[i];
    ops_rx.fmt = fmt[i];
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.interlaced = interlaced[i];
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready =
        interlaced[i] ? st20_digest_rx_field_ready : st20_digest_rx_frame_ready;
    /* ST20_RX_FLAG_DMA_OFFLOAD set in helper */
    if (dynamic) {
      ops_rx.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
      ops_rx.query_ext_frame = rx_query_ext_frame;
    } else {
      ops_rx.ext_frames = test_ctx_rx[i]->ext_frames;
    }

    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    test_ctx_rx[i]->frame_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->fb_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->width = ops_rx.width;
    st20_get_pgroup(ops_rx.fmt, &test_ctx_rx[i]->st20_pg);
    memcpy(test_ctx_rx[i]->shas, test_ctx_tx[i]->shas,
           TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
    test_ctx_rx[i]->total_pkts_in_frame = test_ctx_tx[i]->total_pkts_in_frame;
    test_ctx_rx[i]->handle = rx_handle[i];

    test_ctx_rx[i]->stop = false;
    if (interlaced[i]) {
      rtp_thread_rx[i] = std::thread(st20_digest_rx_field_check, test_ctx_rx[i]);
    } else {
      rtp_thread_rx[i] = std::thread(st20_digest_rx_frame_check, test_ctx_rx[i]);
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

    EXPECT_LE(test_ctx_rx[i]->incomplete_frame_cnt, 4);
    EXPECT_EQ(test_ctx_rx[i]->incomplete_slice_cnt, 0);
    EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);
    info("%s, session %d fb_rec %d framerate %f fb_send %d\n", __func__, i,
         test_ctx_rx[i]->fb_rec, framerate[i], test_ctx_tx[i]->fb_send);
    if (check_fps) {
      EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    }
  }
}

TEST(St20_rx, ext_frame_digest_frame_1080p_fps59_94_s1) {
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  bool interlaced[1] = {false};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  st20_tx_ext_frame_rx_digest_test(packing, fps, width, height, interlaced, fmt, true,
                                   ST_TEST_LEVEL_ALL);
}

TEST(St20_rx, ext_frame_digest20_field_1080p_fps59_94_s1) {
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  bool interlaced[1] = {true};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  st20_tx_ext_frame_rx_digest_test(packing, fps, width, height, interlaced, fmt, true,
                                   ST_TEST_LEVEL_ALL);
}

TEST(St20_rx, ext_frame_digest_frame_720p_fps59_94_s1_gpm) {
  enum st20_packing packing[1] = {ST20_PACKING_GPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1280};
  int height[1] = {720};
  bool interlaced[1] = {false};
  enum st20_fmt fmt[1] = {ST20_FMT_YUV_422_10BIT};
  st20_tx_ext_frame_rx_digest_test(packing, fps, width, height, interlaced, fmt, true,
                                   ST_TEST_LEVEL_ALL);
}

TEST(St20_rx, ext_frame_s3) {
  enum st20_packing packing[3] = {ST20_PACKING_BPM, ST20_PACKING_BPM, ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P50};
  int width[3] = {1280, 1920, 1920};
  int height[3] = {720, 1080, 1080};
  bool interlaced[3] = {true, true, true};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_tx_ext_frame_rx_digest_test(packing, fps, width, height, interlaced, fmt, true,
                                   ST_TEST_LEVEL_MANDATORY, 3);
}

TEST(St20_rx, ext_frame_s3_2) {
  enum st20_packing packing[3] = {ST20_PACKING_BPM, ST20_PACKING_BPM, ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P50};
  int width[3] = {1280, 1920, 1920};
  int height[3] = {720, 1080, 1080};
  bool interlaced[3] = {true, false, true};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_12BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_8BIT};
  st20_tx_ext_frame_rx_digest_test(packing, fps, width, height, interlaced, fmt, true,
                                   ST_TEST_LEVEL_MANDATORY, 3);
}

TEST(St20_rx, dynamic_ext_frame_s3) {
  enum st20_packing packing[3] = {ST20_PACKING_BPM, ST20_PACKING_BPM, ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P29_97};
  int width[3] = {1280, 1280, 1920};
  int height[3] = {720, 720, 1080};
  bool interlaced[3] = {false, false, false};
  enum st20_fmt fmt[3] = {ST20_FMT_YUV_422_10BIT, ST20_FMT_YUV_422_10BIT,
                          ST20_FMT_YUV_422_10BIT};
  st20_tx_ext_frame_rx_digest_test(packing, fps, width, height, interlaced, fmt, true,
                                   ST_TEST_LEVEL_MANDATORY, 3, true);
}