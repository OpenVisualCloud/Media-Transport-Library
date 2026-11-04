/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "sch/mt_sch_harness.h"

constexpr size_t kSmall = 4096;
constexpr size_t kLarge = 3 * 4096;

TEST(MtSchLcoreShm, UnattachedSmallerSegmentIsRecreatedAtNewSize) {
  size_t size = 0;
  EXPECT_GE(ut_sch_lcore_shm_get(kSmall, 0, kLarge, &size), 0);
  EXPECT_GE(size, kLarge);
}

TEST(MtSchLcoreShm, AttachedSmallerSegmentIsRefused) {
  size_t size = 0;
  EXPECT_LT(ut_sch_lcore_shm_get(kSmall, 1, kLarge, &size), 0);
}

TEST(MtSchLcoreShm, LargerSegmentIsUsedAsIs) {
  size_t size = 0;
  EXPECT_GE(ut_sch_lcore_shm_get(kLarge, 1, kSmall, &size), 0);
  EXPECT_GE(size, kLarge);
}
