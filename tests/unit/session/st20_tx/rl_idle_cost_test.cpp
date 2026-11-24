/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "session/st20_tx/rl_vrx_model.h"
#include "session/st20_tx_harness.h"
#include "session/st_video_transmitter_harness.h"

/* What a transmitter call costs beyond its bursts: the clock reads, the NIC calls and the
 * session lookups, for 1, 13 and 60 sessions on reclaiming RL ports, idle or sending the
 * one bulk the builder added. Counts only: the time of each is not judged. */
namespace {

constexpr int kPortP = 0;
constexpr uint64_t kTargetTsc = 100000000;
constexpr uint64_t kCallNs = 2000;
constexpr int kCallCap = 1000;
constexpr uint64_t kStallNs = 1000000; /* 16 batch times */
constexpr uint64_t kAnchorStallNs = 3000;
constexpr int kWaitWarmup = 1; /* ST_TX_VIDEO_RL_STATE_WAIT_WARMUP */

class St20TxRlIdleCost : public ::testing::TestWithParam<int> {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
    const float trained[1] = {164};
    ut_txv_ctx* txv = ut_txv_create();
    ASSERT_NE(nullptr, txv);
    ut_txv_set_rl_format(txv, 1920, 1080, false, ST_FPS_P59_94);
    ASSERT_EQ(0, ut_txv_init_rl_pacing(txv, 1, 0, false, trained, false));
    ASSERT_TRUE(ut_txv_rl_armed(txv, kPortP));
    for (int i = 0; i < GetParam(); i++) {
      ut_trs_ctx* c = ut_trs_create();
      ASSERT_NE(nullptr, c);
      ut_trs_set_trs(c, ut_txv_pacing_trs(txv));
      ut_trs_set_warm_pkts_cap(c, ut_txv_pacing_warm_pkts(txv));
      ut_trs_set_drain(c, kPortP, ut_txv_rl_drain(txv, kPortP));
      ut_trs_set_credit(c, kPortP, ut_txv_rl_credit(txv, kPortP));
      EXPECT_EQ(0, ut_trs_set_pad_sizes(c, ut_txv_normal_size(txv),
                                        ut_txv_rl_pad_size(txv, kPortP)));
      ut_trs_set_mock_tsc_script(c, &now_, 1);
      ut_trs_set_pmd_done_batches(c, kUtRlRsThresh, 0, 0);
      ctx_.push_back(c);
    }
    window_ns_ = (uint64_t)(ut_txv_pacing_warm_pkts(txv) * ut_txv_pacing_trs(txv));
    ut_txv_destroy(txv);
    sch_ = ut_trs_sch_create(ctx_.data(), (int)ctx_.size(), &now_);
    ASSERT_NE(nullptr, sch_);
  }

  void TearDown() override {
    ut_trs_sch_destroy(sch_);
    for (ut_trs_ctx* c : ctx_) ut_trs_destroy(c);
  }

  /* the NIC calls of all the ports so far: bursts, and the empty ones of the reclaim */
  uint32_t NicCalls() const {
    uint32_t calls = 0;
    for (ut_trs_ctx* c : ctx_)
      calls += ut_trs_burst_call_count(c) + ut_trs_reclaim_calls(c);
    return calls;
  }

  /* Calls the transmitter every kCallNs, after a first call that dequeues what the rings
   * hold, until a call makes no NIC call: past the calls that catch up the ring's
   * batches, between two reclaims. Returns the NIC calls of the last call. */
  uint32_t IdleCall() {
    ut_trs_sch_call(sch_);
    uint32_t calls = 1;
    for (int call = 0; call < kCallCap && calls; call++) {
      const uint32_t before = NicCalls();
      now_ += kCallNs;
      ut_trs_sch_call(sch_);
      calls = NicCalls() - before;
    }
    return calls;
  }

  uint64_t now_ = kTargetTsc - 50000000;
  uint64_t window_ns_ = 0;
  std::vector<ut_trs_ctx*> ctx_;
  ut_trs_sch* sch_ = nullptr;
};

/* packet 0 dequeued, its window not open: the visit reads the clock to test the window,
 * and the reclaim is timed by that read */
TEST_P(St20TxRlIdleCost, APortWaitingForItsWindowReadsTheClockOnce) {
  for (ut_trs_ctx* c : ctx_) ut_trs_enqueue_first_pkt(c, kTargetTsc);
  ASSERT_EQ(0u, IdleCall());
  for (ut_trs_ctx* c : ctx_) ASSERT_EQ(0u, ut_trs_real_send_count(c));
  EXPECT_EQ(1u + ctx_.size(), ut_trs_sch_tsc_reads(sch_)) << "the call's and one a port";
  EXPECT_EQ(ctx_.size(), ut_trs_sch_dp_gets(sch_)) << "the visits' only";
}

/* the builder ring empty: no burst, and one clock read, which times the reclaim */
TEST_P(St20TxRlIdleCost, APortWithAnEmptyRingReadsTheClockOnce) {
  ASSERT_EQ(0u, IdleCall());
  EXPECT_EQ(1u + ctx_.size(), ut_trs_sch_tsc_reads(sch_)) << "the call's and one a port";
  EXPECT_EQ(ctx_.size(), ut_trs_sch_dp_gets(sch_)) << "the visits' only";
}

/* a port that does not reclaim reads no clock for an empty ring */
TEST_P(St20TxRlIdleCost, APortWithNoRsThreshReadsNoClock) {
  for (ut_trs_ctx* c : ctx_) ut_trs_set_pmd_done_batches(c, 0, 0, 0);
  ASSERT_EQ(0u, IdleCall());
  EXPECT_EQ(1u, ut_trs_sch_tsc_reads(sch_)) << "the call's only";
  EXPECT_EQ(ctx_.size(), ut_trs_sch_dp_gets(sch_)) << "the visits' only";
}

TEST_P(St20TxRlIdleCost, AnSt22PortReadsNoClock) {
  for (ut_trs_ctx* c : ctx_) ut_trs_set_st22(c);
  ASSERT_EQ(0u, IdleCall());
  EXPECT_EQ(1u, ut_trs_sch_tsc_reads(sch_)) << "the call's only";
  EXPECT_EQ(ctx_.size(), ut_trs_sch_dp_gets(sch_)) << "the visits' only";
}

/* a port that does not reclaim, waiting to start over after its stalled anchor, reads
 * the clock for the window and the pre-arm's entry only */
TEST_P(St20TxRlIdleCost, APortWithNoRsThreshWaitingToStartOverReadsNoClockToReclaim) {
  for (ut_trs_ctx* c : ctx_) ut_trs_set_pmd_done_batches(c, 0, 0, 0);
  ut_trs_enqueue_first_pkt(ctx_[0], kTargetTsc);
  ut_trs_set_frame_burst_stall(ctx_[0], 1, kAnchorStallNs, false);
  ASSERT_EQ(0u, IdleCall());
  now_ = kTargetTsc - window_ns_ + kCallNs;
  ut_trs_sch_call(sch_);
  ASSERT_EQ(1u, ut_trs_stat_recalculate_warmup(ctx_[0])) << "the anchor stalled";
  now_ += kCallNs;
  const uint32_t before = NicCalls();
  ut_trs_sch_call(sch_);
  ASSERT_EQ(kWaitWarmup, ut_trs_rl_state(ctx_[0])) << "waits to start over";
  EXPECT_EQ(before, NicCalls());
  EXPECT_EQ(ctx_.size() + 4, ut_trs_sch_tsc_reads(sch_))
      << "the call's, a release scan's before each visit and after the last, the "
         "window's and the pre-arm's";
}

/* mid-frame behind a full NIC TX queue: the retry is one burst and the clock read that
 * times the refusal, and it makes no empty burst */
TEST_P(St20TxRlIdleCost, APortBehindAFullNicQueueRetriesInOneBurst) {
  for (ut_trs_ctx* c : ctx_) {
    ut_trs_set_inflight_num(c, 1);
    ut_trs_set_burst_force_fail(c, true);
  }
  ut_trs_sch_call(sch_);
  const uint32_t before = NicCalls();
  now_ += kCallNs;
  ut_trs_sch_call(sch_);
  EXPECT_EQ(ctx_.size(), NicCalls() - before);
  for (ut_trs_ctx* c : ctx_) EXPECT_EQ(0u, ut_trs_reclaim_calls(c));
  EXPECT_EQ(1u + ctx_.size(), ut_trs_sch_tsc_reads(sch_)) << "the call's and one a port";
  EXPECT_EQ(ctx_.size(), ut_trs_sch_dp_gets(sch_)) << "the visits' only";
}

/* A scheduler that falls behind sends each port the bulk its builder just added, and
 * finds the ring empty behind it: the burst is stamped by one clock read, and the reclaim
 * reads none, as no batch comes due within a tick of that stamp. */
TEST_P(St20TxRlIdleCost, APortThatSentItsOnlyBulkReadsTheClockOnce) {
  ASSERT_EQ(0u, IdleCall());
  /* to the call that reclaims: the next batch is due a batch time on */
  uint32_t reclaims = 0;
  for (int call = 0; call < kCallCap && !reclaims; call++) {
    const uint32_t before = NicCalls();
    now_ += kCallNs;
    ut_trs_sch_call(sch_);
    reclaims = NicCalls() - before;
  }
  ASSERT_EQ(ctx_.size(), reclaims);
  for (ut_trs_ctx* c : ctx_) ut_trs_enqueue_mid_pkt(c);
  const uint32_t before = NicCalls();
  now_ += kCallNs;
  ut_trs_sch_call(sch_);
  EXPECT_EQ(ctx_.size(), NicCalls() - before) << "the bursts, no empty one";
  for (ut_trs_ctx* c : ctx_) EXPECT_EQ(1u, ut_trs_real_send_count(c));
  EXPECT_EQ(1u + ctx_.size(), ut_trs_sch_tsc_reads(sch_)) << "the call's and one a port";
  EXPECT_EQ(ctx_.size(), ut_trs_sch_dp_gets(sch_)) << "the visits' only";
}

/* a burst after a stall of many batch times leaves a batch due: the reclaim behind it
 * reads the clock and makes the empty call */
TEST_P(St20TxRlIdleCost, ABurstAfterAStallIsFollowedByAnEmptyOne) {
  ASSERT_EQ(0u, IdleCall());
  now_ += kStallNs;
  for (ut_trs_ctx* c : ctx_) ut_trs_enqueue_mid_pkt(c);
  const uint32_t before = NicCalls();
  ut_trs_sch_call(sch_);
  EXPECT_EQ(2 * ctx_.size(), NicCalls() - before) << "a burst and an empty one a port";
  for (ut_trs_ctx* c : ctx_) EXPECT_EQ(1u, ut_trs_real_send_count(c));
  EXPECT_EQ(1u + 2 * ctx_.size(), ut_trs_sch_tsc_reads(sch_))
      << "the call's and two a port";
}

INSTANTIATE_TEST_SUITE_P(Sessions, St20TxRlIdleCost, ::testing::Values(1, 13, 60));

} /* namespace */
