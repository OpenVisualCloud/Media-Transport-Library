/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * A zero-copy frame stays referenced by its pkts after they left the wire until the PMD
 * frees them, which a PMD freeing an mbuf only when it reuses the descriptor does a TX
 * ring later. Where the PMD frees them on request, the builder asks the app for a frame
 * only once every frame but the last one built is back, reclaiming the sent pkts while
 * it waits: the app never hands over a frame the NIC still reads.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxZeroCopyReclaimTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20_tx_harness.h"

class St20TxZeroCopyReclaimTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
    ut_txv_set_mock_tsc_time(ctx_, kStartNs);
  }
  void TearDown() override {
    ut_txv_zc_teardown(ctx_);
    ut_txv_destroy(ctx_);
  }
  /* frame 1 went out last, frame 0 before it */
  void SetUpFrames(bool tx_reclaim, int num_port = 1) {
    ASSERT_EQ(ut_txv_zc_setup(ctx_, tx_reclaim, num_port), 0);
    ut_txv_zc_set_last_built(ctx_, 1);
    ut_txv_zc_set_app(ctx_, 0, false);
  }
  void At(uint64_t ns) {
    now_ns_ = ns;
    ut_txv_set_mock_tsc_time(ctx_, ns);
  }
  bool FramesOutAt(uint64_t ns) {
    At(ns);
    return ut_txv_zc_frames_out(ctx_);
  }
  /* the builder's reclaim cadence while a frame stays out */
  void ReclaimUntil(uint64_t end_ns, uint64_t step_ns = ut_txv_reclaim_interval_ns()) {
    for (uint64_t ns = now_ns_ + step_ns; ns <= end_ns; ns += step_ns)
      ASSERT_TRUE(FramesOutAt(ns));
  }
  static constexpr uint64_t kStartNs = 1000000;
  static constexpr uint64_t kHangNs = 10000000;
  static constexpr uint64_t kSmallHangNs = 1000000;
  ut_txv_ctx* ctx_ = nullptr;
  uint64_t now_ns_ = kStartNs;
};

TEST_F(St20TxZeroCopyReclaimTest, AppIsAskedOnceTheEarlierFrameIsBack) {
  SetUpFrames(true);
  ut_txv_zc_set_nic_refs(ctx_, 0, 1);
  ut_txv_zc_nic_sent(ctx_, 0);

  ut_txv_zc_run(ctx_);

  EXPECT_EQ(ut_txv_zc_reclaim_calls(ctx_), 1);
  EXPECT_EQ(ut_txv_get_next_frame_calls(ctx_), 1);
  EXPECT_EQ(ut_txv_zc_built_pkts(ctx_), 1u);
}

TEST_F(St20TxZeroCopyReclaimTest, AppWaitsWhileAnEarlierFrameIsOnTheWire) {
  SetUpFrames(true);
  ut_txv_zc_set_nic_refs(ctx_, 0, 1);

  for (int i = 0; i < 3; i++) ut_txv_zc_run(ctx_);
  /* reclaimed once in the interval, the app not asked */
  EXPECT_EQ(ut_txv_zc_reclaim_calls(ctx_), 1);
  EXPECT_EQ(ut_txv_get_next_frame_calls(ctx_), 0);

  ut_txv_zc_nic_sent(ctx_, 0);
  ut_txv_set_mock_tsc_time(ctx_, kStartNs + ut_txv_reclaim_interval_ns());
  ut_txv_zc_run(ctx_);

  EXPECT_EQ(ut_txv_zc_reclaim_calls(ctx_), 2);
  EXPECT_EQ(ut_txv_get_next_frame_calls(ctx_), 1);
  EXPECT_EQ(ut_txv_zc_built_pkts(ctx_), 1u);
}

/* each leg holds the frame through its own queue */
TEST_F(St20TxZeroCopyReclaimTest, RedundantSessionReclaimsBothLegs) {
  SetUpFrames(true, 2);
  ut_txv_zc_set_nic_refs(ctx_, 0, 1);
  ut_txv_zc_nic_sent(ctx_, 0);

  EXPECT_FALSE(ut_txv_zc_frames_out(ctx_));
  EXPECT_EQ(ut_txv_zc_leg_reclaims(ctx_, 0), 1);
  EXPECT_EQ(ut_txv_zc_leg_reclaims(ctx_, 1), 1);
}

/* the frame built last is still on its way: the app may have the others */
TEST_F(St20TxZeroCopyReclaimTest, FrameBuiltLastDoesNotHoldTheApp) {
  SetUpFrames(true);
  ut_txv_zc_set_nic_refs(ctx_, 1, 1);

  ut_txv_zc_run(ctx_);

  EXPECT_EQ(ut_txv_zc_reclaim_calls(ctx_), 0);
  EXPECT_EQ(ut_txv_get_next_frame_calls(ctx_), 1);
}

/* a shared TX queue, RTCP or a PMD that cannot reclaim: the app is asked at once, and a
 * frame still referenced is refused as before */
TEST_F(St20TxZeroCopyReclaimTest, SessionWithoutReclaimAsksTheAppAtOnce) {
  SetUpFrames(false);
  ut_txv_zc_set_nic_refs(ctx_, 0, 1);

  ut_txv_zc_run(ctx_);

  EXPECT_EQ(ut_txv_zc_reclaim_calls(ctx_), 0);
  EXPECT_EQ(ut_txv_get_next_frame_calls(ctx_), 1);
  EXPECT_EQ(ut_txv_zc_built_pkts(ctx_), 0u);
}

/* with nothing to send the transmitter never sees a hung queue's burst fail: a wait past
 * the hang threshold flags each leg idle as long, once per threshold */
TEST_F(St20TxZeroCopyReclaimTest, WaitPastTheHangThresholdRecoversTheIdleLeg) {
  SetUpFrames(true);
  ut_txv_zc_set_hang_ns(ctx_, kHangNs);
  ut_txv_zc_set_nic_refs(ctx_, 0, 1);

  EXPECT_TRUE(ut_txv_zc_frames_out(ctx_));
  ReclaimUntil(kStartNs + kHangNs);
  EXPECT_FALSE(ut_txv_zc_take_recovery(ctx_, 0));

  EXPECT_TRUE(FramesOutAt(kStartNs + kHangNs + ut_txv_reclaim_interval_ns()));
  EXPECT_TRUE(ut_txv_zc_take_recovery(ctx_, 0));

  EXPECT_TRUE(FramesOutAt(kStartNs + kHangNs + 2 * ut_txv_reclaim_interval_ns()));
  EXPECT_FALSE(ut_txv_zc_take_recovery(ctx_, 0));
}

/* the legs idled through the wait before: a new wait counts from its own start */
TEST_F(St20TxZeroCopyReclaimTest, WaitJustBegunDoesNotRecoverAnIdleLeg) {
  SetUpFrames(true);
  ut_txv_zc_set_hang_ns(ctx_, kHangNs);
  ut_txv_zc_set_nic_refs(ctx_, 0, 1);
  EXPECT_TRUE(ut_txv_zc_frames_out(ctx_));
  ReclaimUntil(kStartNs + kHangNs - ut_txv_reclaim_interval_ns());
  ut_txv_zc_nic_sent(ctx_, 0);
  EXPECT_FALSE(FramesOutAt(kStartNs + kHangNs));

  ut_txv_zc_set_last_built(ctx_, 0);
  ut_txv_zc_set_nic_refs(ctx_, 1, 1);
  const uint64_t wait_ns = kStartNs + kHangNs + ut_txv_reclaim_interval_ns();
  EXPECT_TRUE(FramesOutAt(wait_ns));
  ReclaimUntil(wait_ns + kHangNs);
  EXPECT_FALSE(ut_txv_zc_take_recovery(ctx_, 0));
}

/* neither tasklet ran: the transmitter gets its turn before the wait counts again */
TEST_F(St20TxZeroCopyReclaimTest, SchedulerStallDoesNotRecover) {
  SetUpFrames(true);
  ut_txv_zc_set_hang_ns(ctx_, kHangNs);
  ut_txv_zc_set_nic_refs(ctx_, 0, 1);

  EXPECT_TRUE(ut_txv_zc_frames_out(ctx_));
  EXPECT_TRUE(FramesOutAt(kStartNs + 2 * kHangNs));
  EXPECT_FALSE(ut_txv_zc_take_recovery(ctx_, 0));
}

/* a stall under the threshold but past half of it restarts the wait too */
TEST_F(St20TxZeroCopyReclaimTest, StallPastHalfTheThresholdDoesNotRecover) {
  SetUpFrames(true);
  ut_txv_zc_set_hang_ns(ctx_, kHangNs);
  ut_txv_zc_set_nic_refs(ctx_, 0, 1);

  EXPECT_TRUE(ut_txv_zc_frames_out(ctx_));
  ReclaimUntil(kStartNs + kHangNs / 2);
  EXPECT_TRUE(FramesOutAt(kStartNs + kHangNs / 2 + kHangNs * 3 / 5));
  EXPECT_FALSE(ut_txv_zc_take_recovery(ctx_, 0));
}

/* reclaims further apart than a small user threshold still add up to a hang */
TEST_F(St20TxZeroCopyReclaimTest, HangReclaimedLessOftenThanTheThresholdRecovers) {
  constexpr uint64_t kFrameNs = 4 * kSmallHangNs;
  SetUpFrames(true);
  ut_txv_zc_set_hang_ns(ctx_, kSmallHangNs);
  ut_txv_set_frame_time(ctx_, kFrameNs);
  ut_txv_zc_set_nic_refs(ctx_, 0, 1);

  EXPECT_TRUE(ut_txv_zc_frames_out(ctx_));
  ReclaimUntil(kStartNs + 2 * kFrameNs, kFrameNs);
  EXPECT_FALSE(ut_txv_zc_take_recovery(ctx_, 0));
  ReclaimUntil(kStartNs + 3 * kFrameNs, kFrameNs);
  EXPECT_TRUE(ut_txv_zc_take_recovery(ctx_, 0));
}

TEST_F(St20TxZeroCopyReclaimTest, LegStillSendingIsNotRecovered) {
  SetUpFrames(true, 2);
  ut_txv_zc_set_hang_ns(ctx_, kHangNs);
  ut_txv_zc_set_nic_refs(ctx_, 0, 1);
  EXPECT_TRUE(ut_txv_zc_frames_out(ctx_));

  At(kStartNs + ut_txv_reclaim_interval_ns());
  ut_txv_zc_leg_burst_now(ctx_, 1);
  ReclaimUntil(kStartNs + kHangNs + ut_txv_reclaim_interval_ns());
  EXPECT_TRUE(ut_txv_zc_take_recovery(ctx_, 0));
  EXPECT_FALSE(ut_txv_zc_take_recovery(ctx_, 1));
}

/* the wait spans the frame period from build to send whatever the user threshold */
TEST_F(St20TxZeroCopyReclaimTest, HangThresholdIsAtLeastTwoFrames) {
  SetUpFrames(true);
  ut_txv_zc_set_hang_ns(ctx_, kHangNs);
  ut_txv_set_frame_time(ctx_, kHangNs);
  ut_txv_zc_set_nic_refs(ctx_, 0, 1);
  EXPECT_TRUE(ut_txv_zc_frames_out(ctx_));

  ReclaimUntil(kStartNs + 2 * kHangNs);
  EXPECT_FALSE(ut_txv_zc_take_recovery(ctx_, 0));
  ReclaimUntil(kStartNs + 2 * kHangNs + ut_txv_reclaim_interval_ns());
  EXPECT_TRUE(ut_txv_zc_take_recovery(ctx_, 0));
}
