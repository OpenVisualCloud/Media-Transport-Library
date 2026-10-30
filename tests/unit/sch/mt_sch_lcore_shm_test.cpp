/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>
#include <mtl_lcore_shm_api.h>
#include <sched.h>
#include <unistd.h>

#include <cerrno>

#include "sch/mt_sch_harness.h"

/* The cross-process lcore table that MTL processes share without MtlManager. An lcore id
 * is the CPU id, so the table has one entry per CPU id the OS can name, whatever
 * RTE_MAX_LCORE the DPDK of each process was built with. */
class MtSchLcoreShmTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_sch_shm_private_begin(), 0);
  }
  void TearDown() override {
    ut_sch_shm_private_end();
  }

  static int Clean(unsigned int lcore) {
    struct mtl_lcore_clean_pid_info info = {.lcore = lcore};
    return mtl_lcore_shm_clean(MTL_LCORE_CLEAN_LCORE, &info, sizeof(info));
  }

  static constexpr unsigned int kOldEntries = 128;
};

TEST_F(MtSchLcoreShmTest, FirstUserCreatesAnEntryForEveryCpuId) {
  EXPECT_EQ(ut_sch_shm_join(true), ut_sch_shm_table_size(CPU_SETSIZE));
}

TEST_F(MtSchLcoreShmTest, CleanReachesTheLastCpuId) {
  ASSERT_GT(ut_sch_shm_join(true), 0);
  ASSERT_EQ(ut_sch_shm_mark_active(CPU_SETSIZE - 1, getpid()), 0);

  EXPECT_EQ(Clean(CPU_SETSIZE - 1), 0);
}

/* A table an older MTL left behind when it crashed is replaced, not inherited. */
TEST_F(MtSchLcoreShmTest, FirstUserReplacesAStaleSmallerTable) {
  ASSERT_EQ(ut_sch_shm_create(ut_sch_shm_table_size(kOldEntries), false), 0);

  EXPECT_EQ(ut_sch_shm_join(true), ut_sch_shm_table_size(CPU_SETSIZE));
}

/* MTL 26.01 to 26.09 keep remapped lcore ids in their 128 entries, not CPU ids. */
TEST_F(MtSchLcoreShmTest, RefusesTheSmallerTableOfARunningOlderMtl) {
  ASSERT_EQ(ut_sch_shm_create(ut_sch_shm_table_size(kOldEntries), true), 0);

  EXPECT_EQ(ut_sch_shm_join(true), -EBUSY);
}

TEST_F(MtSchLcoreShmTest, LeavesTheTableOfARunningOlderMtlAlone) {
  ASSERT_EQ(ut_sch_shm_create(ut_sch_shm_table_size(kOldEntries), true), 0);

  ut_sch_shm_join(true);

  EXPECT_EQ(ut_sch_shm_size(), ut_sch_shm_table_size(kOldEntries));
}

/* LcoreMgr reads and cleans without the init lock, so it replaces no table, stale or not.
 */
TEST_F(MtSchLcoreShmTest, CleanLeavesAStaleSmallerTableAlone) {
  ASSERT_EQ(ut_sch_shm_create(ut_sch_shm_table_size(kOldEntries), false), 0);

  EXPECT_EQ(Clean(1), -EIO);
  EXPECT_EQ(ut_sch_shm_size(), ut_sch_shm_table_size(kOldEntries));
}

/* Not header plus whole entries of this layout, which MTL must not touch. */
class MtSchUnusableShmTest : public MtSchLcoreShmTest,
                             public testing::WithParamInterface<long> {};

TEST_P(MtSchUnusableShmTest, IsRejected) {
  ASSERT_EQ(ut_sch_shm_create(GetParam(), false), 0);

  EXPECT_EQ(ut_sch_shm_join(true), -EIO);
}

TEST_P(MtSchUnusableShmTest, IsLeftAlone) {
  ASSERT_EQ(ut_sch_shm_create(GetParam(), false), 0);

  ut_sch_shm_join(true);

  EXPECT_EQ(ut_sch_shm_size(), GetParam());
}

/* some other program's segment on the key */
INSTANTIATE_TEST_SUITE_P(Foreign, MtSchUnusableShmTest,
                         testing::Values(3, ut_sch_shm_table_size(0),
                                         ut_sch_shm_table_size(1) - 1,
                                         ut_sch_shm_table_size(CPU_SETSIZE) + 1));

/* the 132-byte table of an MTL older than 23.12 */
INSTANTIATE_TEST_SUITE_P(MtlBefore2312, MtSchUnusableShmTest, testing::Values(132));
