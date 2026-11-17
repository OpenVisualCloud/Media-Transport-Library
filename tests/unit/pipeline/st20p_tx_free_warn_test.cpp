/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * mtl_stop() stops the transport before the app frees the session, so frames stay in the
 * states of the pipeline. Only a frame that the app did not put back is an app fault.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*St20pTxFreeWarn*'
 */

#include <gtest/gtest.h>

#include "pipeline/st20p_tx_harness.h"

class St20pTxFreeWarn : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut20p_tx_init(), 0) << "EAL init failed";
    ctx_ = ut20p_tx_ctx_create(1);
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut20p_tx_ctx_destroy(ctx_);
  }

  ut20p_tx_ctx* ctx_ = nullptr;
};

TEST_F(St20pTxFreeWarn, WarnsForAFrameTheAppHolds) {
  ASSERT_NE(ut20p_tx_get_frame(ctx_), nullptr);

  EXPECT_EQ(ut20p_tx_uinit_warns(ctx_), 1);
}

TEST_F(St20pTxFreeWarn, DoesNotWarnForAConvertedFrame) {
  struct st_frame* frame = ut20p_tx_get_frame(ctx_);
  ASSERT_NE(frame, nullptr);
  ASSERT_EQ(ut20p_tx_put_frame(ctx_, frame), 0);

  EXPECT_EQ(ut20p_tx_uinit_warns(ctx_), 0);
}

TEST_F(St20pTxFreeWarn, DoesNotWarnForATransmittingFrame) {
  uint16_t idx;
  struct st_frame* frame = ut20p_tx_get_frame(ctx_);
  ASSERT_NE(frame, nullptr);
  ASSERT_EQ(ut20p_tx_put_frame(ctx_, frame), 0);
  ASSERT_EQ(ut20p_tx_next_frame(ctx_, &idx), 0);

  EXPECT_EQ(ut20p_tx_uinit_warns(ctx_), 0);
}
