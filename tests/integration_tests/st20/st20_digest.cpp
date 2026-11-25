/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20_common.h"

namespace {

struct St20DigestStream {
  enum st20_type tx_type = ST20_TYPE_FRAME_LEVEL;
  enum st20_type rx_type = ST20_TYPE_FRAME_LEVEL;
  enum st20_packing packing = ST20_PACKING_BPM;
  enum st_fps fps = ST_FPS_P59_94;
  int width = 1920;
  int height = 1080;
  bool interlaced = false;
  enum st20_fmt fmt = ST20_FMT_YUV_422_10BIT;
};

struct St20DigestScenario {
  std::vector<St20DigestStream> streams;
  bool check_fps = false;
  enum st_test_level level = ST_TEST_LEVEL_ALL;
  bool out_of_order = false;
  bool hdr_split = false;
  bool enable_rtcp = false;
  uint16_t rtp_pkt_size = 0;
};

void st20_rx_digest_test(const St20DigestScenario& s) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  const int sessions = static_cast<int>(s.streams.size());
  constexpr int kSlicesPerFrame = 32;

  MTL_TEST_REQUIRE_LEVEL(ctx, s.level);
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
    expect_framerate[i] = st_frame_rate(st.fps);

    test_ctx_tx[i] = init_test_ctx(ctx, i, TEST_SHA_HIST_NUM, true);
    ASSERT_TRUE(test_ctx_tx[i] != NULL);
    test_ctx_tx[i]->ready.store(false, std::memory_order_relaxed);
    test_ctx_tx[i]->stop = false;

    struct st20_tx_ops ops_tx;
    init_single_port_tx(ops_tx, test_ctx_tx[i], "st20_digest_test",
                        udp_port_for_idx(i, s.hdr_split));
    ops_tx.packing = st.packing;
    ops_tx.type = st.tx_type;
    ops_tx.width = st.width;
    ops_tx.height = st.height;
    ops_tx.interlaced = st.interlaced;
    ops_tx.fps = st.fps;
    ops_tx.fmt = st.fmt;
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = st.interlaced ? tx_next_video_field : tx_next_video_frame;
    ops_tx.query_frame_lines_ready = tx_frame_lines_ready;
    if (st.tx_type == ST20_TYPE_RTP_LEVEL) {
      rtp_tx_specific_init(&ops_tx, test_ctx_tx[i], s.rtp_pkt_size);
    }
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

    tx_created_ns[i] = st_test_get_monotonic_time();
    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    /* sha calculate */
    struct st20_pgroup st20_pg;
    st20_get_pgroup(ops_tx.fmt, &st20_pg);
    size_t frame_size = ops_tx.width * ops_tx.height * st20_pg.size / st20_pg.coverage;
    if (st.interlaced) frame_size = frame_size >> 1;
    if (st.tx_type == ST20_TYPE_FRAME_LEVEL) {
      EXPECT_EQ(st20_tx_get_framebuffer_size(tx_handle[i]), frame_size);
      EXPECT_EQ(st20_tx_get_framebuffer_count(tx_handle[i]), test_ctx_tx[i]->fb_cnt);
    }
    test_ctx_tx[i]->frame_size = frame_size;
    test_ctx_tx[i]->slice = (st.tx_type == ST20_TYPE_SLICE_LEVEL);
    test_ctx_tx[i]->lines_per_slice = ops_tx.height / 30;
    test_ctx_tx[i]->height = ops_tx.height;
    test_ctx_tx[i]->stride = ops_tx.width / st20_pg.coverage * st20_pg.size;
    for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
      uint8_t* fb;
      if (st.tx_type == ST20_TYPE_FRAME_LEVEL) {
        fb = (uint8_t*)st20_tx_get_framebuffer(tx_handle[i], frame);
      } else {
        test_ctx_tx[i]->frame_buf[frame] = (uint8_t*)st_test_zmalloc(frame_size);
        fb = test_ctx_tx[i]->frame_buf[frame];
      }
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

    struct st20_rx_ops ops_rx;
    init_single_port_rx(ops_rx, test_ctx_rx[i], "st20_digest_test",
                        udp_port_for_idx(i, s.hdr_split));
    ops_rx.type = st.rx_type;
    ops_rx.width = st.width;
    ops_rx.height = st.height;
    ops_rx.fps = st.fps;
    ops_rx.fmt = st.fmt;
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.interlaced = st.interlaced;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.slice_lines = st.height / kSlicesPerFrame;
    ops_rx.notify_frame_ready =
        st.interlaced ? st20_digest_rx_field_ready : st20_digest_rx_frame_ready;
    ops_rx.notify_slice_ready = st20_digest_rx_slice_ready;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024 * 2;
    ops_rx.flags = ST20_RX_FLAG_DMA_OFFLOAD;
    if (s.hdr_split) ops_rx.flags |= ST20_RX_FLAG_HDR_SPLIT;
    if (s.enable_rtcp) {
      ops_rx.flags |= ST20_RX_FLAG_ENABLE_RTCP | ST20_RX_FLAG_SIMULATE_PKT_LOSS;
      ops_rx.rtcp.nack_interval_us = 250;
      ops_rx.rtcp.seq_bitmap_size = 32;
      ops_rx.rtcp.seq_skip_window = 10;
      ops_rx.rtcp.burst_loss_max = 32;
      ops_rx.rtcp.sim_loss_rate = 0.0001;
    }

    if (st.rx_type == ST20_TYPE_SLICE_LEVEL) {
      auto meta =
          (struct st20_rx_slice_meta*)st_test_zmalloc(sizeof(struct st20_rx_slice_meta));
      ASSERT_TRUE(meta != NULL);
      meta->width = ops_rx.width;
      meta->height = ops_rx.height;
      meta->fps = ops_rx.fps;
      meta->fmt = ops_rx.fmt;
      meta->frame_total_size = test_ctx_tx[i]->frame_size;
      meta->uframe_total_size = 0;
      meta->second_field = false;
      test_ctx_rx[i]->priv = meta;
      ops_rx.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
    }

    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    test_ctx_rx[i]->frame_time = (double)NS_PER_S / st_frame_rate(ops_rx.fps);
    dbg("%s(%d), frame_time %f\n", __func__, i, test_ctx_rx[i]->frame_time);
    test_ctx_rx[i]->frame_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->fb_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->width = ops_rx.width;
    st20_get_pgroup(ops_rx.fmt, &test_ctx_rx[i]->st20_pg);
    memcpy(test_ctx_rx[i]->shas, test_ctx_tx[i]->shas,
           TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
    test_ctx_rx[i]->total_pkts_in_frame = test_ctx_tx[i]->total_pkts_in_frame;
    st20_set_handle(test_ctx_rx[i], rx_handle[i]);
    if (st.rx_type == ST20_TYPE_RTP_LEVEL) {
      rtp_thread_rx[i] = std::thread(rx_get_packet, test_ctx_rx[i]);
      sha_check[i] = std::thread(sha_frame_check, test_ctx_rx[i]);
    } else if (st.interlaced) {
      rtp_thread_rx[i] = std::thread(st20_digest_rx_field_check, test_ctx_rx[i]);
    } else {
      rtp_thread_rx[i] = std::thread(st20_digest_rx_frame_check, test_ctx_rx[i]);
    }

    bool dma_enabled = st20_rx_dma_enabled(rx_handle[i]);
    if (has_dma && (st.rx_type != ST20_TYPE_RTP_LEVEL)) {
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
    if (s.streams[i].tx_type == ST20_TYPE_RTP_LEVEL)
      rtp_thread_tx[i] = std::thread(tx_feed_packet, test_ctx_tx[i]);
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

  /* Auto-start makes mtl_stop() a no-op. Release sessions to freeze the counters. */
  for (int i = 0; i < sessions; i++)
    test_ctx_tx[i]->ready.store(false, std::memory_order_release);
  guard.release_sessions();

  for (int i = 0; i < sessions; i++) {
    const auto& st = s.streams[i];
    int64_t start_delta_ns =
        (int64_t)test_ctx_rx[i]->start_time - (int64_t)test_ctx_tx[i]->start_time;
    info(
        "%s, session %d first RX minus first TX %.3f ms, sent %d received %d "
        "incomplete %d\n",
        __func__, i, (double)start_delta_ns / NS_PER_MS, test_ctx_tx[i]->fb_send,
        test_ctx_rx[i]->fb_rec, test_ctx_rx[i]->incomplete_frame_cnt);
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    EXPECT_GT(test_ctx_rx[i]->check_sha_frame_cnt, 0);
    if (st.rx_type == ST20_TYPE_SLICE_LEVEL)
      EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 2 * 8);
    else
      EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 4);
    if (s.check_fps && !s.enable_rtcp) {
      EXPECT_LT(test_ctx_rx[i]->meta_timing_fail_cnt, 4);
      EXPECT_LT(test_ctx_tx[i]->tx_tmstamp_delta_fail_cnt, 4);
    }
    EXPECT_EQ(test_ctx_rx[i]->incomplete_slice_cnt, 0);
    if (st.rx_type == ST20_TYPE_FRAME_LEVEL)
      EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);
    else
      EXPECT_LE(test_ctx_rx[i]->sha_fail_cnt, 2);
    info("%s, session %d fb_rec %d framerate %f fb_send %d\n", __func__, i,
         test_ctx_rx[i]->fb_rec, res.rate[i], test_ctx_tx[i]->fb_send);
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

TEST(St20_rx, digest_frame_1080p_fps59_94_s1) {
  st20_rx_digest_test({
      {{ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
        1920, 1080, false}},
      true,
  });
}

TEST(St20_rx, digest20_field_1080p_fps59_94_s1) {
  st20_rx_digest_test({
      {{ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
        1920, 1080, true}},
      true,
  });
}

TEST(St20_rx, digest_frame_720p_fps59_94_s1_gpm) {
  st20_rx_digest_test({
      {{ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P59_94,
        1280, 720, false}},
      true,
  });
}

TEST(St20_rx, digest20_field_720p_fps59_94_s1_gpm) {
  st20_rx_digest_test({
      {{ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P59_94,
        1280, 720, true}},
      true,
  });
}

TEST(St20_rx, digest_frame_720p_fps29_97_s1_bpm) {
  st20_rx_digest_test({
      {{ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P29_97,
        1280, 720, false}},
      true,
  });
}

TEST(St20_rx, digest20_field_720p_fps29_97_s1_bpm) {
  st20_rx_digest_test({
      {{ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P29_97,
        1280, 720, true}},
      true,
  });
}

TEST(St20_rx, digest_rtp_1080p_fps59_94_s1) {
  st20_rx_digest_test({
      {{ST20_TYPE_RTP_LEVEL, ST20_TYPE_RTP_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94, 1920,
        1080, false}},
      true,
  });
}

/* RTP-level TX/RX of a session sized like ST 2022-6, 1396-byte pkts (12 RTP + 8 HBRMT +
 * 1376 payload): its largest pkt here is exactly 1396 bytes, 1438 on the wire. */
TEST(St20_rx, digest_rtp_1080p_fps59_94_s1_pkt1396) {
  st20_rx_digest_test({
      {{ST20_TYPE_RTP_LEVEL, ST20_TYPE_RTP_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94, 1920,
        1080, false}},
      true,
      ST_TEST_LEVEL_MANDATORY,
      false,
      false,
      false,
      1396,
  });
}

/* RTP-level TX/RX of a session at the 1452-byte rtp_pkt_size limit. Whole 5-byte
 * pgroups cap its largest pkt at 1451 bytes, the session's mbufs and rate are 1452. */
TEST(St20_rx, digest_rtp_1080p_fps59_94_s1_pkt1452) {
  st20_rx_digest_test({
      {{ST20_TYPE_RTP_LEVEL, ST20_TYPE_RTP_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94, 1920,
        1080, false}},
      true,
      ST_TEST_LEVEL_ALL,
      false,
      false,
      false,
      1452,
  });
}

TEST(St20_rx, digest_frame_720p_fps59_94_s3) {
  st20_rx_digest_test({{
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P59_94,
       1280, 720, false},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P59_94,
       1280, 720, false},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
       1280, 720, false},
  }});
}

TEST(St20_rx, digest20_field_720p_fps59_94_s3) {
  st20_rx_digest_test({{
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P59_94,
       1280, 720, true},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P59_94,
       1280, 720, false},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
       1280, 720, true},
  }});
}

TEST(St20_rx, digest_frame_1080p_fps_mix_s3) {
  st20_rx_digest_test({{
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P59_94,
       1920, 1080, false},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P60, 1920,
       1080, false},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P30, 1920,
       1080, false},
  }});
}

TEST(St20_rx, digest20_field_1080p_fps59_94_s3) {
  st20_rx_digest_test({{
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P59_94,
       1920, 1080, true},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P59_94,
       1920, 1080, true},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
       1920, 1080, true},
  }});
}

TEST(St20_rx, digest_frame_1080p_fps59_94_s4_8bit) {
  st20_rx_digest_test({{
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P59_94,
       1920, 1080, false, ST20_FMT_YUV_422_8BIT},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P50,
       1920, 1080, false, ST20_FMT_YUV_420_8BIT},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
       1920, 1080, false, ST20_FMT_YUV_444_8BIT},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1280,
       720, false, ST20_FMT_RGB_8BIT},
  }});
}

TEST(St20_rx, digest20_field_4320p_fps59_94_s1) {
  st20_rx_digest_test({{
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
       1920 * 4, 1080 * 4, true},
  }});
}

TEST(St20_rx, digest_frame_s3) {
  st20_rx_digest_test({
      {{ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P59_94,
        1920, 1080, false},
       {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1080,
        720, false},
       {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P29_97,
        1920 * 2, 1080 * 2, false}},
      false,
      ST_TEST_LEVEL_MANDATORY,
  });
}

TEST(St20_rx, digest_frame_field_s3) {
  st20_rx_digest_test({{
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P59_94,
       1920, 1080, true},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1080,
       720, true},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P29_97,
       1920 * 2, 1080 * 2, true},
  }});
}

TEST(St20_rx, digest_frame_rtp_s3) {
  st20_rx_digest_test({
      {{ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P59_94,
        1920, 1080, false},
       {ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1080,
        720, false},
       {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P29_97,
        1920 * 2, 1080 * 2, false}},
      false,
      ST_TEST_LEVEL_MANDATORY,
  });
}

TEST(St20_rx, digest_frame_s4_8bit) {
  st20_rx_digest_test({
      {{ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P59_94,
        1920, 1080, false, ST20_FMT_YUV_422_8BIT},
       {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P50,
        1920, 1080, false, ST20_FMT_YUV_420_8BIT},
       {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
        1920, 1080, false, ST20_FMT_YUV_444_8BIT},
       {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P119_88,
        1280, 720, false, ST20_FMT_RGB_8BIT}},
      false,
      ST_TEST_LEVEL_MANDATORY,
  });
}

TEST(St20_rx, digest_frame_s4_10bit) {
  st20_rx_digest_test({
      {{ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P59_94,
        1920, 1080, false, ST20_FMT_YUV_422_10BIT},
       {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1920,
        1080, false, ST20_FMT_YUV_420_10BIT},
       {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
        1920, 1080, false, ST20_FMT_YUV_444_10BIT},
       {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P50, 1280,
        720, false, ST20_FMT_RGB_10BIT}},
      false,
      ST_TEST_LEVEL_MANDATORY,
  });
}

TEST(St20_rx, digest_rtp_s3) {
  st20_rx_digest_test({
      {{ST20_TYPE_RTP_LEVEL, ST20_TYPE_RTP_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P59_94,
        1920, 1080, false},
       {ST20_TYPE_RTP_LEVEL, ST20_TYPE_RTP_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1920,
        1080, false},
       {ST20_TYPE_RTP_LEVEL, ST20_TYPE_RTP_LEVEL, ST20_PACKING_BPM, ST_FPS_P29_97, 1920,
        1080, false}},
      false,
      ST_TEST_LEVEL_MANDATORY,
  });
}

TEST(St20_rx, digest_ooo_frame_s3) {
  st20_rx_digest_test({
      {{ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P50, 1920,
        1080, false},
       {ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1280,
        720, false},
       {ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94, 1280,
        720, false}},
      false,
      ST_TEST_LEVEL_MANDATORY,
      true,
  });
}

TEST(St20_rx, digest_tx_slice_s3) {
  st20_rx_digest_test({{
      {ST20_TYPE_SLICE_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P50,
       1920, 1080, false},
      {ST20_TYPE_SLICE_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1280,
       720, false},
      {ST20_TYPE_SLICE_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
       1280, 720, false},
  }});
}

TEST(St20_rx, digest_slice_s3) {
  st20_rx_digest_test({
      {{ST20_TYPE_FRAME_LEVEL, ST20_TYPE_SLICE_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P50,
        1920, 1080, false},
       {ST20_TYPE_SLICE_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1280,
        720, false},
       {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_SLICE_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
        1280, 720, false}},
      false,
      ST_TEST_LEVEL_MANDATORY,
  });
}

TEST(St20_rx, digest20_field_slice_s3) {
  st20_rx_digest_test({{
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_SLICE_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P50,
       1920, 1080, true},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1280,
       720, true},
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_SLICE_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
       1280, 720, true},
  }});
}

TEST(St20_rx, digest_ooo_slice_s3) {
  st20_rx_digest_test({
      {{ST20_TYPE_RTP_LEVEL, ST20_TYPE_SLICE_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P50, 1920,
        1080, false},
       {ST20_TYPE_RTP_LEVEL, ST20_TYPE_SLICE_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1280,
        720, false},
       {ST20_TYPE_RTP_LEVEL, ST20_TYPE_SLICE_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94, 1280,
        720, false}},
      false,
      ST_TEST_LEVEL_MANDATORY,
      true,
  });
}

TEST(St20_rx, digest_frame_4320p_fps59_94_s1) {
  st20_rx_digest_test({{
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
       1920 * 4, 1080 * 4, false},
  }});
}

TEST(St20_rx, digest_frame_4096_2160_fps59_94_12bit_yuv444_s1) {
  st20_rx_digest_test({
      {{ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
        4096, 2160, false, ST20_FMT_YUV_444_12BIT}},
      false,
      ST_TEST_LEVEL_MANDATORY,
  });
}

TEST(St20_rx, digest_slice_4320p) {
  MTL_TEST_REQUIRE_DMA(st_test_ctx());
  st20_rx_digest_test({{
      {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_SLICE_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
       1920 * 4, 1080 * 4, false},
  }});
}

TEST(St20_rx, digest_ooo_slice_4320p) {
  MTL_TEST_REQUIRE_DMA(st_test_ctx());
  st20_rx_digest_test({
      {{ST20_TYPE_RTP_LEVEL, ST20_TYPE_SLICE_LEVEL, ST20_PACKING_BPM, ST_FPS_P25,
        1920 * 4, 1080 * 4, false}},
      false,
      ST_TEST_LEVEL_MANDATORY,
      true,
  });
}

TEST(St20_rx, digest_hdr_split) {
  if (!st_test_ctx()->hdr_split) {
    GTEST_SKIP() << "Header split is not enabled";
  }
  st20_rx_digest_test({
      {{ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P59_94,
        1920, 1080, false}},
      false,
      ST_TEST_LEVEL_MANDATORY,
      false,
      true,
  });
}

TEST(St20_rx, digest_rtcp_s1) {
  /* check fps */
  st20_rx_digest_test({
      {{ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P50, 1920,
        1080, false}},
      true,
      ST_TEST_LEVEL_ALL,
      false,
      false,
      true,
  });
}

TEST(St20_rx, digest_rtcp_s3) {
  /* no fps check */
  st20_rx_digest_test({
      {{ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM_SL, ST_FPS_P59_94,
        1920, 1080, true},
       {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_GPM, ST_FPS_P50, 1920,
        1080, false},
       {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL, ST20_PACKING_BPM, ST_FPS_P29_97,
        1280, 720, false}},
      false,
      ST_TEST_LEVEL_MANDATORY,
      false,
      false,
      true,
  });
}
