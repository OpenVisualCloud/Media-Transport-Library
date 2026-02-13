/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

static int st20_rx_detected(void* priv, const struct st20_detect_meta* meta,
                            struct st20_detect_reply* reply) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO; /* not ready */

  struct st20_rx_slice_meta* s_meta = (struct st20_rx_slice_meta*)ctx->priv;

  ctx->lines_per_slice = meta->height / 32;
  if (s_meta) reply->slice_lines = ctx->lines_per_slice;
  if (ctx->uframe_size != 0) {
    /* uframe fmt: yuv422 10bit planar */
    ctx->uframe_size = (size_t)meta->width * meta->height * 2 * sizeof(uint16_t);
    reply->uframe_size = ctx->uframe_size;
    if (s_meta) s_meta->uframe_total_size = ctx->uframe_size;
  }

  return 0;
}

static void st20_rx_detect_test(enum st20_type tx_type[], enum st20_type rx_type[],
                                enum st20_packing packing[], enum st_fps fps[],
                                int width[], int height[], bool interlaced[],
                                bool user_frame, enum st20_fmt fmt, bool check_fps,
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
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread_tx;
  std::vector<std::thread> rtp_thread_rx;
  int slices_per_frame = 32;

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
  guard.set_rx_ctx_cleanup(st20_rx_drain_bufq_put_framebuff);

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(fps[i]);

    test_ctx_tx[i] = init_test_ctx(ctx, i, TEST_SHA_HIST_NUM, true);
    ASSERT_TRUE(test_ctx_tx[i] != NULL);
    test_ctx_tx[i]->stop = false;

    init_single_port_tx(ops_tx, test_ctx_tx[i], "st20_detect_test", udp_port_for_idx(i));
    ops_tx.packing = packing[i];
    ops_tx.type = tx_type[i];
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.interlaced = interlaced[i];
    ops_tx.fps = fps[i];
    ops_tx.fmt = fmt;
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = interlaced[i] ? tx_next_video_field : tx_next_video_frame;
    ops_tx.query_frame_lines_ready = tx_frame_lines_ready;

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
    uint8_t* fb;
    if (user_frame) {
      /* uframe fmt: yuv422 10bit planar */
      size_t uframe_size = (size_t)ops_tx.width * ops_tx.height * 2 * sizeof(uint16_t);
      if (interlaced[i]) uframe_size = uframe_size >> 1;
      test_ctx_tx[i]->uframe_size = uframe_size;
      test_ctx_tx[i]->slice = false;
      for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
        test_ctx_tx[i]->frame_buf[frame] = (uint8_t*)st_test_zmalloc(uframe_size);
        fb = test_ctx_tx[i]->frame_buf[frame];
        ASSERT_TRUE(fb != NULL);
        uint16_t* p10_u16 = (uint16_t*)fb;
        for (size_t i = 0; i < (uframe_size / 2); i++) {
          p10_u16[i] = rand() & 0x3ff; /* only 10 bit */
        }
        unsigned char* result = test_ctx_tx[i]->shas[frame];
        SHA256((unsigned char*)fb, uframe_size, result);
        test_sha_dump("st20_rx", result);

        struct st20_rfc4175_422_10_pg2_be* pg =
            (struct st20_rfc4175_422_10_pg2_be*)st20_tx_get_framebuffer(tx_handle[i],
                                                                        frame);
        st20_yuv422p10le_to_rfc4175_422be10(
            p10_u16, (p10_u16 + ops_tx.width * ops_tx.height),
            (p10_u16 + ops_tx.width * ops_tx.height * 3 / 2), pg, ops_tx.width,
            ops_tx.height);
      }
    } else {
      test_ctx_tx[i]->lines_per_slice = ops_tx.height / 30;
      test_ctx_tx[i]->slice = (tx_type[i] == ST20_TYPE_SLICE_LEVEL);
      for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
        fb = (uint8_t*)st20_tx_get_framebuffer(tx_handle[i], frame);
        ASSERT_TRUE(fb != NULL);
        st_test_rand_data(fb, frame_size, frame);
        unsigned char* result = test_ctx_tx[i]->shas[frame];
        SHA256((unsigned char*)fb, frame_size, result);
        test_sha_dump("st20_rx", result);
      }
    }

    test_ctx_tx[i]->handle = tx_handle[i];
  }

  for (int i = 0; i < sessions; i++) {
    test_ctx_rx[i] = init_test_ctx(ctx, i, 3, true);
    ASSERT_TRUE(test_ctx_rx[i] != NULL);
    test_ctx_rx[i]->stop = false;

    init_single_port_rx(ops_rx, test_ctx_rx[i], "st20_detect_test", udp_port_for_idx(i));
    ops_rx.type = rx_type[i];
    ops_rx.width = 1920;
    ops_rx.height = 1080;
    ops_rx.fps = ST_FPS_P59_94;
    ops_rx.fmt = fmt;
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.interlaced = interlaced[i];
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.slice_lines = height[i] / slices_per_frame;
    ops_rx.notify_frame_ready =
        interlaced[i] ? st20_digest_rx_field_ready : st20_digest_rx_frame_ready;
    ops_rx.notify_slice_ready = st20_digest_rx_slice_ready;
    ops_rx.notify_detected = st20_rx_detected;
    if (user_frame) {
      ops_rx.uframe_size = 1;
      ops_rx.uframe_pg_callback = st20_rx_uframe_pg_callback;
    } else {
      ops_rx.uframe_size = 0;
    }
    ops_rx.flags |= ST20_RX_FLAG_AUTO_DETECT;

    if (rx_type[i] == ST20_TYPE_SLICE_LEVEL) {
      /* set expect meta data to private */
      auto meta =
          (struct st20_rx_slice_meta*)st_test_zmalloc(sizeof(struct st20_rx_slice_meta));
      ASSERT_TRUE(meta != NULL);
      meta->width = width[i];
      meta->height = height[i];
      meta->fps = fps[i];
      meta->fmt = fmt;
      meta->frame_total_size = test_ctx_tx[i]->frame_size;
      meta->uframe_total_size = 0;
      meta->second_field = false;
      test_ctx_rx[i]->priv = meta;
      ops_rx.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
    }

    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);

    ASSERT_TRUE(rx_handle[i] != NULL);

    test_ctx_rx[i]->frame_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->fb_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->uframe_size = ops_rx.uframe_size;
    test_ctx_rx[i]->width = ops_tx.width;
    st20_get_pgroup(ops_rx.fmt, &test_ctx_rx[i]->st20_pg);
    memcpy(test_ctx_rx[i]->shas, test_ctx_tx[i]->shas,
           TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
    test_ctx_rx[i]->total_pkts_in_frame = test_ctx_tx[i]->total_pkts_in_frame;
    ASSERT_TRUE(rx_handle[i] != NULL);
    test_ctx_rx[i]->handle = rx_handle[i];
    test_ctx_rx[i]->stop = false;
    if (interlaced[i]) {
      rtp_thread_rx[i] = std::thread(st20_digest_rx_field_check, test_ctx_rx[i]);
    } else {
      rtp_thread_rx[i] = std::thread(st20_digest_rx_frame_check, test_ctx_rx[i]);
    }
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
    if ((rx_type[i] == ST20_TYPE_SLICE_LEVEL) && (height[i] >= (1080 * 4)))
      EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 2 * 8);
    else
      EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 2 * 2);
    EXPECT_EQ(test_ctx_rx[i]->incomplete_slice_cnt, 0);
    if (rx_type[i] == ST20_TYPE_FRAME_LEVEL)
      EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);
    else
      EXPECT_LE(test_ctx_rx[i]->sha_fail_cnt, 2);
    info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
         framerate[i]);
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

TEST(St20_rx, detect_1080p_fps59_94_s1) {
  enum st20_type tx_type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_BPM};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  bool interlaced[1] = {false};
  st20_rx_detect_test(tx_type, rx_type, packing, fps, width, height, interlaced, false,
                      ST20_FMT_YUV_422_10BIT, true, ST_TEST_LEVEL_ALL, 1);
}

TEST(St20_rx, detect_uframe_mix_s2) {
  enum st20_type tx_type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_SLICE_LEVEL};
  enum st20_packing packing[2] = {ST20_PACKING_BPM, ST20_PACKING_BPM};
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P29_97};
  int width[2] = {1280, 1280};
  int height[2] = {720, 720};
  bool interlaced[2] = {false, false};
  st20_rx_detect_test(tx_type, rx_type, packing, fps, width, height, interlaced, true,
                      ST20_FMT_YUV_422_10BIT, false, ST_TEST_LEVEL_MANDATORY, 2);
}

TEST(St20_rx, detect_mix_frame_s3) {
  enum st20_type tx_type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_BPM, ST20_PACKING_GPM,
                                  ST20_PACKING_GPM_SL};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P29_97};
  int width[3] = {1280, 1920, 3840};
  int height[3] = {720, 1080, 2160};
  bool interlaced[3] = {false, false, true};
  st20_rx_detect_test(tx_type, rx_type, packing, fps, width, height, interlaced, false,
                      ST20_FMT_YUV_422_10BIT, true, ST_TEST_LEVEL_MANDATORY, 3);
}

TEST(St20_rx, detect_mix_slice_s3) {
  enum st20_type tx_type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                               ST20_TYPE_FRAME_LEVEL};
  enum st20_type rx_type[3] = {ST20_TYPE_SLICE_LEVEL, ST20_TYPE_SLICE_LEVEL,
                               ST20_TYPE_SLICE_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_BPM, ST20_PACKING_GPM,
                                  ST20_PACKING_GPM_SL};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P50, ST_FPS_P29_97};
  int width[3] = {1280, 1920, 3840};
  int height[3] = {720, 1080, 2160};
  bool interlaced[3] = {false, false, true};
  st20_rx_detect_test(tx_type, rx_type, packing, fps, width, height, interlaced, false,
                      ST20_FMT_YUV_422_10BIT, true, ST_TEST_LEVEL_MANDATORY, 3);
}