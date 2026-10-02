/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pipeline stats and queue-meta calls on a handle that is being destroyed
 * must report an error: they return without filling the output, so a 0
 * return makes the caller read an unfilled struct as valid stats.
 */

#include <gtest/gtest.h>

#include "pipeline/st20p_harness.h"
#include "pipeline/st20p_tx_harness.h"
#include "pipeline/st22p_harness.h"
#include "pipeline/st30p_harness.h"
#include "pipeline/st30p_tx_harness.h"
#include "pipeline/st40p_tx_harness.h"

TEST(PipelineStaleHandle, St20pTxGetSessionStatsFails) {
  ASSERT_EQ(ut20p_tx_init(), 0) << "EAL init failed";
  ut20p_tx_ctx* ctx = ut20p_tx_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  ut20p_tx_force_destroying(ctx);

  struct st20_tx_user_stats stats;
  EXPECT_LT(st20p_tx_get_session_stats(ut20p_tx_handle(ctx), &stats), 0);

  ut20p_tx_ctx_destroy(ctx);
}

TEST(PipelineStaleHandle, St20pTxResetSessionStatsFails) {
  ASSERT_EQ(ut20p_tx_init(), 0) << "EAL init failed";
  ut20p_tx_ctx* ctx = ut20p_tx_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  ut20p_tx_force_destroying(ctx);

  EXPECT_LT(st20p_tx_reset_session_stats(ut20p_tx_handle(ctx)), 0);

  ut20p_tx_ctx_destroy(ctx);
}

TEST(PipelineStaleHandle, St30pTxResetSessionStatsFails) {
  ASSERT_EQ(ut30p_tx_init(), 0) << "EAL init failed";
  ut30p_tx_ctx* ctx = ut30p_tx_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  ut30p_tx_force_destroying(ctx);

  EXPECT_LT(st30p_tx_reset_session_stats(ut30p_tx_handle(ctx)), 0);

  ut30p_tx_ctx_destroy(ctx);
}

TEST(PipelineStaleHandle, St40pTxResetSessionStatsFails) {
  ASSERT_EQ(ut40p_tx_init(), 0) << "EAL init failed";
  ut40p_tx_ctx* ctx = ut40p_tx_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  ut40p_tx_force_destroying(ctx);

  EXPECT_LT(st40p_tx_reset_session_stats(ut40p_tx_handle(ctx)), 0);

  ut40p_tx_ctx_destroy(ctx);
}

TEST(PipelineStaleHandle, St20pRxGetSessionStatsFails) {
  ASSERT_EQ(ut20p_init(), 0) << "EAL init failed";
  ut20p_ctx* ctx = ut20p_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  ut20p_force_destroying(ctx);

  struct st20_rx_user_stats stats;
  EXPECT_LT(st20p_rx_get_session_stats(ut20p_handle(ctx), &stats), 0);

  ut20p_ctx_destroy(ctx);
}

TEST(PipelineStaleHandle, St20pRxResetSessionStatsFails) {
  ASSERT_EQ(ut20p_init(), 0) << "EAL init failed";
  ut20p_ctx* ctx = ut20p_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  ut20p_force_destroying(ctx);

  EXPECT_LT(st20p_rx_reset_session_stats(ut20p_handle(ctx)), 0);

  ut20p_ctx_destroy(ctx);
}

TEST(PipelineStaleHandle, St20pRxGetQueueMetaFails) {
  ASSERT_EQ(ut20p_init(), 0) << "EAL init failed";
  ut20p_ctx* ctx = ut20p_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  ut20p_force_destroying(ctx);

  struct st_queue_meta meta;
  EXPECT_LT(st20p_rx_get_queue_meta(ut20p_handle(ctx), &meta), 0);

  ut20p_ctx_destroy(ctx);
}

TEST(PipelineStaleHandle, St22pRxGetQueueMetaFails) {
  ASSERT_EQ(ut22p_init(), 0) << "EAL init failed";
  ut22p_ctx* ctx = ut22p_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  ut22p_force_destroying(ctx);

  struct st_queue_meta meta;
  EXPECT_LT(st22p_rx_get_queue_meta(ut22p_handle(ctx), &meta), 0);

  ut22p_ctx_destroy(ctx);
}

TEST(PipelineStaleHandle, St30pRxGetSessionStatsFails) {
  ASSERT_EQ(ut30p_init(), 0) << "EAL init failed";
  ut30p_ctx* ctx = ut30p_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  ut30p_force_destroying(ctx);

  struct st30_rx_user_stats stats;
  EXPECT_LT(st30p_rx_get_session_stats(ut30p_handle(ctx), &stats), 0);

  ut30p_ctx_destroy(ctx);
}

TEST(PipelineStaleHandle, St30pRxResetSessionStatsFails) {
  ASSERT_EQ(ut30p_init(), 0) << "EAL init failed";
  ut30p_ctx* ctx = ut30p_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  ut30p_force_destroying(ctx);

  EXPECT_LT(st30p_rx_reset_session_stats(ut30p_handle(ctx)), 0);

  ut30p_ctx_destroy(ctx);
}

TEST(PipelineStaleHandle, St30pRxGetQueueMetaFails) {
  ASSERT_EQ(ut30p_init(), 0) << "EAL init failed";
  ut30p_ctx* ctx = ut30p_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  ut30p_force_destroying(ctx);

  struct st_queue_meta meta;
  EXPECT_LT(ut30p_get_queue_meta(ctx, &meta), 0);

  ut30p_ctx_destroy(ctx);
}
