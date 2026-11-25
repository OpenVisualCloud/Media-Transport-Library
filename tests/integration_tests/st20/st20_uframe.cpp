/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

int st20_rx_uframe_pg_callback(void* /*priv*/, void* frame,
                               struct st20_rx_uframe_pg_meta* meta) {
  uint32_t w = meta->width;
  uint32_t h = meta->height;
  uint16_t* p10_u16 = (uint16_t*)frame;
  uint16_t* p10_u16_y = p10_u16;
  uint16_t* p10_u16_b = p10_u16 + w * h;
  uint16_t* p10_u16_r = p10_u16 + w * h * 3 / 2;
  struct st20_rfc4175_422_10_pg2_be* pg =
      (struct st20_rfc4175_422_10_pg2_be*)meta->payload;
  uint32_t p10_offset = meta->row_number * w + meta->row_offset;
  p10_u16_y += p10_offset;
  p10_u16_b += p10_offset / 2;
  p10_u16_r += p10_offset / 2;

  st20_rfc4175_422be10_to_yuv422p10le(pg, p10_u16_y, p10_u16_b, p10_u16_r,
                                      meta->pg_cnt * 2, 1);
  return 0;
}

namespace {

struct St20UframeStream {
  enum st20_type rx_type;
  enum st20_packing packing;
  enum st_fps fps;
  int width;
  int height;
  bool interlaced = false;
};

struct St20UframeScenario {
  std::vector<St20UframeStream> streams;
  enum st20_fmt fmt = ST20_FMT_YUV_422_10BIT;
  bool check_fps = true;
  enum st_test_level level = ST_TEST_LEVEL_ALL;
};

void st20_rx_uframe_test(const St20UframeScenario& s) {
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
  std::vector<std::thread> digest_thread_rx(sessions);

  St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle, nullptr,
                        &digest_thread_rx);
  guard.set_rx_ctx_cleanup(st20_rx_drain_bufq_put_framebuff);

  for (int i = 0; i < sessions; i++) {
    const auto& st = s.streams[i];
    expect_framerate[i] = st_frame_rate(st.fps);

    test_ctx_tx[i] = init_test_ctx(ctx, i, TEST_SHA_HIST_NUM, true);
    ASSERT_TRUE(test_ctx_tx[i] != NULL);
    test_ctx_tx[i]->ready.store(false, std::memory_order_relaxed);
    test_ctx_tx[i]->stop = false;

    struct st20_tx_ops ops_tx;
    init_single_port_tx(ops_tx, test_ctx_tx[i], "st20_uframe_test", udp_port_for_idx(i));
    ops_tx.packing = st.packing;
    ops_tx.type = ST20_TYPE_FRAME_LEVEL;
    ops_tx.width = st.width;
    ops_tx.height = st.height;
    ops_tx.interlaced = st.interlaced;
    ops_tx.fps = st.fps;
    ops_tx.fmt = s.fmt;
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = st.interlaced ? tx_next_video_field : tx_next_video_frame;

    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    /* sha calculate */
    struct st20_pgroup st20_pg;
    st20_get_pgroup(ops_tx.fmt, &st20_pg);
    size_t frame_size = ops_tx.width * ops_tx.height * st20_pg.size / st20_pg.coverage;
    if (st.interlaced) frame_size = frame_size >> 1;
    test_ctx_tx[i]->frame_size = frame_size;
    /* uframe fmt: yuv422 10bit planar */
    size_t uframe_size = (size_t)ops_tx.width * ops_tx.height * 2 * sizeof(uint16_t);
    if (st.interlaced) uframe_size = uframe_size >> 1;
    test_ctx_tx[i]->uframe_size = uframe_size;
    test_ctx_tx[i]->slice = false;
    test_ctx_tx[i]->height = ops_tx.height;
    test_ctx_tx[i]->stride = ops_tx.width / st20_pg.coverage * st20_pg.size;
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
  }

  for (int i = 0; i < sessions; i++) {
    const auto& st = s.streams[i];
    test_ctx_rx[i] = init_test_ctx(ctx, i, 3, true);
    ASSERT_TRUE(test_ctx_rx[i] != NULL);
    test_ctx_rx[i]->stop = false;

    struct st20_rx_ops ops_rx;
    init_single_port_rx(ops_rx, test_ctx_rx[i], "st20_uframe_test", udp_port_for_idx(i));
    ops_rx.type = st.rx_type;
    ops_rx.width = st.width;
    ops_rx.height = st.height;
    ops_rx.fps = st.fps;
    ops_rx.fmt = s.fmt;
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.interlaced = st.interlaced;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.slice_lines = st.height / kSlicesPerFrame;
    ops_rx.notify_frame_ready =
        st.interlaced ? st20_digest_rx_field_ready : st20_digest_rx_frame_ready;
    ops_rx.notify_slice_ready = st20_digest_rx_slice_ready;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024 * 2;
    /* uframe fmt: yuv422 10bit planar */
    ops_rx.uframe_size = (size_t)ops_rx.width * ops_rx.height * 2 * sizeof(uint16_t);
    ops_rx.uframe_pg_callback = st20_rx_uframe_pg_callback;

    if (st.rx_type == ST20_TYPE_SLICE_LEVEL) {
      auto meta =
          (struct st20_rx_slice_meta*)st_test_zmalloc(sizeof(struct st20_rx_slice_meta));
      ASSERT_TRUE(meta != NULL);
      meta->width = ops_rx.width;
      meta->height = ops_rx.height;
      meta->fps = ops_rx.fps;
      meta->fmt = ops_rx.fmt;
      meta->frame_total_size = test_ctx_tx[i]->frame_size;
      meta->uframe_total_size = ops_rx.uframe_size;
      meta->second_field = false;
      test_ctx_rx[i]->priv = meta;
      ops_rx.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
    }

    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    test_ctx_rx[i]->frame_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->fb_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->width = ops_rx.width;
    test_ctx_rx[i]->uframe_size = ops_rx.uframe_size;
    st20_get_pgroup(ops_rx.fmt, &test_ctx_rx[i]->st20_pg);
    memcpy(test_ctx_rx[i]->shas, test_ctx_tx[i]->shas,
           TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
    st20_set_handle(test_ctx_rx[i], rx_handle[i]);
    if (st.interlaced) {
      digest_thread_rx[i] = std::thread(st20_digest_rx_field_check, test_ctx_rx[i]);
    } else {
      digest_thread_rx[i] = std::thread(st20_digest_rx_frame_check, test_ctx_rx[i]);
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
    EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 2);
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

TEST(St20_rx, uframe_1080p_fps59_94_s1) {
  st20_rx_uframe_test({
      {{ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94, 1920, 1080}},
      ST20_FMT_YUV_422_10BIT,
      true,
      ST_TEST_LEVEL_ALL,
  });
}

TEST(St20_rx, uframe_mix_s2) {
  /* Keeps original single-session workload (session 0 only). */
  st20_rx_uframe_test({
      {{ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94, 1280, 720}},
      ST20_FMT_YUV_422_10BIT,
      true,
      ST_TEST_LEVEL_MANDATORY,
  });
}
