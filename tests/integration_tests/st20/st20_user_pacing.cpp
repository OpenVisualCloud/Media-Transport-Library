/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <algorithm>
#include <cinttypes>
#include <cstdlib>

#include "st20_common.h"

namespace {

struct St20UserPacingStream {
  int width;
  int height;
  enum st20_fmt fmt;
  bool user_pacing;
  bool user_timestamp;
};

void st20_tx_user_pacing_test(const std::vector<St20UserPacingStream>& streams,
                              enum st_test_level level) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  const int sessions = static_cast<int>(streams.size());

  MTL_TEST_REQUIRE_LEVEL(ctx, level);
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  std::vector<tests_context*> test_ctx_tx(sessions);
  std::vector<tests_context*> test_ctx_rx(sessions);
  std::vector<st20_tx_handle> tx_handle(sessions);
  std::vector<st20_rx_handle> rx_handle(sessions);
  std::vector<double> expect_framerate(sessions);
  std::vector<std::thread> sha_thread_rx(sessions);

  St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle, nullptr,
                        &sha_thread_rx);
  guard.set_rx_ctx_cleanup(st20_rx_drain_bufq_put_framebuff);

  enum st_fps fps = ST_FPS_P59_94;

  for (int i = 0; i < sessions; i++) {
    const auto& st = streams[i];
    expect_framerate[i] = st.user_pacing ? (st_frame_rate(fps) / 2) : st_frame_rate(fps);
    test_ctx_tx[i] = init_test_ctx(ctx, i, TEST_SHA_HIST_NUM, true);
    ASSERT_TRUE(test_ctx_tx[i] != NULL);
    test_ctx_tx[i]->user_pacing = st.user_pacing;
    test_ctx_tx[i]->user_timestamp = st.user_timestamp;
    test_ctx_tx[i]->frame_time = (double)NS_PER_S / st_frame_rate(fps);

    struct st20_tx_ops ops_tx;
    init_single_port_tx(ops_tx, test_ctx_tx[i], "st20_timestamp_test",
                        udp_port_for_idx(i));
    ops_tx.packing = ST20_PACKING_BPM;
    ops_tx.type = ST20_TYPE_FRAME_LEVEL;
    ops_tx.width = st.width;
    ops_tx.height = st.height;
    ops_tx.interlaced = false;
    ops_tx.fps = fps;
    ops_tx.fmt = st.fmt;
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = tx_next_video_frame_timestamp;
    ops_tx.notify_frame_done = tx_notify_timestamp_frame_done;
    if (st.user_pacing) ops_tx.flags |= ST20_TX_FLAG_USER_PACING;
    if (st.user_timestamp) ops_tx.flags |= ST20_TX_FLAG_USER_TIMESTAMP;

    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    /* sha calculate */
    struct st20_pgroup st20_pg;
    st20_get_pgroup(ops_tx.fmt, &st20_pg);
    size_t frame_size = ops_tx.width * ops_tx.height * st20_pg.size / st20_pg.coverage;
    test_ctx_tx[i]->frame_size = frame_size;
    test_ctx_tx[i]->height = ops_tx.height;
    test_ctx_tx[i]->stride = ops_tx.width / st20_pg.coverage * st20_pg.size;
    for (int frame = 0; frame < TEST_SHA_HIST_NUM; frame++) {
      uint8_t* fb = (uint8_t*)st20_tx_get_framebuffer(tx_handle[i], frame);
      ASSERT_TRUE(fb != NULL);
      st_test_rand_data(fb, frame_size, frame);
      unsigned char* result = test_ctx_tx[i]->shas[frame];
      SHA256((unsigned char*)fb, frame_size, result);
      test_sha_dump("st20_rx", result);
    }
  }

  for (int i = 0; i < sessions; i++) {
    const auto& st = streams[i];
    test_ctx_rx[i] = init_test_ctx(ctx, i, 3, true);
    ASSERT_TRUE(test_ctx_rx[i] != NULL);
    test_ctx_rx[i]->user_pacing = st.user_pacing;
    test_ctx_rx[i]->user_timestamp = st.user_timestamp;

    struct st20_rx_ops ops_rx;
    init_single_port_rx(ops_rx, test_ctx_rx[i], "st20_timestamp_test",
                        udp_port_for_idx(i));
    ops_rx.type = ST20_TYPE_FRAME_LEVEL;
    ops_rx.width = st.width;
    ops_rx.height = st.height;
    ops_rx.fps = fps;
    ops_rx.fmt = st.fmt;
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready = st20_digest_rx_frame_ready;

    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    test_ctx_rx[i]->frame_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->fb_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->width = ops_rx.width;
    st20_get_pgroup(ops_rx.fmt, &test_ctx_rx[i]->st20_pg);
    memcpy(test_ctx_rx[i]->shas, test_ctx_tx[i]->shas,
           TEST_SHA_HIST_NUM * SHA256_DIGEST_LENGTH);
    st20_set_handle(test_ctx_rx[i], rx_handle[i]);
    test_ctx_rx[i]->stop = false;
    sha_thread_rx[i] = std::thread(st20_digest_rx_frame_check, test_ctx_rx[i]);
  }

  /* Publish TX handles only after every RX session is ready. */
  for (int i = 0; i < sessions; i++) {
    st20_set_handle(test_ctx_tx[i], tx_handle[i]);
  }

  int ret = mtl_start(m_handle);
  EXPECT_GE(ret, 0);
  guard.set_started(ret >= 0);

  /*
   * Measure TX and RX FPS over one >=1000 ms steady-state window:
   * warmup=3 absorbs RL pacing training, >=25 frames (~30 at half-rate 29.97 fps)
   * keeps edge error <=3.3% vs +/-10% tolerance.
   */
  auto res = mtl_test::measure_pipeline_window(
      test_ctx_tx, test_ctx_rx, kSt20WarmupFrames, kSt20MinRateFrames,
      kSt20MinRateWindowMs, kSt20WarmupTimeoutMs, kSt20WindowTimeoutMs);
  EXPECT_TRUE(res.warmed_up);
  EXPECT_TRUE(res.complete);
  EXPECT_TRUE(mtl_test::wait_for_all(test_ctx_rx, mtl_test::sha_checked_frames,
                                     kSt20MinShaFrames, kSt20EventTimeoutMs));

  guard.release_sessions();

  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    EXPECT_GT(test_ctx_rx[i]->check_sha_frame_cnt, 0);
    EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 2);
    EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);

    info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
         res.rx_fps[i]);
    info("%s, session %d fb_send %d framerate %f\n", __func__, i, test_ctx_tx[i]->fb_send,
         res.tx_fps[i]);

    EXPECT_NEAR(res.tx_fps[i], expect_framerate[i], expect_framerate[i] * 0.1);
    EXPECT_NEAR(res.rx_fps[i], expect_framerate[i], expect_framerate[i] * 0.1);
  }
}

/* first-packet arrival minus target TAI of each complete frame, one session */
struct RtpExactPacingRx {
  std::mutex mtx;
  std::vector<int64_t> deltas;
};
RtpExactPacingRx rtp_exact_pacing_rx;

/* frame k carries RTP timestamp k + 1, the feed starting at 1 */
uint64_t rtp_exact_target_tai(tests_context* s, uint32_t rtp_ts) {
  return s->ptp_time_first_frame + (uint64_t)((rtp_ts - 1) * s->frame_time);
}

void tx_feed_packet_tai(void* args) {
  auto ctx = (tests_context*)args;
  auto handle = (st20_tx_handle)st20_get_handle(ctx);
  void* usrptr = nullptr;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  while (!ctx->stop) {
    void* mbuf = st20_tx_get_mbuf(handle, &usrptr);
    if (!mbuf) {
      lck.lock();
      mbuf = st20_tx_get_mbuf(handle, &usrptr);
      if (!mbuf) {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
      lck.unlock();
    }
    uint64_t tai = ctx->pkt_idx ? 0 : rtp_exact_target_tai(ctx, ctx->rtp_tmstamp);
    tx_video_build_rtp_packet(ctx, (struct st20_rfc4175_rtp_hdr*)usrptr, &mbuf_len);
    st20_tx_put_mbuf_tai(handle, mbuf, mbuf_len, tai);
  }
}

int rx_exact_pacing_frame_ready(void* priv, void* frame,
                                struct st20_rx_frame_meta* meta) {
  auto ctx = (tests_context*)priv;
  auto handle = (st20_rx_handle)st20_get_handle(ctx);
  if (!handle) return -EIO;
  if (st_is_frame_complete(meta->status)) {
    int64_t delta = (int64_t)(meta->timestamp_first_pkt -
                              rtp_exact_target_tai(ctx, meta->rtp_timestamp));
    {
      std::lock_guard<std::mutex> lck(rtp_exact_pacing_rx.mtx);
      rtp_exact_pacing_rx.deltas.push_back(delta);
    }
    __atomic_fetch_add(&ctx->fb_rec, 1, __ATOMIC_RELEASE);
  }
  st20_rx_put_framebuff(handle, frame);
  return 0;
}

/* An RTP-level session with EXACT_USER_PACING sends each frame at the TAI of
 * st20_tx_put_mbuf_tai(): across session restarts, the RX-measured first-packet arrival
 * keeps the same offset from that target, whatever the epoch grid. */
void st20_tx_rtp_exact_user_pacing_test(int restarts, enum st_test_level level) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  enum st_fps fps = ST_FPS_P59_94;
  /* off the epoch grid, so a session paced on epochs would not match */
  constexpr uint64_t kTargetLeadNs = 300 * NS_PER_MS + 333 * 1000;
  constexpr int64_t kMaxSpreadNs = 10'000;
  constexpr int64_t kMaxOffsetNs = 100'000;
  /* > 1 s of frames (~64 frames at 59.94 fps = ~1.07 s + 0.3 s target lead). */
  const int target_frames = static_cast<int>(st_frame_rate(fps)) + 5;
  std::vector<int64_t> run_offsets;

  MTL_TEST_REQUIRE_LEVEL(ctx, level);
  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  mtl_test::ScopedMtlStart mtl_guard(m_handle);
  ASSERT_GE(mtl_guard.start(), 0);

  for (int r = 0; r < restarts; r++) {
    std::vector<tests_context*> test_ctx_tx(1);
    std::vector<tests_context*> test_ctx_rx(1);
    std::vector<st20_tx_handle> tx_handle(1);
    std::vector<st20_rx_handle> rx_handle(1);
    std::vector<std::thread> rtp_thread_tx(1);
    St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle,
                          &rtp_thread_tx);
    struct st20_tx_ops ops_tx;
    struct st20_rx_ops ops_rx;

    {
      std::lock_guard<std::mutex> lck(rtp_exact_pacing_rx.mtx);
      rtp_exact_pacing_rx.deltas.clear();
    }

    test_ctx_rx[0] = init_test_ctx(ctx, 0, 3);
    ASSERT_TRUE(test_ctx_rx[0] != nullptr);
    init_single_port_rx(ops_rx, test_ctx_rx[0], "st20_rtp_exact_pacing",
                        udp_port_for_idx(0));
    ops_rx.type = ST20_TYPE_FRAME_LEVEL;
    ops_rx.width = 1280;
    ops_rx.height = 720;
    ops_rx.fps = fps;
    ops_rx.fmt = ST20_FMT_YUV_422_10BIT;
    ops_rx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_rx.framebuff_cnt = test_ctx_rx[0]->fb_cnt;
    ops_rx.notify_frame_ready = rx_exact_pacing_frame_ready;
    rx_handle[0] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[0] != nullptr);

    test_ctx_tx[0] = init_test_ctx(ctx, 0, 3);
    ASSERT_TRUE(test_ctx_tx[0] != nullptr);
    init_single_port_tx(ops_tx, test_ctx_tx[0], "st20_rtp_exact_pacing",
                        udp_port_for_idx(0));
    ops_tx.type = ST20_TYPE_RTP_LEVEL;
    ops_tx.packing = ST20_PACKING_BPM;
    ops_tx.width = 1280;
    ops_tx.height = 720;
    ops_tx.fps = fps;
    ops_tx.fmt = ST20_FMT_YUV_422_10BIT;
    ops_tx.payload_type = ST20_TEST_PAYLOAD_TYPE;
    ops_tx.flags = ST20_TX_FLAG_USER_PACING | ST20_TX_FLAG_EXACT_USER_PACING |
                   ST20_TX_FLAG_USER_TIMESTAMP;
    rtp_tx_specific_init(&ops_tx, test_ctx_tx[0]);
    tx_handle[0] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[0] != nullptr);

    uint64_t base = mtl_ptp_read_time(m_handle) + kTargetLeadNs;
    for (tests_context* c : {test_ctx_tx[0], test_ctx_rx[0]}) {
      c->ptp_time_first_frame = base;
      c->frame_time = (double)NS_PER_S / st_frame_rate(fps);
    }
    test_ctx_tx[0]->rtp_tmstamp = 1;
    st20_set_handle(test_ctx_rx[0], rx_handle[0]);
    st20_set_handle(test_ctx_tx[0], tx_handle[0]);
    test_ctx_tx[0]->stop = false;
    rtp_thread_tx[0] = std::thread(tx_feed_packet_tai, test_ctx_tx[0]);

    EXPECT_TRUE(mtl_test::wait_for_all(test_ctx_rx, mtl_test::rx_frames, target_frames,
                                       kSt20WarmupTimeoutMs));
    struct st20_tx_user_stats stats;
    ASSERT_EQ(st20_tx_get_session_stats(tx_handle[0], &stats), 0);
    guard.release_sessions();
    /* a frame that fell back to the default pacing would skew the offsets */
    EXPECT_EQ(stats.common.stat_error_user_timestamp, 0u);

    std::vector<int64_t> deltas;
    {
      std::lock_guard<std::mutex> lck(rtp_exact_pacing_rx.mtx);
      deltas = rtp_exact_pacing_rx.deltas;
    }
    ASSERT_GT(deltas.size(), (size_t)st_frame_rate(fps));
    std::nth_element(deltas.begin(),
                     deltas.begin() + static_cast<std::ptrdiff_t>(deltas.size() / 2),
                     deltas.end());
    int64_t median = deltas[deltas.size() / 2];
    info("%s, run %d frames %zu median arrival - target %" PRId64 "ns\n", __func__, r,
         deltas.size(), median);
    /* a constant bias, such as tr_offset (~1ms at 720p) or the warm-up lead added to
     * the TAI, keeps the spread small; measured ~7-11us on E835 VFs */
    EXPECT_LT(std::abs(median), kMaxOffsetNs);
    run_offsets.push_back(median);
  }

  auto minmax = std::minmax_element(run_offsets.begin(), run_offsets.end());
  EXPECT_LE(*minmax.second - *minmax.first, kMaxSpreadNs);
}

}  // namespace

TEST(St20_tx, tx_user_pacing) {
  st20_tx_user_pacing_test(
      {
          {1280, 720, ST20_FMT_YUV_422_10BIT, false, true},
          {1920, 1080, ST20_FMT_YUV_422_10BIT, true, false},
          {1280, 720, ST20_FMT_YUV_422_10BIT, true, true},
      },
      ST_TEST_LEVEL_MANDATORY);
}

TEST(St20_tx, rtp_exact_user_pacing_repeatable) {
  st20_tx_rtp_exact_user_pacing_test(3, ST_TEST_LEVEL_ALL);
}
