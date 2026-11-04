/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Lip-synced ancillary sessions all receive at one instant. One call of the RX ancillary
 * tasklet reads packets for at most 4 of them, and the next call starts at the session
 * after the last one it read, so the other tasklets of the scheduler run between the
 * parts of the burst and no session waits more than its turn. A call that sees all
 * sessions puts the start back at session 0, so each stream keeps its slot in the next
 * burst.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St40RxTaskletBound*'
 */

#include <gtest/gtest.h>

#include <array>

#include "mtl_sch_api.h"
#include "session/st40_harness.h"

namespace {
constexpr int kSessions = 6;
using Left = std::array<int, kSessions>;
}  // namespace

class St40RxTaskletBoundTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut40_init(), 0);
    ut40_drain_ring();
    for (int i = 0; i < kSessions; i++) {
      ctx_[i] = ut40_ctx_create(1);
      ASSERT_NE(ctx_[i], nullptr);
      ASSERT_EQ(ut40_mgr_add(ctx_[0], i, ctx_[i]), 0);
    }
  }

  void TearDown() override {
    ut40_drain_ring();
    for (ut_test_ctx* ctx : ctx_) ut40_ctx_destroy(ctx);
  }

  Left left() const {
    Left l{};
    for (int i = 0; i < kSessions; i++) l[i] = ut40_rxq_left(ctx_[i]);
    return l;
  }

  std::array<ut_test_ctx*, kSessions> ctx_{};
};

TEST_F(St40RxTaskletBoundTest, OneCallReadsFourSessionsAndTheNextResumes) {
  for (ut_test_ctx* ctx : ctx_) ut40_rxq_add(ctx, 1);

  EXPECT_EQ(ut40_mgr_tasklet(ctx_[0]), MTL_TASKLET_HAS_PENDING);
  EXPECT_EQ(left(), (Left{0, 0, 0, 0, 1, 1}));

  /* sessions 0..3 receive again, but sessions 4 and 5 go first */
  for (int i = 0; i < 4; i++) ut40_rxq_add(ctx_[i], 1);
  EXPECT_EQ(ut40_mgr_tasklet(ctx_[0]), MTL_TASKLET_HAS_PENDING);
  EXPECT_EQ(left(), (Left{0, 0, 1, 1, 0, 0}));
}

TEST_F(St40RxTaskletBoundTest, TheNextBurstStartsAtTheFirstSession) {
  for (ut_test_ctx* ctx : ctx_) ut40_rxq_add(ctx, 1);
  EXPECT_EQ(ut40_mgr_tasklet(ctx_[0]), MTL_TASKLET_HAS_PENDING);
  /* sessions 4 and 5, then a scan of all sessions: the burst is done */
  ut40_mgr_tasklet(ctx_[0]);
  ASSERT_EQ(left(), (Left{0, 0, 0, 0, 0, 0}));

  for (ut_test_ctx* ctx : ctx_) ut40_rxq_add(ctx, 1);
  EXPECT_EQ(ut40_mgr_tasklet(ctx_[0]), MTL_TASKLET_HAS_PENDING);
  EXPECT_EQ(left(), (Left{0, 0, 0, 0, 1, 1}));
}
