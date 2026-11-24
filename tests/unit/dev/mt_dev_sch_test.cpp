/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "dev/mt_dev_harness.h"

namespace {

/* the quota a 2160p119 YUV 4:2:2 10-bit TX video session asks for */
constexpr int kTx2160p119QuotaMbs = 20715;

/* An ancillary session takes zero quota, which fits the system scheduler too. */
TEST(MtDevSchTest, DedicatedSysLcoreStaysSystemAfterAnAncillarySession) {
  struct ut_dev_sch_setup setup = {};
  setup.flags = MTL_FLAG_DEDICATED_SYS_LCORE;
  setup.anc_session_first = true;
  setup.tx_video_quota_mbs = kTx2160p119QuotaMbs;
  struct ut_dev_sch_layout layout = {};
  ASSERT_EQ(ut_dev_sch_layout(&setup, &layout), 0);
  EXPECT_NE(layout.tx_video_sch, layout.main_sch);
}

}  // namespace
