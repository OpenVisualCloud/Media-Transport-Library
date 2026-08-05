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

  EXPECT_TRUE(ut_trs_recovery_pending(ctx_));
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
  ASSERT_TRUE(ut_trs_prepare_cleanup_state(ctx_));
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

TEST_F(St20TxTransmitterBugsTest, RepeatedRecoveryCleanupReleasesAllPortState) {
  const int kPortR = 1; /* MTL_SESSION_PORT_R */
  unsigned int pool_avail = ut_trs_priv_pool_avail();
  unsigned int expected_avail_after_cleanup = pool_avail + 3;

  for (int recovery = 0; recovery < 20; recovery++) {
    ASSERT_TRUE(ut_trs_prepare_redundant_cleanup_state(ctx_));
    ut_trs_call_recovery_cleanup(ctx_);

    /* Shared pools require every port to release held mbufs. */
    EXPECT_EQ(expected_avail_after_cleanup, ut_trs_priv_pool_avail());
    EXPECT_EQ(0, ut_trs_rl_state(ctx_));
    EXPECT_EQ(0u, ut_trs_pad_inflight_num(ctx_));
    EXPECT_EQ(0u, ut_trs_inflight_num(ctx_));
    EXPECT_EQ(0, ut_trs_rl_state_port(ctx_, kPortR));
  }
}

TEST_F(St20TxTransmitterBugsTest, SimultaneousRedundantRecoveryRequestsAreProcessed) {
  constexpr int kPortP = 0;
  constexpr int kPortR = 1;

  ASSERT_TRUE(ut_trs_prepare_redundant_cleanup_state(ctx_));
  ut_trs_set_recovery_pending_port(ctx_, kPortP, true);

  ASSERT_TRUE(ut_trs_process_recovery_cleanup(ctx_, kPortP));
  EXPECT_FALSE(ut_trs_recovery_pending_port(ctx_, kPortP));
  ASSERT_TRUE(ut_trs_recovery_pending_port(ctx_, kPortR));

  EXPECT_TRUE(ut_trs_process_recovery_cleanup(ctx_, kPortR));
  EXPECT_FALSE(ut_trs_recovery_pending_port(ctx_, kPortR));
}

TEST_F(St20TxTransmitterBugsTest, PartialBurstBeforeBoundaryRetriesBeforeNewFrame) {
  constexpr uint64_t kBeforeTargetTsc = 8000;
  constexpr uint64_t kTargetTsc = 10000;
  const uint64_t before_target_script[] = {kBeforeTargetTsc, kBeforeTargetTsc,
                                           kBeforeTargetTsc, kBeforeTargetTsc};

  ut_trs_set_trs(ctx_, 1000.0L);
  ut_trs_set_warm_pkts_cap(ctx_, 0);
  ut_trs_set_hang_detect_thresh_ns(ctx_, UINT64_MAX);
  ut_trs_set_mock_tsc_script(ctx_, before_target_script, 4);
  ut_trs_enqueue_frame_boundary(ctx_, kTargetTsc);

  ut_trs_set_burst_force_fail(ctx_, true);
  ut_trs_call_rl_tasklet(ctx_);

  ASSERT_EQ(1u, ut_trs_inflight_num2(ctx_));
  ASSERT_EQ(1u, ut_trs_inflight_num(ctx_));
  ASSERT_EQ(0u, ut_trs_real_send_count(ctx_));

  ut_trs_set_burst_force_fail(ctx_, false);
  ut_trs_call_rl_tasklet(ctx_);

  ASSERT_EQ(1u, ut_trs_real_send_count(ctx_));
  EXPECT_EQ(1u, ut_trs_sent_pkt_idx(ctx_, 0));
  EXPECT_EQ(0u, ut_trs_inflight_num2(ctx_));
  EXPECT_EQ(1u, ut_trs_inflight_num(ctx_));

  const uint64_t target_script[] = {kTargetTsc, kTargetTsc, kTargetTsc};
  ut_trs_set_mock_tsc_script(ctx_, target_script, 3);
  ut_trs_call_rl_tasklet(ctx_);

  ASSERT_EQ(2u, ut_trs_real_send_count(ctx_));
  EXPECT_EQ(0u, ut_trs_sent_pkt_idx(ctx_, 1));
  EXPECT_EQ(0u, ut_trs_inflight_num(ctx_));
}

}  // namespace
