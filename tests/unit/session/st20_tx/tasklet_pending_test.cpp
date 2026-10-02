/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins what the TX video builder tasklet tells the scheduler: with
 * MTL_FLAG_TASKLET_SLEEP the scheduler sleeps only when every tasklet returns
 * MTL_TASKLET_ALL_DONE, so one tasklet serving several sessions must report
 * pending work if any of them has it.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxTaskletPendingTest.*'
 */

#include <gtest/gtest.h>

#include "mtl_sch_api.h"
#include "session/st20_tx_harness.h"

namespace {

class St20TxTaskletPendingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

TEST_F(St20TxTaskletPendingTest, IdleLaterSessionDoesNotHideEarlierSessionsWork) {
  int pending = ut_txv_run_tasklet_with_idle_peer(ctx_);
  ASSERT_GE(pending, 0) << "harness setup failed";
  EXPECT_NE(pending, MTL_TASKLET_ALL_DONE);
}

}  // namespace
