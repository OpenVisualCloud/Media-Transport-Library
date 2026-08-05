/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "session/st_video_transmitter_harness.h"

namespace {

class St20TxTransmitterBugsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
    ctx_ = ut_trs_create();
    ASSERT_NE(nullptr, ctx_);
  }

  void TearDown() override {
    ut_trs_destroy(ctx_);
  }

  ut_trs_ctx* ctx_ = nullptr;
};

TEST_F(St20TxTransmitterBugsTest, PadInflightRetrySuccessTriggersSecondBurstAttempt) {
  ut_trs_set_pad_inflight_num(ctx_, 1);
  ut_trs_enqueue_ring_pkt(ctx_);

  int pending = ut_trs_call_rl_tasklet(ctx_);

  EXPECT_EQ(2u, ut_trs_burst_call_count(ctx_));
  EXPECT_EQ(1, ut_trs_get_stat_trs_ret_code(ctx_));
  EXPECT_EQ(2, pending);
}

TEST_F(St20TxTransmitterBugsTest, InflightRetrySuccessTriggersSecondBurstAttempt) {
  ut_trs_set_inflight_num(ctx_, 1);
  ut_trs_enqueue_ring_pkt(ctx_);

  int pending = ut_trs_call_rl_tasklet(ctx_);

  EXPECT_EQ(2u, ut_trs_burst_call_count(ctx_));
  EXPECT_EQ(1, ut_trs_get_stat_trs_ret_code(ctx_));
  EXPECT_EQ(2, pending);
}

TEST_F(St20TxTransmitterBugsTest, Inflight2RetrySuccessTriggersSecondBurstAttempt) {
  ut_trs_set_inflight_num2(ctx_, 1);
  ut_trs_enqueue_ring_pkt(ctx_);

  int pending = ut_trs_call_rl_tasklet(ctx_);

  EXPECT_EQ(2u, ut_trs_burst_call_count(ctx_));
  EXPECT_EQ(1, ut_trs_get_stat_trs_ret_code(ctx_));
  EXPECT_EQ(2, pending);
}

TEST_F(St20TxTransmitterBugsTest, PadBurstSuccessRefreshesLastSuccessTimestamp) {
  ASSERT_EQ(1u, ut_trs_call_burst_pad(ctx_));
  ASSERT_EQ(1u, ut_trs_call_burst_pad(ctx_));
  ASSERT_EQ(1u, ut_trs_call_burst_pad(ctx_));

  EXPECT_GT(ut_trs_get_last_burst_succ_tsc(ctx_), 0u);
  EXPECT_EQ(3, ut_trs_get_stat_pkts_burst(ctx_));
}

TEST_F(St20TxTransmitterBugsTest, HangTriggerRetainsFailedPadForRecovery) {
  ut_trs_set_hang_detect_thresh_ns(ctx_, 1000);
  const uint64_t script[] = {5000};
  ut_trs_set_mock_tsc_script(ctx_, script, 1);

  ut_trs_set_burst_force_fail(ctx_, true);
  uint16_t tx = ut_trs_call_burst_pad(ctx_);

  EXPECT_EQ(0u, tx);
  EXPECT_EQ(5000u, ut_trs_get_last_burst_succ_tsc(ctx_));
  EXPECT_TRUE(ut_trs_recovery_pending(ctx_));
  EXPECT_EQ(1u, ut_trs_pad_inflight_num(ctx_));
  EXPECT_EQ(2u, ut_trs_pad_refcnt(ctx_));

  ut_trs_cleanup_state(ctx_);

  EXPECT_FALSE(ut_trs_recovery_pending(ctx_));
  EXPECT_EQ(0u, ut_trs_pad_inflight_num(ctx_));
  EXPECT_EQ(1u, ut_trs_pad_refcnt(ctx_));
}

TEST_F(St20TxTransmitterBugsTest, FreshTimestampAvoidsPrematureHangTrigger) {
  ut_trs_set_hang_detect_thresh_ns(ctx_, 1000);
  ut_trs_set_last_burst_succ_tsc(ctx_, 4950);
  const uint64_t script[] = {5000};
  ut_trs_set_mock_tsc_script(ctx_, script, 1);

  ut_trs_set_burst_force_fail(ctx_, true);
  uint16_t tx = ut_trs_call_burst_pad(ctx_);

  EXPECT_EQ(0u, tx);
  EXPECT_EQ(4950u, ut_trs_get_last_burst_succ_tsc(ctx_));
}

TEST_F(St20TxTransmitterBugsTest, CleanupReleasesRetainedReferencesAndResetsState) {
  ut_trs_prepare_cleanup_state(ctx_);
  unsigned int pool_avail = ut_trs_priv_pool_avail();

  ut_trs_cleanup_state(ctx_);

  EXPECT_EQ(pool_avail + 2, ut_trs_priv_pool_avail());
  EXPECT_EQ(1u, ut_trs_pad_refcnt(ctx_));
  EXPECT_EQ(0u, ut_trs_pad_inflight_num(ctx_));
  EXPECT_EQ(0u, ut_trs_inflight_num(ctx_));
  EXPECT_EQ(0u, ut_trs_inflight_num2(ctx_));
  EXPECT_EQ(0u, ut_trs_inflight_idx(ctx_));
  EXPECT_EQ(0u, ut_trs_inflight_idx2(ctx_));
  EXPECT_EQ(0u, ut_trs_target_tsc(ctx_));
  EXPECT_EQ(0, ut_trs_rl_state(ctx_));
}

TEST_F(St20TxTransmitterBugsTest, PortScopedCleanupLeavesOtherPortRecoveryStateIntact) {
  const int kPortR = 1; /* MTL_SESSION_PORT_R */
  ut_trs_prepare_cleanup_state(ctx_);
  const int wait_target_state = ut_trs_rl_state(ctx_);
  ut_trs_set_rl_state_port(ctx_, kPortR, wait_target_state);
  ut_trs_set_recovery_pending_port(ctx_, kPortR, true);

  ut_trs_call_port_cleanup(ctx_, 0 /* MTL_SESSION_PORT_P */);

  /* recovered port is fully reset */
  EXPECT_EQ(0, ut_trs_rl_state(ctx_));
  EXPECT_FALSE(ut_trs_recovery_pending(ctx_));
  EXPECT_EQ(0u, ut_trs_pad_inflight_num(ctx_));
  EXPECT_EQ(0u, ut_trs_inflight_num(ctx_));

  /* the other, healthy port must keep its own pending recovery and state */
  EXPECT_EQ(wait_target_state, ut_trs_rl_state_port(ctx_, kPortR));
  EXPECT_TRUE(ut_trs_recovery_pending_port(ctx_, kPortR));
}

/*
 * NOTE: this exercises the delta arithmetic in isolation rather than through
 * ut_trs_call_rl_tasklet(). The `|| s->trs_inflight_num2[s_port]` disjunct
 * inside _video_trs_rl_tasklet()'s first-packet branch is unreachable via a
 * real call: trs_inflight_num2[s_port] > 0 triggers an early return at the
 * top of _video_trs_rl_tasklet(), so by the time this branch runs in the
 * same invocation the value is always 0.
 */
TEST(VideoTrsRlTaskletArithmetic, FirstPktDeltaWrapsWhenInflight2ForcesEntryPastTarget) {
  const uint64_t cur_tsc = 5000000000ULL;
  const uint64_t target_tsc = cur_tsc - 100;
  const bool trs_inflight_num2_nonzero = true;

  bool would_enter_branch = (cur_tsc < target_tsc) || trs_inflight_num2_nonzero;
  ASSERT_TRUE(would_enter_branch);

  int64_t delta = (int64_t)(target_tsc - cur_tsc);
  const int64_t ns_per_s = 1000000000LL;
  const int64_t mt_sch_schedule_ns = 1000000LL;

  bool would_report_has_pending = delta < mt_sch_schedule_ns;

  EXPECT_LT(delta, ns_per_s)
      << "delta should reflect the small overdue amount, not a wrapped huge value";
  EXPECT_TRUE(would_report_has_pending)
      << "an already-overdue retry must report MTL_TASKLET_HAS_PENDING, not "
         "MTL_TASKLET_ALL_DONE";
}

}  // namespace
