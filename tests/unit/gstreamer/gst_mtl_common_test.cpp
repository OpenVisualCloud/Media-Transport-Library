/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "gstreamer/gst_mtl_common_harness.h"

static timespec g_clock_time;
static clockid_t g_clock_id;
static int g_clock_result;

/* Fills *ts even on failure, so a reader that ignores the error returns nonzero. */
int ut_gst_clock_gettime(clockid_t clock_id, struct timespec* ts) {
  g_clock_id = clock_id;
  *ts = g_clock_time;
  return g_clock_result;
}

class GstreamerMtlCommonTest : public testing::Test {
 protected:
  void SetUp() override {
    g_clock_time = {};
    g_clock_id = static_cast<clockid_t>(-1);
    g_clock_result = 0;
  }
};

TEST_F(GstreamerMtlCommonTest, SoftwareClockReturnsTaiNanoseconds) {
  mtl_init_params params;
  ASSERT_TRUE(ut_gst_parse_general_arguments(false, &params));
  ASSERT_NE(params.ptp_get_time_fn, nullptr);
  g_clock_time = {1700000037, 123456789};
  EXPECT_EQ(params.ptp_get_time_fn(params.priv), UINT64_C(1700000037123456789));
  EXPECT_EQ(g_clock_id, CLOCK_TAI);
}

TEST_F(GstreamerMtlCommonTest, SoftwareClockReadFailureReturnsZero) {
  mtl_init_params params;
  ASSERT_TRUE(ut_gst_parse_general_arguments(false, &params));
  ASSERT_NE(params.ptp_get_time_fn, nullptr);
  g_clock_time = {1700000037, 123456789};
  g_clock_result = -1;
  EXPECT_EQ(params.ptp_get_time_fn(params.priv), 0u);
}

TEST_F(GstreamerMtlCommonTest, OnboardPtpInstallsNoSoftwareClock) {
  mtl_init_params params;
  ASSERT_TRUE(ut_gst_parse_general_arguments(true, &params));
  EXPECT_EQ(params.ptp_get_time_fn, nullptr);
}
