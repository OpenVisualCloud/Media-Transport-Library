/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St40TxRedundantAllocTest.*'
 */

#include <gtest/gtest.h>

#include "session/st40_tx_harness.h"

TEST(St40TxRedundantAllocTest, FrameLevelReturnsThePayloadToItsPoolOnce) {
  ASSERT_EQ(ut_txa_init(), 0);
  ut_txa_ctx* ctx = ut_txa_create();
  ASSERT_NE(ctx, nullptr);

  ASSERT_EQ(ut_txa_prepare_frame_tasklet(ctx, ST10_TIMESTAMP_FMT_TAI, 0, 1), 0);
  ASSERT_EQ(ut_txa_set_chain_r_dry(ctx), 0);
  ut_txa_step_frame_tasklet(ctx);
  ut_txa_set_mock_tsc_time(ctx, ut_txa_tsc_time_cursor(ctx));
  ut_txa_step_frame_tasklet(ctx);
  EXPECT_EQ(ut_txa_chain_pool_in_use(ctx), 0u);
  ut_txa_destroy(ctx);
}
