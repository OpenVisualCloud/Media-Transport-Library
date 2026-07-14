/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * st_json_parse_tx_fmd() fps-string validation: a genuinely invalid fps
 * string must still be rejected, and must not accidentally match one of the
 * 11 back-to-back strcmp("pNN", ...) branches (7 of which were added by
 * this fix), which is the real regression risk of a broken/prefix-matching
 * comparison.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='ParseJsonTxFmdFpsTest.*'
 */

#include <gtest/gtest.h>

#include "app/parse_json_harness.h"

TEST(ParseJsonTxFmdFpsTest, RejectsUnsupportedFps) {
  EXPECT_EQ(ut_parse_tx_fmd_fps("p999"), ut_parse_json_not_valid_rc());
}

TEST(ParseJsonTxFmdFpsTest, RejectsTypoNearValidValue) {
  EXPECT_EQ(ut_parse_tx_fmd_fps("p12"), ut_parse_json_not_valid_rc());
}

TEST(ParseJsonTxFmdFpsTest, AcceptsNewlySupportedFps) {
  EXPECT_EQ(ut_parse_tx_fmd_fps("p120"), 0);
}
