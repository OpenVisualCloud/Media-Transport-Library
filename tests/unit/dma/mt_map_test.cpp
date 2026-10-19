/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <cerrno>

#include "dma/mt_map_harness.h"

class MtMapAddTest : public ::testing::Test {
 protected:
  static constexpr uintptr_t kBase = 0x7f0000000000ull;
  static constexpr size_t kPage = 0x200000;

  void SetUp() override {
    ctx_ = ut_map_create();
    ASSERT_NE(ctx_, nullptr);
    uint64_t iova = 0;
    ASSERT_EQ(ut_map_add(ctx_, kBase + 2 * kPage, 2 * kPage, &iova), 0);
    EXPECT_EQ(iova, 0x10000u);
  }

  void TearDown() override {
    ut_map_destroy(ctx_);
  }

  ut_map_ctx* ctx_ = nullptr;
};

TEST_F(MtMapAddTest, RejectsRangeStartingInsideExisting) {
  uint64_t iova = 0;
  EXPECT_EQ(ut_map_add(ctx_, kBase + 3 * kPage, 2 * kPage, &iova), -EINVAL);
}

TEST_F(MtMapAddTest, RejectsRangeEndingInsideExisting) {
  uint64_t iova = 0;
  EXPECT_EQ(ut_map_add(ctx_, kBase + kPage, 2 * kPage, &iova), -EINVAL);
}

TEST_F(MtMapAddTest, RejectsRangeEnclosingExisting) {
  uint64_t iova = 0;
  EXPECT_EQ(ut_map_add(ctx_, kBase, 6 * kPage, &iova), -EINVAL);
}

TEST_F(MtMapAddTest, AcceptsAdjacentRanges) {
  uint64_t iova = 0;
  EXPECT_EQ(ut_map_add(ctx_, kBase, 2 * kPage, &iova), 0);
  EXPECT_EQ(iova, 0x10000u + 2 * kPage);
  EXPECT_EQ(ut_map_add(ctx_, kBase + 4 * kPage, 2 * kPage, &iova), 0);
  EXPECT_EQ(iova, 0x10000u + 4 * kPage);
}

TEST_F(MtMapAddTest, RejectsWrappingRangeStartingInsideExisting) {
  uint64_t iova = 0;
  const uintptr_t start = kBase + 3 * kPage;
  EXPECT_EQ(ut_map_add(ctx_, start, UINTPTR_MAX - start + 1 + kPage, &iova), -EINVAL);
}

TEST_F(MtMapAddTest, RejectsWrappingRangeAboveExisting) {
  uint64_t iova = 0;
  const uintptr_t start = kBase + 8 * kPage;
  EXPECT_EQ(ut_map_add(ctx_, start, UINTPTR_MAX - start + 1 + kPage, &iova), -EINVAL);
}
