/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * st20p_tx_free() from a callback must refuse before it tears anything down: the
 * transport refuses its own free there, so the pipeline would free a ctx the transport
 * keeps calling back into.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*St20pTxCallbackFree*'
 */

#include <gtest/gtest.h>

#include <cerrno>

#include "pipeline/st20p_tx_harness.h"

TEST(St20pTxCallbackFree, RefusesOnTheTransportSchedulerThread) {
  ASSERT_EQ(ut20p_tx_init(), 0) << "EAL init failed";
  ut20p_tx_ctx* ctx = ut20p_tx_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  bool destroying = true;

  EXPECT_EQ(-EBUSY, ut20p_tx_free_in_callback(ctx, &destroying));
  EXPECT_FALSE(destroying);

  ut20p_tx_ctx_destroy(ctx);
}
