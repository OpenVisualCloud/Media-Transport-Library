/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "mtl/mtl_harness.h"

class MtlGetHwTimestampActiveTest : public testing::Test {
 protected:
  void SetUp() override {
    ctx_ = ut_mtl_create_ctx(MTL_PORT_MAX);
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut_mtl_destroy_ctx(ctx_);
  }

  ut_mtl_ctx* ctx_ = nullptr;
};

TEST_F(MtlGetHwTimestampActiveTest, ActiveWhenOffloadFeatureSet) {
  ut_mtl_set_hw_rx_timestamp(ctx_, MTL_PORT_P, true);
  EXPECT_EQ(mtl_get_hw_timestamp_active(ut_mtl_handle(ctx_), MTL_PORT_P), 1);
}

TEST_F(MtlGetHwTimestampActiveTest, InactiveWhenOffloadFeatureClear) {
  EXPECT_EQ(mtl_get_hw_timestamp_active(ut_mtl_handle(ctx_), MTL_PORT_P), 0);
}

TEST_F(MtlGetHwTimestampActiveTest, InvalidPortReturnsNegative) {
  EXPECT_LT(mtl_get_hw_timestamp_active(ut_mtl_handle(ctx_),
                                        static_cast<enum mtl_port>(MTL_PORT_MAX)),
            0);
}

TEST_F(MtlGetHwTimestampActiveTest, InvalidHandleTypeReturnsNegative) {
  ut_mtl_set_invalid_handle_type(ctx_);
  EXPECT_LT(mtl_get_hw_timestamp_active(ut_mtl_handle(ctx_), MTL_PORT_P), 0);
}
