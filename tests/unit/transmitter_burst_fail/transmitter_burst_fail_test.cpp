// SPDX-License-Identifier: BSD-3-Clause
// Copyright(c) 2026 Intel Corporation
//
// video_trs_burst_fail is the hang-detection escalator inside the
// video transmitter. Its contract:
//
//   1. While elapsed_since_last_success <= tx_hang_detect_time_thresh:
//      - return 0 (caller stays in retry loop)
//      - leave last_burst_succ_time_tsc unchanged
//      - do NOT call st20_tx_queue_fatal_error
//
//   2. As soon as elapsed > threshold:
//      - return nb_pkts (caller drops the batch instead of looping)
//      - call st20_tx_queue_fatal_error exactly once
//      - reset last_burst_succ_time_tsc to "now" so the next call
//        starts a fresh observation window

#include <gtest/gtest.h>
#include <stdint.h>

extern "C" {
void test_burst_fail_reset(void);
void test_burst_fail_set_tsc(uint64_t tsc);
int test_burst_fail_fatal_calls(void);
uint16_t test_burst_fail_run(uint64_t last_succ_tsc, uint64_t threshold_ns,
                             uint16_t nb_pkts, uint64_t* out_last_succ_after);
}

namespace {

constexpr uint64_t kThresh = 1'000'000;  // 1ms in ns

class TransmitterBurstFail : public ::testing::Test {
 protected:
  void SetUp() override {
    test_burst_fail_reset();
  }
};

TEST_F(TransmitterBurstFail, BelowThresholdReturnsZeroAndKeepsLastSuccess) {
  test_burst_fail_set_tsc(500'000);  // now = 0.5ms after last success at 0
  uint64_t after = 0xdeadbeef;
  uint16_t r = test_burst_fail_run(/*last_succ=*/0, kThresh, /*nb_pkts=*/8, &after);
  EXPECT_EQ(0u, r);
  EXPECT_EQ(0u, after);
  EXPECT_EQ(0, test_burst_fail_fatal_calls());
}

TEST_F(TransmitterBurstFail, EqualToThresholdStillBelow) {
  // Production check is `> threshold`, so equal is below.
  test_burst_fail_set_tsc(kThresh);
  uint64_t after = 0;
  uint16_t r = test_burst_fail_run(0, kThresh, 8, &after);
  EXPECT_EQ(0u, r);
  EXPECT_EQ(0, test_burst_fail_fatal_calls());
}

TEST_F(TransmitterBurstFail, AboveThresholdEscalatesAndReturnsNbPkts) {
  test_burst_fail_set_tsc(kThresh + 1);
  uint64_t after = 0;
  uint16_t r = test_burst_fail_run(0, kThresh, 16, &after);
  EXPECT_EQ(16u, r);
  EXPECT_EQ(1, test_burst_fail_fatal_calls());
  EXPECT_EQ(kThresh + 1, after);  // last_succ_tsc reset to "now"
}

TEST_F(TransmitterBurstFail, EscalationResetsTheObservationWindow) {
  // After a fatal-error escalation the helper should restart from
  // "now"; a follow-up call within the threshold must NOT escalate
  // again.
  test_burst_fail_set_tsc(kThresh + 1);
  uint64_t after = 0;
  test_burst_fail_run(0, kThresh, 16, &after);
  ASSERT_EQ(1, test_burst_fail_fatal_calls());

  // Advance just under threshold from the new "now".
  test_burst_fail_set_tsc(after + (kThresh / 2));
  uint64_t after2 = 0;
  uint16_t r = test_burst_fail_run(after, kThresh, 16, &after2);
  EXPECT_EQ(0u, r);
  EXPECT_EQ(1, test_burst_fail_fatal_calls());  // still 1 — no second escalate
}

}  // namespace
