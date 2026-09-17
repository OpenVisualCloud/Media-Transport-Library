/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * gst_mtl_st40p_tx_start() creates the ST 2110-40 session. It used to discard the
 * result and return TRUE regardless, so an element with no session went to PLAYING and
 * the pipeline only learned about it per buffer, from the "Tx handle not initialized"
 * of the render path.
 *
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='GstMtlSt40pTx*'
 */

#include <gtest/gtest.h>

#include "gstreamer/gst_mtl_st40p_tx_harness.h"

namespace {
struct RejectedCase {
  enum ut_gst_st40p_tx_start_case c;
  const char* rejected_for;
};
}  // namespace

/* The three rejection exits of the session create that a value the properties admit can
 * reach. Of the other four, its MTL handle check cannot be reached through start(), which
 * checks the handle itself first (MtlHandleFailureFailsStart below); "tx_handle already
 * initialized" is a restart, which start() treats as success; and the DID and SDID range
 * checks sit behind properties GObject clamps to 0xff. */
TEST(GstMtlSt40pTxStart, SessionCreateFailureFailsStart) {
  const RejectedCase cases[] = {
      {UT_GST_ST40P_TX_START_PORT_UNSET, "port arguments"},
      {UT_GST_ST40P_TX_START_FPS_OFF_TABLE, "framerate off the ST 2110 set"},
      {UT_GST_ST40P_TX_START_CREATE_FAILS, "st40p_tx_create"},
  };

  for (auto& c : cases) {
    struct ut_gst_st40p_tx_start_result r = {};

    ut_gst_st40p_tx_start(c.c, &r);
    EXPECT_FALSE(r.started) << c.rejected_for;
    EXPECT_FALSE(r.handle_set) << c.rejected_for;
    /* a sink that cannot transmit must not be the one that starts the pipeline */
    EXPECT_EQ(r.set_state_calls, 0) << c.rejected_for;
  }
}

TEST(GstMtlSt40pTxStart, MtlHandleFailureFailsStart) {
  struct ut_gst_st40p_tx_start_result r = {};

  ut_gst_st40p_tx_start(UT_GST_ST40P_TX_START_NO_MTL_HANDLE, &r);
  EXPECT_FALSE(r.started);
  EXPECT_FALSE(r.handle_set);
  EXPECT_EQ(r.set_state_calls, 0);
}

TEST(GstMtlSt40pTxStart, SessionCreateSuccessStartsAndPlays) {
  struct ut_gst_st40p_tx_start_result r = {};

  ut_gst_st40p_tx_start(UT_GST_ST40P_TX_START_OK, &r);
  EXPECT_TRUE(r.started);
  EXPECT_TRUE(r.handle_set);
  EXPECT_EQ(r.create_calls, 1);
  /* Positive control for the counter the rejection cases assert is 0: this pins that
   * start() does reach its self-transition to PLAYING when the session was created. */
  EXPECT_EQ(r.set_state_calls, 1);
}

/* Nothing frees tx_handle before finalize, so the second READY -> PAUSED of an element
 * that is stopped and started again finds the session still there. Failing the session
 * create through would turn that restart into a dead pipeline. */
TEST(GstMtlSt40pTxStart, RestartKeepsTheSessionTheFirstStartMade) {
  struct ut_gst_st40p_tx_start_result r = {};

  ut_gst_st40p_tx_start(UT_GST_ST40P_TX_START_RESTART, &r);
  EXPECT_TRUE(r.started);
  EXPECT_TRUE(r.handle_set);
  EXPECT_EQ(r.create_calls, 0);
  EXPECT_EQ(r.set_state_calls, 1);
}
