/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

namespace {

struct St20DetectStream {
  enum st20_type tx_type;
  enum st20_type rx_type;
  enum st20_packing packing;
  enum st_fps fps;
  int width;
  int height;
  bool interlaced = false;
};

struct St20DetectScenario {
  std::vector<St20DetectStream> streams;
  bool user_frame = false;
  enum st20_fmt fmt = ST20_FMT_YUV_422_10BIT;
  bool check_fps = true;
  enum st_test_level level = ST_TEST_LEVEL_ALL;
};

int st20_rx_detected(void* priv, const struct st20_detect_meta* meta,
                     struct st20_detect_reply* reply) {
  auto ctx = (tests_context*)priv;

  if (!st20_get_handle(ctx)) return -EIO; /* not ready */

  struct st20_rx_slice_meta* s_meta = (struct st20_rx_slice_meta*)ctx->priv;

  int slice_lines = meta->height / 32;
  __atomic_store_n(&ctx->lines_per_slice, slice_lines, __ATOMIC_RELEASE);
  if (s_meta) reply->slice_lines = slice_lines;
  if (ctx->uframe_size != 0) {
    /* uframe fmt: yuv422 10bit planar */
    ctx->uframe_size = (size_t)meta->width * meta->height * 2 * sizeof(uint16_t);
    reply->uframe_size = ctx->uframe_size;
    if (s_meta) s_meta->uframe_total_size = ctx->uframe_size;
  }

  return 0;
}

int rx_detected_lines(const tests_context* c) {
  return mtl_test::relaxed_load(&c->lines_per_slice);
}

void st20_rx_detect_test(const St20DetectScenario& s) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  const int sessions = static_cast<int>(s.streams.size());
  constexpr int kSlicesPerFrame = 32;

  MTL_TEST_REQUIRE_LEVEL(ctx, s.level);
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  std::vector<tests_context*> test_ctx_tx(sessions);
  std::vector<tests_context*> test_ctx_rx(sessions);
  std::vector<st20_tx_handle> tx_handle(sessions);
  std::vector<st20_rx_handle> rx_handle(sessions);
  std::vector<double> expect_framerate(sessions);
  std::vector<std::thread> rtp_thread_tx(sessions);
  std::vector<std::thread> rtp_thread_rx(sessions);

  St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle,
                        &rtp_thread_tx, &rtp_thread_rx);
  guard.set_rx_ctx_cleanup(st20_rx_drain_bufq_put_framebuff);

  for (int i = 0; i < sessions; i++) {
    const auto& st = s.streams[i];
    expect_framerate[i] = st_frame_rate(st.fps);

    test_ctx_tx[i] = init_test_ctx(ctx, i, TEST_SHA_HIST_NUM, true);
    ASSERT_TRUE(test_ctx_tx[i] != NULL);
    test_ctx_tx[i]->ready.store(false, std::memory_order_relaxed);
    test_ctx_tx[i]->stop = false;

    struct st20_tx_ops ops_tx;
    init_single_port_tx(ops_tx, test_ctx_tx[i], "st20_detect_test", udp_port_for_idx(i));
    ops_tx.packing = st.packing;
    ops_tx.type = st.tx_type;
    ops_tx.width = st.width;
    ops_tx.height = st.height;
    ops_tx.interlaced = st.interlaced;
    ops_tx.fps = st.fps;
    ops_tx.fmt = s.fmt;
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = st.interlaced ? tx_next_video_field : tx_next_video_frame;
    ops_tx.query_frame_lines_ready = tx_frame_lines_ready;

    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    /* sha calculate */
    struct st20_pgroup st20_pg;
    st20_get_pgroup(ops_tx.fmt, &st20_pg);
    size_t frame_size = ops_tx.width * ops_tx.height * st20_pg.size / st20_pg.coverage;
    if (st.interlaced) frame_size = frame_size >> 1;
    test_ctx_tx[i]->frame_size = frame_size;
    test_ctx_tx[i]->height = ops_tx.height;
    test_ctx_tx[i]->stride = ops_tx.width / st20_pg.coverage * st20_pg.size;
    if (s.user_frame) {
      /* uframe fmt: yuv422 10bit planar */
      size_t uframe_size = (size_t)ops_tx.width * ops_tx.height * 2 * sizeof(uint16_t);
      if (st.interlaced) uframe_size = uframe_size >> 1;
      test_ctx_tx[i]->uframe_size = uframe_size;
      test_ctx_tx[i]->slice = false;
      for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
        test_ctx_tx[i]->frame_buf[frame] = (uint8_t*)st_test_zmalloc(uframe_size);
        uint8_t* fb = test_ctx_tx[i]->frame_buf[frame];
        ASSERT_TRUE(fb != NULL);
        uint16_t* p10_u16 = (uint16_t*)fb;
        for (size_t k = 0; k < (uframe_size / 2); k++) {
          p10_u16[k] = rand() & 0x3ff; /* only 10 bit */
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
      test_ctx_tx[i]->slice = (st.tx_type == ST20_TYPE_SLICE_LEVEL);
      for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
        uint8_t* fb = (uint8_t*)st20_tx_get_framebuffer(tx_handle[i], frame);
        ASSERT_TRUE(fb != NULL);
        st_test_rand_data(fb, frame_size, frame);
        unsigned char* result = test_ctx_tx[i]->shas[frame];
        SHA256((unsigned char*)fb, frame_size, result);
        test_sha_dump("st20_rx", result);
      }
    }
  }

  for (int i = 0; i < sessions; i++) {
    const auto& st = s.streams[i];
    test_ctx_rx[i] = init_test_ctx(ctx, i, 3, true);
    ASSERT_TRUE(test_ctx_rx[i] != NULL);
    test_ctx_rx[i]->stop = false;

    struct st20_rx_ops ops_rx;
    init_single_port_rx(ops_rx, test_ctx_rx[i], "st20_detect_test", udp_port_for_idx(i));
    ops_rx.type = st.rx_type;
    ops_rx.width = 1920;
    ops_rx.height = 1080;
    ops_rx.fps = ST_FPS_P59_94;
    ops_rx.fmt = s.fmt;
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.interlaced = st.interlaced;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.slice_lines = st.height / kSlicesPerFrame;
    ops_rx.notify_frame_ready =
        st.interlaced ? st20_digest_rx_field_ready : st20_digest_rx_frame_ready;
    ops_rx.notify_slice_ready = st20_digest_rx_slice_ready;
    ops_rx.notify_detected = st20_rx_detected;
    if (s.user_frame) {
      ops_rx.uframe_size = 1;
      ops_rx.uframe_pg_callback = st20_rx_uframe_pg_callback;
    } else {
      ops_rx.uframe_size = 0;
    }
    ops_rx.flags |= ST20_RX_FLAG_AUTO_DETECT;

    if (st.rx_type == ST20_TYPE_SLICE_LEVEL) {
      auto meta =
          (struct st20_rx_slice_meta*)st_test_zmalloc(sizeof(struct st20_rx_slice_meta));
      ASSERT_TRUE(meta != NULL);
      meta->width = st.width;
      meta->height = st.height;
      meta->fps = st.fps;
      meta->fmt = s.fmt;
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
    test_ctx_rx[i]->width = st.width;
    st20_get_pgroup(ops_rx.fmt, &test_ctx_rx[i]->st20_pg);
    memcpy(test_ctx_rx[i]->shas, test_ctx_tx[i]->shas,
           TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
    test_ctx_rx[i]->total_pkts_in_frame = test_ctx_tx[i]->total_pkts_in_frame;
    st20_set_handle(test_ctx_rx[i], rx_handle[i]);
    if (st.interlaced) {
      rtp_thread_rx[i] = std::thread(st20_digest_rx_field_check, test_ctx_rx[i]);
    } else {
      rtp_thread_rx[i] = std::thread(st20_digest_rx_frame_check, test_ctx_rx[i]);
    }
  }

  for (int i = 0; i < sessions; i++) {
    st20_set_handle(test_ctx_tx[i], tx_handle[i]);
    test_ctx_tx[i]->ready.store(true, std::memory_order_release);
  }

  int ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  guard.set_started(ret >= 0);

  bool has_slice_rx = false;
  for (const auto& st : s.streams) {
    if (st.rx_type == ST20_TYPE_SLICE_LEVEL) has_slice_rx = true;
  }

  /* Wait for auto-detect callback on every RX session, then run rate/digest window. */
  EXPECT_TRUE(
      mtl_test::wait_for_all(test_ctx_rx, rx_detected_lines, 1, kSt20WarmupTimeoutMs));
  auto res = st20_wait_rx_window(test_ctx_rx, s.check_fps, true,
                                 has_slice_rx ? kSt20MinSliceRxFrames : kSt20MinRxFrames);
  EXPECT_TRUE(res.warmed_up);
  EXPECT_TRUE(res.complete);

  for (int i = 0; i < sessions; i++)
    test_ctx_tx[i]->ready.store(false, std::memory_order_release);
  guard.release_sessions();

  for (int i = 0; i < sessions; i++) {
    const auto& st = s.streams[i];
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    EXPECT_GT(test_ctx_rx[i]->check_sha_frame_cnt, 0);
    if ((st.rx_type == ST20_TYPE_SLICE_LEVEL) && (st.height >= (1080 * 4)))
      EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 2 * 8);
    else
      EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 2 * 2);
    EXPECT_EQ(test_ctx_rx[i]->incomplete_slice_cnt, 0);
    if (st.rx_type == ST20_TYPE_FRAME_LEVEL)
      EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);
    else
      EXPECT_LE(test_ctx_rx[i]->sha_fail_cnt, 2);
    info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
         res.rate[i]);
    if (st.rx_type == ST20_TYPE_SLICE_LEVEL) {
      int expect_slice_cnt = test_ctx_rx[i]->fb_rec * kSlicesPerFrame;
      if (st.interlaced) expect_slice_cnt /= 2;
      EXPECT_NEAR(test_ctx_rx[i]->slice_cnt, expect_slice_cnt, expect_slice_cnt * 0.1);
    }
    if (s.check_fps) {
      EXPECT_NEAR(res.rate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    }
  }
}

}  // namespace

TEST(St20_rx, detect_1080p_fps59_94_s1) {
  st20_rx_detect_test({
      {{ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
        1920, 1080, false}},
      false,
      ST20_FMT_YUV_422_10BIT,
      true,
      ST_TEST_LEVEL_ALL,
  });
}

TEST(St20_rx, detect_uframe_mix_s2) {
  st20_rx_detect_test({
      {{ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
        1280, 720, false},
       {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_SLICE_LEVEL, ST20_PACKING_BPM, ST_FPS_P29_97,
        1280, 720, false}},
      true,
      ST20_FMT_YUV_422_10BIT,
      false,
      ST_TEST_LEVEL_MANDATORY,
  });
}

TEST(St20_rx, detect_mix_frame_s3) {
  st20_rx_detect_test({
      {{ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
        1280, 720, false},
       {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1920,
        1080, false},
       {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P29_97,
        3840, 2160, true}},
      false,
      ST20_FMT_YUV_422_10BIT,
      true,
      ST_TEST_LEVEL_MANDATORY,
  });
}

TEST(St20_rx, detect_mix_slice_s3) {
  st20_rx_detect_test({
      {{ST20_TYPE_FRAME_LEVEL, ST20_TYPE_SLICE_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
        1280, 720, false},
       {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_SLICE_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1920,
        1080, false},
       {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_SLICE_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P29_97,
        3840, 2160, true}},
      false,
      ST20_FMT_YUV_422_10BIT,
      true,
      ST_TEST_LEVEL_MANDATORY,
  });
}
