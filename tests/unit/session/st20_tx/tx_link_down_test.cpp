/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The TX hang detector reads the link flag of the port, which the control path sets. A
 * queue of a port with no link is not hung: a failed burst while the flag says down
 * restarts the hang time, so a queue still hung after the link is up recovers a hang
 * threshold later. The transmitter itself makes no link call.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxLinkDownTest.*'
 */

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <vector>

#include "session/st_video_transmitter_harness.h"
#include "session/stderr_capture.h"

namespace {

constexpr uint64_t kThreshNs = 1000;
constexpr uint64_t kDownNs = 5000;

class St20TxLinkDownTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
    ctx_ = ut_trs_create();
    ASSERT_NE(nullptr, ctx_);
    ut_trs_set_hang_detect_thresh_ns(ctx_, kThreshNs);
  }

  void TearDown() override {
    ut_trs_destroy(ctx_);
  }

  /* one pad burst at each tsc, the queue refusing them or not */
  std::string BurstPadsAt(bool fail, std::initializer_list<uint64_t> tsc) {
    std::vector<uint64_t> script(tsc);
    ut_trs_set_mock_tsc_script(ctx_, script.data(), (int)script.size());
    ut_trs_set_burst_force_fail(ctx_, fail);
    return ut_session::capture_stderr([&] {
      for (size_t i = 0; i < script.size(); i++) ut_trs_call_burst_pad(ctx_);
    });
  }

  /* packet 0 of a frame through the TSC path at tsc, the queue refusing it or not */
  std::string Pkt0At(bool fail, uint64_t tsc) {
    ut_trs_set_mock_tsc_script(ctx_, &tsc, 1);
    ut_trs_set_burst_force_fail(ctx_, fail);
    return ut_session::capture_stderr([&] { ut_trs_call_burst_pkt0(ctx_, tsc); });
  }

  ut_trs_ctx* ctx_ = nullptr;
};

TEST_F(St20TxLinkDownTest, HangWithTheLinkDownDoesNotRecover) {
  ut_trs_set_link_down(ctx_, true);

  BurstPadsAt(true, {kDownNs, kDownNs + 2 * kThreshNs});

  EXPECT_FALSE(ut_trs_recovery_pending(ctx_));
}

TEST_F(St20TxLinkDownTest, HangWithTheLinkUpRecovers) {
  BurstPadsAt(true, {kDownNs});

  EXPECT_TRUE(ut_trs_recovery_pending(ctx_));
}

TEST_F(St20TxLinkDownTest, LinkUpWithTheQueueStillHungRecoversAThresholdLater) {
  ut_trs_set_link_down(ctx_, true);
  BurstPadsAt(true, {kDownNs});
  ut_trs_set_link_down(ctx_, false);

  Pkt0At(true, kDownNs + kThreshNs / 2);
  ASSERT_FALSE(ut_trs_recovery_pending(ctx_));
  BurstPadsAt(true, {kDownNs + 2 * kThreshNs});

  EXPECT_TRUE(ut_trs_recovery_pending(ctx_));
}

TEST_F(St20TxLinkDownTest, HangDetectionMakesNoLinkCall) {
  int calls = ut_trs_link_calls();

  BurstPadsAt(true, {kDownNs, kDownNs + 2 * kThreshNs});
  ut_trs_set_link_down(ctx_, true);
  BurstPadsAt(true, {kDownNs + 4 * kThreshNs, kDownNs + 6 * kThreshNs});

  EXPECT_EQ(calls, ut_trs_link_calls());
}

}  // namespace
