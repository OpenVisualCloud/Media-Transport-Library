/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "sch/mt_sch_harness.h"

/* The TX video round err judges the loop avg, so a restart must not keep the last run's
 */
TEST(MtSchLoopAvgTest, ARestartedSchedulerHasNoLoopAvgOfItsPreviousRun) {
  ASSERT_EQ(0, ut_sch_init());
  constexpr uint64_t kLoopNs = 20000000;
  uint64_t avg_ns = 0, restart_avg_ns = 1;
  ASSERT_EQ(0, ut_sch_loop_avg_restart(kLoopNs, &avg_ns, &restart_avg_ns));
  EXPECT_GE(avg_ns, kLoopNs);
  EXPECT_EQ(0u, restart_avg_ns);
}
