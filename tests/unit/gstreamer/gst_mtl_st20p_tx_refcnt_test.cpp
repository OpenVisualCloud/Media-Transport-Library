/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Reproduces the "frame %u refcnt not zero" / stuck-frame symptom of the ST20p
 * TX zero-copy path at unit scope.
 *
 * The literal message is emitted by the data-plane tasklet
 * (st_tx_video_session.c:1883), which needs a NIC + lcore and therefore cannot
 * run in UnitTest. Instead the harness models the transport's per-slot refcnt
 * contract faithfully and drives the REAL plugin release path
 * (gst_mtl_st20p_tx_frame_done) and the REAL static finalize through it, so the
 * teardown ordering that leads to stuck frames is exercised without hardware.
 *
 * Negative terminology: each test asserts that the failure CONDITION is
 * reproduced (the guard trips / the frame stays referenced / the buffer leaks).
 */

#include <glib.h>
#include <gtest/gtest.h>
#include <pthread.h>

extern "C" {
#include "gstreamer/gst_mtl_st20p_tx_harness.h"
}

class St20pTxRefcntContractNegativeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ut_st20p_tx_init();
    ut_st20p_tx_fake_reset();
  }
};

/* A transport slot reused before its previous TX drained trips the
 * "frame refcnt not zero" guard — the steady-state root cause. */
TEST_F(St20pTxRefcntContractNegativeTest, ReproducesRefcntNotZeroOnSlotReuse) {
  Gst_Mtl_St20p_Tx* sink = ut_st20p_tx_new();
  ASSERT_NE(sink, nullptr);
  ut_st20p_tx_fake_attach(sink);

  int slot = ut_st20p_tx_put_ext_buffer(sink); /* refcnt 0->1, pending 1 */
  ASSERT_GE(slot, 0);
  ASSERT_EQ(ut_st20p_tx_get_pending(sink), 1);

  /* Reuse the same transport slot before frame_done drained it (the
   * tv_frame_free_cb window: pipeline slot FREE but transport refcnt still 1). */
  ut_st20p_tx_transport_reacquire(sink, slot);

  EXPECT_TRUE(ut_st20p_tx_fake_refcnt_violation())
      << "reusing a transport slot while refcnt!=0 must trip 'frame refcnt not zero'";

  /* Drain so the test does not leak. */
  ut_st20p_tx_transport_complete(sink, slot);
  EXPECT_EQ(ut_st20p_tx_get_pending(sink), 0);
}

/* Tearing down while a zero-copy frame is still in flight (frame_done never
 * delivered) leaves the transport frame referenced and leaks the GstBuffer —
 * the user's shutdown symptom. */
TEST_F(St20pTxRefcntContractNegativeTest, ReproducesStuckFrameAndBufferLeakOnTeardown) {
  Gst_Mtl_St20p_Tx* sink = ut_st20p_tx_new();
  ASSERT_NE(sink, nullptr);
  ut_st20p_tx_fake_attach(sink);

  int slot = ut_st20p_tx_put_ext_buffer(sink); /* in flight: refcnt 1, pending 1 */
  ASSERT_GE(slot, 0);
  ASSERT_EQ(ut_st20p_tx_get_pending(sink), 1);

  /* App tears down before the NIC finished TX. The grace wait times out, then
   * st20p_tx_free runs without the frame having drained. */
  ut_st20p_tx_finalize(sink);

  EXPECT_GE(ut_st20p_tx_fake_stuck_frames(), 1)
      << "transport frame still refcnt!=0 at free (reproduces stuck frame)";
  EXPECT_EQ(ut_st20p_tx_get_pending(sink), 1)
      << "GstBuffer leaked: frame_done never reclaimed it";
}

/* Option A: finalize's authoritative drain. When the in-flight frame completes
 * shortly after teardown begins (the common case — NIC finishes TX), finalize
 * waits for frame_done to reclaim it before st20p_tx_free, so the transport is
 * freed with no frame still referenced and the GstBuffer is not leaked. */
class St20pTxFinalizeDrainTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ut_st20p_tx_init();
    ut_st20p_tx_fake_reset();
  }
};

namespace {
struct DrainCtx {
  Gst_Mtl_St20p_Tx* sink;
  int slot;
};

void* drain_complete_thread(void* arg) {
  DrainCtx* c = static_cast<DrainCtx*>(arg);
  /* NIC frees the frame's mbufs shortly after teardown starts, well inside the
   * grace window. */
  g_usleep(20 * 1000);
  ut_st20p_tx_transport_complete(c->sink, c->slot);
  return nullptr;
}
}  // namespace

TEST_F(St20pTxFinalizeDrainTest, FinalizeWaitsForInFlightFrameThenFreesClean) {
  Gst_Mtl_St20p_Tx* sink = ut_st20p_tx_new();
  ASSERT_NE(sink, nullptr);
  ut_st20p_tx_fake_attach(sink);

  int slot = ut_st20p_tx_put_ext_buffer(sink); /* in flight: refcnt 1, pending 1 */
  ASSERT_GE(slot, 0);
  ASSERT_EQ(ut_st20p_tx_get_pending(sink), 1);

  DrainCtx ctx = {sink, slot};
  pthread_t th;
  ASSERT_EQ(pthread_create(&th, nullptr, drain_complete_thread, &ctx), 0);

  ut_st20p_tx_finalize(sink); /* blocks on the grace wait until the frame drains */

  pthread_join(th, nullptr);

  EXPECT_EQ(ut_st20p_tx_get_pending(sink), 0)
      << "finalize must wait for the frame to drain before freeing";
  EXPECT_EQ(ut_st20p_tx_fake_stuck_frames(), 0)
      << "no transport frame left referenced at free";
  EXPECT_FALSE(ut_st20p_tx_fake_refcnt_violation())
      << "clean drain must not trip the refcnt guard";
}
