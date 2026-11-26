/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

namespace {

int st20_rx_meta_frame_ready(void* priv, void* frame, struct st20_rx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;
  auto expect_meta = (struct st20_rx_frame_meta*)ctx->priv;

  auto handle = (st20_rx_handle)st20_get_handle(ctx);
  if (!handle) return -EIO;

  __atomic_fetch_add(&ctx->fb_rec, 1, __ATOMIC_RELAXED);
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  if (expect_meta->width != meta->width) ctx->rx_meta_fail_cnt++;
  if (expect_meta->height != meta->height) ctx->rx_meta_fail_cnt++;
  if (expect_meta->fps != meta->fps) ctx->rx_meta_fail_cnt++;
  if (expect_meta->fmt != meta->fmt) ctx->rx_meta_fail_cnt++;
  if (expect_meta->timestamp == meta->timestamp) ctx->rx_meta_fail_cnt++;
  expect_meta->timestamp = meta->timestamp;
  if (!st_is_frame_complete(meta->status)) {
    ctx->incomplete_frame_cnt++;
    if (meta->frame_total_size <= meta->frame_recv_size) ctx->rx_meta_fail_cnt++;
  } else {
    if (meta->frame_total_size != meta->frame_recv_size) ctx->rx_meta_fail_cnt++;
  }
  st20_rx_put_framebuff(handle, frame);

  return 0;
}

void st20_rx_meta_test(const std::vector<St20StreamSpec>& streams) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  const int sessions = static_cast<int>(streams.size());

  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  std::vector<tests_context*> test_ctx_tx(sessions);
  std::vector<tests_context*> test_ctx_rx(sessions);
  std::vector<st20_tx_handle> tx_handle(sessions);
  std::vector<st20_rx_handle> rx_handle(sessions);
  std::vector<std::thread> rtp_thread_tx(sessions);

  St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle,
                        &rtp_thread_tx, nullptr);

  for (int i = 0; i < sessions; i++) {
    test_ctx_tx[i] = init_test_ctx(ctx, i, 3);
    struct st20_tx_ops ops_tx;
    st20_fill_loopback_tx_ops(ops_tx, test_ctx_tx[i], streams[i], "st20_meta_test",
                              udp_port_for_idx(i));
    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);
  }

  for (int i = 0; i < sessions; i++) {
    test_ctx_rx[i] = init_test_ctx(ctx, i, 3);
    struct st20_rx_ops ops_rx;
    st20_fill_loopback_rx_ops(ops_rx, test_ctx_rx[i], streams[i], "st20_meta_test",
                              udp_port_for_idx(i));
    ops_rx.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
    ops_rx.notify_frame_ready = st20_rx_meta_frame_ready;
    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);
    test_ctx_rx[i]->frame_size = test_ctx_tx[i]->frame_size;

    auto* meta =
        (struct st20_rx_frame_meta*)st_test_zmalloc(sizeof(struct st20_rx_frame_meta));
    ASSERT_TRUE(meta != NULL);
    meta->width = ops_rx.width;
    meta->height = ops_rx.height;
    meta->fps = ops_rx.fps;
    meta->fmt = ops_rx.fmt;
    test_ctx_rx[i]->priv = meta;
    st20_set_handle(test_ctx_rx[i], rx_handle[i]);
  }

  /* Publish TX handles and start incomplete-odd-frame feeds after every RX is ready. */
  for (int i = 0; i < sessions; i++) {
    st20_set_handle(test_ctx_tx[i], tx_handle[i]);
    rtp_thread_tx[i] = std::thread(
        [](tests_context* c) { tx_feed_packet_ex(c, /*incomplete_odd_frames=*/true); },
        test_ctx_tx[i]);
  }

  int ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  guard.set_started(ret >= 0);

  /*
   * Warmup=12, then >=35 frames over >=1500 ms (~90 frames at 59.94 fps):
   * <=1.1% FPS edge error and >=102 total frames for the 50% incomplete ratio.
   */
  auto res = st20_wait_rx_window(test_ctx_rx, true, false);
  EXPECT_TRUE(res.warmed_up);
  EXPECT_TRUE(res.complete);
  guard.release_sessions();

  for (int i = 0; i < sessions; i++) {
    const double expect_fps = st_frame_rate(streams[i].fps);
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    float expect_incomplete_frame_cnt = (float)test_ctx_rx[i]->fb_rec / 2;
    EXPECT_NEAR(test_ctx_rx[i]->incomplete_frame_cnt, expect_incomplete_frame_cnt,
                expect_incomplete_frame_cnt * 0.1);
    EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);
    EXPECT_EQ(test_ctx_rx[i]->rx_meta_fail_cnt, 0);
    info("%s, session %d fb_rec %d fb_incomplete %d framerate %f\n", __func__, i,
         test_ctx_rx[i]->fb_rec, test_ctx_rx[i]->incomplete_frame_cnt, res.rate[i]);
    EXPECT_NEAR(res.rate[i], expect_fps, expect_fps * 0.1);
  }
}

}  // namespace

TEST(St20_rx, frame_meta_1080p_fps59_94_s1) {
  st20_rx_meta_test({{ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM,
                      ST_FPS_P59_94, 1920, 1080}});
}
