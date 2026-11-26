/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

namespace {

struct St20DigestScenario {
  std::vector<St20StreamSpec> streams;
  bool check_fps = false;
  bool out_of_order = false;
  bool hdr_split = false;
  bool enable_rtcp = false;
  uint16_t rtp_pkt_size = 0;
};

void st20_rx_digest_test(const St20DigestScenario& s) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  const int sessions = static_cast<int>(s.streams.size());

  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  bool has_dma = st_test_dma_available(ctx);

  std::vector<tests_context*> test_ctx_tx(sessions);
  std::vector<tests_context*> test_ctx_rx(sessions);
  std::vector<st20_tx_handle> tx_handle(sessions);
  std::vector<st20_rx_handle> rx_handle(sessions);
  std::vector<std::thread> rtp_thread_tx(sessions);
  std::vector<std::thread> rtp_thread_rx(sessions);
  std::vector<std::thread> sha_check(sessions);

  St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle,
                        &rtp_thread_tx, &rtp_thread_rx);
  guard.add_thread_group(sha_check);
  guard.set_rx_ctx_cleanup([&s](tests_context* rx_c) {
    if (!rx_c) return;
    if (s.streams[rx_c->idx].rx_type == ST20_TYPE_RTP_LEVEL) {
      st20_rx_drain_bufq_free(rx_c);
    } else {
      st20_rx_drain_bufq_put_framebuff(rx_c);
    }
  });

  for (int i = 0; i < sessions; i++) {
    const auto& st = s.streams[i];
    test_ctx_tx[i] = init_test_ctx(ctx, i, TEST_SHA_HIST_NUM, true);
    test_ctx_tx[i]->ready.store(false, std::memory_order_relaxed);

    struct st20_tx_ops ops_tx;
    st20_fill_loopback_tx_ops(ops_tx, test_ctx_tx[i], st, "st20_digest_test",
                              udp_port_for_idx(i, s.hdr_split), s.rtp_pkt_size);
    ops_tx.query_frame_lines_ready = tx_frame_lines_ready;
    if (s.enable_rtcp) {
      ops_tx.flags |= ST20_TX_FLAG_ENABLE_RTCP;
      ops_tx.rtcp.buffer_size = 1024;
    }

    if (s.out_of_order) {
      test_ctx_tx[i]->ooo_mapping =
          (int*)st_test_zmalloc(sizeof(int) * test_ctx_tx[i]->total_pkts_in_frame);
      ASSERT_TRUE(test_ctx_tx[i]->ooo_mapping != NULL);
      tx_video_build_ooo_mapping(test_ctx_tx[i]);
    }
    test_ctx_tx[i]->out_of_order_pkt = s.out_of_order;

    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    st20_init_tx_digest_frames(m_handle, test_ctx_tx[i], tx_handle[i], st);
    if (::testing::Test::HasFatalFailure()) return;
  }

  for (int i = 0; i < sessions; i++) {
    const auto& st = s.streams[i];
    test_ctx_rx[i] = init_test_ctx(ctx, i, 3, true);

    struct st20_rx_ops ops_rx;
    st20_fill_loopback_rx_ops(ops_rx, test_ctx_rx[i], st, "st20_digest_test",
                              udp_port_for_idx(i, s.hdr_split));
    ops_rx.notify_frame_ready =
        st.interlaced ? st20_digest_rx_field_ready : st20_digest_rx_frame_ready;
    ops_rx.rtp_ring_size = 1024 * 2;
    if (s.hdr_split) ops_rx.flags |= ST20_RX_FLAG_HDR_SPLIT;
    if (s.enable_rtcp) {
      ops_rx.flags |= ST20_RX_FLAG_ENABLE_RTCP | ST20_RX_FLAG_SIMULATE_PKT_LOSS;
      ops_rx.rtcp.nack_interval_us = 250;
      ops_rx.rtcp.seq_bitmap_size = 32;
      ops_rx.rtcp.seq_skip_window = 10;
      ops_rx.rtcp.burst_loss_max = 32;
      ops_rx.rtcp.sim_loss_rate = 0.001f;
    }
    st20_init_rx_slice_meta(test_ctx_rx[i], ops_rx, st, test_ctx_tx[i]->frame_size);
    if (::testing::Test::HasFatalFailure()) return;

    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    test_ctx_rx[i]->frame_time = (double)NS_PER_S / st_frame_rate(ops_rx.fps);
    dbg("%s(%d), frame_time %f\n", __func__, i, test_ctx_rx[i]->frame_time);
    st20_bind_rx_digest_ctx(test_ctx_rx[i], test_ctx_tx[i], rx_handle[i], st);
    if (st.rx_type == ST20_TYPE_RTP_LEVEL) {
      rtp_thread_rx[i] = std::thread(rx_get_packet, test_ctx_rx[i]);
      sha_check[i] = std::thread(sha_frame_check, test_ctx_rx[i]);
    } else {
      rtp_thread_rx[i] = std::thread(
          st.interlaced ? st20_digest_rx_field_check : st20_digest_rx_frame_check,
          test_ctx_rx[i]);
    }

    st20_verify_rx_dma_and_queue(rx_handle[i], has_dma, st.rx_type);
  }

  /* Auto-start can run tasklets during setup. Publish TX only after every RX is ready. */
  st20_publish_tx_sessions(test_ctx_tx, tx_handle, s.streams, &rtp_thread_tx);

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

  std::vector<struct st20_rx_user_stats> rx_stats(sessions);
  if (s.enable_rtcp) {
    for (int i = 0; i < sessions; i++) {
      ASSERT_GE(st20_rx_get_session_stats(rx_handle[i], &rx_stats[i]), 0);
    }
  }

  /* Auto-start makes mtl_stop() a no-op. Release sessions to freeze the counters. */
  st20_stop_tx_ready(test_ctx_tx);
  guard.release_sessions();

  for (int i = 0; i < sessions; i++) {
    const auto& st = s.streams[i];
    const double expect_fps = st_frame_rate(st.fps);
    int64_t start_delta_ns =
        (int64_t)test_ctx_rx[i]->start_time - (int64_t)test_ctx_tx[i]->start_time;
    info(
        "%s, session %d first RX minus first TX %.3f ms, sent %d received %d "
        "incomplete %d\n",
        __func__, i, (double)start_delta_ns / NS_PER_MS, test_ctx_tx[i]->fb_send,
        test_ctx_rx[i]->fb_rec, test_ctx_rx[i]->incomplete_frame_cnt);
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    EXPECT_GT(test_ctx_rx[i]->check_sha_frame_cnt, 0);
    if (s.enable_rtcp) {
      EXPECT_GT(rx_stats[i].stat_pkts_simulate_loss, 0u);
      EXPECT_GT(rx_stats[i].stat_pkts_retransmit, 0u);
      EXPECT_LT(rx_stats[i].stat_frames_incomplete, 8u);
    }
    if (st.rx_type == ST20_TYPE_SLICE_LEVEL)
      EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 2 * 8);
    else
      EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 4);
    if (s.check_fps && !s.enable_rtcp) {
      EXPECT_LT(test_ctx_rx[i]->meta_timing_fail_cnt, 4);
    }
    EXPECT_EQ(test_ctx_rx[i]->incomplete_slice_cnt, 0);
    EXPECT_EQ(mtl_test::relaxed_load(&test_ctx_rx[i]->rx_field_fail_cnt), 0);
    if (st.rx_type == ST20_TYPE_FRAME_LEVEL)
      EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);
    else
      EXPECT_LE(test_ctx_rx[i]->sha_fail_cnt, 2);
    info("%s, session %d fb_rec %d framerate %f fb_send %d\n", __func__, i,
         test_ctx_rx[i]->fb_rec, res.rate[i], test_ctx_tx[i]->fb_send);
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

/* RTP-level TX/RX of a session at the 1452-byte rtp_pkt_size limit. Whole 5-byte
 * pgroups cap its largest pkt at 1451 bytes, the session's mbufs and rate are 1452. */
TEST(St20_rx, digest_rtp_1080p_fps59_94_s1_pkt1452) {
  St20DigestScenario sc;
  sc.streams = {{ST20_TYPE_RTP_LEVEL, ST20_TYPE_RTP_LEVEL, ST20_PACKING_BPM,
                 ST_FPS_P59_94, 1920, 1080, false}};
  sc.check_fps = true;
  sc.rtp_pkt_size = 1452;
  st20_rx_digest_test(sc);
}

TEST(St20_rx, digest_frame_rtp_s3) {
  St20DigestScenario sc;
  sc.streams = {
      {ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P59_94,
       1920, 1080, false},
      {ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1080,
       720, false},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P29_97,
       1920 * 2, 1080 * 2, false},
  };
  st20_rx_digest_test(sc);
}

TEST(St20_rx, digest_frame_s4_8bit) {
  St20DigestScenario sc;
  sc.streams = {
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P59_94,
       1920, 1080, false, ST20_FMT_YUV_422_8BIT},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P50,
       1920, 1080, false, ST20_FMT_YUV_420_8BIT},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
       1920, 1080, false, ST20_FMT_YUV_444_8BIT},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P119_88,
       1280, 720, false, ST20_FMT_RGB_8BIT},
  };
  st20_rx_digest_test(sc);
}

TEST(St20_rx, digest_frame_s4_10bit) {
  St20DigestScenario sc;
  sc.streams = {
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P59_94,
       1920, 1080, false, ST20_FMT_YUV_422_10BIT},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1920,
       1080, false, ST20_FMT_YUV_420_10BIT},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
       1920, 1080, false, ST20_FMT_YUV_444_10BIT},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P50, 1280,
       720, false, ST20_FMT_RGB_10BIT},
  };
  st20_rx_digest_test(sc);
}

TEST(St20_rx, digest_rtp_s3) {
  St20DigestScenario sc;
  sc.streams = {
      {ST20_TYPE_RTP_LEVEL, ST20_TYPE_RTP_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P59_94, 1920,
       1080, false},
      {ST20_TYPE_RTP_LEVEL, ST20_TYPE_RTP_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1920, 1080,
       false},
      {ST20_TYPE_RTP_LEVEL, ST20_TYPE_RTP_LEVEL, ST20_PACKING_BPM, ST_FPS_P29_97, 1920,
       1080, false},
  };
  st20_rx_digest_test(sc);
}

TEST(St20_rx, digest_ooo_frame_s3) {
  St20DigestScenario sc;
  sc.streams = {
      {ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P50, 1920,
       1080, false},
      {ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1280,
       720, false},
      {ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94, 1280,
       720, false},
  };
  sc.out_of_order = true;
  st20_rx_digest_test(sc);
}

TEST(St20_rx, digest_slice_s3) {
  St20DigestScenario sc;
  sc.streams = {
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_SLICE_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P50,
       1920, 1080, false},
      {ST20_TYPE_SLICE_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1280,
       720, false},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_SLICE_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
       1280, 720, false},
  };
  st20_rx_digest_test(sc);
}

TEST(St20_rx, digest20_field_slice_s3) {
  St20DigestScenario sc;
  sc.streams = {
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_SLICE_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P50,
       1920, 1080, true},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1280,
       720, true},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_SLICE_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
       1280, 720, true},
  };
  st20_rx_digest_test(sc);
}

TEST(St20_rx, digest_ooo_slice_s3) {
  St20DigestScenario sc;
  sc.streams = {
      {ST20_TYPE_RTP_LEVEL, ST20_TYPE_SLICE_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P50, 1920,
       1080, false},
      {ST20_TYPE_RTP_LEVEL, ST20_TYPE_SLICE_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1280,
       720, false},
      {ST20_TYPE_RTP_LEVEL, ST20_TYPE_SLICE_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94, 1280,
       720, false},
  };
  sc.out_of_order = true;
  st20_rx_digest_test(sc);
}

TEST(St20_rx, digest_frame_4096_2160_fps59_94_12bit_yuv444_s1) {
  St20DigestScenario sc;
  sc.streams = {{ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM,
                 ST_FPS_P59_94, 4096, 2160, false, ST20_FMT_YUV_444_12BIT}};
  st20_rx_digest_test(sc);
}

TEST(St20_rx, digest_ooo_slice_4320p) {
  MTL_TEST_REQUIRE_DMA(st_test_ctx());
  St20DigestScenario sc;
  sc.streams = {{ST20_TYPE_RTP_LEVEL, ST20_TYPE_SLICE_LEVEL, ST20_PACKING_BPM, ST_FPS_P25,
                 1920 * 4, 1080 * 4, false}};
  sc.out_of_order = true;
  st20_rx_digest_test(sc);
}

TEST(St20_rx, digest_hdr_split) {
  if (!st_test_ctx()->hdr_split) {
    GTEST_SKIP() << "Header split is not enabled";
  }
  St20DigestScenario sc;
  sc.streams = {{ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM,
                 ST_FPS_P59_94, 1920, 1080, false}};
  sc.hdr_split = true;
  st20_rx_digest_test(sc);
}

TEST(St20_rx, digest_rtcp_s3) {
  /* no fps check */
  St20DigestScenario sc;
  sc.streams = {
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P59_94,
       1920, 1080, true},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1920,
       1080, false},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P29_97,
       1280, 720, false},
  };
  sc.enable_rtcp = true;
  st20_rx_digest_test(sc);
}
