/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * An iavf VF reset frees every rte_flow of the port. mt_flow.c lists each rte_flow based
 * response, holds the flow mutex from the pre to the post reset callback, and creates
 * each listed flow again in place, so the responses the queues hold stay valid. The
 * callbacks are registered post first and unregistered pre first, retrying while the VF
 * resets.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='MtFlowIavfResetTest.*'
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "dev/mt_flow_harness.h"

class MtFlowIavfResetTest : public testing::Test {
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

#define NEEDS_RESET_CB()                                                              \
  if (!ut_flow_has_reset_cb()) GTEST_SKIP() << "no iavf reset callbacks before DPDK " \
                                               "26.03"

TEST_F(MtFlowIavfResetTest, CreateListsTheFlowAndFreeUnlistsIt) {
  int id = ut_flow_create(ctx_, 20000);

  ASSERT_GE(id, 0);
  EXPECT_EQ(1, ut_flow_listed_count(ctx_));
  EXPECT_EQ(0, ut_flow_free(ctx_, id));
  EXPECT_EQ(0, ut_flow_listed_count(ctx_));
  EXPECT_EQ(1, ut_flow_destroy_calls(ctx_));
}

TEST_F(MtFlowIavfResetTest, ResponseWithoutAFlowIsNotListed) {
  NEEDS_RESET_CB();
  ut_flow_set_no_flow(ctx_, true);
  int id = ut_flow_create(ctx_, 20000);

  ASSERT_GE(id, 0);
  EXPECT_EQ(nullptr, ut_flow_handle(ctx_, id));
  EXPECT_EQ(0, ut_flow_listed_count(ctx_));
  ut_flow_iavf_pre(ctx_);
  ut_flow_iavf_post(ctx_, 0);
  EXPECT_EQ(0, ut_flow_create_calls(ctx_));
  EXPECT_EQ(0, ut_flow_free(ctx_, id));
}

TEST_F(MtFlowIavfResetTest, ResetCreatesEveryFlowAgainFromItsRequest) {
  NEEDS_RESET_CB();
  const uint16_t ports[] = {20000, 20002, 20004};
  int ids[3];
  const void* old_flows[3];

  for (int i = 0; i < 3; i++) {
    ids[i] = ut_flow_create(ctx_, ports[i]);
    ASSERT_GE(ids[i], 0);
    old_flows[i] = ut_flow_handle(ctx_, ids[i]);
  }
  ut_flow_iavf_pre(ctx_);
  ut_flow_iavf_post(ctx_, 0);

  EXPECT_EQ(6, ut_flow_create_calls(ctx_));
  EXPECT_EQ(0, ut_flow_destroy_calls(ctx_)) << "the PMD freed the old flows";
  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(ports[i], ut_flow_create_dst_port(ctx_, 3 + i)) << "flow " << i;
    EXPECT_EQ(0, ut_flow_create_queue(ctx_, 3 + i)) << "flow " << i;
    EXPECT_NE(nullptr, ut_flow_handle(ctx_, ids[i])) << "flow " << i;
    EXPECT_NE(old_flows[i], ut_flow_handle(ctx_, ids[i])) << "flow " << i;
  }
  EXPECT_EQ(3, ut_flow_listed_count(ctx_));
}

TEST_F(MtFlowIavfResetTest, FailedResetLeavesNoStaleFlow) {
  NEEDS_RESET_CB();
  int a = ut_flow_create(ctx_, 20000), b = ut_flow_create(ctx_, 20002);

  ut_flow_iavf_pre(ctx_);
  ut_flow_iavf_post(ctx_, -EIO);

  EXPECT_EQ(2, ut_flow_create_calls(ctx_));
  EXPECT_EQ(nullptr, ut_flow_handle(ctx_, a));
  EXPECT_EQ(nullptr, ut_flow_handle(ctx_, b));
  EXPECT_EQ(0, ut_flow_free(ctx_, a));
  EXPECT_EQ(0, ut_flow_free(ctx_, b));
  EXPECT_EQ(0, ut_flow_destroy_calls(ctx_)) << "no destroy of a freed handle";
  EXPECT_EQ(0, ut_flow_listed_count(ctx_));
}

TEST_F(MtFlowIavfResetTest, FreeWaitsOutTheResetThenDestroysTheNewFlow) {
  NEEDS_RESET_CB();
  int id = ut_flow_create(ctx_, 20000);
  std::atomic<bool> freed{false};

  ASSERT_GE(id, 0);
  ut_flow_iavf_pre(ctx_);
  std::thread t([&] {
    ut_flow_free(ctx_, id);
    freed = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(freed) << "a free must not destroy a flow the reset is freeing";
  ut_flow_iavf_post(ctx_, 0);
  t.join();

  EXPECT_TRUE(freed);
  EXPECT_EQ(1, ut_flow_destroy_calls(ctx_));
  EXPECT_EQ(ut_flow_last_created(ctx_), ut_flow_last_destroyed(ctx_));
  EXPECT_EQ(0, ut_flow_listed_count(ctx_));
}

TEST_F(MtFlowIavfResetTest, PostWithoutPreCreatesTheFlowsAgain) {
  NEEDS_RESET_CB();
  int id = ut_flow_create(ctx_, 20000);
  const void* old_flow = ut_flow_handle(ctx_, id);

  ut_flow_iavf_post(ctx_, 0);
  EXPECT_NE(old_flow, ut_flow_handle(ctx_, id));
  EXPECT_GE(ut_flow_create(ctx_, 20002), 0) << "post left the flow mutex free";
}

TEST_F(MtFlowIavfResetTest, RegisterSetsPostBeforePre) {
  NEEDS_RESET_CB();
  ut_flow_reset_cb_register(ctx_);

  EXPECT_STREQ("Pp", ut_flow_reg_log(ctx_));
  EXPECT_TRUE(ut_flow_reset_cb_on(ctx_));
}

TEST_F(MtFlowIavfResetTest, UnregisterClearsPreFirstAndWaitsOutAReset) {
  NEEDS_RESET_CB();
  ut_flow_reset_cb_register(ctx_);
  ut_flow_set_reg_busy(ctx_, 2); /* -EBUSY twice, as while the VF resets */
  ut_flow_reset_cb_unregister(ctx_);

  EXPECT_STREQ("PpxX", ut_flow_reg_log(ctx_));
  EXPECT_FALSE(ut_flow_reset_cb_on(ctx_));
  EXPECT_EQ(1, ut_flow_uinit_frees(ctx_));
}

TEST_F(MtFlowIavfResetTest, UnregisterThatStaysBusyGivesUp) {
  NEEDS_RESET_CB();
  ut_flow_reset_cb_register(ctx_);
  ut_flow_set_reg_busy(ctx_, 100);
  ut_flow_reset_cb_unregister(ctx_);

  EXPECT_STREQ("Pp", ut_flow_reg_log(ctx_));
  EXPECT_FALSE(ut_flow_reset_cb_on(ctx_));
  EXPECT_EQ(1, ut_flow_uinit_frees(ctx_));
}

TEST_F(MtFlowIavfResetTest, RegisterThatStaysBusySetsNoPre) {
  NEEDS_RESET_CB();
  ut_flow_set_reg_busy(ctx_, 100);
  ut_flow_reset_cb_register(ctx_);

  EXPECT_STREQ("", ut_flow_reg_log(ctx_));
  EXPECT_FALSE(ut_flow_reset_cb_on(ctx_));
}
