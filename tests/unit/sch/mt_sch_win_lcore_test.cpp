/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "sch/mt_sch_win_harness.h"

/* Each handle stands in for one MTL process sharing the lcore shm and lock file. */
class MtSchWinLcoreTest : public ::testing::Test {
 protected:
  static constexpr unsigned int kLcores = 4;

  void TearDown() override {
    ut_sch_win_destroy_main(b_);
    ut_sch_win_destroy_main(a_);
  }

  mtl_handle a_ = nullptr;
  mtl_handle b_ = nullptr;
};

TEST_F(MtSchWinLcoreTest, SecondInstanceSkipsTheLcoreOfTheFirst) {
  unsigned int first = 0, second = 0;
  a_ = ut_sch_win_create_main(kLcores);
  ASSERT_NE(a_, nullptr);
  ASSERT_EQ(ut_sch_win_get_lcore(a_, &first), 0);

  b_ = ut_sch_win_create_main(kLcores);
  ASSERT_NE(b_, nullptr);
  ASSERT_EQ(ut_sch_win_get_lcore(b_, &second), 0);
  EXPECT_NE(second, first);
}

TEST_F(MtSchWinLcoreTest, LcoreOfAnExitedProcessIsFreeAgain) {
  unsigned int orphaned = 0, taken = 0;
  a_ = ut_sch_win_create_main(kLcores);
  ASSERT_NE(a_, nullptr);
  b_ = ut_sch_win_create_main(kLcores);
  ASSERT_NE(b_, nullptr);
  ASSERT_EQ(ut_sch_win_get_lcore(a_, &orphaned), 0);
  ASSERT_EQ(ut_sch_win_orphan_lcore(a_, orphaned), 0);

  ASSERT_EQ(ut_sch_win_get_lcore(b_, &taken), 0);
  EXPECT_EQ(taken, orphaned);
}

TEST_F(MtSchWinLcoreTest, HeldLockShutsOutAnotherOpenOfTheLockFile) {
  a_ = ut_sch_win_create_main(kLcores);
  ASSERT_NE(a_, nullptr);
  EXPECT_EQ(ut_sch_win_lock_excludes(a_), 1);
}
