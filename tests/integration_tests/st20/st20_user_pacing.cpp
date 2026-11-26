/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdlib>
#include <deque>

#include "st20_common.h"

namespace {

struct St20UserPacingStream {
  int width;
  int height;
  enum st20_fmt fmt;
  bool user_pacing;
  bool user_timestamp;
};

struct UserTsRing {
  static constexpr size_t kCap = 256;
  std::atomic<uint32_t> write_idx{0};
  uint32_t rtp_ts[kCap] = {};
  std::atomic<int> rx_checked_cnt{0};
  std::atomic<int> rx_mismatch_cnt{0};

  void push(uint32_t ts) {
    const uint32_t idx = write_idx.load(std::memory_order_relaxed);
    rtp_ts[idx % kCap] = ts;
    write_idx.store(idx + 1, std::memory_order_release);
  }

  bool contains(uint32_t ts) const {
    const uint32_t n = write_idx.load(std::memory_order_acquire);
    const uint32_t count = std::min<uint32_t>(n, static_cast<uint32_t>(kCap));
    const uint32_t start = n - count;
    for (uint32_t i = start; i < n; i++) {
      if (rtp_ts[i % kCap] == ts) return true;
    }
    return false;
  }
};

struct UserPacingSessionPriv {
  tests_context* ctx = nullptr;
  UserTsRing* ring = nullptr;
};

int tx_next_video_frame_user_ts(void* priv, uint16_t* next_frame_idx,
                                struct st20_tx_frame_meta* meta) {
  auto* p = static_cast<UserPacingSessionPriv*>(priv);
  if (!p || !p->ctx) return -EIO;
  int ret = tx_next_video_frame_timestamp(p->ctx, next_frame_idx, meta);
  if (ret == 0 && p->ring && meta->tfmt == ST10_TIMESTAMP_FMT_TAI) {
    p->ring->push(st10_tai_to_media_clk(meta->timestamp, 90000));
  }
  return ret;
}

int tx_notify_frame_done_user_ts(void* priv, uint16_t frame_idx,
                                 struct st20_tx_frame_meta* meta) {
  auto* p = static_cast<UserPacingSessionPriv*>(priv);
  if (!p || !p->ctx) return -EIO;
  return tx_notify_timestamp_frame_done(p->ctx, frame_idx, meta);
}

int rx_digest_frame_ready_user_ts(void* priv, void* frame,
                                  struct st20_rx_frame_meta* meta) {
  auto* p = static_cast<UserPacingSessionPriv*>(priv);
  if (!p || !p->ctx) return -EIO;
  if (p->ring && st_is_frame_complete(meta->status)) {
    p->ring->rx_checked_cnt.fetch_add(1, std::memory_order_relaxed);
    if (!p->ring->contains(meta->rtp_timestamp)) {
      p->ring->rx_mismatch_cnt.fetch_add(1, std::memory_order_relaxed);
    }
  }
  return st20_digest_rx_frame_ready(p->ctx, frame, meta);
}

void st20_tx_user_pacing_test(const std::vector<St20UserPacingStream>& streams) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  const int sessions = static_cast<int>(streams.size());

  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  /* Declare callback state before St20DeinitGuard so sessions stop before destroy. */
  std::deque<UserTsRing> user_ts_rings(sessions);
  std::vector<UserPacingSessionPriv> tx_ts_priv(sessions);
  std::vector<UserPacingSessionPriv> rx_ts_priv(sessions);

  std::vector<tests_context*> test_ctx_tx(sessions);
  std::vector<tests_context*> test_ctx_rx(sessions);
  std::vector<st20_tx_handle> tx_handle(sessions);
  std::vector<st20_rx_handle> rx_handle(sessions);
  std::vector<double> expect_framerate(sessions);
  std::vector<std::thread> sha_thread_rx(sessions);

  St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle, nullptr,
                        &sha_thread_rx);
  guard.set_rx_ctx_cleanup(st20_rx_drain_bufq_put_framebuff);

  constexpr enum st_fps kFps = ST_FPS_P59_94;

  for (int i = 0; i < sessions; i++) {
    const auto& st = streams[i];
    const St20StreamSpec spec{ST20_TYPE_FRAME_LEVEL,
                              ST20_TYPE_FRAME_LEVEL,
                              ST20_PACKING_BPM,
                              kFps,
                              st.width,
                              st.height,
                              false,
                              st.fmt};
    expect_framerate[i] =
        st.user_pacing ? (st_frame_rate(kFps) / 2) : st_frame_rate(kFps);
    test_ctx_tx[i] = init_test_ctx(ctx, i, TEST_SHA_HIST_NUM, true);
    test_ctx_tx[i]->user_pacing = st.user_pacing;
    test_ctx_tx[i]->user_timestamp = st.user_timestamp;
    test_ctx_tx[i]->frame_time = (double)NS_PER_S / st_frame_rate(kFps);

    struct st20_tx_ops ops_tx;
    st20_fill_loopback_tx_ops(ops_tx, test_ctx_tx[i], spec, "st20_timestamp_test",
                              udp_port_for_idx(i));
    if (st.user_pacing && st.user_timestamp) {
      tx_ts_priv[i] = {test_ctx_tx[i], &user_ts_rings[i]};
      ops_tx.priv = &tx_ts_priv[i];
      ops_tx.get_next_frame = tx_next_video_frame_user_ts;
      ops_tx.notify_frame_done = tx_notify_frame_done_user_ts;
    } else {
      ops_tx.get_next_frame = tx_next_video_frame_timestamp;
      ops_tx.notify_frame_done = tx_notify_timestamp_frame_done;
    }
    if (st.user_pacing) ops_tx.flags |= ST20_TX_FLAG_USER_PACING;
    if (st.user_timestamp) ops_tx.flags |= ST20_TX_FLAG_USER_TIMESTAMP;

    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[i] != NULL);

    st20_init_tx_digest_frames(m_handle, test_ctx_tx[i], tx_handle[i], spec);
    if (::testing::Test::HasFatalFailure()) return;
  }

  for (int i = 0; i < sessions; i++) {
    const auto& st = streams[i];
    const St20StreamSpec spec{ST20_TYPE_FRAME_LEVEL,
                              ST20_TYPE_FRAME_LEVEL,
                              ST20_PACKING_BPM,
                              kFps,
                              st.width,
                              st.height,
                              false,
                              st.fmt};
    test_ctx_rx[i] = init_test_ctx(ctx, i, 3, true);
    test_ctx_rx[i]->user_pacing = st.user_pacing;
    test_ctx_rx[i]->user_timestamp = st.user_timestamp;

    struct st20_rx_ops ops_rx;
    st20_fill_loopback_rx_ops(ops_rx, test_ctx_rx[i], spec, "st20_timestamp_test",
                              udp_port_for_idx(i));
    if (st.user_pacing && st.user_timestamp) {
      rx_ts_priv[i] = {test_ctx_rx[i], &user_ts_rings[i]};
      ops_rx.priv = &rx_ts_priv[i];
      ops_rx.notify_frame_ready = rx_digest_frame_ready_user_ts;
    } else {
      ops_rx.notify_frame_ready = st20_digest_rx_frame_ready;
    }

    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[i] != NULL);

    st20_bind_rx_digest_ctx(test_ctx_rx[i], test_ctx_tx[i], rx_handle[i], spec);
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
   * Measure TX and RX FPS over one >=1500 ms steady-state window:
   * warmup=12 absorbs RL pacing training, >=35 frames (~45 at half-rate 29.97 fps)
   * keeps edge error <=2.2% vs +/-10% tolerance.
   */
  auto res = mtl_test::measure_pipeline_window(
      test_ctx_tx, test_ctx_rx, kSt20WarmupFrames, kSt20MinRateFrames,
      kSt20MinRateWindowMs, kSt20WarmupTimeoutMs, kSt20WindowTimeoutMs);
  EXPECT_TRUE(res.warmed_up);
  EXPECT_TRUE(res.complete);
  EXPECT_TRUE(mtl_test::wait_for_all(test_ctx_rx, mtl_test::sha_checked_frames,
                                     kSt20MinShaFrames, kSt20EventTimeoutMs));

  std::vector<struct st20_tx_user_stats> tx_stats(sessions);
  for (int i = 0; i < sessions; i++) {
    ASSERT_GE(st20_tx_get_session_stats(tx_handle[i], &tx_stats[i]), 0);
  }

  guard.release_sessions();

  for (int i = 0; i < sessions; i++) {
    EXPECT_GT(test_ctx_rx[i]->fb_rec, 0);
    EXPECT_GT(test_ctx_rx[i]->check_sha_frame_cnt, 0);
    EXPECT_LT(test_ctx_rx[i]->incomplete_frame_cnt, 2);
    EXPECT_EQ(test_ctx_rx[i]->sha_fail_cnt, 0);
    EXPECT_EQ(tx_stats[i].common.stat_error_user_timestamp, 0u);
    if (streams[i].user_pacing && streams[i].user_timestamp) {
      EXPECT_GT(user_ts_rings[i].rx_checked_cnt.load(std::memory_order_relaxed), 0);
      EXPECT_EQ(user_ts_rings[i].rx_mismatch_cnt.load(std::memory_order_relaxed), 0);
    }

    info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
         res.rx_fps[i]);
    info("%s, session %d fb_send %d framerate %f\n", __func__, i, test_ctx_tx[i]->fb_send,
         res.tx_fps[i]);

    EXPECT_NEAR(res.tx_fps[i], expect_framerate[i], expect_framerate[i] * 0.1);
    EXPECT_NEAR(res.rx_fps[i], expect_framerate[i], expect_framerate[i] * 0.1);
  }
}

/* Per-run RX state for exact pacing; must be declared before St20DeinitGuard so
 * guard frees rx_handle before rx_state is destroyed on normal exit or ASSERT_*. */
struct RtpExactPacingRxCtx {
  tests_context* ctx = nullptr;
  std::mutex mtx;
  std::vector<int64_t> deltas;
  std::atomic<int> incomplete_frames{0};
};

/* frame k carries RTP timestamp k + 1, the feed starting at 1 */
uint64_t rtp_exact_target_tai(tests_context* s, uint32_t rtp_ts) {
  return s->ptp_time_first_frame + (uint64_t)((rtp_ts - 1) * s->frame_time);
}

int rx_exact_pacing_frame_ready(void* priv, void* frame,
                                struct st20_rx_frame_meta* meta) {
  auto* rx_state = static_cast<RtpExactPacingRxCtx*>(priv);
  if (!rx_state || !rx_state->ctx) return -EIO;
  auto* ctx = rx_state->ctx;
  auto handle = (st20_rx_handle)st20_get_handle(ctx);
  if (!handle) return -EIO;
  if (st_is_frame_complete(meta->status)) {
    int64_t delta = (int64_t)(meta->timestamp_first_pkt -
                              rtp_exact_target_tai(ctx, meta->rtp_timestamp));
    {
      std::lock_guard<std::mutex> lck(rx_state->mtx);
      rx_state->deltas.push_back(delta);
    }
    __atomic_fetch_add(&ctx->fb_rec, 1, __ATOMIC_RELEASE);
  } else {
    rx_state->incomplete_frames.fetch_add(1, std::memory_order_relaxed);
  }
  st20_rx_put_framebuff(handle, frame);
  return 0;
}

/* An RTP-level session with EXACT_USER_PACING sends each frame at the TAI of
 * st20_tx_put_mbuf_tai(): across session restarts, the RX-measured first-packet arrival
 * keeps the same offset from that target, whatever the epoch grid. */
void st20_tx_rtp_exact_user_pacing_test(int restarts) {
  auto ctx = st_test_ctx();
  auto m_handle = ctx->handle;
  const St20StreamSpec spec{ST20_TYPE_RTP_LEVEL,
                            ST20_TYPE_FRAME_LEVEL,
                            ST20_PACKING_BPM,
                            ST_FPS_P59_94,
                            1280,
                            720};
  /* off the epoch grid, so a session paced on epochs would not match */
  constexpr uint64_t kTargetLeadNs = 300 * NS_PER_MS + 333 * 1000;
  constexpr int64_t kMaxSpreadNs = 10'000;
  constexpr int64_t kMaxOffsetNs = 100'000;
  /* > 1 s of frames (~64 frames at 59.94 fps = ~1.07 s + 0.3 s target lead). */
  const int target_frames = static_cast<int>(st_frame_rate(spec.fps)) + 5;
  std::vector<int64_t> run_offsets;

  MTL_TEST_REQUIRE_PORTS(ctx, 2);

  mtl_test::ScopedMtlStart mtl_guard(m_handle);
  ASSERT_GE(mtl_guard.start(), 0);

  for (int r = 0; r < restarts; r++) {
    RtpExactPacingRxCtx rx_state;
    std::vector<tests_context*> test_ctx_tx(1);
    std::vector<tests_context*> test_ctx_rx(1);
    std::vector<st20_tx_handle> tx_handle(1);
    std::vector<st20_rx_handle> rx_handle(1);
    std::vector<std::thread> rtp_thread_tx(1);
    St20DeinitGuard guard(m_handle, test_ctx_tx, test_ctx_rx, tx_handle, rx_handle,
                          &rtp_thread_tx);

    test_ctx_rx[0] = init_test_ctx(ctx, 0, 3);
    rx_state.ctx = test_ctx_rx[0];
    struct st20_rx_ops ops_rx;
    st20_fill_loopback_rx_ops(ops_rx, test_ctx_rx[0], spec, "st20_rtp_exact_pacing",
                              udp_port_for_idx(0));
    ops_rx.priv = &rx_state;
    ops_rx.notify_frame_ready = rx_exact_pacing_frame_ready;
    rx_handle[0] = st20_rx_create(m_handle, &ops_rx);
    ASSERT_TRUE(rx_handle[0] != nullptr);

    test_ctx_tx[0] = init_test_ctx(ctx, 0, 3);
    struct st20_tx_ops ops_tx;
    st20_fill_loopback_tx_ops(ops_tx, test_ctx_tx[0], spec, "st20_rtp_exact_pacing",
                              udp_port_for_idx(0));
    ops_tx.flags = ST20_TX_FLAG_USER_PACING | ST20_TX_FLAG_EXACT_USER_PACING |
                   ST20_TX_FLAG_USER_TIMESTAMP;
    tx_handle[0] = st20_tx_create(m_handle, &ops_tx);
    ASSERT_TRUE(tx_handle[0] != nullptr);

    uint64_t base = mtl_ptp_read_time(m_handle) + kTargetLeadNs;
    for (tests_context* c : {test_ctx_tx[0], test_ctx_rx[0]}) {
      c->ptp_time_first_frame = base;
      c->frame_time = (double)NS_PER_S / st_frame_rate(spec.fps);
    }
    test_ctx_tx[0]->rtp_tmstamp = 1;
    st20_set_handle(test_ctx_rx[0], rx_handle[0]);
    st20_set_handle(test_ctx_tx[0], tx_handle[0]);
    rtp_thread_tx[0] = std::thread(
        [](tests_context* c) {
          tx_feed_packet_ex(c, false, [](tests_context* s) {
            return rtp_exact_target_tai(s, s->rtp_tmstamp);
          });
        },
        test_ctx_tx[0]);

    EXPECT_TRUE(mtl_test::wait_for_all(test_ctx_rx, mtl_test::rx_frames, target_frames,
                                       kSt20WarmupTimeoutMs));
    struct st20_tx_user_stats stats = {};
    struct st20_rx_user_stats rx_stats = {};
    ASSERT_EQ(st20_tx_get_session_stats(tx_handle[0], &stats), 0);
    ASSERT_EQ(st20_rx_get_session_stats(rx_handle[0], &rx_stats), 0);
    guard.release_sessions();
    /* a frame that fell back to the default pacing would skew the offsets */
    EXPECT_EQ(stats.common.stat_error_user_timestamp, 0u);
    EXPECT_EQ(rx_stats.stat_frames_incomplete, 0u);
    EXPECT_EQ(rx_state.incomplete_frames.load(std::memory_order_relaxed), 0);

    auto& deltas = rx_state.deltas;
    ASSERT_GT(deltas.size(), (size_t)st_frame_rate(spec.fps));
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
  st20_tx_user_pacing_test({
      {1280, 720, ST20_FMT_YUV_422_10BIT, false, true},
      {1920, 1080, ST20_FMT_YUV_422_10BIT, true, false},
      {1280, 720, ST20_FMT_YUV_422_10BIT, true, true},
  });
}

TEST(St20_tx, rtp_exact_user_pacing_repeatable) {
  st20_tx_rtp_exact_user_pacing_test(3);
}
