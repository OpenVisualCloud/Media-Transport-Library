/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St30TxRedundantAllocTest.*'
 */

#include <gtest/gtest.h>

#include "session/st30_tx_harness.h"

TEST(St30TxRedundantAllocTest, FrameLevelReturnsThePayloadToItsPoolOnce) {
  ASSERT_EQ(ut_txa30_init(), 0);
  ut_txa30_ctx* ctx = ut_txa30_create();
  ASSERT_NE(ctx, nullptr);
  unsigned int in_use = 0;

  ASSERT_EQ(ut_txa30_build_chain_r_dry(ctx, &in_use), 0);
  EXPECT_EQ(in_use, 0u);
  ut_txa30_destroy(ctx);
}
