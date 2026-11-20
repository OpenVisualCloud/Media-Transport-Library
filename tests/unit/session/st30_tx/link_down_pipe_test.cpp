/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The production audio session and its manager transmitter, 1 ms PCM24 pkts on a shared
 * queue, through a link down period with the default 1 s hang threshold. When the
 * transmitter finds the link down, the sessions on that port drop what they hold and
 * start their next frame on a new epoch; while down it drops each pkt. So the other leg
 * of a redundant session keeps sending, and no pkt leaves after its time. The pkts the
 * NIC ring holds when the link goes down are the one residual.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St30TxLinkDownPipeTest.*'
 */

#include <gtest/gtest.h>

#include "common/ut_tx_ring.h"
#include "session/st30_tx_harness.h"

namespace {

constexpr int kP = 0;
constexpr int kR = 1;
constexpr uint16_t kNbDesc = 64;
constexpr uint64_t kStepNs = 1000;
constexpr uint64_t kHangNs = 1000000000;
constexpr uint64_t kUpNs = 50000000;
constexpr uint64_t kDownNs = 300000000;
/* the link read period while down, then 100 ms of pkts */
constexpr uint64_t kAfterNs = 200000000;
constexpr uint64_t kPktNs = 1000000;
/* 64 descriptors fill in 64 ms, then 100 ms of failed bursts before the link read */
constexpr uint64_t kFindNs = 170000000;
constexpr int64_t kOnTimeNs = 2 * kStepNs;

class St30TxLinkDownPipeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txa30_init(), 0);
    ctx_ = ut_txa30_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    EXPECT_EQ(ut_txa30_pipe_teardown(ctx_), 0);
    ut_txa30_destroy(ctx_);
  }
  void Start(int num_port, unsigned int mgr_ring = 0) {
    ut_txa30_pipe_case c = {};
    c.ptime = ST30_PTIME_1MS;
    c.pacing = UT_TXA30_PIPE_TSC_SHARED;
    c.num_port = num_port;
    c.ring_mode = UT_TXR_FREE_ON_CLEANUP;
    c.free_thresh = 32;
    for (int i = 0; i < num_port; i++) c.nb_tx_desc[i] = kNbDesc;
    c.mgr_ring = mgr_ring;
    ASSERT_EQ(ut_txa30_pipe_setup(ctx_, &c), 0);
    ut_txa30_set_hang_ns(ctx_, kHangNs);
    ut_txa30_pipe_run(ctx_, kUpNs, kStepNs);
  }
  ut_txa30_ctx* ctx_ = nullptr;
};

TEST_F(St30TxLinkDownPipeTest, NoPktLeavesAfterItsTimeOnceTheLinkIsBack) {
  Start(1);
  ut_txa30_pipe_set_link(ctx_, kP, false);
  ut_txa30_pipe_run(ctx_, kDownNs, kStepNs);
  ut_txa30_pipe_set_link(ctx_, kP, true);
  ut_txa30_pipe_mark(ctx_);

  ut_txa30_pipe_run(ctx_, kAfterNs, kStepNs);

  EXPECT_GT(ut_txa30_pipe_min_gap_ns(ctx_, kP), kPktNs / 2);
  EXPECT_GE(ut_txa30_pipe_accepted(ctx_, kP), kAfterNs / 2 / kPktNs - 1);
  EXPECT_LT(ut_txa30_pipe_max_late_ns(ctx_, kP), kOnTimeNs);
}

/* the residual: what the NIC ring held when the link went down, no more */
TEST_F(St30TxLinkDownPipeTest, OnlyTheNicRingLeavesLateWhenTheLinkIsBack) {
  Start(1);
  ut_txa30_pipe_set_link(ctx_, kP, false);
  ut_txa30_pipe_run(ctx_, kDownNs, kStepNs);
  ut_txa30_pipe_set_link(ctx_, kP, true);

  EXPECT_LT(ut_txa30_pipe_stale_at_up(ctx_, kP), kNbDesc);
}

/* a manager ring of 64 fills before the link read, and then holds the session builder */
TEST_F(St30TxLinkDownPipeTest, RedundantLegKeepsItsTimeWhileTheOtherLinkIsDown) {
  Start(2, 64);
  ut_txa30_pipe_mark(ctx_);
  ut_txa30_pipe_set_link(ctx_, kP, false);

  ut_txa30_pipe_run(ctx_, kDownNs, kStepNs);

  /* a late pkt leaves at once with its time set to now: it shows as a burst */
  EXPECT_GT(ut_txa30_pipe_min_gap_ns(ctx_, kR), kPktNs / 2);
  EXPECT_LT(ut_txa30_pipe_max_late_ns(ctx_, kR), kOnTimeNs);
  EXPECT_GE(ut_txa30_pipe_accepted(ctx_, kR), (kDownNs - kFindNs) / kPktNs - 1);
}

}  // namespace
