/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "session/st_video_transmitter_harness.h"
#include "session/stderr_capture.h"
#include "st2110/st_err.h"

namespace {

/* An unusable pacing target repeats every poll for as long as the anomaly lasts,
 * so these paths must stay silent and report through the counter instead. */
constexpr uint64_t kDrives = 64;
constexpr uint64_t kFarFutureNs = 1500000000; /* > NS_PER_S ahead of the mock clock */

class St20TxTransmitterInvalidTargetTest : public ::testing::Test {
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

/* A latched target stays latched once the clock moves away from it. */
TEST_F(St20TxTransmitterInvalidTargetTest, RlLatchedTargetSurvivesBackwardsClockStep) {
  const uint64_t script[] = {1000000000, 0, 1};
  ut_trs_set_mock_tsc_script(ctx_, script, 3);
  ut_trs_enqueue_first_pkt(ctx_, kFarFutureNs);
  ASSERT_EQ(0u, ut_trs_call_rl_tasklet(ctx_));
  ASSERT_EQ(kFarFutureNs, ut_trs_target_tsc(ctx_));

  std::string out = ut_session::capture_stderr([&] {
    for (uint64_t i = 0; i < kDrives; i++) ut_trs_call_rl_tasklet(ctx_);
  });

  EXPECT_TRUE(out.empty()) << out;
  EXPECT_EQ(kDrives, ut_trs_stat_target_invalid(ctx_));
}

TEST_F(St20TxTransmitterInvalidTargetTest, RlFirstPktFarFutureTarget) {
  std::string out = ut_session::capture_stderr([&] {
    for (uint64_t i = 0; i < kDrives; i++) {
      ut_trs_enqueue_first_pkt(ctx_, kFarFutureNs);
      ut_trs_call_rl_tasklet(ctx_);
    }
  });

  EXPECT_TRUE(out.empty()) << out;
  EXPECT_EQ(kDrives, ut_trs_stat_target_invalid(ctx_));
}

TEST_F(St20TxTransmitterInvalidTargetTest, RlWarmUpZeroTarget) {
  ASSERT_EQ(0u, ut_trs_target_tsc(ctx_));

  std::string out = ut_session::capture_stderr([&] {
    for (uint64_t i = 0; i < kDrives; i++) ut_trs_warm_up(ctx_);
  });

  EXPECT_TRUE(out.empty()) << out;
  EXPECT_EQ(kDrives, ut_trs_stat_target_invalid(ctx_));
}

TEST_F(St20TxTransmitterInvalidTargetTest, TscPendingFarFutureTarget) {
  std::string out = ut_session::capture_stderr([&] {
    for (uint64_t i = 0; i < kDrives; i++) {
      ut_trs_set_target_tsc(ctx_, kFarFutureNs);
      ut_trs_call_tsc_tasklet(ctx_);
    }
  });

  EXPECT_TRUE(out.empty()) << out;
  EXPECT_EQ(kDrives, ut_trs_stat_target_invalid(ctx_));
}

/* The tasklet latches a target and its inflight packets together, so a rejected
 * pending target is followed by an inflight drain that returns without touching
 * stat_trs_ret_code — the reject code is observable there. */
TEST_F(St20TxTransmitterInvalidTargetTest, TscPendingRejectRetCodeSurvivesInflightDrain) {
  ut_trs_set_target_tsc(ctx_, kFarFutureNs);
  ut_trs_set_inflight_num(ctx_, 1);

  std::string out = ut_session::capture_stderr([&] { ut_trs_call_tsc_tasklet(ctx_); });

  EXPECT_TRUE(out.empty()) << out;
  EXPECT_EQ(1u, ut_trs_stat_target_invalid(ctx_));
  EXPECT_EQ(0u, ut_trs_inflight_num(ctx_));
  EXPECT_EQ(-STI_TSCTRS_TARGET_TSC_INVALID, ut_trs_get_stat_trs_ret_code(ctx_));
}

TEST_F(St20TxTransmitterInvalidTargetTest, TscFirstPktFarFutureTarget) {
  std::string out = ut_session::capture_stderr([&] {
    for (uint64_t i = 0; i < kDrives; i++) {
      ut_trs_enqueue_first_pkt(ctx_, kFarFutureNs);
      ut_trs_call_tsc_tasklet(ctx_);
    }
  });

  EXPECT_TRUE(out.empty()) << out;
  EXPECT_EQ(kDrives, ut_trs_stat_target_invalid(ctx_));
}

TEST_F(St20TxTransmitterInvalidTargetTest, PtpPendingFarFutureTarget) {
  std::string out = ut_session::capture_stderr([&] {
    for (uint64_t i = 0; i < kDrives; i++) {
      ut_trs_set_target_tsc(ctx_, kFarFutureNs);
      ut_trs_call_ptp_tasklet(ctx_);
    }
  });

  EXPECT_TRUE(out.empty()) << out;
  EXPECT_EQ(kDrives, ut_trs_stat_target_invalid(ctx_));
}

TEST_F(St20TxTransmitterInvalidTargetTest, PtpFirstPktFarFutureTarget) {
  std::string out = ut_session::capture_stderr([&] {
    for (uint64_t i = 0; i < kDrives; i++) {
      ut_trs_enqueue_ptp_pkt(ctx_, kFarFutureNs);
      ut_trs_call_ptp_tasklet(ctx_);
    }
  });

  EXPECT_TRUE(out.empty()) << out;
  EXPECT_EQ(kDrives, ut_trs_stat_target_invalid(ctx_));
  EXPECT_EQ(-STI_TSCTRS_TARGET_TSC_INVALID, ut_trs_get_stat_trs_ret_code(ctx_));
}

} /* namespace */
