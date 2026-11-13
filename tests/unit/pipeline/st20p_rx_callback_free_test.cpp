/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The RX pipeline frees refuse in a callback of their transport session, on its
 * scheduler: the transport would refuse too and stay attached to the freed ctx.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St2*pRxCallback*'
 */

#include <gtest/gtest.h>

#include <cerrno>

#include "pipeline/st20p_harness.h"
#include "pipeline/st22p_harness.h"

TEST(St20pRxCallbackFree, RefusesInACallbackOfTheTransportOnItsScheduler) {
  ASSERT_EQ(ut20p_init(), 0) << "EAL init failed";
  ut20p_ctx* ctx = ut20p_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  bool destroying = true;

  EXPECT_EQ(-EBUSY, ut20p_rx_free_in_callback(ctx, &destroying));
  EXPECT_FALSE(destroying);

  ut20p_ctx_destroy(ctx);
}

TEST(St22pRxCallbackFree, RefusesInACallbackOfTheTransportOnItsScheduler) {
  ASSERT_EQ(ut22p_init(), 0) << "EAL init failed";
  ut22p_ctx* ctx = ut22p_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  bool destroying = true;

  EXPECT_EQ(-EBUSY, ut22p_rx_free_in_callback(ctx, &destroying));
  EXPECT_FALSE(destroying);

  ut22p_ctx_destroy(ctx);
}

/* The transport reset may now refuse (-EBUSY in its own callback): the pipeline counters
 * must then stay, not reset alone. */
TEST(St20pRxCallbackReset, KeepsThePipelineCountersWhenTheTransportRefuses) {
  ASSERT_EQ(ut20p_init(), 0) << "EAL init failed";
  ut20p_ctx* ctx = ut20p_ctx_create(1);
  ASSERT_NE(ctx, nullptr);

  ut20p_set_stat_frames_received(ctx, 5);
  ut20p_set_transport_reset_ret(-EBUSY);
  EXPECT_EQ(-EBUSY, st20p_rx_reset_session_stats(ut20p_handle(ctx)));
  ut20p_set_transport_reset_ret(0);
  EXPECT_EQ(5u, ut20p_stat_frames_received(ctx));

  EXPECT_EQ(0, st20p_rx_reset_session_stats(ut20p_handle(ctx)));
  EXPECT_EQ(0u, ut20p_stat_frames_received(ctx));

  ut20p_ctx_destroy(ctx);
}
