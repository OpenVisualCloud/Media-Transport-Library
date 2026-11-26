/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <algorithm>

#include "st20_common.h"

namespace {

struct St20ExtFrameScenario {
  std::vector<St20StreamSpec> streams;
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
      if (auto* ext_stats = static_cast<St20ExtFrameStats*>(ctx->priv)) {
        __atomic_fetch_add(&ext_stats->dynamic_query_cnt, 1, __ATOMIC_RELAXED);
      }
      return 0;
    }
  }

  err("%s(%d), all %d ext frames in use\n", __func__, ctx->idx, cnt);
  return -EIO;
}

void st20_tx_ext_frame_rx_digest_test(const St20ExtFrameScenario& s) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  const int sessions = static_cast<int>(s.streams.size());

  MTL_TEST_REQUIRE_IOVA_VA(ctx);
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  bool has_dma = st_test_dma_available(ctx);

  std::vector<tests_context*> test_ctx_tx(sessions);
  std::vector<tests_context*> test_ctx_rx(sessions);
  std::vector<st20_tx_handle> tx_handle(sessions);
  std::vector<st20_rx_handle> rx_handle(sessions);
  std::vector<std::thread> rtp_thread_tx(sessions);
  std::vector<std::thread> rtp_thread_rx(sessions);

  St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle,
                        &rtp_thread_tx, &rtp_thread_rx);
  guard.set_rx_ctx_cleanup(st20_rx_drain_bufq_put_framebuff);

  for (int i = 0; i < sessions; i++) {
    const auto& st = s.streams[i];
    test_ctx_tx[i] = init_test_ctx(ctx, i, TEST_SHA_HIST_NUM, true);
    test_ctx_tx[i]->ready.store(false, std::memory_order_relaxed);

    struct st20_tx_ops ops_tx;
    st20_fill_loopback_tx_ops(ops_tx, test_ctx_tx[i], st, "st20_ext_frame_digest_test",
                              udp_port_for_idx(i));
    ops_tx.flags |= ST20_TX_FLAG_EXT_FRAME;
    ops_tx.get_next_frame = tx_next_ext_video_frame;
    ops_tx.notify_frame_done = tx_notify_ext_frame_done;

    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    st20_init_tx_digest_frames(m_handle, test_ctx_tx[i], tx_handle[i], st,
                               St20TxBufMode::kExtMap);
    if (::testing::Test::HasFatalFailure()) return;
  }

  for (int i = 0; i < sessions; i++) {
    const auto& st = s.streams[i];
    test_ctx_rx[i] = init_test_ctx(ctx, i, 3, true);

    size_t rx_ext_frame_size = st20_frame_size(st.fmt, st.width, st.height);
    ASSERT_TRUE(st20_alloc_ext_frames(m_handle, test_ctx_rx[i], rx_ext_frame_size));

    struct st20_rx_ops ops_rx;
    st20_fill_loopback_rx_ops(ops_rx, test_ctx_rx[i], st, "st20_ext_frame_digest_test",
                              udp_port_for_idx(i));
    ops_rx.notify_frame_ready =
        st.interlaced ? st20_digest_rx_field_ready : st20_digest_rx_frame_ready;
    if (s.dynamic) {
      auto* ext_stats = static_cast<St20ExtFrameStats*>(test_ctx_rx[i]->priv);
      ASSERT_NE(ext_stats, nullptr);
      ext_stats->dynamic_ext = true;
      ops_rx.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
      ops_rx.query_ext_frame = rx_query_ext_frame;
    } else {
      ops_rx.ext_frames = test_ctx_rx[i]->ext_frames;
    }

    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    st20_bind_rx_digest_ctx(test_ctx_rx[i], test_ctx_tx[i], rx_handle[i], st);
    rtp_thread_rx[i] = std::thread(
        st.interlaced ? st20_digest_rx_field_check : st20_digest_rx_frame_check,
        test_ctx_rx[i]);
    st20_verify_rx_dma_and_queue(rx_handle[i], has_dma, st.rx_type);
  }

  st20_publish_tx_sessions(test_ctx_tx, tx_handle, s.streams);

  int ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  guard.set_started(ret >= 0);

  auto res = st20_wait_rx_window(test_ctx_rx, s.check_fps, true);
  EXPECT_TRUE(res.warmed_up);
  EXPECT_TRUE(res.complete);

  st20_stop_tx_ready(test_ctx_tx);
  guard.release_sessions();

  for (int i = 0; i < sessions; i++) {
    const double expect_fps = st_frame_rate(s.streams[i].fps);
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    EXPECT_GT(test_ctx_rx[i]->check_sha_frame_cnt, 0);
    EXPECT_LE(test_ctx_rx[i]->incomplete_frame_cnt, 4);
    EXPECT_EQ(test_ctx_rx[i]->incomplete_slice_cnt, 0);
    EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);
    EXPECT_EQ(mtl_test::relaxed_load(&test_ctx_rx[i]->rx_field_fail_cnt), 0);
    auto* ext_stats = static_cast<St20ExtFrameStats*>(test_ctx_rx[i]->priv);
    ASSERT_NE(ext_stats, nullptr);
    EXPECT_EQ(mtl_test::relaxed_load(&ext_stats->ext_addr_mismatch_cnt), 0);
    if (s.dynamic) {
      EXPECT_GE(mtl_test::relaxed_load(&ext_stats->dynamic_query_cnt),
                test_ctx_rx[i]->fb_rec);
      EXPECT_EQ(mtl_test::relaxed_load(&ext_stats->dynamic_opaque_null_cnt), 0);
    }
    info("%s, session %d fb_rec %d framerate %f fb_send %d\n", __func__, i,
         test_ctx_rx[i]->fb_rec, res.rate[i], test_ctx_tx[i]->fb_send);
    if (s.check_fps) {
      EXPECT_NEAR(res.rate[i], expect_fps, expect_fps * 0.1);
    }
  }
}

}  // namespace

TEST(St20_rx, ext_frame_s3) {
  St20ExtFrameScenario sc;
  sc.streams = {
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
       1280, 720, true, ST20_FMT_YUV_422_12BIT},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P50, 1920,
       1080, false, ST20_FMT_YUV_422_10BIT},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1920,
       1080, true, ST20_FMT_YUV_422_8BIT},
  };
  sc.check_fps = true;
  sc.dynamic = false;
  st20_tx_ext_frame_rx_digest_test(sc);
}

TEST(St20_rx, dynamic_ext_frame_s3) {
  St20ExtFrameScenario sc;
  sc.streams = {
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
       1280, 720, false, ST20_FMT_YUV_422_10BIT},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P50, 1280,
       720, false, ST20_FMT_YUV_422_10BIT},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P29_97,
       1920, 1080, false, ST20_FMT_YUV_422_10BIT},
  };
  sc.check_fps = true;
  sc.dynamic = true;
  st20_tx_ext_frame_rx_digest_test(sc);
}
