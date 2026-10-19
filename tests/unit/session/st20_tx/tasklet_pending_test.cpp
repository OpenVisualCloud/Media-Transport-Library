/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins what the TX video builder tasklet tells the scheduler: with
 * MTL_FLAG_TASKLET_SLEEP the scheduler sleeps only when every tasklet returns
 * MTL_TASKLET_ALL_DONE, so one tasklet serving several sessions must report
 * pending work if any of them has it, wherever that session sits in the manager.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*St20TxTaskletPending*'
 */

#include <gtest/gtest.h>

#include <vector>

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
  int run(ut_txv_work work, std::vector<ut_txv_slot> slots) {
    return ut_txv_run_tasklet_slots(ctx_, work, slots.data(), (int)slots.size());
  }
  ut_txv_ctx* ctx_ = nullptr;
};

class St20TxTaskletPendingBuilderTest
    : public St20TxTaskletPendingTest,
      public ::testing::WithParamInterface<ut_txv_work> {};

TEST_P(St20TxTaskletPendingBuilderTest, BusySessionBeforeIdleOneReportsPending) {
  EXPECT_EQ(MTL_TASKLET_HAS_PENDING,
            run(GetParam(), {UT_TXV_SLOT_SELF, UT_TXV_SLOT_IDLE, UT_TXV_SLOT_IDLE}));
  EXPECT_EQ(2, ut_txv_peer_get_next_frame_calls(ctx_));
}

TEST_P(St20TxTaskletPendingBuilderTest, BusySessionAfterIdleOneReportsPending) {
  EXPECT_EQ(MTL_TASKLET_HAS_PENDING,
            run(GetParam(), {UT_TXV_SLOT_IDLE, UT_TXV_SLOT_IDLE, UT_TXV_SLOT_SELF}));
  EXPECT_EQ(2, ut_txv_peer_get_next_frame_calls(ctx_));
}

TEST_P(St20TxTaskletPendingBuilderTest, BusySessionPastEmptyAndInactiveSlotsIsRun) {
  EXPECT_EQ(MTL_TASKLET_HAS_PENDING,
            run(GetParam(), {UT_TXV_SLOT_EMPTY, UT_TXV_SLOT_INACTIVE, UT_TXV_SLOT_SELF}));
  EXPECT_EQ(0, ut_txv_peer_get_next_frame_calls(ctx_))
      << "an inactive session's builder must not run";
}

INSTANTIATE_TEST_SUITE_P(Builders, St20TxTaskletPendingBuilderTest,
                         ::testing::Values(UT_TXV_WORK_FRAME, UT_TXV_WORK_RTP,
                                           UT_TXV_WORK_ST22),
                         [](const ::testing::TestParamInfo<ut_txv_work>& info) {
                           switch (info.param) {
                             case UT_TXV_WORK_FRAME:
                               return "Frame";
                             case UT_TXV_WORK_RTP:
                               return "Rtp";
                             default:
                               return "St22";
                           }
                         });

/* Otherwise a sleeping scheduler would never sleep. */
TEST_F(St20TxTaskletPendingTest, AllIdleSessionsReportAllDone) {
  EXPECT_EQ(
      MTL_TASKLET_ALL_DONE,
      run(UT_TXV_WORK_NONE, {UT_TXV_SLOT_IDLE, UT_TXV_SLOT_SELF, UT_TXV_SLOT_IDLE}));
  EXPECT_EQ(2, ut_txv_peer_get_next_frame_calls(ctx_));
}

}  // namespace
