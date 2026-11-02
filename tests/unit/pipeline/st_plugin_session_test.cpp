/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The plugin's create_session may already use the session it is handed.
 */

#include <gtest/gtest.h>

#include "pipeline/st_plugin_harness.h"

class StPluginSession : public ::testing::TestWithParam<ut_plugin_kind> {
 protected:
  void SetUp() override {
    ut_plugin_reset();
  }
};

TEST_P(StPluginSession, GetFrameInsideCreateReachesTheNewRequest) {
  int frame0, frame1;

  void* slot = ut_plugin_get(GetParam(), &frame0);
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(ut_plugin_frame_in_create(), &frame0);
  EXPECT_EQ(ut_plugin_get_frame(GetParam(), slot), &frame0);
  ut_plugin_put(GetParam(), slot);

  ASSERT_EQ(ut_plugin_get(GetParam(), &frame1), slot);
  EXPECT_EQ(ut_plugin_frame_in_create(), &frame1);
}

TEST_P(StPluginSession, FailedCreateLeavesTheSlotUncallable) {
  int frame;

  void* slot = ut_plugin_get(GetParam(), &frame);
  ASSERT_NE(slot, nullptr);
  ut_plugin_put(GetParam(), slot);

  ut_plugin_fail_create();
  ASSERT_EQ(ut_plugin_get(GetParam(), &frame), nullptr);
  EXPECT_EQ(ut_plugin_get_frame(GetParam(), slot), nullptr);
}

INSTANTIATE_TEST_SUITE_P(Devs, StPluginSession,
                         ::testing::Values(UT_PLUGIN_ENCODER, UT_PLUGIN_DECODER,
                                           UT_PLUGIN_CONVERTER));

TEST(StPlugin, CreateResponseLandsInTheSession) {
  ut_plugin_reset();
  for (ut_plugin_kind kind : {UT_PLUGIN_ENCODER, UT_PLUGIN_DECODER}) {
    void* slot = ut_plugin_get(kind, nullptr);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(ut_plugin_resp_flag(kind, slot), UT_PLUGIN_RESP_FLAG) << kind;
  }
}
