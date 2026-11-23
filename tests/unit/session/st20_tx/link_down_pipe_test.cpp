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
 * stop is safe, and they leave late when the link is back, or never on an E610. The
 * frames they point into stay with the session, library, ext or direct chain frames
 * alike, until the ring frees the pkts, or a hang threshold after the up: then a dead
 * queue is recovered, and the last pkts of a live ring free nothing more.
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

#include <string>
#include <tuple>

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
  /* down for down_ns, then up with the ring of P dead: past the hang threshold after the
   * flag, then kAfterNs marked */
  void LinkDownAndUpDead(uint64_t down_ns = kDownNs) {
    ut_txv_pipe_set_link(ctx_, kP, false);
    ut_txv_pipe_run(ctx_, down_ns, kStepNs);
    ut_txv_pipe_set_link_up_dead(ctx_, kP);
    ut_txv_pipe_run(ctx_, kPollNs + kHangNs + kFrameNs, kStepNs);
    ut_txv_pipe_mark(ctx_);
    ut_txv_pipe_run(ctx_, kAfterNs, kStepNs);
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
  EXPECT_EQ(ut_txv_pipe_done_while_up_unsent(ctx_), 0);
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
  Start(2, true, true);
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
  Start(2, true, true);
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

  EXPECT_EQ(ut_txv_pipe_done_while_up_unsent(ctx_), 0);
  for (int leg : {kP, kR}) {
    EXPECT_GE(ut_txv_pipe_accepted(ctx_, leg), Pkts(kAfterNs / 2) - Pkts(kFrameNs))
        << "leg " << leg;
    EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, leg), kOnTimeNs) << "leg " << leg;
  }
}

/* ST 2022-7: P loses its link; from the resync on, R has its link and must go on at full
 * rate for the whole down, whatever the frame count and the order the app fills the
 * frames, then both legs resume once P's link returns. */
class St20TxRedundantLegTest
    : public St20TxLinkDownPipeTest,
      public ::testing::WithParamInterface<std::tuple<int, bool>> {};

TEST_P(St20TxRedundantLegTest, HealthyLegGoesOnWhileTheOtherLinkIsDown) {
  const int frames = std::get<0>(GetParam());
  ut_txv_pipe_case c = Case(2, frames);
  c.in_order = std::get<1>(GetParam());
  Start(c);
  ut_txv_pipe_set_link(ctx_, kP, false);
  /* the poll period, the frame the flag cuts, and the resync */
  ut_txv_pipe_run(ctx_, kFindNs + kFrameNs, kStepNs);
  ut_txv_pipe_mark(ctx_);

  constexpr uint64_t kLongDownNs = 1000000000;
  ut_txv_pipe_run(ctx_, kLongDownNs, kStepNs);

  EXPECT_GE(ut_txv_pipe_accepted(ctx_, kR), Pkts(kLongDownNs) - Pkts(kFrameNs))
      << "R sent " << ut_txv_pipe_accepted(ctx_, kR) << " of " << Pkts(kLongDownNs)
      << " pkts in 1 s, " << frames << " frames, in_order " << c.in_order;
  EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, kR), kOnTimeNs);
  EXPECT_EQ(ut_txv_pipe_done_while_up_unsent(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 0);

  ut_txv_pipe_set_link(ctx_, kP, true);
  ut_txv_pipe_run(ctx_, kFindNs + kFrameNs, kStepNs);
  ut_txv_pipe_mark(ctx_);
  ut_txv_pipe_run(ctx_, kAfterNs, kStepNs);
  for (int leg : {kP, kR}) {
    EXPECT_GE(ut_txv_pipe_accepted(ctx_, leg), Pkts(kAfterNs) - 2 * Pkts(kFrameNs))
        << "leg " << leg;
    EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, leg), kOnTimeNs) << "leg " << leg;
  }
}

INSTANTIATE_TEST_SUITE_P(Frames, St20TxRedundantLegTest,
                         ::testing::Combine(::testing::Values(2, 3, 4),
                                            ::testing::Bool()),
                         [](const ::testing::TestParamInfo<std::tuple<int, bool>>& info) {
                           return std::to_string(std::get<0>(info.param)) + "Frames" +
                                  (std::get<1>(info.param) ? "InOrder" : "AnyFree");
                         });

/* Direct-chain redundant session with 3 frames and in-order app: R keeps streaming while
 * P is down and never sees a frame released while R still holds unsent pkts of it. */
TEST_F(St20TxLinkDownPipeTest, RedundantDirectChainHealthyLegGoesOnWhileOtherLinkIsDown) {
  ut_txv_pipe_case c = Case(2, 3);
  c.iavf = true;
  c.in_order = true;
  Start(c);
  ASSERT_TRUE(ut_txv_pipe_direct_chain(ctx_));

  ut_txv_pipe_set_link(ctx_, kP, false);
  ut_txv_pipe_run(ctx_, kFindNs + kFrameNs, kStepNs);
  ut_txv_pipe_mark(ctx_);
  ut_txv_pipe_run(ctx_, kHangNs, kStepNs);

  EXPECT_GE(ut_txv_pipe_accepted(ctx_, kR), Pkts(kHangNs) - 2 * Pkts(kFrameNs));
  EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, kR), kOnTimeNs);
  EXPECT_EQ(ut_txv_pipe_done_while_up_unsent(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 0);

  ut_txv_pipe_set_link(ctx_, kP, true);
  ut_txv_pipe_run(ctx_, kFindNs + kFrameNs, kStepNs);
  ut_txv_pipe_mark(ctx_);
  ut_txv_pipe_run(ctx_, kAfterNs, kStepNs);
  EXPECT_EQ(ut_txv_pipe_done_while_up_unsent(ctx_), 0);
  for (int leg : {kP, kR}) {
    EXPECT_GE(ut_txv_pipe_accepted(ctx_, leg), Pkts(kAfterNs) - 2 * Pkts(kFrameNs))
        << "leg " << leg;
    EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, leg), kOnTimeNs) << "leg " << leg;
  }
}

/* Direct-chain redundant session: P drops link mid-frame (holding only non-last pkts),
 * library frames release so R keeps streaming, then session frees while P is still down.
 * P's per-slot pin must keep frame->addr allocated under P's live ring descriptors. */
TEST_F(St20TxLinkDownPipeTest, RedundantDirectChainSessionFreeWhileLinkDownKeepsBuffers) {
  ut_txv_pipe_case c = Case(2, 3);
  c.iavf = true;
  Start(c);
  ASSERT_TRUE(ut_txv_pipe_direct_chain(ctx_));

  uint64_t pkt0 = 0;
  uint64_t now = ut_txv_pipe_run_to_next_frame(ctx_, kStepNs, &pkt0);
  ASSERT_GT(pkt0, now);
  /* run just past pkt0 so P's ring holds only non-last pkts of this frame */
  ut_txv_pipe_run(ctx_, pkt0 - now + 4 * kStepNs, kStepNs);

  ut_txv_pipe_set_flag_delay(ctx_, 2 * kStepNs);
  ut_txv_pipe_set_link(ctx_, kP, false);
  /* run past up_drain_ns so library frames are retired and released for R */
  ut_txv_pipe_run(ctx_, 2 * kFrameNs, kStepNs);

  bool kept = false;
  bool fatal_marked = false;
  int held = ut_txv_pipe_uinit_ring_holds_freed(ctx_, kP, &kept, &fatal_marked);

  EXPECT_TRUE(kept);
  EXPECT_TRUE(fatal_marked);
  EXPECT_EQ(held, 0) << "direct-chain ring slots pointing into freed frame buffers";
}

/* Direct-chain redundant session (igc, safe tx_done_cleanup): P drops link holding only
 * non-last pkts of a frame, library frames release while P is down, and P returns with a
 * dead ring while app stops. P's pin on the retired sh_info keeps link_stale_sh busy so
 * tv_link_drain recovers P. */
TEST_F(St20TxLinkDownPipeTest,
       RedundantDirectChainDeadRingHoldingOnlyNonLastPktsIsRecovered) {
  ut_txv_pipe_case c = Case(2, 3);
  c.igc = true;
  c.in_order = true;
  Start(c);
  ASSERT_TRUE(ut_txv_pipe_direct_chain(ctx_));

  uint64_t pkt0 = 0;
  uint64_t now = ut_txv_pipe_run_to_next_frame(ctx_, kStepNs, &pkt0);
  ASSERT_GT(pkt0, now);
  /* start the frame on P, then drop link so ~48 unsent non-last pkts (> rs batch) stay
   * pinned in P's ring when the link-down flag arrives */
  uint64_t trs = (uint64_t)ut_txv_pacing_trs(ctx_);
  ut_txv_pipe_run(ctx_, pkt0 - now + 4 * trs, kStepNs);

  ut_txv_pipe_set_flag_delay(ctx_, 48 * trs);
  ut_txv_pipe_set_link(ctx_, kP, false);
  ut_txv_pipe_run(ctx_, 2 * kFrameNs, kStepNs);

  ut_txv_pipe_set_app_busy(ctx_, true);
  ut_txv_pipe_set_link_up_dead(ctx_, kP);
  ut_txv_pipe_run(ctx_, kHangNs + 3 * kFrameNs, kStepNs);

  EXPECT_EQ(ut_txv_pipe_recoveries(ctx_), 1);
  EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 0);
}

/* DPDK >= 26.07 skips tx_done_cleanup() on iavf/ice/i40e. A non-deep live ring with
 * free_thresh = 32 holding a partial batch (nb_free >= 32) frees 0 mbufs on an empty
 * reclaim burst; tv_link_free_done() must not falsely recover the live queue. */
TEST_F(St20TxLinkDownPipeTest,
       RedundantDirectChainLivePartiallyFilledIavfRingDoesNotFalseRecover) {
  ut_txv_pipe_case c = Case(2, 3);
  c.iavf = true;
  c.in_order = true;
  c.free_thresh = 32;
  c.nb_tx_desc[kP] = 512;
  c.nb_tx_desc[kR] = 512;
  Start(c);
  ASSERT_TRUE(ut_txv_pipe_direct_chain(ctx_));

  uint64_t pkt0 = 0;
  uint64_t now = ut_txv_pipe_run_to_next_frame(ctx_, kStepNs, &pkt0);
  ASSERT_GT(pkt0, now);
  uint64_t trs = (uint64_t)ut_txv_pacing_trs(ctx_);
  ut_txv_pipe_run(ctx_, pkt0 - now + 4 * trs, kStepNs);

  ut_txv_pipe_set_flag_delay(ctx_, 48 * trs);
  ut_txv_pipe_set_link(ctx_, kP, false);
  ut_txv_pipe_run(ctx_, 2 * kFrameNs, kStepNs);

  ut_txv_pipe_set_app_busy(ctx_, true);
  ut_txv_pipe_set_link(ctx_, kP, true);
  ut_txv_pipe_run(ctx_, kHangNs + 3 * kFrameNs, kStepNs);
  ut_txv_pipe_set_app_busy(ctx_, false);
  ut_txv_pipe_mark(ctx_);
  ut_txv_pipe_run(ctx_, kAfterNs, kStepNs);

  EXPECT_EQ(ut_txv_pipe_recoveries(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 0);
  for (int leg : {kP, kR}) {
    EXPECT_GE(ut_txv_pipe_accepted(ctx_, leg), Pkts(kAfterNs) - 2 * Pkts(kFrameNs))
        << "leg " << leg;
    EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, leg), kOnTimeNs) << "leg " << leg;
  }
}

/* TSN launch-time pacing bursts packets into the NIC ring ahead of their hardware launch
 * time (across vertical blanking). tx_link_release_tsc must wait past the last queued
 * launch time on surviving UP leg R, not just nb_tx_desc * trs. */
TEST_F(St20TxLinkDownPipeTest, RedundantTsnLinkResyncWaitsPastQueuedLaunchTime) {
  ut_txv_pipe_case c = Case(2);
  c.tsn = true;
  c.nb_tx_desc[kP] = 128;
  c.nb_tx_desc[kR] = 128;
  c.wire_ns = 1;
  Start(c);

  uint64_t pkt0 = 0;
  uint64_t now = ut_txv_pipe_run_to_next_frame(ctx_, kStepNs, &pkt0);
  /* TSN bursts next frame's packets right away while now is still > 1 ms before pkt0 */
  ASSERT_GT(pkt0, now + (uint64_t)(128 * ut_txv_pacing_trs(ctx_)));
  ut_txv_pipe_run(ctx_, 2 * kStepNs, kStepNs);

  ut_txv_pipe_set_flag_delay(ctx_, 1);
  ut_txv_pipe_set_link(ctx_, kP, false);
  ut_txv_pipe_run(ctx_, 3 * kStepNs, kStepNs);

  uint64_t release_tsc = ut_txv_pipe_link_release_tsc(ctx_);
  EXPECT_GE(release_tsc, pkt0) << "release_tsc " << release_tsc
                               << " earlier than queued TSN launch time " << pkt0;
}

/* EXT frames stay link_held while P is down, so tv_link_drain() must still reclaim done
 * descriptors on surviving UP leg R instead of returning early. */
TEST_F(St20TxLinkDownPipeTest, RedundantExtUpLegReclaimsDonePktsWhileSiblingIsDown) {
  Start(2, true);

  ut_txv_pipe_set_link(ctx_, kP, false);
  ut_txv_pipe_run(ctx_, kFindNs + 2 * kFrameNs, kStepNs);

  EXPECT_GT(ut_txv_pipe_done_cleanups(ctx_, kR), 0);
  EXPECT_EQ(ut_txv_pipe_done_while_held(ctx_), 0);
}

/* EXT frames are app-owned DMA memory: even on a redundant session, frames held by a
 * down leg stay held until that leg drains so the app never frees them under DMA. */
TEST_F(St20TxLinkDownPipeTest, RedundantExtFramesStayHeldWhileOtherLinkIsDown) {
  Start(2, true);

  LinkDownAndUp();

  EXPECT_EQ(ut_txv_pipe_done_while_held(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 0);
  for (int leg : {kP, kR}) {
    EXPECT_GE(ut_txv_pipe_accepted(ctx_, leg), Pkts(kAfterNs / 2) - Pkts(kFrameNs))
        << "leg " << leg;
    EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, leg), kOnTimeNs) << "leg " << leg;
  }
}

/* After a dead-queue recovery leaves mbufs on an old retired sh_info, a later live flap
 * on the other leg must not mistake that old dead slot for a new dead queue. */
TEST_F(St20TxLinkDownPipeTest, RedundantSecondFlapAfterDeadQueueDoesNotFalseRecover) {
  ut_txv_pipe_case c = Case(2);
  c.in_order = true;
  Start(c);

  LinkDownAndUpDead(kHangNs + kDownNs);
  ASSERT_EQ(ut_txv_pipe_recoveries(ctx_), 1);

  ut_txv_pipe_set_link(ctx_, kR, false);
  ut_txv_pipe_run(ctx_, kDownNs, kStepNs);
  ut_txv_pipe_set_link(ctx_, kR, true);
  ut_txv_pipe_run(ctx_, kPollNs + kHangNs + kFrameNs, kStepNs);
  ut_txv_pipe_mark(ctx_);
  ut_txv_pipe_run(ctx_, kAfterNs, kStepNs);

  EXPECT_EQ(ut_txv_pipe_recoveries(ctx_), 1);
  EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 0);
  for (int leg : {kP, kR}) {
    EXPECT_GE(ut_txv_pipe_accepted(ctx_, leg), Pkts(kAfterNs) - 2 * Pkts(kFrameNs))
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

TEST_F(St20TxLinkDownRtpTest, LinkResyncNotifiesAppWhenDrainingPacketRing) {
  EXPECT_GE(ut_txv_rtp_resync_notifies_app(ctx_), 1);
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

  EXPECT_EQ(ut_txv_pipe_first_alloc_fail(ctx_), 0u);
  EXPECT_EQ(ut_txv_pipe_recoveries(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_done_while_up_unsent(ctx_), 0);
  for (int leg : {kP, kR}) {
    EXPECT_GE(ut_txv_pipe_accepted(ctx_, leg), Pkts(kAfterNs / 2) - Pkts(kFrameNs))
        << "leg " << leg;
    EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, leg), kOnTimeNs) << "leg " << leg;
  }
}

/* The ring of P held pkts when it lost the link and never sends again, as an E610 ring:
 * the app, which fills its frames in turn as RxTxApp does, waits for a frame that ring
 * holds. A hang threshold after the up the session recovers the queue, and sends again.
 * The recovery takes the dead queue off the leg before it hands the frames back: their
 * chain mbufs there count on a retired sh_info. */
TEST_F(St20TxLinkDownPipeTest, DeadRingAfterTheLinkUpIsRecovered) {
  ut_txv_pipe_case c = Case(1);
  c.in_order = true;
  Start(c);

  LinkDownAndUpDead();

  EXPECT_EQ(ut_txv_pipe_recoveries(ctx_), 1);
  EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_done_while_held(ctx_), 0);
  EXPECT_GE(ut_txv_pipe_accepted(ctx_, kP), Pkts(kAfterNs) - 2 * Pkts(kFrameNs));
  EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, kP), kOnTimeNs);
}

/* the queue of the first recovery dies at the next link loss too: a frame both dead
 * queues hold counts on a third sh_info */
TEST_F(St20TxLinkDownPipeTest, SecondDeadRingIsRecoveredToo) {
  ut_txv_pipe_case c = Case(1);
  c.in_order = true;
  Start(c);
  LinkDownAndUpDead();

  LinkDownAndUpDead();

  EXPECT_EQ(ut_txv_pipe_recoveries(ctx_), 2);
  EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_done_while_held(ctx_), 0);
  EXPECT_GE(ut_txv_pipe_accepted(ctx_, kP), Pkts(kAfterNs) - 2 * Pkts(kFrameNs));
  EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, kP), kOnTimeNs);
}

/* The dead queue keeps its mbufs until the port stops, after the session free: the
 * sh_info of the frames they count on stay, and their late free calls no callback. */
TEST_F(St20TxLinkDownPipeTest, FramesOfADeadQueueStayAtTheSessionFree) {
  ut_txv_pipe_case c = Case(1);
  c.in_order = true;
  Start(c);
  LinkDownAndUpDead();
  ASSERT_EQ(ut_txv_pipe_recoveries(ctx_), 1);

  EXPECT_FALSE(ut_txv_pipe_free_frames(ctx_));
}

/* The app frees the session while the link is down. The ring is live: at the up it sends
 * what it holds. No slot it holds may point into a freed frame buffer, the frames array
 * stays, and the unflushed queue is marked fatal. */
TEST_F(St20TxLinkDownPipeTest, SessionFreeWhileTheLinkIsDownKeepsTheFramesTheRingHolds) {
  Start(1);
  ut_txv_pipe_set_link(ctx_, kP, false);
  ut_txv_pipe_run(ctx_, kDownNs, kStepNs);

  bool kept = false;
  bool fatal_marked = false;
  int held = ut_txv_pipe_uinit_ring_holds_freed(ctx_, kP, &kept, &fatal_marked);

  EXPECT_TRUE(kept);
  EXPECT_TRUE(fatal_marked);
  EXPECT_EQ(held, 0) << "ring slots that point into freed frame buffers";
}

/* Shared TX queue (TSQ) freed while link is down: frame buffers stay safe, and the
 * shared hardware queue must not be poisoned via mt_txq_fatal_error(). */
TEST_F(St20TxLinkDownPipeTest, SessionFreeWhileTheLinkIsDownDoesNotMarkSharedTxqFatal) {
  ut_txv_pipe_case c = Case(1);
  c.shared_txq = true;
  Start(c);
  ut_txv_pipe_set_link(ctx_, kP, false);
  ut_txv_pipe_run(ctx_, kDownNs, kStepNs);

  bool kept = false;
  bool fatal_marked = false;
  int held = ut_txv_pipe_uinit_ring_holds_freed(ctx_, kP, &kept, &fatal_marked);

  EXPECT_TRUE(kept);
  EXPECT_FALSE(fatal_marked);
  EXPECT_EQ(held, 0) << "ring slots that point into freed frame buffers";
}

/* IGC TSN LaunchTime uses queue 0 exclusively: freeing a TSN session while link is down
 * keeps frame buffers safe without permanently burning queue 0. */
TEST_F(St20TxLinkDownPipeTest, SessionFreeWhileTheLinkIsDownDoesNotMarkIgcTsnQueueFatal) {
  ut_txv_pipe_case c = Case(1);
  c.igc = true;
  c.tsn = true;
  Start(c);
  ut_txv_pipe_set_link(ctx_, kP, false);
  ut_txv_pipe_run(ctx_, kDownNs, kStepNs);

  bool kept = false;
  bool fatal_marked = false;
  int held = ut_txv_pipe_uinit_ring_holds_freed(ctx_, kP, &kept, &fatal_marked);

  EXPECT_TRUE(kept);
  EXPECT_FALSE(fatal_marked);
  EXPECT_EQ(held, 0) << "ring slots that point into freed frame buffers";
}

/* An empty ring (private header pool has zero mbufs in use) must not be marked fatal
 * when mt_txq_flush() returns -EBUSY solely because the link is down. */
TEST_F(St20TxLinkDownPipeTest, SessionFreeWhileTheLinkIsDownDoesNotMarkEmptyQueueFatal) {
  Start(1);
  ut_txv_pipe_set_link(ctx_, kP, false);
  ut_txv_pipe_run(ctx_, kDownNs, kStepNs);
  ut_txv_pipe_free_ring(ctx_, kP);

  bool kept = false;
  bool fatal_marked = false;
  int held = ut_txv_pipe_uinit_ring_holds_freed(ctx_, kP, &kept, &fatal_marked);

  EXPECT_FALSE(kept);
  EXPECT_FALSE(fatal_marked);
  EXPECT_EQ(held, 0);
}

/* While the link is UP, a -EBUSY flush on an otherwise empty dedicated ring indicates a
 * genuinely wedged queue and must still mark it fatal. */
TEST_F(St20TxLinkDownPipeTest, SessionFreeWhileLinkUpMarksHungEmptyQueueFatal) {
  Start(1);
  ut_txv_pipe_free_ring(ctx_, kP);

  bool kept = false;
  bool fatal_marked = false;
  int held = ut_txv_pipe_uinit_ring_holds_freed(ctx_, kP, &kept, &fatal_marked);

  EXPECT_FALSE(kept);
  EXPECT_TRUE(fatal_marked);
  EXPECT_EQ(held, 0);
}

/* After a sibling leg recovers while P is down, P's renewed header pool is empty while
 * its parked tx_link_stale_pool[P] still holds P's pre-recovery ring packets. Freeing
 * while P is down must still mark P's unflushed queue fatal. */
TEST_F(St20TxLinkDownPipeTest,
       SessionFreeWhileTheLinkIsDownMarksQueueFatalWhenStalePoolBusy) {
  ut_txv_pipe_case c = Case(2);
  Start(c);

  ut_txv_pipe_set_link(ctx_, kP, false);
  ut_txv_pipe_run(ctx_, kFindNs + kFrameNs, kStepNs);
  ut_txv_pipe_hang(ctx_, kR);
  ut_txv_pipe_run(ctx_, kHangNs + 3 * kFrameNs, kStepNs);
  ASSERT_EQ(ut_txv_pipe_recoveries(ctx_), 1);

  bool kept = false;
  bool fatal_marked = false;
  int held = ut_txv_pipe_uinit_ring_holds_freed(ctx_, kP, &kept, &fatal_marked);

  EXPECT_TRUE(kept);
  EXPECT_TRUE(fatal_marked);
  EXPECT_EQ(held, 0);
}

/* only the queue of P is dead: its recovery flushes the live ring of R */
TEST_F(St20TxLinkDownPipeTest, RedundantSessionRecoversOnlyTheDeadLeg) {
  ut_txv_pipe_case c = Case(2);
  c.in_order = true;
  c.reclaim = true;
  Start(c);

  LinkDownAndUpDead(kHangNs + kDownNs);

  EXPECT_EQ(ut_txv_pipe_recoveries(ctx_), 1);
  EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_done_while_up_unsent(ctx_), 0);
  for (int leg : {kP, kR}) {
    EXPECT_GE(ut_txv_pipe_accepted(ctx_, leg), Pkts(kAfterNs) - 2 * Pkts(kFrameNs))
        << "leg " << leg;
    EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, leg), kOnTimeNs) << "leg " << leg;
  }
}

/* R's ring hangs while P's link is down: R's recovery skips flushing down leg P, recovers
 * R without ST_EVENT_FATAL_ERROR, and R keeps sending until P's link returns. */
TEST_F(St20TxLinkDownPipeTest, RedundantLegRecoversWhileOtherLinkIsDown) {
  ut_txv_pipe_case c = Case(2);
  Start(c);

  ut_txv_pipe_set_link(ctx_, kP, false);
  ut_txv_pipe_run(ctx_, kFindNs + kFrameNs, kStepNs);
  ut_txv_pipe_hang(ctx_, kR);
  ut_txv_pipe_run(ctx_, kHangNs + 3 * kFrameNs, kStepNs);

  ut_txv_pipe_mark(ctx_);
  ut_txv_pipe_run(ctx_, kAfterNs, kStepNs);
  EXPECT_EQ(ut_txv_pipe_recoveries(ctx_), 1);
  EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_done_while_up_unsent(ctx_), 0);
  EXPECT_GE(ut_txv_pipe_accepted(ctx_, kR), Pkts(kAfterNs) - 2 * Pkts(kFrameNs));
  EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, kR), kOnTimeNs);

  ut_txv_pipe_set_link(ctx_, kP, true);
  ut_txv_pipe_run(ctx_, kFindNs + kFrameNs, kStepNs);
  ut_txv_pipe_mark(ctx_);
  ut_txv_pipe_run(ctx_, kAfterNs, kStepNs);
  EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_done_while_up_unsent(ctx_), 0);
  for (int leg : {kP, kR}) {
    EXPECT_GE(ut_txv_pipe_accepted(ctx_, leg), Pkts(kAfterNs) - 2 * Pkts(kFrameNs))
        << "leg " << leg;
    EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, leg), kOnTimeNs) << "leg " << leg;
  }
}

/* Same with EXT frames: P's link-held EXT frames stay held during R's recovery so P's
 * stalled ring never loses its frame buffers. */
TEST_F(St20TxLinkDownPipeTest, RedundantExtLegRecoversWhileOtherLinkIsDown) {
  ut_txv_pipe_case c = Case(2);
  c.ext = true;
  Start(c);

  ut_txv_pipe_set_link(ctx_, kP, false);
  ut_txv_pipe_run(ctx_, kFindNs + kFrameNs, kStepNs);
  ut_txv_pipe_hang(ctx_, kR);
  ut_txv_pipe_run(ctx_, kHangNs + 3 * kFrameNs, kStepNs);

  EXPECT_EQ(ut_txv_pipe_recoveries(ctx_), 1);
  EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_done_while_held(ctx_), 0);

  ut_txv_pipe_set_link(ctx_, kP, true);
  ut_txv_pipe_run(ctx_, kFindNs + kFrameNs, kStepNs);
  ut_txv_pipe_mark(ctx_);
  ut_txv_pipe_run(ctx_, kAfterNs, kStepNs);
  EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_done_while_held(ctx_), 0);
  for (int leg : {kP, kR}) {
    EXPECT_GE(ut_txv_pipe_accepted(ctx_, leg), Pkts(kAfterNs) - 2 * Pkts(kFrameNs))
        << "leg " << leg;
    EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, leg), kOnTimeNs) << "leg " << leg;
  }
}

/* Direct-chain: R's link flag goes down just before P hits its hang threshold (so R has
 * not resynced yet when P recovers on s_port 0). P recovers without stranding frames,
 * and when R later comes back with a dead ring, tv_link_drain() still detects and
 * recovers R. */
TEST_F(St20TxLinkDownPipeTest, RedundantDirectChainLegRecoversBeforeOtherLinkResyncs) {
  ut_txv_pipe_case c = Case(2);
  c.igc = true;
  Start(c);
  ASSERT_TRUE(ut_txv_pipe_direct_chain(ctx_));

  ut_txv_pipe_hang(ctx_, kP);
  /* stops on the first refused burst of P so the hang clock starts here */
  ut_txv_pipe_run(ctx_, kFrameNs, kStepNs);
  ASSERT_GT(ut_txv_pipe_first_burst_fail(ctx_), 0u);
  ut_txv_pipe_set_link(ctx_, kR, true);
  ut_txv_pipe_run(ctx_, kHangNs - 2 * kStepNs, kStepNs);
  ut_txv_pipe_set_flag_delay(ctx_, 1);
  ut_txv_pipe_set_link(ctx_, kR, false);
  ut_txv_pipe_run(ctx_, 3 * kFrameNs, kStepNs);

  EXPECT_EQ(ut_txv_pipe_recoveries(ctx_), 1);
  EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 0);

  ut_txv_pipe_set_link_up_dead(ctx_, kR);
  ut_txv_pipe_run(ctx_, kHangNs + 4 * kFrameNs, kStepNs);
  ut_txv_pipe_mark(ctx_);
  ut_txv_pipe_run(ctx_, kAfterNs, kStepNs);

  EXPECT_EQ(ut_txv_pipe_recoveries(ctx_), 2);
  EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_done_while_up_unsent(ctx_), 0);
  for (int leg : {kP, kR}) {
    EXPECT_GE(ut_txv_pipe_accepted(ctx_, leg), Pkts(kAfterNs) - 2 * Pkts(kFrameNs))
        << "leg " << leg;
    EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, leg), kOnTimeNs) << "leg " << leg;
  }
}

/* Same simultaneous trigger as above, except R self-drains while down (E610) and comes
 * back up live: dead P's shared sh_info must not keep tv_link_drain() stuck or falsely
 * declare live R dead. */
TEST_F(St20TxLinkDownPipeTest,
       RedundantLegRecoversBeforeOtherLinkResyncsAndLiveSiblingDoesNotFalseRecover) {
  ut_txv_pipe_case c = Case(2);
  Start(c);

  ut_txv_pipe_hang(ctx_, kP);
  ut_txv_pipe_run(ctx_, kFrameNs, kStepNs);
  ASSERT_GT(ut_txv_pipe_first_burst_fail(ctx_), 0u);
  ut_txv_pipe_set_link(ctx_, kR, true);
  ut_txv_pipe_run(ctx_, kHangNs - 2 * kStepNs, kStepNs);
  ut_txv_pipe_set_flag_delay(ctx_, 1);
  ut_txv_pipe_set_link(ctx_, kR, false);
  ut_txv_pipe_run(ctx_, 3 * kFrameNs, kStepNs);

  ASSERT_EQ(ut_txv_pipe_recoveries(ctx_), 1);
  /* R self-drains while down, app pauses, then R comes back up live */
  ut_txv_pipe_free_ring(ctx_, kR);
  ut_txv_pipe_set_app_busy(ctx_, true);
  ut_txv_pipe_set_link(ctx_, kR, true);
  ut_txv_pipe_run(ctx_, kHangNs + 4 * kFrameNs, kStepNs);
  ut_txv_pipe_set_app_busy(ctx_, false);
  ut_txv_pipe_mark(ctx_);
  ut_txv_pipe_run(ctx_, kAfterNs, kStepNs);

  EXPECT_EQ(ut_txv_pipe_recoveries(ctx_), 1);
  EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_done_while_up_unsent(ctx_), 0);
  for (int leg : {kP, kR}) {
    EXPECT_GE(ut_txv_pipe_accepted(ctx_, leg), Pkts(kAfterNs) - 2 * Pkts(kFrameNs))
        << "leg " << leg;
    EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, leg), kOnTimeNs) << "leg " << leg;
  }
}

/* Same with EXT frames: dead P keeps the EXT frame link_held while R self-drains down
 * and returns up with app paused. tv_link_held() must still notice R's empty stale pool
 * and mark R live instead of falsely recovering R. */
TEST_F(St20TxLinkDownPipeTest,
       RedundantExtLegRecoversBeforeOtherLinkResyncsAndLiveSiblingDoesNotFalseRecover) {
  ut_txv_pipe_case c = Case(2);
  c.ext = true;
  Start(c);

  ut_txv_pipe_hang(ctx_, kP);
  ut_txv_pipe_run(ctx_, kFrameNs, kStepNs);
  ASSERT_GT(ut_txv_pipe_first_burst_fail(ctx_), 0u);
  ut_txv_pipe_set_link(ctx_, kR, true);
  ut_txv_pipe_run(ctx_, kHangNs - 2 * kStepNs, kStepNs);
  ut_txv_pipe_set_flag_delay(ctx_, 1);
  ut_txv_pipe_set_link(ctx_, kR, false);
  ut_txv_pipe_run(ctx_, 3 * kFrameNs, kStepNs);

  ASSERT_EQ(ut_txv_pipe_recoveries(ctx_), 1);
  /* R self-drains while down, app pauses, then R comes back up live */
  ut_txv_pipe_free_ring(ctx_, kR);
  ut_txv_pipe_set_app_busy(ctx_, true);
  ut_txv_pipe_set_link(ctx_, kR, true);
  ut_txv_pipe_run(ctx_, kHangNs + 4 * kFrameNs, kStepNs);
  ut_txv_pipe_set_app_busy(ctx_, false);
  ut_txv_pipe_mark(ctx_);
  ut_txv_pipe_run(ctx_, kAfterNs, kStepNs);

  EXPECT_EQ(ut_txv_pipe_recoveries(ctx_), 1);
  EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_done_while_held(ctx_), 0);
  for (int leg : {kP, kR}) {
    EXPECT_GE(ut_txv_pipe_accepted(ctx_, leg), Pkts(kAfterNs) - 2 * Pkts(kFrameNs))
        << "leg " << leg;
    EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, leg), kOnTimeNs) << "leg " << leg;
  }
}

/* Copy mode (tx_no_chain): NIC rings hold no frame buffer references, so P recovering
 * while R is down and unresynced must release frames immediately so P keeps streaming. */
TEST_F(St20TxLinkDownPipeTest, RedundantCopyModeLegRecoversBeforeOtherLinkResyncs) {
  ut_txv_pipe_case c = Case(2, 2);
  c.copy = true;
  c.in_order = true;
  Start(c);

  ut_txv_pipe_hang(ctx_, kP);
  /* stops on the first refused burst of P so the hang clock starts here */
  ut_txv_pipe_run(ctx_, kFrameNs, kStepNs);
  ASSERT_GT(ut_txv_pipe_first_burst_fail(ctx_), 0u);
  ut_txv_pipe_set_link(ctx_, kR, true);
  ut_txv_pipe_run(ctx_, kHangNs - 2 * kStepNs, kStepNs);
  ut_txv_pipe_set_flag_delay(ctx_, 1);
  ut_txv_pipe_set_link(ctx_, kR, false);
  ut_txv_pipe_run(ctx_, 3 * kFrameNs, kStepNs);

  ut_txv_pipe_mark(ctx_);
  ut_txv_pipe_run(ctx_, kAfterNs, kStepNs);
  EXPECT_EQ(ut_txv_pipe_recoveries(ctx_), 1);
  EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 0);
  EXPECT_GE(ut_txv_pipe_accepted(ctx_, kP), Pkts(kAfterNs) - 2 * Pkts(kFrameNs));
  EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, kP), kOnTimeNs);
}

/* P loses link and comes back with a dead ring; while the session waits for P's hang
 * threshold, R keeps streaming free frames and flaps down and up live. R's resync must
 * preserve P's stale bit so P is still detected as dead and recovered. */
TEST_F(St20TxLinkDownPipeTest, StaggeredRedundantFlapStillRecoversDeadFirstLeg) {
  ut_txv_pipe_case c = Case(2);
  Start(c);

  ut_txv_pipe_set_link(ctx_, kP, false);
  ut_txv_pipe_run(ctx_, kPollNs + kFrameNs / 4, kStepNs);
  ut_txv_pipe_set_flag_delay(ctx_, 2 * kStepNs);
  ut_txv_pipe_set_link_up_dead(ctx_, kP);
  ut_txv_pipe_run(ctx_, kFrameNs / 4, kStepNs);

  ut_txv_pipe_set_app_busy(ctx_, true);
  ut_txv_pipe_set_link(ctx_, kR, false);
  ut_txv_pipe_run(ctx_, kFrameNs, kStepNs);
  ut_txv_pipe_set_link(ctx_, kR, true);
  ut_txv_pipe_run(ctx_, kHangNs + 3 * kFrameNs, kStepNs);
  ut_txv_pipe_set_app_busy(ctx_, false);
  ut_txv_pipe_mark(ctx_);
  ut_txv_pipe_run(ctx_, kAfterNs, kStepNs);

  EXPECT_EQ(ut_txv_pipe_recoveries(ctx_), 1);
  EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_done_while_up_unsent(ctx_), 0);
  for (int leg : {kP, kR}) {
    EXPECT_GE(ut_txv_pipe_accepted(ctx_, leg), Pkts(kAfterNs) - 2 * Pkts(kFrameNs))
        << "leg " << leg;
    EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, leg), kOnTimeNs) << "leg " << leg;
  }
}

/* An E610 ring frees what it held while the link is still down, and the app then has no
 * frame for a few frame times: the drain goes on until the link is up, so the next frame
 * starts on the next epoch after the time, not late, and the drop of its pkts at once
 * does not hand it back while the builder builds it. */
TEST_F(St20TxLinkDownPipeTest, LateAppWhileTheLinkIsStillDownStartsNoFrameLate) {
  Start(1);
  ut_txv_pipe_set_link(ctx_, kP, false);
  ut_txv_pipe_run(ctx_, kFindNs + kFrameNs, kStepNs);
  ut_txv_pipe_free_ring(ctx_, kP);
  ut_txv_pipe_run(ctx_, kFrameNs, kStepNs);
  ut_txv_pipe_set_app_busy(ctx_, true);
  ut_txv_pipe_run(ctx_, 3 * kFrameNs + kFrameNs / 2, kStepNs);
  ut_txv_pipe_set_app_busy(ctx_, false);
  ut_txv_pipe_run(ctx_, 3 * kFrameNs, kStepNs);
  ut_txv_pipe_set_link(ctx_, kP, true);
  ut_txv_pipe_mark(ctx_);

  ut_txv_pipe_run(ctx_, kAfterNs, kStepNs);

  EXPECT_EQ(ut_txv_pipe_late_starts(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_done_while_built(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_done_twice(ctx_), 0);
  EXPECT_GE(ut_txv_pipe_accepted(ctx_, kP), Pkts(kAfterNs / 2) - Pkts(kFrameNs));
  EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, kP), kOnTimeNs);
}

/* A live ring, as an E810 one, sends at the up what it held: past the hang threshold
 * after the up no leg recovers, and no frame came back while the ring held its pkts. */
TEST_F(St20TxLinkDownPipeTest, LiveRingPastTheHangThresholdAfterTheUpRecoversNothing) {
  Start(1);
  ut_txv_pipe_set_link(ctx_, kP, false);
  ut_txv_pipe_run(ctx_, kDownNs, kStepNs);
  ut_txv_pipe_set_link(ctx_, kP, true);
  ut_txv_pipe_run(ctx_, kPollNs + kHangNs + kFrameNs, kStepNs);
  ut_txv_pipe_mark(ctx_);

  ut_txv_pipe_run(ctx_, kAfterNs, kStepNs);

  EXPECT_EQ(ut_txv_pipe_recoveries(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_done_while_held(ctx_), 0);
  EXPECT_GE(ut_txv_pipe_accepted(ctx_, kP), Pkts(kAfterNs) - 2 * Pkts(kFrameNs));
  EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, kP), kOnTimeNs);
}

/* The app waits for the frame the ring held, as RxTxApp: with no later burst the live
 * ring keeps the last pkts of it, short of an rs batch, which the done cleanup does not
 * free. A hang threshold after the up the session hands the frame back and sends again.
 */
TEST_F(St20TxLinkDownPipeTest, FrameALiveRingKeepsIsHandedBackAHangThresholdAfterTheUp) {
  ut_txv_pipe_case c = Case(1);
  c.in_order = true;
  Start(c);
  ut_txv_pipe_set_link(ctx_, kP, false);
  ut_txv_pipe_run(ctx_, kDownNs, kStepNs);
  ut_txv_pipe_set_link(ctx_, kP, true);
  ut_txv_pipe_run(ctx_, kPollNs + kHangNs + kFrameNs, kStepNs);
  ut_txv_pipe_mark(ctx_);

  ut_txv_pipe_run(ctx_, kAfterNs, kStepNs);

  EXPECT_EQ(ut_txv_pipe_recoveries(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 0);
  EXPECT_GE(ut_txv_pipe_accepted(ctx_, kP), Pkts(kAfterNs) - 2 * Pkts(kFrameNs));
  EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, kP), kOnTimeNs);
}

/* The link goes in the gap between two frames, and the flag follows before the next
 * one: a leg with no pads holds only the last pkts of a frame, short of an rs batch, so
 * the NIC frees none after the up either. The leg is not taken as dead: a hang threshold
 * after the up the session hands the frame back, with those pkts on the wire but still
 * in the ring, takes no new queue, and sends again. */
class St20TxLinkLossBetweenFramesTest : public St20TxLinkDownPipeTest {
 protected:
  void LoseLink(bool ptp, bool sh_info_held, bool zero_rs_thresh = false) {
    ut_txv_pipe_case c = Case(1);
    c.in_order = true;
    c.ptp = ptp;
    c.zero_rs_thresh = zero_rs_thresh;
    Start(c);
    if (sh_info_held) ut_txv_pipe_hold_sh_info(ctx_);
    uint64_t pkt0 = 0;
    uint64_t now = ut_txv_pipe_run_to_next_frame(ctx_, kStepNs, &pkt0);
    ASSERT_GT(pkt0, now + kFrameNs / 50);
    ut_txv_pipe_run(ctx_, pkt0 - now - kFrameNs / 100, kStepNs);
    txq_gets_ = ut_txv_recovery_txq_gets(ctx_);

    ut_txv_pipe_set_flag_delay(ctx_, 2 * kStepNs);
    ut_txv_pipe_set_link(ctx_, kP, false);
    ut_txv_pipe_run(ctx_, kDownNs, kStepNs);
    ut_txv_pipe_set_link(ctx_, kP, true);
    ut_txv_pipe_run(ctx_, kPollNs + kHangNs + kFrameNs, kStepNs);
    ut_txv_pipe_mark(ctx_);
    ut_txv_pipe_run(ctx_, kAfterNs, kStepNs);

    ASSERT_LT(ut_txv_pipe_stale_at_up(ctx_, kP), UT_TXV_PIPE_RS_THRESH);
  }
  void Run(bool ptp, bool zero_rs_thresh = false) {
    ASSERT_NO_FATAL_FAILURE(LoseLink(ptp, false, zero_rs_thresh));
    EXPECT_EQ(ut_txv_pipe_recoveries(ctx_), 0);
    EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 0);
    EXPECT_EQ(ut_txv_recovery_txq_gets(ctx_), txq_gets_);
    EXPECT_EQ(ut_txv_pipe_done_while_unsent(ctx_), 0);
    EXPECT_EQ(ut_txv_pipe_done_twice(ctx_), 0);
    EXPECT_GE(ut_txv_pipe_accepted(ctx_, kP), Pkts(kAfterNs) - 2 * Pkts(kFrameNs));
    EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, kP), kOnTimeNs);
  }
  int txq_gets_ = 0;
};

TEST_F(St20TxLinkLossBetweenFramesTest, TscLegIsNotTakenAsDead) {
  Run(false);
}

TEST_F(St20TxLinkLossBetweenFramesTest, TscLegWithZeroDefaultRsThreshIsNotTakenAsDead) {
  Run(false, true);
}

TEST_F(St20TxLinkLossBetweenFramesTest, PtpLegIsNotTakenAsDead) {
  Run(true);
}

/* dead queues of earlier recoveries hold each other sh_info: the frame the ring keeps
 * cannot come back on a retired one, so the leg takes the dead path and its recovery
 * ends the session */
TEST_F(St20TxLinkLossBetweenFramesTest, LegWithNoFreeSh_infoEndsTheSession) {
  ASSERT_NO_FATAL_FAILURE(LoseLink(false, true));

  EXPECT_EQ(ut_txv_pipe_recoveries(ctx_), 1);
  EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 1);
  EXPECT_EQ(ut_txv_recovery_txq_gets(ctx_), txq_gets_);
}

/* The link goes in a frame, and the flag follows 16 pkts later: the dead ring holds an
 * rs batch of descriptors in less than an rs batch of header mbufs, as a chain pkt takes
 * two. A hang threshold after the up the session recovers the queue, and hands back no
 * frame while that ring holds pkts of it still to leave the wire. */
TEST_F(St20TxLinkDownPipeTest, DeadRingOfAnRsBatchInFewerPktsIsRecovered) {
  ut_txv_pipe_case c = Case(1);
  c.in_order = true;
  Start(c);
  uint64_t pkt0 = 0;
  uint64_t now = ut_txv_pipe_run_to_next_frame(ctx_, kStepNs, &pkt0);
  ut_txv_pipe_run(ctx_, pkt0 - now + kFrameNs / 2, kStepNs);
  ut_txv_pipe_set_flag_delay(ctx_, (uint64_t)(16 * ut_txv_pacing_trs(ctx_)));

  LinkDownAndUpDead();

  ASSERT_GE(ut_txv_pipe_stale_at_up(ctx_, kP), UT_TXV_PIPE_RS_THRESH);
  ASSERT_LT(ut_txv_pipe_stale_at_up(ctx_, kP), 2 * UT_TXV_PIPE_RS_THRESH);
  EXPECT_EQ(ut_txv_pipe_recoveries(ctx_), 1);
  EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_done_while_unsent(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_done_twice(ctx_), 0);
  EXPECT_GE(ut_txv_pipe_accepted(ctx_, kP), Pkts(kAfterNs) - 2 * Pkts(kFrameNs));
  EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, kP), kOnTimeNs);
}

/* Direct chain: the link of P goes while the last pkts of a frame wait in the
 * transmitter. The flag drops them before tv_link_resync() pins the frame. */
TEST_F(St20TxLinkDownPipeTest, DirectChainFrameWhoseLastPktsTheFlagDropsWaitsForItsRing) {
  Start(1, false, true);
  ASSERT_TRUE(ut_txv_pipe_direct_chain(ctx_));
  uint64_t pkt0 = 0, now = 0;
  for (int i = 0; i < 3; i++) now = ut_txv_pipe_run_to_next_frame(ctx_, kStepNs, &pkt0);
  ut_txv_pipe_run(ctx_, pkt0 + 15010000 - now, kStepNs);

  ut_txv_pipe_set_link(ctx_, kP, false);
  ut_txv_pipe_run(ctx_, 150000000, kStepNs);
  ut_txv_pipe_set_link(ctx_, kP, true);
  ut_txv_pipe_run(ctx_, 100000000, kStepNs);

  EXPECT_EQ(ut_txv_pipe_done_while_unsent(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_done_while_held(ctx_), 0);
}

/* DPDK 26.07 ci_tx_done_cleanup() double-frees mbufs on iavf, ice and i40e: a non-deep
 * iavf port must not call mt_txq_done_cleanup() during link resync or drain. */
TEST_F(St20TxLinkDownPipeTest, NonDeepIavfLinkDrainSkipsTxDoneCleanup) {
  Start(1, false, true);

  LinkDownAndUp();

  EXPECT_EQ(ut_txv_pipe_done_cleanups(ctx_, kP), 0);
  EXPECT_EQ(ut_txv_pipe_done_while_held(ctx_), 0);
  EXPECT_GE(ut_txv_pipe_accepted(ctx_, kP), Pkts(kAfterNs / 2) - Pkts(kFrameNs));
}

/* the done cleanup of a vector TX path frees nothing and returns -ENOTSUP: the leg is
 * live, not dead */
TEST_F(St20TxLinkDownPipeTest, LegWithNoDoneCleanupIsNotTakenAsDead) {
  ut_txv_pipe_case c = Case(1);
  c.in_order = true;
  c.cleanup_enotsup = true;
  Start(c);
  ut_txv_pipe_set_link(ctx_, kP, false);
  ut_txv_pipe_run(ctx_, kDownNs, kStepNs);
  ut_txv_pipe_set_link(ctx_, kP, true);
  ut_txv_pipe_run(ctx_, kPollNs + kHangNs + kFrameNs, kStepNs);
  ut_txv_pipe_mark(ctx_);

  ut_txv_pipe_run(ctx_, kAfterNs, kStepNs);

  EXPECT_EQ(ut_txv_pipe_recoveries(ctx_), 0);
  EXPECT_EQ(ut_txv_pipe_fatal_events(ctx_), 0);
  EXPECT_GE(ut_txv_pipe_accepted(ctx_, kP), Pkts(kAfterNs) - 2 * Pkts(kFrameNs));
}

}  // namespace
