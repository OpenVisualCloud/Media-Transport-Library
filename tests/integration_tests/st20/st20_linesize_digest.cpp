/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

namespace {

struct St20LinesizeScenario {
  std::vector<St20StreamSpec> streams;
  bool check_fps = true;
  bool ext = false;
};

void st20_linesize_digest_test(const St20LinesizeScenario& s) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  const int sessions = static_cast<int>(s.streams.size());

  if (s.ext) MTL_TEST_REQUIRE_IOVA_VA(ctx);
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  bool has_dma = st_test_dma_available(ctx);

  std::vector<tests_context*> test_ctx_tx(sessions);
  std::vector<tests_context*> test_ctx_rx(sessions);
  std::vector<st20_tx_handle> tx_handle(sessions);
  std::vector<st20_rx_handle> rx_handle(sessions);
  std::vector<std::thread> sha_check(sessions);

  St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle);
  guard.add_thread_group(sha_check);
  guard.set_rx_ctx_cleanup(st20_rx_drain_bufq_put_framebuff);

  for (int i = 0; i < sessions; i++) {
    const auto& st = s.streams[i];
    test_ctx_tx[i] = init_test_ctx(ctx, i, TEST_SHA_HIST_NUM, true);
    test_ctx_tx[i]->ready.store(false, std::memory_order_relaxed);

    struct st20_tx_ops ops_tx;
    st20_fill_loopback_tx_ops(ops_tx, test_ctx_tx[i], st, "st20_linesize_digest_test",
                              udp_port_for_idx(i));
    if (s.ext) {
      ops_tx.flags |= ST20_TX_FLAG_EXT_FRAME;
      ops_tx.get_next_frame = tx_next_ext_video_frame;
      ops_tx.notify_frame_done = tx_notify_ext_frame_done;
    }

    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    st20_init_tx_digest_frames(m_handle, test_ctx_tx[i], tx_handle[i], st,
                               s.ext ? St20TxBufMode::kExtDma : St20TxBufMode::kInternal);
    if (::testing::Test::HasFatalFailure()) return;
  }

  for (int i = 0; i < sessions; i++) {
    const auto& st = s.streams[i];
    test_ctx_rx[i] = init_test_ctx(ctx, i, 3, true);

    if (s.ext) {
      ASSERT_TRUE(
          st20_alloc_dma_ext_frames(m_handle, test_ctx_rx[i], test_ctx_tx[i]->fb_size));
    }

    struct st20_rx_ops ops_rx;
    st20_fill_loopback_rx_ops(ops_rx, test_ctx_rx[i], st, "st20_linesize_digest_test",
                              udp_port_for_idx(i));
    ops_rx.notify_frame_ready =
        st.interlaced ? st20_digest_rx_field_ready : st20_digest_rx_frame_ready;
    if (s.ext) ops_rx.ext_frames = test_ctx_rx[i]->ext_frames;

    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    st20_bind_rx_digest_ctx(test_ctx_rx[i], test_ctx_tx[i], rx_handle[i], st);
    sha_check[i] = std::thread(
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
    EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 2);
    EXPECT_EQ(test_ctx_rx[i]->incomplete_slice_cnt, 0);
    EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);
    EXPECT_EQ(mtl_test::relaxed_load(&test_ctx_rx[i]->rx_field_fail_cnt), 0);
    if (s.ext) {
      auto* ext_stats = static_cast<St20ExtFrameStats*>(test_ctx_rx[i]->priv);
      ASSERT_NE(ext_stats, nullptr);
      EXPECT_EQ(mtl_test::relaxed_load(&ext_stats->ext_addr_mismatch_cnt), 0);
    }
    info("%s, session %d fb_rec %d framerate %f fb_send %d\n", __func__, i,
         test_ctx_rx[i]->fb_rec, res.rate[i], test_ctx_tx[i]->fb_send);
    if (s.check_fps) {
      EXPECT_NEAR(res.rate[i], expect_fps, expect_fps * 0.1);
    }
  }
}

}  // namespace

TEST(St20_rx, linesize_digest_s3) {
  St20LinesizeScenario sc;
  sc.streams = {
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P59_94,
       1280, 720, false, ST20_FMT_YUV_422_10BIT, 4096},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1920,
       1080, true, ST20_FMT_YUV_422_10BIT, 5120},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P50, 1920,
       1080, false, ST20_FMT_YUV_422_10BIT, 8192},
  };
  sc.check_fps = true;
  sc.ext = false;
  st20_linesize_digest_test(sc);
}

TEST(St20_rx, linesize_digest_ext_s3) {
  St20LinesizeScenario sc;
  sc.streams = {
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P59_94,
       1280, 720, true, ST20_FMT_YUV_422_10BIT, 4096},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P50,
       1920, 1080, false, ST20_FMT_YUV_422_10BIT, 5120},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P50,
       1920, 1080, false, ST20_FMT_YUV_422_10BIT, 8192},
  };
  sc.check_fps = true;
  sc.ext = true;
  st20_linesize_digest_test(sc);
}
