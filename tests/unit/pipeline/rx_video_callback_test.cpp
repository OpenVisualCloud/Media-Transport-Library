/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The st20p/st22p RX free and st20rc_rx_free() on the scheduler thread of their
 * transport, where its callbacks into them run: they must return -EBUSY before they
 * change anything, so no transport is left calling back into a freed ctx.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*RxVideoCallback*'
 */

#include <gtest/gtest.h>

#include <cerrno>

#include "experimental/st20_combined_api.h"
#include "pipeline/st20p_harness.h"
#include "pipeline/st22p_harness.h"
#include "session/st20_harness.h"

namespace {

int St20pFree(void* handle) {
  return st20p_rx_free(static_cast<st20p_rx_handle>(handle));
}

int St22pFree(void* handle) {
  return st22p_rx_free(static_cast<st22p_rx_handle>(handle));
}

int St20rcFree(void* handle) {
  return st20rc_rx_free(static_cast<st20rc_rx_handle>(handle));
}

/* The transport's own callback, then another session's on the same scheduler. */
constexpr int kPolls[] = {0, 1};

class St20pRxVideoCallbackTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut20p_init());
    transport_ = ut20_cb_create();
    ASSERT_NE(nullptr, transport_);
    ctx_ = ut20p_ctx_create(1);
    ASSERT_NE(nullptr, ctx_);
    ut20p_set_transport(ctx_, ut20_cb_handle(transport_));
  }
  void TearDown() override {
    if (ut20_cb_wedged(transport_)) return; /* a call stuck in both still uses them */
    ut20p_ctx_destroy(ctx_);
    ut20_cb_destroy(transport_);
  }
  ut20_cb_ctx* transport_ = nullptr;
  ut20p_ctx* ctx_ = nullptr;
};

class St22pRxVideoCallbackTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut22p_init());
    transport_ = ut20_cb_create();
    ASSERT_NE(nullptr, transport_);
    ctx_ = ut22p_ctx_create(1);
    ASSERT_NE(nullptr, ctx_);
    ut22p_set_transport(ctx_, ut20_cb_handle22(transport_));
  }
  void TearDown() override {
    if (ut20_cb_wedged(transport_)) return;
    ut22p_ctx_destroy(ctx_);
    ut20_cb_destroy(transport_);
  }
  ut20_cb_ctx* transport_ = nullptr;
  ut22p_ctx* ctx_ = nullptr;
};

class St20rcRxVideoCallbackTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut20_init());
    transport_ = ut20_cb_create();
    ASSERT_NE(nullptr, transport_);
  }
  void TearDown() override {
    ut20_cb_destroy(transport_);
  }
  ut20_cb_ctx* transport_ = nullptr;
};

TEST_F(St20pRxVideoCallbackTest, FreeOnItsSchedulerKeepsThePipelineAndItsTransport) {
  for (int poll : kPolls) {
    SCOPED_TRACE(poll);
    ASSERT_EQ(-EBUSY,
              ut20_cb_run_in_poll(transport_, poll, St20pFree, ut20p_handle(ctx_)));
    EXPECT_FALSE(ut20p_destroying(ctx_));
    EXPECT_FALSE(ut20_cb_took_effect(transport_, UT20_CB_FREE));
  }
  ASSERT_EQ(0, ut20p_inject_frame(ctx_, ST_FRAME_STATUS_COMPLETE, 1));
  EXPECT_NE(nullptr, ut20p_get_frame(ctx_));
}

TEST_F(St22pRxVideoCallbackTest, FreeOnItsSchedulerKeepsThePipelineAndItsTransport) {
  for (int poll : kPolls) {
    SCOPED_TRACE(poll);
    ASSERT_EQ(-EBUSY,
              ut20_cb_run_in_poll(transport_, poll, St22pFree, ut22p_handle(ctx_)));
    EXPECT_FALSE(ut22p_destroying(ctx_));
    EXPECT_FALSE(ut20_cb_took_effect(transport_, UT20_CB_ST22_FREE));
  }
  ASSERT_EQ(0, ut22p_inject_frame(ctx_, ST_FRAME_STATUS_COMPLETE, 1));
  EXPECT_NE(nullptr, ut22p_get_frame(ctx_));
}

TEST_F(St20rcRxVideoCallbackTest, FreeOnATransportsSchedulerKeepsTheSession) {
  ASSERT_EQ(-EBUSY, ut20_cb_run_in_poll(transport_, 0, St20rcFree,
                                        ut20_cb_st20rc_handle(transport_)));
  EXPECT_TRUE(ut20_cb_st20rc_kept(transport_));
  EXPECT_FALSE(ut20_cb_took_effect(transport_, UT20_CB_FREE));
}

}  // namespace
