/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <algorithm>

#include "st20_common.h"

namespace {

struct St20ExtFrameStream {
  enum st20_packing packing;
  enum st_fps fps;
  int width;
  int height;
  bool interlaced = false;
  enum st20_fmt fmt = ST20_FMT_YUV_422_10BIT;
};

struct St20ExtFrameScenario {
  std::vector<St20ExtFrameStream> streams;
  bool check_fps = true;
  bool dynamic = false;
};

int rx_query_ext_frame(void* priv, st20_ext_frame* ext_frame,
                       struct st20_rx_frame_meta* /*meta*/) {
  auto ctx = (tests_context*)priv;
  if (!st20_get_handle(ctx)) return -EIO; /* not ready */

  const int max_slots =
      static_cast<int>(sizeof(ctx->ext_fb_in_use) / sizeof(ctx->ext_fb_in_use[0]));
  const int cnt = std::min(ctx->fb_cnt, max_slots);
  if (cnt <= 0) return -EIO;

  const int start = ctx->ext_idx % cnt;
  for (int attempt = 0; attempt < cnt; attempt++) {
    const int i = (start + attempt) % cnt;
    bool expected = false;
    if (__atomic_compare_exchange_n(&ctx->ext_fb_in_use[i], &expected, true, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      ext_frame->buf_addr = ctx->ext_frames[i].buf_addr;
      ext_frame->buf_iova = ctx->ext_frames[i].buf_iova;
      ext_frame->buf_len = ctx->ext_frames[i].buf_len;
      ext_frame->opaque = &ctx->ext_fb_in_use[i];
      dbg("%s(%d), set ext frame %d(%p) to use\n", __func__, ctx->idx, i,
          ext_frame->buf_addr);
      ctx->ext_idx = (i + 1) % cnt;
      return 0;
    }
  }

  err("%s(%d), all %d ext frames in use\n", __func__, ctx->idx, cnt);
  return -EIO;
}

void st20_tx_ext_frame_rx_digest_test(const St20ExtFrameScenario& s) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  const int sessions = static_cast<int>(s.streams.size());

  MTL_TEST_REQUIRE_IOVA_VA(ctx);
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  bool has_dma = st_test_dma_available(ctx);

  std::vector<tests_context*> test_ctx_tx(sessions);
  std::vector<tests_context*> test_ctx_rx(sessions);
  std::vector<st20_tx_handle> tx_handle(sessions);
  std::vector<st20_rx_handle> rx_handle(sessions);
  std::vector<double> expect_framerate(sessions);
  std::vector<uint64_t> tx_created_ns(sessions);
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
    init_single_port_tx(ops_tx, test_ctx_tx[i], "st20_ext_frame_digest_test",
                        udp_port_for_idx(i));
    ops_tx.packing = st.packing;
    ops_tx.type = ST20_TYPE_FRAME_LEVEL;
    ops_tx.width = st.width;
    ops_tx.height = st.height;
    ops_tx.interlaced = st.interlaced;
    ops_tx.fps = st.fps;
    ops_tx.fmt = st.fmt;
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_tx.flags |= ST20_TX_FLAG_EXT_FRAME;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame =
        st.interlaced ? tx_next_ext_video_field : tx_next_ext_video_frame;
    ops_tx.notify_frame_done = tx_notify_ext_frame_done;

    tx_created_ns[i] = st_test_get_monotonic_time();
    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    /* sha calculate */
    struct st20_pgroup st20_pg;
    st20_get_pgroup(ops_tx.fmt, &st20_pg);
    size_t frame_size = ops_tx.width * ops_tx.height * st20_pg.size / st20_pg.coverage;
    if (st.interlaced) frame_size = frame_size >> 1;
    EXPECT_EQ(st20_tx_get_framebuffer_size(tx_handle[i]), frame_size);
    EXPECT_EQ(st20_tx_get_framebuffer_count(tx_handle[i]), test_ctx_tx[i]->fb_cnt);

    test_ctx_tx[i]->frame_size = frame_size;
    test_ctx_tx[i]->height = ops_tx.height;
    test_ctx_tx[i]->stride = ops_tx.width / st20_pg.coverage * st20_pg.size;
    ASSERT_TRUE(st20_alloc_ext_frames(m_handle, test_ctx_tx[i], frame_size));

    for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
      uint8_t* fb = (uint8_t*)test_ctx_tx[i]->ext_fb + frame * frame_size;
      ASSERT_TRUE(fb != NULL);
      st_test_rand_data(fb, frame_size, frame);
      unsigned char* result = test_ctx_tx[i]->shas[frame];
      SHA256((unsigned char*)fb, frame_size, result);
      test_sha_dump("st20_rx", result);
    }
  }

  for (int i = 0; i < sessions; i++) {
    const auto& st = s.streams[i];
    test_ctx_rx[i] = init_test_ctx(ctx, i, 3, true);
    ASSERT_TRUE(test_ctx_rx[i] != NULL);
    test_ctx_rx[i]->stop = false;

    size_t rx_ext_frame_size = st20_frame_size(st.fmt, st.width, st.height);
    ASSERT_TRUE(st20_alloc_ext_frames(m_handle, test_ctx_rx[i], rx_ext_frame_size));

    struct st20_rx_ops ops_rx;
    init_single_port_rx(ops_rx, test_ctx_rx[i], "st20_ext_frame_digest_test",
                        udp_port_for_idx(i));
    ops_rx.type = ST20_TYPE_FRAME_LEVEL;
    ops_rx.width = st.width;
    ops_rx.height = st.height;
    ops_rx.fps = st.fps;
    ops_rx.fmt = st.fmt;
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.interlaced = st.interlaced;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready =
        st.interlaced ? st20_digest_rx_field_ready : st20_digest_rx_frame_ready;
    if (s.dynamic) {
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
    st20_set_handle(test_ctx_rx[i], rx_handle[i]);

    if (st.interlaced) {
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
    EXPECT_GE(st20_rx_get_queue_meta(rx_handle[i], &meta), 0);
  }

  /* Auto-start can run tasklets during setup. Publish TX only after every RX is ready. */
  uint64_t tx_release_ns = st_test_get_monotonic_time();
  for (int i = 0; i < sessions; i++) {
    info("%s, session %d TX gated for %.3f ms until RX ready\n", __func__, i,
         (double)(tx_release_ns - tx_created_ns[i]) / NS_PER_MS);
    st20_set_handle(test_ctx_tx[i], tx_handle[i]);
    test_ctx_tx[i]->ready.store(true, std::memory_order_release);
  }

  int ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  guard.set_started(ret >= 0);

  auto res = st20_wait_rx_window(test_ctx_rx, s.check_fps, true);
  EXPECT_TRUE(res.warmed_up);
  EXPECT_TRUE(res.complete);

  /* Auto-start makes mtl_stop() a no-op. Release sessions to freeze the counters. */
  for (int i = 0; i < sessions; i++)
    test_ctx_tx[i]->ready.store(false, std::memory_order_release);
  guard.release_sessions();

  for (int i = 0; i < sessions; i++) {
    int64_t start_delta_ns =
        (int64_t)test_ctx_rx[i]->start_time - (int64_t)test_ctx_tx[i]->start_time;
    info(
        "%s, session %d first RX minus first TX %.3f ms, sent %d received %d "
        "incomplete %d\n",
        __func__, i, (double)start_delta_ns / NS_PER_MS, test_ctx_tx[i]->fb_send,
        test_ctx_rx[i]->fb_rec, test_ctx_rx[i]->incomplete_frame_cnt);
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    EXPECT_GT(test_ctx_rx[i]->check_sha_frame_cnt, 0);

    EXPECT_LE(test_ctx_rx[i]->incomplete_frame_cnt, 4);
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

TEST(St20_rx, ext_frame_s3) {
  St20ExtFrameScenario sc;
  sc.streams = {
      {ST20_PACKING_BPM, ST_FPS_P59_94, 1280, 720, true, ST20_FMT_YUV_422_10BIT},
      {ST20_PACKING_BPM, ST_FPS_P50, 1920, 1080, true, ST20_FMT_YUV_422_10BIT},
      {ST20_PACKING_GPM, ST_FPS_P50, 1920, 1080, true, ST20_FMT_YUV_422_10BIT},
  };
  sc.check_fps = true;
  sc.dynamic = false;
  st20_tx_ext_frame_rx_digest_test(sc);
}

TEST(St20_rx, ext_frame_s3_2) {
  St20ExtFrameScenario sc;
  sc.streams = {
      {ST20_PACKING_BPM, ST_FPS_P59_94, 1280, 720, true, ST20_FMT_YUV_422_12BIT},
      {ST20_PACKING_BPM, ST_FPS_P50, 1920, 1080, false, ST20_FMT_YUV_422_10BIT},
      {ST20_PACKING_BPM, ST_FPS_P50, 1920, 1080, true, ST20_FMT_YUV_422_8BIT},
  };
  sc.check_fps = true;
  sc.dynamic = false;
  st20_tx_ext_frame_rx_digest_test(sc);
}

TEST(St20_rx, dynamic_ext_frame_s3) {
  St20ExtFrameScenario sc;
  sc.streams = {
      {ST20_PACKING_BPM, ST_FPS_P59_94, 1280, 720, false, ST20_FMT_YUV_422_10BIT},
      {ST20_PACKING_BPM, ST_FPS_P50, 1280, 720, false, ST20_FMT_YUV_422_10BIT},
      {ST20_PACKING_BPM, ST_FPS_P29_97, 1920, 1080, false, ST20_FMT_YUV_422_10BIT},
  };
  sc.check_fps = true;
  sc.dynamic = true;
  st20_tx_ext_frame_rx_digest_test(sc);
}
