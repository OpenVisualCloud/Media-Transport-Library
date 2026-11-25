/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

namespace {

struct St20LinesizeStream {
  enum st20_packing packing;
  enum st_fps fps;
  int width;
  int height;
  int linesize;
  bool interlaced = false;
  enum st20_fmt fmt = ST20_FMT_YUV_422_10BIT;
};

struct St20LinesizeScenario {
  std::vector<St20LinesizeStream> streams;
  bool check_fps = true;
  enum st_test_level level = ST_TEST_LEVEL_MANDATORY;
  bool ext = false;
};

void st20_linesize_digest_test(const St20LinesizeScenario& s) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  const int sessions = static_cast<int>(s.streams.size());

  MTL_TEST_REQUIRE_LEVEL(ctx, s.level);
  if (s.ext) MTL_TEST_REQUIRE_IOVA_VA(ctx);
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  bool has_dma = st_test_dma_available(ctx);

  std::vector<tests_context*> test_ctx_tx(sessions);
  std::vector<tests_context*> test_ctx_rx(sessions);
  std::vector<st20_tx_handle> tx_handle(sessions);
  std::vector<st20_rx_handle> rx_handle(sessions);
  std::vector<double> expect_framerate(sessions);
  std::vector<std::thread> sha_check(sessions);

  St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle);
  guard.add_thread_group(sha_check);
  guard.set_rx_ctx_cleanup(st20_rx_drain_bufq_put_framebuff);

  for (int i = 0; i < sessions; i++) {
    const auto& st = s.streams[i];
    expect_framerate[i] = st_frame_rate(st.fps);

    test_ctx_tx[i] = init_test_ctx(ctx, i, TEST_SHA_HIST_NUM, true);
    ASSERT_TRUE(test_ctx_tx[i] != NULL);
    test_ctx_tx[i]->ready.store(false, std::memory_order_relaxed);
    test_ctx_tx[i]->stop = false;

    struct st20_tx_ops ops_tx;
    init_single_port_tx(ops_tx, test_ctx_tx[i], "st20_linesize_digest_test",
                        udp_port_for_idx(i));
    ops_tx.packing = st.packing;
    ops_tx.type = ST20_TYPE_FRAME_LEVEL;
    ops_tx.width = st.width;
    ops_tx.height = st.height;
    ops_tx.linesize = st.linesize;
    ops_tx.interlaced = st.interlaced;
    ops_tx.fps = st.fps;
    ops_tx.fmt = st.fmt;
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    if (s.ext) {
      ops_tx.flags |= ST20_TX_FLAG_EXT_FRAME;
      ops_tx.get_next_frame =
          st.interlaced ? tx_next_ext_video_field : tx_next_ext_video_frame;
      ops_tx.notify_frame_done = tx_notify_ext_frame_done;
    } else {
      ops_tx.get_next_frame = st.interlaced ? tx_next_video_field : tx_next_video_frame;
    }

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

    size_t fb_size = frame_size;
    if (st.linesize > test_ctx_tx[i]->stride) {
      test_ctx_tx[i]->stride = st.linesize;
      fb_size = (size_t)st.linesize * st.height;
      if (st.interlaced) fb_size = fb_size >> 1;
    }
    test_ctx_tx[i]->fb_size = fb_size;
    EXPECT_EQ(st20_tx_get_framebuffer_size(tx_handle[i]), fb_size);
    EXPECT_EQ(st20_tx_get_framebuffer_count(tx_handle[i]), test_ctx_tx[i]->fb_cnt);

    if (s.ext) {
      ASSERT_TRUE(st20_alloc_dma_ext_frames(m_handle, test_ctx_tx[i], fb_size));
    }

    int total_lines = st.interlaced ? st.height / 2 : st.height;
    size_t bytes_per_line = (size_t)ops_tx.width / st20_pg.coverage * st20_pg.size;
    for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
      uint8_t* fb = s.ext ? (uint8_t*)test_ctx_tx[i]->ext_frames[frame].buf_addr
                          : (uint8_t*)st20_tx_get_framebuffer(tx_handle[i], frame);
      ASSERT_TRUE(fb != NULL);
      for (int line = 0; line < total_lines; line++) {
        st_test_rand_data(fb + test_ctx_tx[i]->stride * line, bytes_per_line, frame);
      }
      unsigned char* result = test_ctx_tx[i]->shas[frame];
      SHA256((unsigned char*)fb, fb_size, result);
      test_sha_dump("st20_rx", result);
    }
  }

  for (int i = 0; i < sessions; i++) {
    const auto& st = s.streams[i];
    test_ctx_rx[i] = init_test_ctx(ctx, i, 3, true);
    ASSERT_TRUE(test_ctx_rx[i] != NULL);
    test_ctx_rx[i]->stop = false;
    test_ctx_rx[i]->fb_size = test_ctx_tx[i]->fb_size;
    test_ctx_rx[i]->frame_size = test_ctx_tx[i]->frame_size;

    if (s.ext) {
      ASSERT_TRUE(
          st20_alloc_dma_ext_frames(m_handle, test_ctx_rx[i], test_ctx_rx[i]->fb_size));
    }

    struct st20_rx_ops ops_rx;
    init_single_port_rx(ops_rx, test_ctx_rx[i], "st20_linesize_digest_test",
                        udp_port_for_idx(i));
    ops_rx.type = ST20_TYPE_FRAME_LEVEL;
    ops_rx.width = st.width;
    ops_rx.height = st.height;
    ops_rx.linesize = st.linesize;
    ops_rx.fps = st.fps;
    ops_rx.fmt = st.fmt;
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.interlaced = st.interlaced;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready =
        st.interlaced ? st20_digest_rx_field_ready : st20_digest_rx_frame_ready;
    if (s.ext) ops_rx.ext_frames = test_ctx_rx[i]->ext_frames;

    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    test_ctx_rx[i]->width = ops_rx.width;
    test_ctx_rx[i]->height = ops_rx.height;
    st20_get_pgroup(ops_rx.fmt, &test_ctx_rx[i]->st20_pg);
    memcpy(test_ctx_rx[i]->shas, test_ctx_tx[i]->shas,
           TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
    test_ctx_rx[i]->total_pkts_in_frame = test_ctx_tx[i]->total_pkts_in_frame;
    st20_set_handle(test_ctx_rx[i], rx_handle[i]);

    if (st.interlaced) {
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
    EXPECT_GE(st20_rx_get_queue_meta(rx_handle[i], &meta), 0);
  }

  for (int i = 0; i < sessions; i++) {
    st20_set_handle(test_ctx_tx[i], tx_handle[i]);
    test_ctx_tx[i]->ready.store(true, std::memory_order_release);
  }

  int ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  guard.set_started(ret >= 0);

  auto res = st20_wait_rx_window(test_ctx_rx, s.check_fps, true);
  EXPECT_TRUE(res.warmed_up);
  EXPECT_TRUE(res.complete);

  for (int i = 0; i < sessions; i++)
    test_ctx_tx[i]->ready.store(false, std::memory_order_release);
  guard.release_sessions();

  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    EXPECT_GT(test_ctx_rx[i]->check_sha_frame_cnt, 0);

    EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 2);
    EXPECT_EQ(test_ctx_rx[i]->incomplete_slice_cnt, 0);
    EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);
    info("%s, session %d fb_rec %d framerate %f fb_send %d\n", __func__, i,
         test_ctx_rx[i]->fb_rec, res.rate[i], test_ctx_tx[i]->fb_send);
    if (s.check_fps) {
      EXPECT_NEAR(res.rate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    }
  }
}

}  // namespace

TEST(St20_rx, linesize_digest_s3) {
  st20_linesize_digest_test({
      {{ST20_PACKING_GPM_SL, ST_FPS_P59_94, 1280, 720, 4096, false},
       {ST20_PACKING_GPM_SL, ST_FPS_P50, 1920, 1080, 5120, true},
       {ST20_PACKING_GPM_SL, ST_FPS_P50, 1920, 1080, 8192, false}},
  });
}

TEST(St20_rx, linesize_digest_crosslines_s3) {
  st20_linesize_digest_test({
      {{ST20_PACKING_BPM, ST_FPS_P59_94, 1280, 720, 4096, true},
       {ST20_PACKING_GPM, ST_FPS_P50, 1920, 1080, 5120, false},
       {ST20_PACKING_BPM, ST_FPS_P50, 1920, 1080, 8192, false}},
  });
}

TEST(St20_rx, linesize_digest_ext_s3) {
  st20_linesize_digest_test({
      {{ST20_PACKING_GPM_SL, ST_FPS_P59_94, 1280, 720, 4096, true},
       {ST20_PACKING_GPM_SL, ST_FPS_P50, 1920, 1080, 5120, false},
       {ST20_PACKING_GPM_SL, ST_FPS_P50, 1920, 1080, 8192, false}},
      true,
      ST_TEST_LEVEL_MANDATORY,
      true,
  });
}
