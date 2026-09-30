/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins how tv_frame_free_cb() hands a completed ext frame back.
 * notify_frame_done is where st20p marks the slot FREE and wakes the app, so the
 * frame must already be free in the transport when it runs: the app, or the
 * builder on another lcore, may re-arm and re-queue the slot straight away.
 * Apps map frame_idx back to their buffer with st20_tx_get_framebuffer() there.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxExtFrameDoneTest.*'
 */

#include <gtest/gtest.h>

#include <cstdint>

#include "session/st20_tx_harness.h"

namespace {

class St20TxExtFrameDoneTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_txv_init());
    ctx_ = ut_txv_create();
    ASSERT_NE(nullptr, ctx_);
    ASSERT_EQ(0, ut_txv_ext_frame_setup(ctx_, buf_a_));
  }

  void TearDown() override {
    ut_txv_destroy(ctx_);
  }

  ut_txv_ctx* ctx_ = nullptr;
  uint8_t buf_a_[UT_TXV_FRAME_SIZE] = {};
  uint8_t buf_b_[UT_TXV_FRAME_SIZE] = {};
};

TEST_F(St20TxExtFrameDoneTest, FrameCanBeRearmedFromNotifyFrameDone) {
  ut_txv_set_rearm_on_done(ctx_, buf_b_);

  ut_txv_ext_frame_complete(ctx_);

  EXPECT_EQ(0, ut_txv_rearm_ret(ctx_));
  EXPECT_EQ(static_cast<void*>(buf_b_), ut_txv_framebuffer(ctx_))
      << "the buffer armed from notify_frame_done must not be overwritten";
}

TEST_F(St20TxExtFrameDoneTest, BuilderAcceptsFrameRequeuedFromNotifyFrameDone) {
  ut_txv_set_rearm_on_done(ctx_, buf_b_);
  ut_txv_set_build_on_done(ctx_);

  ut_txv_ext_frame_complete(ctx_);

  EXPECT_EQ(1u, ut_txv_built_on_done(ctx_))
      << "a builder refusing the slot leaves st20p's frame IN_TRANSMITTING for good";
}

TEST_F(St20TxExtFrameDoneTest, BuilderRunFromNotifyFrameDoneKeepsItsMeta) {
  ut_txv_set_rearm_on_done(ctx_, buf_b_);
  ut_txv_set_build_on_done(ctx_);

  ut_txv_ext_frame_complete(ctx_);

  ASSERT_EQ(1u, ut_txv_built_on_done(ctx_));
  EXPECT_TRUE(ut_txv_done_meta_kept(ctx_))
      << "the frame restarted from notify_frame_done rewrote the meta it was given";
}

TEST_F(St20TxExtFrameDoneTest, CompletedBufferIsReadableInNotifyFrameDone) {
  ut_txv_ext_frame_complete(ctx_);

  EXPECT_EQ(static_cast<void*>(buf_a_), ut_txv_done_framebuffer(ctx_));
}

TEST_F(St20TxExtFrameDoneTest, RearmedBufferIsReadableInNotifyFrameDone) {
  ut_txv_set_rearm_on_done(ctx_, buf_b_);

  ut_txv_ext_frame_complete(ctx_);

  EXPECT_EQ(static_cast<void*>(buf_b_), ut_txv_rearmed_framebuffer(ctx_));
}

TEST_F(St20TxExtFrameDoneTest, CompletedFrameNotRearmedHasNoBuffer) {
  ut_txv_ext_frame_complete(ctx_);

  EXPECT_EQ(nullptr, ut_txv_framebuffer(ctx_));
  EXPECT_EQ(0u, ut_txv_ext_frame_iova(ctx_));
}

TEST_F(St20TxExtFrameDoneTest, FrameRearmedAfterCompletionKeepsNewBuffer) {
  ut_txv_ext_frame_complete(ctx_);

  ASSERT_EQ(0, ut_txv_set_ext_frame(ctx_, 0, buf_b_));
  EXPECT_EQ(static_cast<void*>(buf_b_), ut_txv_framebuffer(ctx_));
}

}  // namespace
