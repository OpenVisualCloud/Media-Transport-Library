/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

int st20_rx_uframe_pg_callback(void* /*priv*/, void* frame,
                               struct st20_rx_uframe_pg_meta* meta) {
  uint32_t w = meta->width;
  if (!w || !meta->uframe_total_size) return -EINVAL;
  uint32_t data_h =
      static_cast<uint32_t>(meta->uframe_total_size / (w * 2 * sizeof(uint16_t)));
  if (!data_h) return -EINVAL;
  uint16_t* p10_u16 = (uint16_t*)frame;
  uint16_t* p10_u16_y = p10_u16;
  uint16_t* p10_u16_b = p10_u16 + static_cast<size_t>(w) * data_h;
  uint16_t* p10_u16_r = p10_u16 + static_cast<size_t>(w) * data_h * 3 / 2;
  auto* pg = (struct st20_rfc4175_422_10_pg2_be*)meta->payload;
  uint32_t p10_offset = meta->row_number * w + meta->row_offset;
  p10_u16_y += p10_offset;
  p10_u16_b += p10_offset / 2;
  p10_u16_r += p10_offset / 2;

  st20_rfc4175_422be10_to_yuv422p10le(pg, p10_u16_y, p10_u16_b, p10_u16_r,
                                      meta->pg_cnt * 2, 1);
  return 0;
}

namespace {

void st20_rx_uframe_test(const std::vector<St20StreamSpec>& streams, bool check_fps) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  const int sessions = static_cast<int>(streams.size());

  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  std::vector<tests_context*> test_ctx_tx(sessions);
  std::vector<tests_context*> test_ctx_rx(sessions);
  std::vector<st20_tx_handle> tx_handle(sessions);
  std::vector<st20_rx_handle> rx_handle(sessions);
  std::vector<std::thread> digest_thread_rx(sessions);

  St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle, nullptr,
                        &digest_thread_rx);
  guard.set_rx_ctx_cleanup(st20_rx_drain_bufq_put_framebuff);

  for (int i = 0; i < sessions; i++) {
    const auto& st = streams[i];
    test_ctx_tx[i] = init_test_ctx(ctx, i, TEST_SHA_HIST_NUM, true);
    test_ctx_tx[i]->ready.store(false, std::memory_order_relaxed);

    struct st20_tx_ops ops_tx;
    st20_fill_loopback_tx_ops(ops_tx, test_ctx_tx[i], st, "st20_uframe_test",
                              udp_port_for_idx(i));
    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    st20_init_tx_digest_frames(m_handle, test_ctx_tx[i], tx_handle[i], st,
                               St20TxBufMode::kUserFrame);
    if (::testing::Test::HasFatalFailure()) return;
  }

  for (int i = 0; i < sessions; i++) {
    const auto& st = streams[i];
    test_ctx_rx[i] = init_test_ctx(ctx, i, 3, true);

    struct st20_rx_ops ops_rx;
    st20_fill_loopback_rx_ops(ops_rx, test_ctx_rx[i], st, "st20_uframe_test",
                              udp_port_for_idx(i));
    ops_rx.notify_frame_ready =
        st.interlaced ? st20_digest_rx_field_ready : st20_digest_rx_frame_ready;
    ops_rx.rtp_ring_size = 1024 * 2;
    /* uframe fmt: yuv422 10bit planar */
    ops_rx.uframe_size = (size_t)ops_rx.width * ops_rx.height * 2 * sizeof(uint16_t);
    if (st.interlaced) ops_rx.uframe_size >>= 1;
    ops_rx.uframe_pg_callback = st20_rx_uframe_pg_callback;
    st20_init_rx_slice_meta(test_ctx_rx[i], ops_rx, st, test_ctx_tx[i]->frame_size,
                            ops_rx.uframe_size);
    if (::testing::Test::HasFatalFailure()) return;

    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    st20_bind_rx_digest_ctx(test_ctx_rx[i], test_ctx_tx[i], rx_handle[i], st,
                            ops_rx.uframe_size);
    digest_thread_rx[i] = std::thread(
        st.interlaced ? st20_digest_rx_field_check : st20_digest_rx_frame_check,
        test_ctx_rx[i]);
  }

  st20_publish_tx_sessions(test_ctx_tx, tx_handle, streams);

  int ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  guard.set_started(ret >= 0);

  bool has_slice_rx = false;
  for (const auto& st : streams) {
    if (st.rx_type == ST20_TYPE_SLICE_LEVEL) has_slice_rx = true;
  }
  auto res = st20_wait_rx_window(test_ctx_rx, check_fps, true,
                                 has_slice_rx ? kSt20MinSliceRxFrames : kSt20MinRxFrames);
  EXPECT_TRUE(res.warmed_up);
  EXPECT_TRUE(res.complete);

  st20_stop_tx_ready(test_ctx_tx);
  guard.release_sessions();

  for (int i = 0; i < sessions; i++) {
    const auto& st = streams[i];
    const double expect_fps = st_frame_rate(st.fps);
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
      int expect_slice_cnt = test_ctx_rx[i]->fb_rec * kSt20SlicesPerFrame;
      if (st.interlaced) expect_slice_cnt /= 2;
      EXPECT_NEAR(test_ctx_rx[i]->slice_cnt, expect_slice_cnt, expect_slice_cnt * 0.1);
    }
    if (check_fps) {
      EXPECT_NEAR(res.rate[i], expect_fps, expect_fps * 0.1);
    }
  }
}

}  // namespace

TEST(St20_rx, uframe_mix_s2) {
  st20_rx_uframe_test({{ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM,
                        ST_FPS_P59_94, 1280, 720}},
                      true);
}
