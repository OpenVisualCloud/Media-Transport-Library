/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * An iavf VF reset frees every rte_flow of the port. mt_flow.c lists each rte_flow based
 * response, holds the flow mutex from the pre to the post reset callback, and creates
 * each listed flow again in place, so the responses the queues hold stay valid. The
 * callbacks are registered post first and unregistered pre first, retrying while the VF
 * resets: a register gives up, an unregister waits the reset out.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='MtFlowIavfResetTest.*'
 */

#include <gtest/gtest.h>
#include <link.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "common/ut_common.h"
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

#define NEEDS_RESET_CB() \
  if (!ut_flow_has_reset_cb()) GTEST_SKIP() << "iavf reset callbacks need DPDK 26.03"

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
  const uint16_t queues[] = {1, 3, 2};
  int ids[3];
  const void* old_flows[3];

  for (int i = 0; i < 3; i++) {
    ids[i] = ut_flow_create_q(ctx_, ports[i], queues[i]);
    ASSERT_GE(ids[i], 0);
    ASSERT_EQ(queues[i], ut_flow_create_queue(ctx_, i));
    old_flows[i] = ut_flow_handle(ctx_, ids[i]);
  }
  ut_flow_iavf_pre(ctx_);
  ut_flow_iavf_post(ctx_, 0);

  EXPECT_EQ(6, ut_flow_create_calls(ctx_));
  EXPECT_EQ(0, ut_flow_destroy_calls(ctx_)) << "the PMD freed the old flows";
  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(ports[i], ut_flow_create_dst_port(ctx_, 3 + i)) << "flow " << i;
    EXPECT_EQ(queues[i], ut_flow_create_queue(ctx_, 3 + i)) << "flow " << i;
    EXPECT_NE(nullptr, ut_flow_handle(ctx_, ids[i])) << "flow " << i;
    EXPECT_NE(old_flows[i], ut_flow_handle(ctx_, ids[i])) << "flow " << i;
  }
  EXPECT_EQ(3, ut_flow_listed_count(ctx_));
}

TEST_F(MtFlowIavfResetTest, FailedResetStillCreatesTheFlowsAgain) {
  NEEDS_RESET_CB();
  int a = ut_flow_create(ctx_, 20000), b = ut_flow_create(ctx_, 20002);
  const void* old_b = ut_flow_handle(ctx_, b);

  ut_flow_iavf_pre(ctx_);
  ut_flow_iavf_post(ctx_, -EIO); /* as a failed settings restore, on a running port */

  EXPECT_EQ(4, ut_flow_create_calls(ctx_));
  EXPECT_NE(nullptr, ut_flow_handle(ctx_, a));
  EXPECT_NE(old_b, ut_flow_handle(ctx_, b));
  EXPECT_EQ(0, ut_flow_free(ctx_, b));
  EXPECT_EQ(ut_flow_last_created(ctx_), ut_flow_last_destroyed(ctx_));
}

TEST_F(MtFlowIavfResetTest, FirstFailedCreateEndsTheResetWithNoStaleFlow) {
  NEEDS_RESET_CB();
  int a = ut_flow_create(ctx_, 20000), b = ut_flow_create(ctx_, 20002);

  ut_flow_set_create_fail_count(ctx_, -1); /* the port is dead after the reset */
  ut_flow_iavf_pre(ctx_);
  ut_flow_iavf_post(ctx_, -EIO);

  EXPECT_EQ(2 + 5, ut_flow_create_calls(ctx_)) << "the first flow's 5 tries, none after";
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

TEST_F(MtFlowIavfResetTest, PostWithoutPreWaitsForAFlowCallInProgress) {
  NEEDS_RESET_CB();
  int id = ut_flow_create(ctx_, 20000);
  std::atomic<bool> done{false};

  ASSERT_GE(id, 0);
  ut_flow_lock(ctx_);
  std::thread t([&] {
    ut_flow_iavf_post(ctx_, 0);
    done = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(done) << "post must not replace a flow while a flow call holds the mutex";
  ut_flow_unlock(ctx_);
  t.join();

  EXPECT_TRUE(done);
  EXPECT_EQ(2, ut_flow_create_calls(ctx_));
}

TEST_F(MtFlowIavfResetTest, CallbacksIgnoreANullArg) {
  NEEDS_RESET_CB();
  ASSERT_GE(ut_flow_create(ctx_, 20000), 0);
  ut_flow_iavf_null_arg();

  EXPECT_EQ(1, ut_flow_create_calls(ctx_));
  EXPECT_GE(ut_flow_create(ctx_, 20002), 0) << "pre took no flow mutex";
}

static int find_iavf(struct dl_phdr_info* info, size_t, void* data) {
  if (!info->dlpi_name || !strstr(info->dlpi_name, "librte_net_iavf.so")) return 0;
  *static_cast<bool*>(data) = true;
  return 1;
}

/* MTL does not link the PMD: without the lookup in the loaded plugin, a VF reset only
 * warns at registration and loses the flows */
TEST_F(MtFlowIavfResetTest, ResolveFindsTheRegisterFunctionsInTheIavfPlugin) {
  NEEDS_RESET_CB();
  ASSERT_EQ(ut_eal_init(), 0);
  bool loaded = false;

  dl_iterate_phdr(find_iavf, &loaded);
  if (!loaded) GTEST_SKIP() << "no iavf PMD plugin loaded by rte_eal_init()";
  EXPECT_TRUE(ut_flow_iavf_resolve());
}

TEST_F(MtFlowIavfResetTest, InitSetsEachPortsStateForTheCallbacks) {
  ASSERT_EQ(0, ut_flow_init(ctx_, 2));

  EXPECT_TRUE(ut_flow_init_port_ok(ctx_, 0));
  EXPECT_TRUE(ut_flow_init_port_ok(ctx_, 1));
  EXPECT_GE(ut_flow_create(ctx_, 20000), 0);
  EXPECT_EQ(1, ut_flow_listed_count(ctx_));
  ut_flow_uinit_frees(ctx_);
}

TEST_F(MtFlowIavfResetTest, RegisterSetsPostBeforePre) {
  NEEDS_RESET_CB();
  ut_flow_reset_cb_register(ctx_);

  EXPECT_STREQ("Pp", ut_flow_reg_log(ctx_));
  EXPECT_TRUE(ut_flow_reset_cb_on(ctx_));
}

TEST_F(MtFlowIavfResetTest, RegisterWaitsOutABriefReset) {
  NEEDS_RESET_CB();
  ut_flow_set_reg_busy(ctx_, 2, 2); /* -EBUSY twice for each, as while the VF resets */
  ut_flow_reset_cb_register(ctx_);

  EXPECT_STREQ("Pp", ut_flow_reg_log(ctx_));
  EXPECT_TRUE(ut_flow_reset_cb_on(ctx_));
}

TEST_F(MtFlowIavfResetTest, UnregisterClearsPreFirstAndWaitsOutAReset) {
  NEEDS_RESET_CB();
  ut_flow_reset_cb_register(ctx_);
  ut_flow_set_reg_busy(ctx_, 2, 2);
  ut_flow_reset_cb_unregister(ctx_);

  EXPECT_STREQ("PpxX", ut_flow_reg_log(ctx_));
  EXPECT_FALSE(ut_flow_reset_cb_on(ctx_));
  EXPECT_EQ(1, ut_flow_uinit_frees(ctx_));
}

TEST_F(MtFlowIavfResetTest, UnregisterWaitsOutAResetPastTheRegisterBound) {
  NEEDS_RESET_CB();
  ut_flow_reset_cb_register(ctx_);
  /* the harness's register gives up after 3 tries: a callback left set would run on
   * the state mt_flow_uinit() frees */
  ut_flow_set_reg_busy(ctx_, 20, 20);
  ut_flow_reset_cb_unregister(ctx_);

  EXPECT_STREQ("PpxX", ut_flow_reg_log(ctx_));
  EXPECT_FALSE(ut_flow_reset_cb_on(ctx_));
  EXPECT_EQ(1, ut_flow_uinit_frees(ctx_));
}

TEST_F(MtFlowIavfResetTest, RegisterThatStaysBusySetsNoPre) {
  NEEDS_RESET_CB();
  ut_flow_set_reg_busy(ctx_, 0, 100);
  ut_flow_reset_cb_register(ctx_);

  EXPECT_STREQ("", ut_flow_reg_log(ctx_));
  EXPECT_FALSE(ut_flow_reset_cb_on(ctx_));
}
