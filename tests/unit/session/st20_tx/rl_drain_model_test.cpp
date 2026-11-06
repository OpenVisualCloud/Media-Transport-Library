/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The RL drain model: when the NIC ends what a port's queue holds, a lower bound. A NIC
 * that drains a queue slower than the stream leaves the frame before still queued when
 * the next one's warm-up opens; the model keeps pre-arm from queueing its pads behind
 * it, and leaves the in-frame pads out while the queue is behind the schedule.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "session/st_video_transmitter_harness.h"

namespace {

constexpr int kPortP = 0;
constexpr long double kTrs = 1000.0L;
constexpr uint16_t kRsThresh = 32;

class St20TxRlDrainModel : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
    ctx_ = ut_trs_create();
    ASSERT_NE(nullptr, ctx_);
    /* a pad drains in one trs, on a port whose PMD frees in rs batches, as a deep
     * queue's does */
    ut_trs_set_trs(ctx_, kTrs);
    ut_trs_set_drain(ctx_, kPortP, kTrs);
    ut_trs_set_pmd_done_batches(ctx_, kRsThresh, 0, 0);
    ASSERT_TRUE(ut_trs_set_drain_model(ctx_, true));
  }

  void TearDown() override {
    ut_trs_destroy(ctx_);
  }

  void At(uint64_t tsc) {
    tsc_ = tsc;
    ut_trs_set_mock_tsc_script(ctx_, &tsc_, 1);
  }

  /* a burst of tx pkts returning at tsc */
  void Burst(uint64_t tsc, uint16_t tx, bool ring_full) {
    At(tsc);
    ut_trs_drain_after_burst(ctx_, kPortP, tx, ring_full);
  }

  /* pre-arm 30 us before the target, with 2 us of credit: 32 pads on an idle shaper */
  void PreArmFrom(uint64_t now) {
    ut_trs_set_credit(ctx_, kPortP, 2000.0L);
    ut_trs_set_warm_pkts_cap(ctx_, 30);
    ut_trs_set_target_tsc(ctx_, kTargetTsc);
    At(now);
  }

  static constexpr uint64_t kTargetTsc = 40000;
  ut_trs_ctx* ctx_ = nullptr;
  uint64_t tsc_ = 0;
};

/* the PMD of a shallow queue frees as it sends, which leaves a full ring's time unknown;
 * a port without a plan pre-arms nothing */
TEST_F(St20TxRlDrainModel, OnlyAnArmedDeepPortRunsTheModel) {
  ut_trs_set_pmd_done_batches(ctx_, 0, 0, 0);
  EXPECT_FALSE(ut_trs_drain_init(ctx_, kPortP));
  ut_trs_set_pmd_done_batches(ctx_, kRsThresh, 0, 0);
  EXPECT_TRUE(ut_trs_drain_init(ctx_, kPortP));
  ut_trs_set_drain(ctx_, kPortP, 0);
  EXPECT_FALSE(ut_trs_drain_init(ctx_, kPortP));
}

/* a pad every 5 pkts: the shaper sends 6 pkts in 5 trs, unless its pads drain faster */
TEST_F(St20TxRlDrainModel, APktCountsTheFrameMeanOrThePadTimeIfLess) {
  ut_trs_set_pad_interval(ctx_, 5.0f);
  ASSERT_TRUE(ut_trs_set_drain_model(ctx_, true));
  EXPECT_DOUBLE_EQ(1000.0 * 5 / 6, ut_trs_drain_pkt_ns(ctx_, kPortP));

  ut_trs_set_drain(ctx_, kPortP, 700.0L);
  ASSERT_TRUE(ut_trs_set_drain_model(ctx_, true));
  EXPECT_DOUBLE_EQ(700.0, ut_trs_drain_pkt_ns(ctx_, kPortP));
}

/* A third of the pkts line tails of a quarter pad: the shaper drains a pkt in about 3/4
 * of a NORMAL one's time, under the frame mean of a pad every 5 pkts. */
TEST_F(St20TxRlDrainModel, LineTailsShortenThePktTime) {
  const double pad = ut_trs_pad_size(), tail = ut_trs_pad_size() / 4;
  ut_trs_set_pad_interval(ctx_, 5.0f);
  ut_trs_set_line_tails(ctx_, 200, 100, (uint32_t)tail);
  ASSERT_TRUE(ut_trs_set_drain_model(ctx_, true));
  EXPECT_DOUBLE_EQ(1000.0 * (200 * pad + 100 * tail) / (300 * pad),
                   ut_trs_drain_pkt_ns(ctx_, kPortP));
}

TEST_F(St20TxRlDrainModel, AcceptedBurstAddsItsShaperTime) {
  Burst(5000, 3, false);
  EXPECT_EQ(5000u + 3 * 1000, ut_trs_drain_tsc(ctx_, kPortP));

  /* queued behind the pkts still draining */
  Burst(6000, 1, false);
  EXPECT_EQ(9000u, ut_trs_drain_tsc(ctx_, kPortP));
}

/* an idle shaper sends its bucket at once: a burst on it starts that credit back */
TEST_F(St20TxRlDrainModel, AnIdleShaperStartsABurstItsCreditBack) {
  ut_trs_set_credit(ctx_, kPortP, 2000.0L);
  Burst(5000, 3, false);
  EXPECT_EQ(3000u + 3 * 1000, ut_trs_drain_tsc(ctx_, kPortP));
}

/* the deep ring less three rs batches, two descriptors a pkt: 976 pkts */
TEST_F(St20TxRlDrainModel, RefusedBurstEndsAFullRingFromNow) {
  const uint64_t ring_ns = (ut_trs_deep_nb_tx_desc() - 3 * kRsThresh) / 2 * 1000;
  ASSERT_EQ(ring_ns, ut_trs_drain_ring_ns(ctx_, kPortP));

  Burst(5000, 2, true);
  EXPECT_EQ(5000 + ring_ns, ut_trs_drain_tsc(ctx_, kPortP));

  /* a burst the NIC took none of finds the ring full as well */
  Burst(7000, 0, true);
  EXPECT_EQ(7000 + ring_ns, ut_trs_drain_tsc(ctx_, kPortP));
}

TEST_F(St20TxRlDrainModel, AFullRingNeverMovesTheDrainEarlier) {
  constexpr uint64_t kLater = 5000000;
  ut_trs_set_drain_tsc(ctx_, kPortP, kLater);
  Burst(5000, 2, true);
  EXPECT_EQ(kLater, ut_trs_drain_tsc(ctx_, kPortP));
}

/* The 4-pkt slice of packet 0 has a pad due after it, when the end is past pkt 4's
 * target by more than a trs: an end 1 trs past packet 0's target leaves it exactly
 * there after the slice, so the pad goes; a ns more leaves it out. */
TEST_F(St20TxRlDrainModel, InFramePadIsLeftOutOnlyPastATrsOfSlack) {
  constexpr uint64_t kFrameTsc = 100000;
  ut_trs_set_pad_interval(ctx_, 4.0f);
  ASSERT_TRUE(ut_trs_set_drain_model(ctx_, true));
  ut_trs_set_stream_target(ctx_, kFrameTsc);
  const uint64_t slice = (uint64_t)(4 * ut_trs_drain_pkt_ns(ctx_, kPortP));
  const uint64_t kept = kFrameTsc + 5 * (uint64_t)kTrs - slice;
  At(50000);
  ut_trs_set_drain_tsc(ctx_, kPortP, kept);
  EXPECT_EQ(1u, ut_trs_rl_slice(ctx_, 0, 4));
  EXPECT_EQ(0u, ut_trs_pads_skipped(ctx_));

  At(50000);
  ut_trs_set_drain_tsc(ctx_, kPortP, kept + 1);
  EXPECT_EQ(0u, ut_trs_rl_slice(ctx_, 0, 4));
  EXPECT_EQ(1u, ut_trs_pads_skipped(ctx_));
}

TEST_F(St20TxRlDrainModel, OffTheModelTracksNothing) {
  ut_trs_set_drain_model(ctx_, false);
  Burst(5000, 2, true);
  Burst(6000, 3, false);
  EXPECT_EQ(0u, ut_trs_drain_tsc(ctx_, kPortP));
}

/* the frame before ends 2 us from now: the pads end at the target behind it, with no
 * credit as the shaper is busy */
TEST_F(St20TxRlDrainModel, PreArmPadsStartAtTheDrainEnd) {
  PreArmFrom(10000);
  ut_trs_set_drain_tsc(ctx_, kPortP, 12000);
  EXPECT_TRUE(ut_trs_pre_arm(ctx_));
  EXPECT_EQ(28u, ut_trs_pad_send_count(ctx_));
}

/* 27.5 pads to the target: the plan cuts the last ones, which the model counts whole, so
 * after pre-arm the bound is the plan's end */
TEST_F(St20TxRlDrainModel, PreArmLeavesItsPlanAsTheBound) {
  PreArmFrom(10000);
  ut_trs_set_drain_tsc(ctx_, kPortP, 12500);
  EXPECT_TRUE(ut_trs_pre_arm(ctx_));
  EXPECT_EQ(28u, ut_trs_pad_send_count(ctx_));
  EXPECT_NEAR((double)kTargetTsc, (double)ut_trs_drain_tsc(ctx_, kPortP), 10.0);
}

/* a refused pad leaves the ring full: pre-arm keeps the full ring's end, past its plan */
TEST_F(St20TxRlDrainModel, PreArmWithARefusedPadKeepsTheFullRingEnd) {
  PreArmFrom(10000);
  ut_trs_set_drain_tsc(ctx_, kPortP, 12500);
  ut_trs_set_burst_accept_limit(ctx_, 4);
  ut_trs_pre_arm(ctx_);
  EXPECT_GE(ut_trs_drain_tsc(ctx_, kPortP), 10000 + ut_trs_drain_ring_ns(ctx_, kPortP));
}

/* 10 us left are too few pads to trim packet 0 onto its target: it is held for it */
TEST_F(St20TxRlDrainModel, PreArmHoldsPacket0WhenTooFewPadsFitBehindTheDrain) {
  PreArmFrom(10000);
  ut_trs_set_drain_tsc(ctx_, kPortP, 30000);
  EXPECT_FALSE(ut_trs_pre_arm(ctx_));
  EXPECT_EQ(0u, ut_trs_pad_send_count(ctx_));
}

/* a queue that drains past the target takes no pads ahead of packet 0, which goes right
 * behind it */
TEST_F(St20TxRlDrainModel, PreArmQueuesNoPadsBehindADrainPastTheTarget) {
  PreArmFrom(10000);
  ut_trs_set_drain_tsc(ctx_, kPortP, 50000);
  ut_trs_pre_arm(ctx_);
  EXPECT_EQ(0u, ut_trs_pad_send_count(ctx_));
}

TEST_F(St20TxRlDrainModel, PreArmIgnoresADrainAlreadyOver) {
  PreArmFrom(10000);
  ut_trs_set_drain_tsc(ctx_, kPortP, 9000);
  EXPECT_TRUE(ut_trs_pre_arm(ctx_));
  EXPECT_EQ(32u, ut_trs_pad_send_count(ctx_));
}

/* frames mode: 70-pkt frames, a pad due every 5 pkts, one trs a pad */
class St20TxRlDrainFrames : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
  }
  void TearDown() override {
    for (ut_trs_ctx* ctx : ctxs_) ut_trs_destroy(ctx);
  }

  /* the wire of the run, empty if it stopped short */
  std::vector<uint32_t> Run(uint64_t hold_ns, bool model) {
    ut_trs_frames_case c = {};
    c.bulk = 4;
    c.total_pkts = 70;
    c.frames = 2;
    c.pad_interval = 5.0f;
    c.hold_ns = hold_ns;
    /* an armed plan, as the model needs, without a warm-up: packet 0 waits for its
     * target */
    c.drain_ns = 1000;
    return Run(c, model);
  }

  std::vector<uint32_t> Run(ut_trs_frames_case c, bool model) {
    ut_trs_ctx* ctx = ut_trs_create();
    if (!ctx) return {};
    ctxs_.push_back(ctx);
    /* the model runs on a deep queue: both runs have one */
    ut_trs_set_pmd_done_batches(ctx, kRsThresh, 0, 0);
    c.drain_model = model;
    if (ut_trs_run_frames(ctx, &c) < 0) return {};
    std::vector<uint32_t> wire;
    for (uint32_t i = 0; i < ut_trs_wire_count(ctx); i++)
      wire.push_back(ut_trs_wire(ctx, i));
    return wire;
  }

  static long Pads(const std::vector<uint32_t>& wire) {
    return std::count(wire.begin(), wire.end(), UT_TRS_WIRE_PAD);
  }
  static std::vector<uint32_t> Pkts(std::vector<uint32_t> wire) {
    wire.erase(std::remove(wire.begin(), wire.end(), UT_TRS_WIRE_PAD), wire.end());
    return wire;
  }
  uint64_t Skipped() const {
    return ut_trs_pads_skipped(ctxs_.back());
  }

  /* packet 0 due well after its bulk is queued, past the 70 pkts and 14 pads of the
   * frame before at the harness's 1 us trs: each frame starts behind an empty queue */
  static constexpr uint64_t kHoldNs = 200000;
  std::vector<ut_trs_ctx*> ctxs_;
};

TEST_F(St20TxRlDrainFrames, FramesOnScheduleKeepTheirPads) {
  std::vector<uint32_t> off = Run(kHoldNs, false);
  ASSERT_FALSE(off.empty());
  ASSERT_GT(Pads(off), 0);
  EXPECT_EQ(off, Run(kHoldNs, true));
  EXPECT_EQ(0u, Skipped());
}

/* Packet 0 pre-armed, entered late so that it leaves with the last pads in one call: the
 * bound follows the plan, which counts the cut pads by their bytes, so no pad is left
 * out of frames on schedule. */
TEST_F(St20TxRlDrainFrames, PreArmedFramesKeepTheirPads) {
  ut_trs_frames_case c = {};
  c.bulk = 4;
  c.total_pkts = 70;
  c.frames = 2;
  c.pad_interval = 5.0f;
  c.hold_ns = kHoldNs;
  c.drain_ns = 1000;
  c.credit_ns = 1500;
  c.absorb_ns = 2000;
  c.warm_pkts = 2;
  std::vector<uint32_t> off = Run(c, false);
  ASSERT_FALSE(off.empty());
  ASSERT_EQ(2u, ut_trs_mixed_calls(ctxs_.back()));
  EXPECT_EQ(off, Run(c, true));
  EXPECT_EQ(2u, ut_trs_mixed_calls(ctxs_.back()));
  EXPECT_EQ(0u, Skipped());
}

/* packet 0 long past due: the queue is behind, so the in-frame pads give their time to
 * the pkts, which keep their order */
TEST_F(St20TxRlDrainFrames, LateFramesLeaveTheirInFramePadsOut) {
  std::vector<uint32_t> off = Run(0, false);
  ASSERT_FALSE(off.empty());
  std::vector<uint32_t> on = Run(0, true);
  ASSERT_FALSE(on.empty());
  EXPECT_LT(Pads(on), Pads(off));
  EXPECT_EQ((uint64_t)(Pads(off) - Pads(on)), Skipped());
  EXPECT_EQ(Pkts(off), Pkts(on));
}

}  // namespace
