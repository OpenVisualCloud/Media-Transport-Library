/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * mt_rxq_get() without a flow, which every RX session requests for
 * ST*_RX_FLAG_DATA_PATH_ONLY: the app steers traffic to the queue itself.
 * Unfixed, both cases crash on the NULL flow.
 */

#include <gtest/gtest.h>

#include "datapath/mt_queue_harness.h"

class MtRxqNoFlowTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_rxq_init(), 0);
  }
  void TearDown() override {
    ut_rxq_destroy(ctx_);
  }
  ut_rxq_ctx* ctx_ = nullptr;
};

TEST_F(MtRxqNoFlowTest, DedicatedQueueHandedOutWithoutFlowRule) {
  ctx_ = ut_rxq_create(false);
  ASSERT_NE(ctx_, nullptr);

  ASSERT_EQ(ut_rxq_get_without_flow(ctx_), 0);
  EXPECT_TRUE(ut_rxq_queue_active(ctx_));
  EXPECT_FALSE(ut_rxq_queue_has_flow(ctx_));

  EXPECT_EQ(ut_rxq_put(ctx_), 0);
  EXPECT_FALSE(ut_rxq_queue_active(ctx_));
}

/* A kernel socket binds to the flow's UDP port, so it cannot serve without one. */
TEST_F(MtRxqNoFlowTest, KernelSocketRejectsMissingFlow) {
  ctx_ = ut_rxq_create(true);
  ASSERT_NE(ctx_, nullptr);

  EXPECT_LT(ut_rxq_get_without_flow(ctx_), 0);
}
