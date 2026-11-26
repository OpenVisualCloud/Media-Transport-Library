/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

namespace {

struct St20DetectScenario {
  std::vector<St20StreamSpec> streams;
  bool user_frame = false;
  bool check_fps = true;
};

int st20_rx_detected(void* priv, const struct st20_detect_meta* meta,
                     struct st20_detect_reply* reply) {
  auto ctx = (tests_context*)priv;
  if (!st20_get_handle(ctx)) return -EIO; /* not ready */

  auto* expect = static_cast<St20RxSliceExpectMeta*>(ctx->priv);
  if (expect && expect->check_detect) {
    if (meta->width != expect->detect_width || meta->height != expect->detect_height ||
        meta->fps != expect->detect_fps || meta->packing != expect->detect_packing ||
        meta->interlaced != expect->detect_interlaced) {
      err("%s(%d), detect mismatch got (%dx%d fps=%d packing=%d interlaced=%d) "
          "expect (%dx%d fps=%d packing=%d interlaced=%d)\n",
          __func__, ctx->idx, meta->width, meta->height, meta->fps, meta->packing,
          meta->interlaced, expect->detect_width, expect->detect_height,
          expect->detect_fps, expect->detect_packing, expect->detect_interlaced);
      __atomic_fetch_add(&expect->detect_fail_cnt, 1, __ATOMIC_RELAXED);
    }
  }
  int slice_lines = meta->height / kSt20SlicesPerFrame;
  __atomic_store_n(&ctx->lines_per_slice, slice_lines, __ATOMIC_RELEASE);
  if (expect && expect->slice_mode) reply->slice_lines = slice_lines;
  if (ctx->uframe_size != 0) {
    /* uframe fmt: yuv422 10bit planar */
    ctx->uframe_size = (size_t)meta->width * meta->height * 2 * sizeof(uint16_t);
    if (meta->interlaced) ctx->uframe_size >>= 1;
    reply->uframe_size = ctx->uframe_size;
    if (expect && expect->slice_mode) expect->meta.uframe_total_size = ctx->uframe_size;
  }
  return 0;
}

int rx_detected_lines(const tests_context* c) {
  return mtl_test::relaxed_load(&c->lines_per_slice);
}

void st20_rx_detect_test(const St20DetectScenario& s) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  const int sessions = static_cast<int>(s.streams.size());

  MTL_TEST_REQUIRE_PORTS(ctx, 2);

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
    st20_fill_loopback_tx_ops(ops_tx, test_ctx_tx[i], st, "st20_detect_test",
                              udp_port_for_idx(i));
    ops_tx.query_frame_lines_ready = tx_frame_lines_ready;
    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    st20_init_tx_digest_frames(
        m_handle, test_ctx_tx[i], tx_handle[i], st,
        s.user_frame ? St20TxBufMode::kUserFrame : St20TxBufMode::kInternal);
    if (::testing::Test::HasFatalFailure()) return;
  }

  for (int i = 0; i < sessions; i++) {
    const auto& st = s.streams[i];
    test_ctx_rx[i] = init_test_ctx(ctx, i, 3, true);

    struct st20_rx_ops ops_rx;
    st20_fill_loopback_rx_ops(ops_rx, test_ctx_rx[i], st, "st20_detect_test",
                              udp_port_for_idx(i));
    /* Intentional defaults so auto-detect must discover actual stream geometry/fps. */
    ops_rx.width = 1920;
    ops_rx.height = 1080;
    ops_rx.fps = ST_FPS_P59_94;
    ops_rx.notify_frame_ready =
        st.interlaced ? st20_digest_rx_field_ready : st20_digest_rx_frame_ready;
    ops_rx.notify_detected = st20_rx_detected;
    ops_rx.flags |= ST20_RX_FLAG_AUTO_DETECT;
    if (s.user_frame) {
      ops_rx.uframe_size = 1;
      ops_rx.uframe_pg_callback = st20_rx_uframe_pg_callback;
    }
    st20_init_rx_slice_meta(test_ctx_rx[i], ops_rx, st, test_ctx_tx[i]->frame_size, 0);
    if (::testing::Test::HasFatalFailure()) return;
    auto* expect = static_cast<St20RxSliceExpectMeta*>(test_ctx_rx[i]->priv);
    if (!expect) {
      expect = (struct St20RxSliceExpectMeta*)st_test_zmalloc(
          sizeof(struct St20RxSliceExpectMeta));
      ASSERT_TRUE(expect != NULL);
      test_ctx_rx[i]->priv = expect;
    }
    expect->check_detect = true;
    expect->detect_width = st.width;
    expect->detect_height = st.height;
    expect->detect_fps = st.fps;
    expect->detect_packing = st.packing;
    expect->detect_interlaced = st.interlaced;

    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    st20_bind_rx_digest_ctx(test_ctx_rx[i], test_ctx_tx[i], rx_handle[i], st,
                            ops_rx.uframe_size);
    rtp_thread_rx[i] = std::thread(
        st.interlaced ? st20_digest_rx_field_check : st20_digest_rx_frame_check,
        test_ctx_rx[i]);
  }

  st20_publish_tx_sessions(test_ctx_tx, tx_handle, s.streams);

  int ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  guard.set_started(ret >= 0);

  bool has_slice_rx = false;
  for (const auto& st : s.streams) {
    if (st.rx_type == ST20_TYPE_SLICE_LEVEL) has_slice_rx = true;
  }

  EXPECT_TRUE(
      mtl_test::wait_for_all(test_ctx_rx, rx_detected_lines, 1, kSt20WarmupTimeoutMs));
  auto res = st20_wait_rx_window(test_ctx_rx, s.check_fps, true,
                                 has_slice_rx ? kSt20MinSliceRxFrames : kSt20MinRxFrames);
  EXPECT_TRUE(res.warmed_up);
  EXPECT_TRUE(res.complete);

  st20_stop_tx_ready(test_ctx_tx);
  guard.release_sessions();

  for (int i = 0; i < sessions; i++) {
    const auto& st = s.streams[i];
    const double expect_fps = st_frame_rate(st.fps);
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    EXPECT_GT(test_ctx_rx[i]->check_sha_frame_cnt, 0);
    if ((st.rx_type == ST20_TYPE_SLICE_LEVEL) && (st.height >= (1080 * 4)))
      EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 2 * 8);
    else
      EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 2 * 2);
    EXPECT_EQ(test_ctx_rx[i]->incomplete_slice_cnt, 0);
    EXPECT_EQ(mtl_test::relaxed_load(&test_ctx_rx[i]->rx_field_fail_cnt), 0);
    auto* expect = static_cast<St20RxSliceExpectMeta*>(test_ctx_rx[i]->priv);
    ASSERT_NE(expect, nullptr);
    EXPECT_EQ(mtl_test::relaxed_load(&expect->detect_fail_cnt), 0);
    if (st.rx_type == ST20_TYPE_FRAME_LEVEL)
      EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);
    else
      EXPECT_LE(test_ctx_rx[i]->sha_fail_cnt, 2);
    info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
         res.rate[i]);
    if (st.rx_type == ST20_TYPE_SLICE_LEVEL) {
      int expect_slice_cnt = test_ctx_rx[i]->fb_rec * kSt20SlicesPerFrame;
      if (st.interlaced) expect_slice_cnt /= 2;
      EXPECT_NEAR(test_ctx_rx[i]->slice_cnt, expect_slice_cnt, expect_slice_cnt * 0.1);
    }
    if (s.check_fps) {
      EXPECT_NEAR(res.rate[i], expect_fps, expect_fps * 0.1);
    }
  }
}

}  // namespace

TEST(St20_rx, detect_uframe_mix_s2) {
  St20DetectScenario sc;
  sc.streams = {
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
       1280, 720, false},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_SLICE_LEVEL, ST20_PACKING_BPM, ST_FPS_P29_97,
       1280, 720, false},
  };
  sc.user_frame = true;
  sc.check_fps = false;
  st20_rx_detect_test(sc);
}

TEST(St20_rx, detect_mix_frame_s3) {
  St20DetectScenario sc;
  sc.streams = {
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
       1280, 720, false},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1920,
       1080, false},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P29_97,
       3840, 2160, true},
  };
  st20_rx_detect_test(sc);
}

TEST(St20_rx, detect_mix_slice_s3) {
  St20DetectScenario sc;
  sc.streams = {
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_SLICE_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
       1280, 720, false},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_SLICE_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1920,
       1080, false},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_SLICE_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P29_97,
       3840, 2160, true},
  };
  st20_rx_detect_test(sc);
}
