/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

/* The lcore allocator. */

#include <gtest/gtest.h>

#include <cerrno>

#include "mtl_lcore.hpp"

TEST(Lcore, GetAndPutOneCore) {
  mtl_lcore lcores;

  EXPECT_EQ(lcores.used_count(), 0u);
  EXPECT_FALSE(lcores.is_used(3));

  EXPECT_EQ(lcores.get_lcore(3), 0);
  EXPECT_TRUE(lcores.is_used(3));
  EXPECT_EQ(lcores.used_count(), 1u);

  EXPECT_EQ(lcores.put_lcore(3), 0);
  EXPECT_FALSE(lcores.is_used(3));
  EXPECT_EQ(lcores.used_count(), 0u);
}

TEST(Lcore, SecondGetIsBusy) {
  mtl_lcore lcores;

  ASSERT_EQ(lcores.get_lcore(7), 0);
  /* The old code answered -1 here, which a client could not tell from any
   * other failure. */
  EXPECT_EQ(lcores.get_lcore(7), -EBUSY);
}

TEST(Lcore, PutOfAFreeCoreIsInvalid) {
  mtl_lcore lcores;

  EXPECT_EQ(lcores.put_lcore(7), -EINVAL);
}

TEST(Lcore, OutOfRangeIsInvalid) {
  mtl_lcore lcores;

  EXPECT_EQ(lcores.get_lcore(MTL_MAX_LCORE), -EINVAL);
  EXPECT_EQ(lcores.get_lcore(MTL_MAX_LCORE + 5), -EINVAL);
  EXPECT_EQ(lcores.put_lcore(MTL_MAX_LCORE), -EINVAL);
  /* An out of range read must answer, not stop the manager. */
  EXPECT_FALSE(lcores.is_used(MTL_MAX_LCORE));
}

TEST(Lcore, TheLastCoreInRangeWorks) {
  mtl_lcore lcores;

  EXPECT_EQ(lcores.get_lcore(MTL_MAX_LCORE - 1), 0);
  EXPECT_EQ(lcores.put_lcore(MTL_MAX_LCORE - 1), 0);
}

TEST(Lcore, EachInstanceHasItsOwnMap) {
  mtl_lcore first;
  mtl_lcore second;

  ASSERT_EQ(first.get_lcore(9), 0);
  /* Two objects must not share state, or a test would depend on the order of
   * the cases. */
  EXPECT_EQ(second.get_lcore(9), 0);
}

TEST(Lcore, TheProcessWideMapIsOneObject) {
  EXPECT_EQ(&mtl_lcore::get_instance(), &mtl_lcore::get_instance());
}
