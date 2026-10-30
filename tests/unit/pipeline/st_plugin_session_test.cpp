/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * st22_get_encoder(), st22_get_decoder() and st20_get_converter() hand the plugin a
 * session it may already use from inside create_session, and keep what the plugin
 * writes back into the create request.
 */

#include <gtest/gtest.h>

#include "pipeline/st_plugin_harness.h"

class StPlugin : public ::testing::Test {
 protected:
  void SetUp() override {
    ctx_ = ut_plugin_create();
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut_plugin_destroy(ctx_);
  }

  ut_plugin_ctx* ctx_ = nullptr;
};

class StPluginSession : public StPlugin,
                        public ::testing::WithParamInterface<ut_plugin_kind> {};

TEST_P(StPluginSession, GetFrameInsideCreateReachesTheNewRequest) {
  int frame0, frame1;

  void* slot = ut_plugin_get(ctx_, GetParam(), &frame0);
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(ut_plugin_frame_in_create(ctx_), &frame0);
  ut_plugin_put(ctx_, GetParam(), slot);

  ASSERT_EQ(ut_plugin_get(ctx_, GetParam(), &frame1), slot);
  EXPECT_EQ(ut_plugin_frame_in_create(ctx_), &frame1);
  ut_plugin_put(ctx_, GetParam(), slot);
}

TEST_P(StPluginSession, FailedCreateLeavesTheSlotUncallable) {
  int frame;

  void* slot = ut_plugin_get(ctx_, GetParam(), &frame);
  ASSERT_NE(slot, nullptr);
  ut_plugin_put(ctx_, GetParam(), slot);

  ut_plugin_fail_create(ctx_);
  ASSERT_EQ(ut_plugin_get(ctx_, GetParam(), &frame), nullptr);
  EXPECT_EQ(ut_plugin_get_frame(GetParam(), slot), nullptr);
}

INSTANTIATE_TEST_SUITE_P(Devs, StPluginSession,
                         ::testing::Values(UT_PLUGIN_ENCODER, UT_PLUGIN_DECODER,
                                           UT_PLUGIN_CONVERTER),
                         [](const ::testing::TestParamInfo<ut_plugin_kind>& info) {
                           switch (info.param) {
                             case UT_PLUGIN_ENCODER:
                               return "Encoder";
                             case UT_PLUGIN_DECODER:
                               return "Decoder";
                             default:
                               return "Converter";
                           }
                         });

TEST_F(StPlugin, CreateResponseLandsInTheSession) {
  void* encoder = ut_plugin_get(ctx_, UT_PLUGIN_ENCODER, nullptr);
  void* decoder = ut_plugin_get(ctx_, UT_PLUGIN_DECODER, nullptr);
  ASSERT_NE(encoder, nullptr);
  ASSERT_NE(decoder, nullptr);

  EXPECT_EQ(ut_plugin_encoder_max_size(encoder), UT_PLUGIN_RESP_MAX_SIZE);
  EXPECT_EQ(ut_plugin_encoder_resp_flag(encoder), UT_PLUGIN_RESP_FLAG);
  EXPECT_EQ(ut_plugin_decoder_resp_flag(decoder), UT_PLUGIN_RESP_FLAG);

  ut_plugin_put(ctx_, UT_PLUGIN_ENCODER, encoder);
  ut_plugin_put(ctx_, UT_PLUGIN_DECODER, decoder);
}
