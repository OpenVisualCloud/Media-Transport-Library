/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "dev/mt_flow_harness.h"

// Pins the retry loops in lib/src/mt_flow.c that work around a transient VF
// flow-rule timeout: rte_rx_flow_create() retries rte_flow_create() up to 5
// times (10ms apart) before giving up or, for no-ip flows, falling back to
// the e810 group-2 attribute; rx_flow_free() retries rte_flow_destroy() the
// same way. The harness shadows the three rte_flow_* entry points so no
// hardware or DPDK PMD is involved.
class MtFlowRetryTest : public testing::Test {
 protected:
  void SetUp() override {
    ctx_ = ut_flow_create_ctx();
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut_flow_destroy_ctx(ctx_);
  }

  ut_flow_ctx* ctx_ = nullptr;
};

// A transient failure that clears up inside the retry budget must still
// produce a usable flow, using exactly the calls it took to succeed (no
// extra call once rte_flow_create() finally returns non-NULL).
TEST_F(MtFlowRetryTest, CreateSucceedsOnFifthRetry) {
  ut_flow_set_create_fail_count(ctx_, 4);
  EXPECT_TRUE(ut_flow_rx_flow_create(ctx_, false));
  EXPECT_EQ(ut_flow_create_calls(ctx_), 5);
}

// A failure that never clears must stop at exactly max_retry attempts, not
// loop forever or under-try. has_ip_flow stays true here, so the e810
// group-2 fallback (which only applies to no-ip flows) must not fire either.
TEST_F(MtFlowRetryTest, CreateExhaustsRetriesWithIpFlow) {
  ut_flow_set_create_fail_count(ctx_, -1);
  EXPECT_FALSE(ut_flow_rx_flow_create(ctx_, false));
  EXPECT_EQ(ut_flow_create_calls(ctx_), 5);
}

// When has_ip_flow is false (MT_RXQ_FLOW_F_NO_IP), exhausting the 5 default
// attempts must still fall through to the e810 group-2 fallback for one more
// rte_flow_validate()+rte_flow_create() pair, and that 6th create call must
// carry attr.group == 2.
TEST_F(MtFlowRetryTest, CreateFallsBackToGroupTwoWithoutIpFlow) {
  ut_flow_set_create_fail_count(ctx_, 5);
  EXPECT_TRUE(ut_flow_rx_flow_create(ctx_, true));
  EXPECT_EQ(ut_flow_create_calls(ctx_), 6);
  EXPECT_EQ(ut_flow_create_call_group(ctx_, 5), 2u);
}

// Mirrors CreateSucceedsOnFifthRetry for the destroy side: a transient
// destroy failure that clears within budget succeeds, and rsp->flow is
// cleared to NULL on the success path.
TEST_F(MtFlowRetryTest, FreeSucceedsOnFourthRetry) {
  ut_flow_set_destroy_fail_count(ctx_, 3);
  EXPECT_EQ(ut_flow_rx_flow_free(ctx_), 0);
  EXPECT_EQ(ut_flow_destroy_calls(ctx_), 4);
  EXPECT_TRUE(ut_flow_last_free_flow_cleared(ctx_));
}

// rx_flow_free() has no error return path: even after exhausting every
// retry it still clears rsp->flow and returns 0, since the caller has no use
// for a destroy failure once the rsp is being torn down anyway.
TEST_F(MtFlowRetryTest, FreeExhaustsRetriesButStillReturnsZero) {
  ut_flow_set_destroy_fail_count(ctx_, -1);
  EXPECT_EQ(ut_flow_rx_flow_free(ctx_), 0);
  EXPECT_EQ(ut_flow_destroy_calls(ctx_), 5);
  EXPECT_TRUE(ut_flow_last_free_flow_cleared(ctx_));
}
