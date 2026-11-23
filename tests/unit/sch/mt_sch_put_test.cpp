/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "sch/mt_sch_harness.h"

/* mtl_stop() stops each scheduler before the application frees its sessions. */
TEST(MtSchPutTest, LastPutAfterStopDoesNotWarn) {
  int warns = -1;

  ASSERT_EQ(ut_sch_init(), 0);
  ASSERT_EQ(ut_sch_last_put_after_stop(&warns), 0);
  EXPECT_EQ(warns, 0) << ut_sch_first_warn();
}
