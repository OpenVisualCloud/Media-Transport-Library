/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The production builder and TSC transmitter of a 1080p59.94 session through a link
 * down period, with the default 1 s hang threshold. When the transmitter finds the link
 * down, the session drops what it holds in software and starts its next frame on a new
 * epoch; while down it drops each pkt at its time. So the other leg of a redundant
 * session keeps sending, and no pkt leaves after its time (ST 2110-21 §6.6, ST 2022-7).
 * The pkts the NIC ring holds when the link goes down are the one residual: no queue
 * stop is safe, and they leave when the link is back.
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
/* the link read period while down, then 100 ms of frames */
constexpr uint64_t kAfterNs = 200000000;
constexpr uint64_t kFrameNs = 16683333;
/* the 100 ms of failed bursts before the link read, and the frame it cuts */
constexpr uint64_t kFindNs = 100000000 + kFrameNs;
/* the TSC transmitter sends a pkt within a run step of its time */
constexpr int64_t kOnTimeNs = 2 * kStepNs;

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
  void Start(int num_port) {
    ut_txv_pipe_case c = {};
    c.width = 1920;
    c.height = 1080;
    c.frames_cnt = 3;
    c.num_port = num_port;
    c.ring_mode = UT_TXR_FREE_ON_CLEANUP;
    c.free_thresh = 32;
    for (int i = 0; i < num_port; i++) c.nb_tx_desc[i] = kNbDesc;
    ASSERT_EQ(ut_txv_pipe_setup(ctx_, &c), 0);
    ut_txv_zc_set_hang_ns(ctx_, kHangNs);
    ut_txv_pipe_run(ctx_, kUpNs, kStepNs);
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

TEST_F(St20TxLinkDownPipeTest, RedundantLegKeepsItsTimeWhileTheOtherLinkIsDown) {
  Start(2);
  ut_txv_pipe_mark(ctx_);
  ut_txv_pipe_set_link(ctx_, kP, false);

  ut_txv_pipe_run(ctx_, kDownNs, kStepNs);

  EXPECT_LT(ut_txv_pipe_max_late_ns(ctx_, kR), kOnTimeNs);
  EXPECT_GE(ut_txv_pipe_accepted(ctx_, kR), Pkts(kDownNs - kFindNs) - Pkts(kFrameNs));
}

/* The frames the NIC ring of P held go back to the app at the link down. Their pkts
 * count on a retired sh_info: a frame is never done while a pkt that counts on its
 * sh_info is in a ring, and their late free at the link up does not end the next use. */
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

}  // namespace
