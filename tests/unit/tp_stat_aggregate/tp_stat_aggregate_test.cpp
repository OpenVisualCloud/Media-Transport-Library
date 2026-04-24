// SPDX-License-Identifier: BSD-3-Clause
// Copyright(c) 2026 Intel Corporation
//
// Per-port stat aggregator invariants for rv_tp_slot_parse_result
// (lib/src/st2110/st_rx_timing_parser.c).
//
// The function folds each per-frame slot's metadata into a running
// per-port stat slot. The running min fields must be monotonically
// non-increasing, and the running max fields must be monotonically
// non-decreasing, across an arbitrary sequence of frames. The tests
// below assert exactly those invariants — they remain meaningful
// independent of any specific implementation defect.
//
// (At the time of writing the running max for vrx and ipt violates
// this invariant: the aggregator passes the running MIN as one
// operand of RTE_MAX. The two *Max tests below therefore fail on
// current code and will pass once the aggregator is corrected.)

#include <gtest/gtest.h>
#include <stdint.h>

extern "C" {
void tp_test_reset(void);
void tp_test_run_frame(int32_t vrx_min, int32_t vrx_max, int32_t ipt_min, int32_t ipt_max,
                       int32_t* out_stat_vrx_max, int32_t* out_stat_vrx_min,
                       int32_t* out_stat_ipt_max, int32_t* out_stat_ipt_min);
}

namespace {

class TpStatAggregate : public ::testing::Test {
 protected:
  void SetUp() override {
    tp_test_reset();
  }
};

// ---------------------------------------------------------------------------
// Invariant: stat vrx_max is monotonically non-decreasing across frames.
// ---------------------------------------------------------------------------
TEST_F(TpStatAggregate, StatVrxMaxIsMonotonicallyNonDecreasing) {
  int32_t stat_vrx_max = 0;

  // Frame 1 establishes a running max of 50.
  tp_test_run_frame(-100, 50, 0, 0, &stat_vrx_max, nullptr, nullptr, nullptr);
  ASSERT_EQ(50, stat_vrx_max)
      << "After the first frame the running stat max must equal the slot max.";
  int32_t prev = stat_vrx_max;

  // Frame 2 carries a smaller per-slot max (10). The running stat max must
  // never drop below the previous running stat max.
  tp_test_run_frame(-200, 10, 0, 0, &stat_vrx_max, nullptr, nullptr, nullptr);
  EXPECT_GE(stat_vrx_max, prev)
      << "Stat vrx_max shrank from " << prev << " to " << stat_vrx_max
      << "; running max must be monotonically non-decreasing.";
}

// ---------------------------------------------------------------------------
// Invariant: stat ipt_max is monotonically non-decreasing across frames.
// ---------------------------------------------------------------------------
TEST_F(TpStatAggregate, StatIptMaxIsMonotonicallyNonDecreasing) {
  int32_t stat_ipt_max = 0;

  // Frame 1 establishes a running max of 1000.
  tp_test_run_frame(0, 0, 50, 1000, nullptr, nullptr, &stat_ipt_max, nullptr);
  ASSERT_EQ(1000, stat_ipt_max)
      << "After the first frame the running stat max must equal the slot max.";
  int32_t prev = stat_ipt_max;

  // Frame 2 carries a smaller per-slot max (100). The running stat max must
  // never drop below the previous running stat max.
  tp_test_run_frame(0, 0, 10, 100, nullptr, nullptr, &stat_ipt_max, nullptr);
  EXPECT_GE(stat_ipt_max, prev)
      << "Stat ipt_max shrank from " << prev << " to " << stat_ipt_max
      << "; running max must be monotonically non-decreasing.";
}

// ---------------------------------------------------------------------------
// Invariant: stat vrx_min and ipt_min are monotonically non-increasing.
// ---------------------------------------------------------------------------
TEST_F(TpStatAggregate, StatVrxMinAndIptMinAreMonotonicallyNonIncreasing) {
  int32_t stat_vrx_min = 0;
  int32_t stat_ipt_min = 0;

  tp_test_run_frame(-100, 50, 200, 300, nullptr, &stat_vrx_min, nullptr, &stat_ipt_min);
  EXPECT_EQ(-100, stat_vrx_min);
  EXPECT_EQ(200, stat_ipt_min);
  int32_t prev_vrx_min = stat_vrx_min;
  int32_t prev_ipt_min = stat_ipt_min;

  // Push lower per-slot mins; running min must drop accordingly and never
  // grow above the previous running min.
  tp_test_run_frame(-300, 0, 50, 250, nullptr, &stat_vrx_min, nullptr, &stat_ipt_min);
  EXPECT_LE(stat_vrx_min, prev_vrx_min)
      << "Stat vrx_min grew from " << prev_vrx_min << " to " << stat_vrx_min
      << "; running min must be monotonically non-increasing.";
  EXPECT_LE(stat_ipt_min, prev_ipt_min)
      << "Stat ipt_min grew from " << prev_ipt_min << " to " << stat_ipt_min
      << "; running min must be monotonically non-increasing.";
}

}  // namespace
