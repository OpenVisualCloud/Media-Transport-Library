// SPDX-License-Identifier: BSD-3-Clause
// Copyright(c) 2026 Intel Corporation
//
// rv_detector_calculate_packing maps two booleans on the detector
// (bpm, single_line) to one of three st20_packing values. Priority:
//   1. bpm        -> ST20_PACKING_BPM
//   2. single_line -> ST20_PACKING_GPM_SL
//   3. otherwise  -> ST20_PACKING_GPM

#include <gtest/gtest.h>

#include "../../../include/st20_api.h"

extern "C" {
int test_calc_packing(int bpm, int single_line);
}

namespace {

TEST(DetectorPacking, BpmTakesPriorityOverSingleLine) {
  EXPECT_EQ((int)ST20_PACKING_BPM, test_calc_packing(/*bpm=*/1, /*sl=*/0));
  EXPECT_EQ((int)ST20_PACKING_BPM, test_calc_packing(/*bpm=*/1, /*sl=*/1));
}

TEST(DetectorPacking, SingleLineWhenBpmIsFalse) {
  EXPECT_EQ((int)ST20_PACKING_GPM_SL, test_calc_packing(/*bpm=*/0, /*sl=*/1));
}

TEST(DetectorPacking, GpmFallbackWhenNeitherIsSet) {
  EXPECT_EQ((int)ST20_PACKING_GPM, test_calc_packing(/*bpm=*/0, /*sl=*/0));
}

}  // namespace
