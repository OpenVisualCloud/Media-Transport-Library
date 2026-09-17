/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * gst_mtl_st20p_tx_session_create() runs once per GST_EVENT_CAPS, and a pipeline that
 * renegotiates hits it again. Two things it got wrong were invisible to the caller:
 * it dropped the GstVideoInfo on every rejection path, and it read the info before
 * checking that gst_video_info_new_from_caps() had returned one.
 *
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='GstMtlSt20pTx*'
 */

#include <gtest/gtest.h>

#include "gstreamer/gst_mtl_st20p_tx_harness.h"

namespace {
struct RejectedCase {
  enum ut_gst_st20p_tx_case c;
  const char* rejected_for;
};
}  // namespace

/* One case per exit taken after the info exists and before the plugin frees it. */
TEST(GstMtlSt20pTxSessionCreate, EveryRejectionReturnsTheVideoInfo) {
  const RejectedCase cases[] = {
      {UT_GST_ST20P_TX_INTERLACE_UNSUPPORTED, "interlace mode"},
      {UT_GST_ST20P_TX_FORMAT_UNSUPPORTED, "input format"},
      {UT_GST_ST20P_TX_FPS_OFF_TABLE, "framerate off the ST 2110 set"},
      {UT_GST_ST20P_TX_FPS_DEN_ZERO, "zero framerate denominator"},
      {UT_GST_ST20P_TX_PORT_UNSET, "port arguments"},
  };

  for (auto& c : cases) {
    struct ut_gst_st20p_tx_result r = {};

    ut_gst_st20p_tx_session_create(c.c, &r);
    EXPECT_FALSE(r.created) << c.rejected_for;
    EXPECT_FALSE(r.handle_set) << c.rejected_for;
    EXPECT_EQ(r.info_allocs, 1) << c.rejected_for;
    EXPECT_EQ(r.info_frees, r.info_allocs) << c.rejected_for;
  }
}

/* A NULL info is what a caps event carrying no format, or one gst cannot map, gives.
 * Before the check this dereferenced NULL for info->width. */
TEST(GstMtlSt20pTxSessionCreate, NullVideoInfoIsRejectedNotDereferenced) {
  struct ut_gst_st20p_tx_result r = {};

  ut_gst_st20p_tx_session_create(UT_GST_ST20P_TX_INFO_NULL, &r);
  EXPECT_FALSE(r.created);
  EXPECT_FALSE(r.handle_set);
  EXPECT_EQ(r.info_allocs, 0);
  EXPECT_EQ(r.info_frees, 0);
}

/* The one rejection that precedes the info, so it must not free anything. */
TEST(GstMtlSt20pTxSessionCreate, NoMtlHandleTakesNoVideoInfo) {
  struct ut_gst_st20p_tx_result r = {};

  ut_gst_st20p_tx_session_create(UT_GST_ST20P_TX_NO_MTL_HANDLE, &r);
  EXPECT_FALSE(r.created);
  EXPECT_EQ(r.info_allocs, 0);
  EXPECT_EQ(r.info_frees, 0);
}

/* st20p_tx_create() is reached after the info is already freed: the count stays at one,
 * which is what says the rejection paths do not free it a second time. */
TEST(GstMtlSt20pTxSessionCreate, HandleCreateFailureFreesTheVideoInfoOnce) {
  struct ut_gst_st20p_tx_result r = {};

  ut_gst_st20p_tx_session_create(UT_GST_ST20P_TX_CREATE_FAILS, &r);
  EXPECT_FALSE(r.created);
  EXPECT_FALSE(r.handle_set);
  EXPECT_EQ(r.info_allocs, 1);
  EXPECT_EQ(r.info_frees, 1);
}

/* Positive control for the counters the rejection cases assert on: this is the one path
 * that reaches the plugin's own free, so info_frees == 1 here is not a leak. */
TEST(GstMtlSt20pTxSessionCreate, ProgressiveCapsCreateTheSession) {
  struct ut_gst_st20p_tx_result r = {};

  ut_gst_st20p_tx_session_create(UT_GST_ST20P_TX_OK, &r);
  EXPECT_TRUE(r.created);
  EXPECT_TRUE(r.handle_set);
  EXPECT_EQ(r.info_allocs, 1);
  EXPECT_EQ(r.info_frees, 1);
}
