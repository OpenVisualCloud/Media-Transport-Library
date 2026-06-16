/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Graceful-shutdown contract for gst_mtl_st20p_tx_finalize():
 *   - finalize must block until in-flight (pending) GstBuffers drain, so the
 *     MTL session is not torn down while the DPDK lcore still owns them;
 *   - if buffers never drain, finalize must still return after the bounded
 *     grace window (shutdown must not hang);
 *   - with nothing pending, finalize must return promptly.
 *
 * The static finalize is driven through a C harness that keeps the MTL
 * handles NULL, so only the graceful drain path executes.
 */

#include <gtest/gtest.h>

#include <thread>

extern "C" {
#include "gstreamer/gst_mtl_st20p_tx_harness.h"
}

namespace {
/* Mirrors GST_MTL_ST20P_TX_FINALIZE_GRACE_MS in gst_mtl_st20p_tx.c. */
constexpr gint64 kGraceMs = 200;

gint64 now_ms() {
  return g_get_monotonic_time() / 1000;
}
}  // namespace

class St20pTxFinalizeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ut_st20p_tx_init();
  }
};

/* A late frame_done that drains the last pending buffer mid-finalize must be
 * waited for: finalize returns only once pending hits 0, well within grace. */
TEST_F(St20pTxFinalizeTest, FinalizeWaitsForPendingDrain) {
  Gst_Mtl_St20p_Tx* sink = ut_st20p_tx_new();
  ASSERT_NE(sink, nullptr);
  ut_st20p_tx_set_pending(sink, 1);

  std::thread drainer([&] {
    g_usleep(20 * 1000);
    ut_st20p_tx_dec_pending(sink);
  });

  gint64 t0 = now_ms();
  ut_st20p_tx_finalize(sink);
  gint64 elapsed = now_ms() - t0;

  drainer.join();

  EXPECT_GE(elapsed, 15) << "finalize must block until the pending buffer drains";
  EXPECT_LT(elapsed, kGraceMs) << "finalize must return as soon as buffers drain";
  EXPECT_EQ(ut_st20p_tx_get_pending(sink), 0);
}

/* Negative case: buffers never drain. finalize must not hang — it returns after
 * the bounded grace window with the buffers still outstanding. */
TEST_F(St20pTxFinalizeTest, FinalizeTimesOutWhenBuffersNeverDrain) {
  Gst_Mtl_St20p_Tx* sink = ut_st20p_tx_new();
  ASSERT_NE(sink, nullptr);
  ut_st20p_tx_set_pending(sink, 2);

  gint64 t0 = now_ms();
  ut_st20p_tx_finalize(sink);
  gint64 elapsed = now_ms() - t0;

  EXPECT_GE(elapsed, kGraceMs - 20) << "finalize must wait the full grace window";
  EXPECT_EQ(ut_st20p_tx_get_pending(sink), 2) << "buffers never drained";
}

/* Nothing pending → finalize returns promptly (no grace wait). */
TEST_F(St20pTxFinalizeTest, FinalizeFastWhenNothingPending) {
  Gst_Mtl_St20p_Tx* sink = ut_st20p_tx_new();
  ASSERT_NE(sink, nullptr);

  gint64 t0 = now_ms();
  ut_st20p_tx_finalize(sink);
  gint64 elapsed = now_ms() - t0;

  EXPECT_LT(elapsed, 50) << "no pending buffers must not trigger the grace wait";
}
