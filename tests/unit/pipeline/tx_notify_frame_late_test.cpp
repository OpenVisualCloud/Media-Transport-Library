/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pipeline TX notify_frame_late on the transport-late path: the transport
 * session calls the late callback it was registered with, passing the priv
 * it was created with (the pipeline ctx). The app must still see ops.priv.
 */

#include <gtest/gtest.h>

#include "pipeline/st20p_tx_harness.h"
#include "pipeline/st22p_tx_harness.h"
#include "pipeline/st30p_tx_harness.h"
#include "pipeline/st40p_tx_harness.h"

namespace {

struct LateSpy {
  int calls = 0;
  void* priv = nullptr;
  uint64_t epoch_skipped = 0;
};

LateSpy g_late;

int OnFrameLate(void* priv, uint64_t epoch_skipped) {
  g_late.calls++;
  g_late.priv = priv; /* recorded only: a wrong priv must not be dereferenced */
  g_late.epoch_skipped = epoch_skipped;
  return 0;
}

constexpr uint64_t kEpochSkipped = 3;

}  // namespace

TEST(PipelineTxNotifyFrameLate, St20pTransportLateReachesAppPriv) {
  ASSERT_EQ(ut20p_tx_init(), 0) << "EAL init failed";
  ut20p_tx_ctx* ctx = ut20p_tx_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  int app_priv;
  g_late = LateSpy();
  ut20p_tx_set_notify_frame_late(ctx, OnFrameLate, &app_priv);

  EXPECT_EQ(ut20p_tx_transport_report_late(ctx, kEpochSkipped), 0);
  EXPECT_EQ(g_late.calls, 1);
  EXPECT_EQ(g_late.priv, &app_priv);
  EXPECT_EQ(g_late.epoch_skipped, kEpochSkipped);

  ut20p_tx_ctx_destroy(ctx);
}

TEST(PipelineTxNotifyFrameLate, St30pTransportLateReachesAppPriv) {
  ASSERT_EQ(ut30p_tx_init(), 0) << "EAL init failed";
  ut30p_tx_ctx* ctx = ut30p_tx_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  int app_priv;
  g_late = LateSpy();
  ut30p_tx_set_notify_frame_late(ctx, OnFrameLate, &app_priv);

  EXPECT_EQ(ut30p_tx_transport_report_late(ctx, kEpochSkipped), 0);
  EXPECT_EQ(g_late.calls, 1);
  EXPECT_EQ(g_late.priv, &app_priv);
  EXPECT_EQ(g_late.epoch_skipped, kEpochSkipped);

  ut30p_tx_ctx_destroy(ctx);
}

TEST(PipelineTxNotifyFrameLate, St40pTransportLateReachesAppPriv) {
  ASSERT_EQ(ut40p_tx_init(), 0) << "EAL init failed";
  ut40p_tx_ctx* ctx = ut40p_tx_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  int app_priv;
  g_late = LateSpy();
  ut40p_tx_set_notify_frame_late(ctx, OnFrameLate, &app_priv);

  EXPECT_EQ(ut40p_tx_transport_report_late(ctx, kEpochSkipped), 0);
  EXPECT_EQ(g_late.calls, 1);
  EXPECT_EQ(g_late.priv, &app_priv);
  EXPECT_EQ(g_late.epoch_skipped, kEpochSkipped);

  ut40p_tx_ctx_destroy(ctx);
}

TEST(PipelineTxNotifyFrameLate, St22pTransportLateReachesAppPriv) {
  ASSERT_EQ(ut22p_tx_init(), 0) << "EAL init failed";
  ut22p_tx_ctx* ctx = ut22p_tx_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  int app_priv;
  g_late = LateSpy();
  ut22p_tx_set_notify_frame_late(ctx, OnFrameLate, &app_priv);

  EXPECT_EQ(ut22p_tx_transport_report_late(ctx, kEpochSkipped), 0);
  EXPECT_EQ(g_late.calls, 1);
  EXPECT_EQ(g_late.priv, &app_priv);
  EXPECT_EQ(g_late.epoch_skipped, kEpochSkipped);

  ut22p_tx_ctx_destroy(ctx);
}
