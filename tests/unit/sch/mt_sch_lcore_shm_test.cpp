/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "sch/mt_sch_harness.h"

/* A process of a DPDK with a lower RTE_MAX_LCORE that died without uinit leaves its
 * smaller table, and shmget() of the larger one fails with EINVAL. */
TEST(MtSchLcoreShm, ReplacesUnusedSmallerTable) {
  size_t size = 0;
  ASSERT_EQ(ut_sch_lcore_shm_init_over_smaller(false, true, &size), 0);
  EXPECT_EQ(size, ut_sch_lcore_shm_size());
}

/* An attached smaller table belongs to a running process of the other build. */
TEST(MtSchLcoreShm, KeepsUsedSmallerTable) {
  size_t size = 0;
  EXPECT_LT(ut_sch_lcore_shm_init_over_smaller(true, true, &size), 0);
  EXPECT_EQ(size, ut_sch_lcore_shm_size() / 2);
}

/* LcoreMgr takes no lock file, so it leaves the table to mtl_init(). */
TEST(MtSchLcoreShm, LcoreMgrKeepsUnusedSmallerTable) {
  size_t size = 0;
  EXPECT_LT(ut_sch_lcore_shm_init_over_smaller(false, false, &size), 0);
  EXPECT_EQ(size, ut_sch_lcore_shm_size() / 2);
}
