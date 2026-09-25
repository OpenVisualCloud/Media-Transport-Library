/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>
#include <mtl_sch_api.h>

#include "datapath/mt_srss_harness.h"

// Under MTL_FLAG_TASKLET_SLEEP a scheduler sleeps once every tasklet reports all done.
class MtSrssTaskletTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_srss_init(), 0);
  }

  void TearDown() override {
    ut_srss_destroy_ctx(ctx_);
  }

  ut_srss_ctx* ctx_ = nullptr;
};

TEST_F(MtSrssTaskletTest, ReportsPendingAfterReceivingPackets) {
  ctx_ = ut_srss_create_ctx(2, 4);
  ASSERT_NE(ctx_, nullptr);
  EXPECT_EQ(ut_srss_tasklet_handler(ctx_), MTL_TASKLET_HAS_PENDING);
}

TEST_F(MtSrssTaskletTest, ReportsAllDoneWhenNoQueueHadPackets) {
  ctx_ = ut_srss_create_ctx(2, 0);
  ASSERT_NE(ctx_, nullptr);
  EXPECT_EQ(ut_srss_tasklet_handler(ctx_), MTL_TASKLET_ALL_DONE);
}
