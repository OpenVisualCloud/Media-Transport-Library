/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The production builder and TSC transmitter of a 1080p59.94 session through a link
 * down period, with the default 1 s hang threshold. The link flag follows the link a
 * poll period later; the NIC rings send a descriptor each 550 ns, 10 Gb/s, and each pkt
 * is judged where it leaves the wire against its place on the frame grid. When the
 * transmitter finds the flag down, the session drops what it holds in software; while
 * down it drops each pkt at its time, and after the up until a frame start that the
 * stale pkts of the ring cannot delay. After the resync a frame starts on the epoch
 * after the time it is built. So no pkt leaves after its time (ST 2110-21:2022 6.6.2
 * p11).
 *
 * The pkts the NIC ring holds when the link goes down are the one residual: no queue
 * stop is safe, and they leave late when the link is back. The frames they point into
 * stay with the session, library, ext or direct chain frames alike, until the ring
 * frees the pkts.
 *
 * Redundant: until the flag says down the stalled ring of P stops the builder, and R
 * with it. Then R goes on while the session has a free frame; with 3 frames the rings
 * can hold them all. The resync drops the pkts of R in software, and the stale pkts
 * of P may carry seqs that R never sends: around the down the legs are not copies of
 * each other (ST 2022-7:2019 6 p5).
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxLinkDownPipeTest.*'
 */

#include <gtest/gtest.h>

#include "common/ut_tx_ring.h"
#include "session/st20_tx_harness.h"

namespace {

constexpr int kP = 0;
constexpr int kR = 1;
constexpr uint16_t kNbDesc = 512;
constexpr uint64_t kStepNs = 1000;
constexpr uint64_t kHangNs = 1000000000;
constexpr uint64_t kUpNs = 50000000;
constexpr uint64_t kDownNs = 300000000;
/* the poll period of the flag, then 100 ms of frames */
constexpr uint64_t kAfterNs = 200000000;
constexpr uint64_t kFrameNs = 16683333;
constexpr uint64_t kPollNs = 100000000;
/* the poll period before the flag says down, and the frame it cuts */
constexpr uint64_t kFindNs = kPollNs + kFrameNs;
/* a descriptor at 10 Gb/s; a pkt is a header and a payload one */
constexpr uint64_t kWireNs = 550;
/* the TSC transmitter posts a pkt within a run step of its time, the wire sends it */
constexpr int64_t kOnTimeNs = 2 * kStepNs + 2 * kWireNs;

class St20TxLinkDownPipeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    EXPECT_EQ(ut_txv_pipe_teardown(ctx_), 0);
    ut_txv_destroy(ctx_);
  }
  /* a redundant session has a frame past the two a stalled ring of P can hold */
  static ut_txv_pipe_case Case(int num_port, uint16_t frames_cnt = 0) {
    ut_txv_pipe_case c = {};
    c.width = 1920;
    c.height = 1080;
    c.frames_cnt = frames_cnt ? frames_cnt : (num_port > 1 ? 4 : 3);
    c.num_port = num_port;
    c.ring_mode = UT_TXR_FREE_ON_CLEANUP;
    c.free_thresh = 32;
    c.wire_ns = kWireNs;
    for (int i = 0; i < num_port; i++) c.nb_tx_desc[i] = kNbDesc;
    return c;
  }
  void Start(const ut_txv_pipe_case& c) {
    ASSERT_EQ(ut_txv_pipe_setup(ctx_, &c), 0);
    ut_txv_zc_set_hang_ns(ctx_, kHangNs);
    ut_txv_pipe_run(ctx_, kUpNs, kStepNs);
  }
  void Start(int num_port, bool ext = false, bool iavf = false, uint16_t frames_cnt = 0) {
    ut_txv_pipe_case c = Case(num_port, frames_cnt);
    c.ext = ext;
    c.iavf = iavf;
    Start(c);
  }
  /* down for down_ns, then up for kAfterNs */
  void LinkDownAndUp(uint64_t down_ns = kDownNs) {
    ut_txv_pipe_set_link(ctx_, kP, false);
    ut_txv_pipe_run(ctx_, down_ns, kStepNs);
    ut_txv_pipe_set_link(ctx_, kP, true);
    ut_txv_pipe_mark(ctx_);
    ut_txv_pipe_run(ctx_, kAfterNs, kStepNs);
  }
  uint64_t Pkts(uint64_t ns) {
    return ns / kFrameNs * ut_txv_pipe_total_pkts(ctx_);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

TEST_F(St20TxLinkDownPipeTest, NoPktLeavesAfterItsTimeOnceTheLinkIsBack) {
  Start(1);
  ut_txv_pipe_set_link(ctx_, kP, false);
  ut_txv_pipe_run(ctx_, kDownNs, kStepNs);
  ut_txv_pipe_set_link(ctx_, kP, true);
  ut_txv_pipe_mark(ctx_);

  ut_txv_pipe_run(ctx_, kAfterNs, kStepNs);

  EXPECT_GE(ut_txv_pipe_accepted(ctx_, kP), Pkts(kAfterNs / 2) - Pkts(kFrameNs));
  EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, kP), kOnTimeNs);
}

/* the residual: what the NIC ring held when the link went down, no more */
TEST_F(St20TxLinkDownPipeTest, OnlyTheNicRingLeavesLateWhenTheLinkIsBack) {
  Start(1);
  ut_txv_pipe_set_link(ctx_, kP, false);
  ut_txv_pipe_run(ctx_, kDownNs, kStepNs);
  ut_txv_pipe_set_link(ctx_, kP, true);

  EXPECT_LT(ut_txv_pipe_stale_at_up(ctx_, kP), kNbDesc);
}

/* The builder ring is empty, and the app asks for a start 2 frame times ago while the
 * link is down: the transmitter drops each pkt as the builder makes it. The frame comes
 * back once, after its last pkt. */
TEST_F(St20TxLinkDownPipeTest, FrameThatStartsLateWhileTheLinkIsDownComesBackOnce) {
  Start(1);
  ut_txv_pipe_set_link(ctx_, kP, false);
  ut_txv_pipe_run(ctx_, kFindNs + kFrameNs, kStepNs);
  ut_txv_pipe_set_app_busy(ctx_, true);
  ut_txv_pipe_run(ctx_, 2 * kFrameNs, kStepNs);
  ut_txv_pipe_set_app_busy(ctx_, false);
  ut_txv_pipe_set_app_start_ago(ctx_, 2 * kFrameNs);
  uint64_t asked = ut_txv_pipe_frames_asked(ctx_);

  ut_txv_pipe_run(ctx_, 3 * kFrameNs, kStepNs);

  ASSERT_GT(ut_txv_pipe_frames_asked(ctx_), asked + 1);
  EXPECT_EQ(ut_txv_pipe_done_while_built(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_done_twice(ctx_), 0);
}

TEST_F(St20TxLinkDownPipeTest, RedundantLegKeepsItsTimeWhileTheOtherLinkIsDown) {
  Start(2);
  ut_txv_pipe_mark(ctx_);
  ut_txv_pipe_set_link(ctx_, kP, false);

  ut_txv_pipe_run(ctx_, kDownNs, kStepNs);

  EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, kR), kOnTimeNs);
  EXPECT_GE(ut_txv_pipe_accepted(ctx_, kR), Pkts(kDownNs - kFindNs) - Pkts(kFrameNs));
}

TEST_F(St20TxLinkDownPipeTest, BothLegsGoOnOnTimeAfterTheLinkIsBack) {
  Start(2);
  ut_txv_pipe_set_link(ctx_, kP, false);
  ut_txv_pipe_run(ctx_, kDownNs, kStepNs);
  ut_txv_pipe_set_link(ctx_, kP, true);
  ut_txv_pipe_mark(ctx_);

  ut_txv_pipe_run(ctx_, kAfterNs, kStepNs);

  EXPECT_EQ(ut_txv_pipe_first_frame_busy(ctx_), 0u);
  EXPECT_EQ(ut_txv_pipe_done_while_held(ctx_), 0);
  for (int leg : {kP, kR}) {
    EXPECT_GE(ut_txv_pipe_accepted(ctx_, leg), Pkts(kAfterNs / 2) - Pkts(kFrameNs))
        << "leg " << leg;
    EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, leg), kOnTimeNs) << "leg " << leg;
  }
}

TEST_F(St20TxLinkDownPipeTest, NoFrameIsDoneWhileTheNicRingHoldsItsPkts) {
  Start(1);

  LinkDownAndUp();

  EXPECT_EQ(ut_txv_pipe_done_while_held(ctx_), 0);
  EXPECT_GE(ut_txv_pipe_accepted(ctx_, kP), Pkts(kAfterNs / 2) - Pkts(kFrameNs));
}

TEST_F(St20TxLinkDownPipeTest, NoExtFrameIsDoneWhileTheNicRingHoldsItsPkts) {
  Start(1, true);

  LinkDownAndUp();

  EXPECT_EQ(ut_txv_pipe_done_while_held(ctx_), 0);
  EXPECT_GE(ut_txv_pipe_accepted(ctx_, kP), Pkts(kAfterNs / 2) - Pkts(kFrameNs));
}

/* 1920x67, 256 pkts: a whole frame fits in the builder ring and the inflights, so the
 * builder takes the next frame before the one it built starts. While the frames of P
 * are held over the down, no frame may take the epoch of the frame before (ST
 * 2110-21:2022 6.6.1 p9, ST 2110-10:2022 7.6.1 p11). */
TEST_F(St20TxLinkDownPipeTest, RedundantSmallFrameTakesANewEpochEachFrameOverADown) {
  ut_txv_pipe_case c = Case(2);
  c.height = 67;
  Start(c);
  ut_txv_pipe_mark(ctx_);

  LinkDownAndUp();

  EXPECT_EQ(ut_txv_pipe_epoch_repeats(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_rtp_repeats(ctx_, kR), 0);
  EXPECT_EQ(ut_txv_pipe_late_starts(ctx_), 0);
  EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, kR), kOnTimeNs);
}

/* only the last pkt of a frame counts on it: the ring holds pkts of the frame it cut */
TEST_F(St20TxLinkDownPipeTest, NoDirectChainFrameIsDoneWhileTheNicRingHoldsItsPkts) {
  Start(1, false, true);
  ASSERT_TRUE(ut_txv_pipe_direct_chain(ctx_));

  LinkDownAndUp();

  EXPECT_EQ(ut_txv_pipe_done_while_held(ctx_), 0);
  EXPECT_GE(ut_txv_pipe_accepted(ctx_, kP), Pkts(kAfterNs / 2) - Pkts(kFrameNs));
}

TEST_F(St20TxLinkDownPipeTest, RedundantDirectChainFramesWaitForTheNicRing) {
  Start(2, false, true);
  ASSERT_TRUE(ut_txv_pipe_direct_chain(ctx_));

  LinkDownAndUp();

  EXPECT_EQ(ut_txv_pipe_done_while_held(ctx_), 0);
  for (int leg : {kP, kR}) {
    EXPECT_GE(ut_txv_pipe_accepted(ctx_, leg), Pkts(kAfterNs / 2) - Pkts(kFrameNs))
        << "leg " << leg;
    EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, leg), kOnTimeNs) << "leg " << leg;
  }
}

/* The link of P goes as the builder takes frame Y: the stalled ring of P fills with the
 * frame before, and never takes packet 0 of Y. R does, and holds the first pkts of Y
 * when the flag goes down just after. Only the last pkt of Y would count on it. */
TEST_F(St20TxLinkDownPipeTest, RedundantDirectChainFrameThatOnlyRStartedWaitsForItsRing) {
  Start(2, false, true);
  ASSERT_TRUE(ut_txv_pipe_direct_chain(ctx_));
  uint64_t pkt0 = 0;
  uint64_t now = ut_txv_pipe_run_to_next_frame(ctx_, kStepNs, &pkt0);
  ASSERT_GT(pkt0, now);

  ut_txv_pipe_set_flag_delay(ctx_, pkt0 - now + 2 * kStepNs);
  ut_txv_pipe_set_link(ctx_, kP, false);
  ut_txv_pipe_run(ctx_, kDownNs, kStepNs);
  ut_txv_pipe_set_flag_delay(ctx_, 0);
  ut_txv_pipe_set_link(ctx_, kP, true);
  ut_txv_pipe_mark(ctx_);
  ut_txv_pipe_run(ctx_, kAfterNs, kStepNs);

  EXPECT_EQ(ut_txv_pipe_done_while_held(ctx_), 0);
  for (int leg : {kP, kR}) {
    EXPECT_GE(ut_txv_pipe_accepted(ctx_, leg), Pkts(kAfterNs / 2) - Pkts(kFrameNs))
        << "leg " << leg;
    EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, leg), kOnTimeNs) << "leg " << leg;
  }
}

/* With 3 frames R may stop for the down: the stalled ring of P holds two frames, the ring
 * of R the last pkt of the third, which only a later burst frees. At the link up the
 * builder frees the done pkts of each leg, so a frame comes back and both legs go on, on
 * the frame grid. */
TEST_F(St20TxLinkDownPipeTest, RedundantSessionOfThreeFramesGoesOnAfterTheLinkIsBack) {
  Start(2, false, false, 3);

  LinkDownAndUp();

  EXPECT_EQ(ut_txv_pipe_done_while_held(ctx_), 0);
  for (int leg : {kP, kR}) {
    EXPECT_GE(ut_txv_pipe_accepted(ctx_, leg), Pkts(kAfterNs / 2) - Pkts(kFrameNs))
        << "leg " << leg;
    EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, leg), kOnTimeNs) << "leg " << leg;
  }
}

class St20TxLinkDownRtpTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

/* an RTP-level session holds no frame: it goes back to the epoch after the one before
 * once each link is up, and reads no PTP time for it each frame */
TEST_F(St20TxLinkDownRtpTest, RtpLevelSessionEndsTheDrainOnceTheLinkIsUp) {
  EXPECT_FALSE(ut_txv_rtp_link_drain(ctx_, false));
}

TEST_F(St20TxLinkDownRtpTest, RtpLevelSessionDrainsWhileTheLinkIsDown) {
  EXPECT_TRUE(ut_txv_rtp_link_drain(ctx_, true));
}

TEST_F(St20TxLinkDownRtpTest, RtpLevelSessionWithAFrameCountEndsTheDrainAfterTheUp) {
  EXPECT_FALSE(ut_txv_rtp_link_flap(ctx_, 3));
}

TEST_F(St20TxLinkDownRtpTest, MidFrameResyncDropsOldFrameAndResyncsNextFrameNoChain) {
  ut_txv_rtp_resync_result res = {};

  ASSERT_EQ(ut_txv_rtp_resync_mid_frame(ctx_, true, &res), 0);

  EXPECT_EQ(res.old_frame_pkts, 0u);
  EXPECT_EQ(res.new_frame_pkts, 4u);
  EXPECT_EQ(res.first_pkt_idx, 0u);
  EXPECT_GE(res.first_pkt_tsc, res.resync_tsc);
  EXPECT_TRUE(res.pool_clean);
}

TEST_F(St20TxLinkDownRtpTest, MidFrameResyncDropsOldFrameAndResyncsNextFrameChain) {
  ut_txv_rtp_resync_result res = {};

  ASSERT_EQ(ut_txv_rtp_resync_mid_frame(ctx_, false, &res), 0);

  EXPECT_EQ(res.old_frame_pkts, 0u);
  EXPECT_EQ(res.new_frame_pkts, 4u);
  EXPECT_EQ(res.first_pkt_idx, 0u);
  EXPECT_GE(res.first_pkt_tsc, res.resync_tsc);
  EXPECT_TRUE(res.pool_clean);
}

/* the stale pkts drain at the wire rate: none of the later pkts leaves behind them */
TEST_F(St20TxLinkDownPipeTest, NoPktLeavesLateWhenTheFlagGoesUpEarlyInThePollPeriod) {
  Start(1);
  ut_txv_pipe_set_link(ctx_, kP, false);
  ut_txv_pipe_run(ctx_, kDownNs, kStepNs);
  ut_txv_pipe_set_flag_delay(ctx_, 50000);
  ut_txv_pipe_set_link(ctx_, kP, true);
  ut_txv_pipe_mark(ctx_);

  ut_txv_pipe_run(ctx_, kAfterNs, kStepNs);

  ASSERT_GT(ut_txv_pipe_stale_at_up(ctx_, kP), kNbDesc / 2);
  EXPECT_GE(ut_txv_pipe_accepted(ctx_, kP), Pkts(kAfterNs) - 2 * Pkts(kFrameNs));
  EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, kP), kOnTimeNs);
}

/* the link of P is down past the hang threshold: no leg recovers, as a recovery flushes
 * the ring of P, and both legs go on on time after the up */
TEST_F(St20TxLinkDownPipeTest,
       RedundantReclaimSessionWaitsOverADownPastTheHangThreshold) {
  ut_txv_pipe_case c = Case(2);
  c.reclaim = true;
  Start(c);

  LinkDownAndUp(kHangNs + kDownNs);

  EXPECT_FALSE(ut_txv_zc_take_recovery(ctx_, kP));
  EXPECT_FALSE(ut_txv_zc_take_recovery(ctx_, kR));
  EXPECT_EQ(ut_txv_pipe_done_while_held(ctx_), 0);
  for (int leg : {kP, kR}) {
    EXPECT_GE(ut_txv_pipe_accepted(ctx_, leg), Pkts(kAfterNs / 2) - Pkts(kFrameNs))
        << "leg " << leg;
    EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, leg), kOnTimeNs) << "leg " << leg;
  }
}

}  // namespace
