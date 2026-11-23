/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * mtl_stop() stops the transport before the app frees the session, so frames stay in the
 * states of the pipeline. Only a frame that the app did not put back is an app fault.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*St40pTxFreeWarn*'
 */

#include <gtest/gtest.h>

#include "pipeline/st40p_tx_harness.h"

class St40pTxFreeWarn : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut40p_tx_init(), 0) << "EAL init failed";
    ctx_ = ut40p_tx_ctx_create(1);
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut40p_tx_ctx_destroy(ctx_);
  }

  ut40p_tx_ctx* ctx_ = nullptr;
};

TEST_F(St40pTxFreeWarn, WarnsForAFrameTheAppHolds) {
  ASSERT_NE(ut40p_tx_get_frame(ctx_), nullptr);

  EXPECT_EQ(ut40p_tx_uinit_warns(ctx_), 1);
}

TEST_F(St40pTxFreeWarn, DoesNotWarnForAReadyFrame) {
  struct st40_frame_info* frame = ut40p_tx_get_frame(ctx_);
  ASSERT_NE(frame, nullptr);
  ASSERT_EQ(ut40p_tx_put_frame(ctx_, frame), 0);

  EXPECT_EQ(ut40p_tx_uinit_warns(ctx_), 0);
}

TEST_F(St40pTxFreeWarn, DoesNotWarnForATransmittingFrame) {
  uint16_t idx;
  struct st40_frame_info* frame = ut40p_tx_get_frame(ctx_);
  ASSERT_NE(frame, nullptr);
  ASSERT_EQ(ut40p_tx_put_frame(ctx_, frame), 0);
  ASSERT_EQ(ut40p_tx_next_frame(ctx_, &idx), 0);

  EXPECT_EQ(ut40p_tx_uinit_warns(ctx_), 0);
}
