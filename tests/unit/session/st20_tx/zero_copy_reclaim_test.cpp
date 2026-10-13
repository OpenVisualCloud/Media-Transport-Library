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

/* The waited frame's last batches leave only behind the next frame, so its due time, less
 * the RL warm-up, is when the hang clock may start: a USER_PACING gap is no hang. */
class St20TxHangClockTest : public St20TxZeroCopyReclaimTest {
 protected:
  /* the first poll at which a wait from kStartNs is past the hang threshold */
  static constexpr uint64_t kPastHangNs = kStartNs + kHangNs + 20000;
  static constexpr uint64_t kNsPerS = 1000000000ull;
  void SetUpWait(uint64_t next_frame_due_ns) {
    SetUpFrames(true);
    ut_txv_zc_set_hang_ns(ctx_, kHangNs);
    ut_txv_zc_set_nic_refs(ctx_, 0, 1);
    ut_txv_zc_set_next_frame_due(ctx_, next_frame_due_ns);
    ASSERT_EQ(ut_txv_reclaim_interval_ns(), 20000u);
    ASSERT_TRUE(ut_txv_zc_frames_out(ctx_));
  }
  /* reclaims at the builder's cadence up to ns, then whether a leg was flagged */
  bool RecoveryBy(uint64_t ns) {
    ReclaimUntil(ns);
    return ut_txv_zc_take_recovery(ctx_, 0);
  }
};

TEST_F(St20TxHangClockTest, UserPacingGapIsNotAHang) {
  SetUpWait(kStartNs + 3 * kHangNs);

  EXPECT_FALSE(RecoveryBy(kStartNs + 3 * kHangNs));
}

/* The warm-up opens the frame 30 us, 1.5 reclaim intervals, before its due time: the
 * poll at due + hang - 40 us is short of the hang, the next one past it. A warm-up twice
 * or half as long moves the flag a poll. */
TEST_F(St20TxHangClockTest, HangCountsFromTheNextFrameDue) {
  constexpr uint64_t kDueNs = kStartNs + 2 * kHangNs;
  ut_txv_set_warm_pkts(ctx_, 8);
  ut_txv_set_trs(ctx_, 3750);
  SetUpWait(kDueNs);

  EXPECT_FALSE(RecoveryBy(kDueNs + kHangNs - 40000));
  EXPECT_TRUE(RecoveryBy(kDueNs + kHangNs - 20000));
}

TEST_F(St20TxHangClockTest, PastDueCountsFromTheWait) {
  SetUpWait(kStartNs / 2);

  EXPECT_FALSE(RecoveryBy(kStartNs + kHangNs));
  EXPECT_TRUE(RecoveryBy(kPastHangNs));
}

/* the transmitters take a target up to 1 s ahead, and send a later one at once */
TEST_F(St20TxHangClockTest, DueOneSecondAheadIsNotOpenYet) {
  SetUpWait(kPastHangNs + kNsPerS);

  EXPECT_FALSE(RecoveryBy(kPastHangNs));
}

TEST_F(St20TxHangClockTest, DuePastOneSecondCountsFromTheWait) {
  SetUpWait(kPastHangNs + kNsPerS + 1);

  EXPECT_FALSE(RecoveryBy(kStartNs + kHangNs));
  EXPECT_TRUE(RecoveryBy(kPastHangNs));
}

TEST_F(St20TxHangClockTest, UnreachableDueCountsFromTheWait) {
  SetUpWait(UINT64_MAX);

  EXPECT_FALSE(RecoveryBy(kStartNs + kHangNs));
  EXPECT_TRUE(RecoveryBy(kPastHangNs));
}
