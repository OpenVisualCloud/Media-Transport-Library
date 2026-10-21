/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins tv_frame_get_offset_iova(), the IOVA a zero-copy chain mbuf gets for a frame
 * offset: contiguous in IOVA:VA mode, through the per-hugepage table in IOVA:PA mode.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxFrameIovaTest.*'
 */

#include <gtest/gtest.h>

#include <cstdint>

#include "session/st20_tx_harness.h"

namespace {

constexpr mtl_iova_t kFrameIova = 0x100000;
/* out of order, so a walk that assumes contiguous pages gets the second one wrong */
constexpr mtl_iova_t kPageIova[UT_TXV_IOVA_PAGES_MAX] = {0x9000, 0x3000};

class St20TxFrameIovaTest : public ::testing::Test {
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

TEST_F(St20TxFrameIovaTest, FrameWithoutPageTableIsContiguous) {
  EXPECT_EQ(ut_txv_frame_offset_iova(ctx_, kFrameIova, nullptr, 0, 1234),
            kFrameIova + 1234);
}

TEST_F(St20TxFrameIovaTest, PageTableMapsOffsetIntoItsPage) {
  EXPECT_EQ(ut_txv_frame_offset_iova(ctx_, kFrameIova, kPageIova, 2, 10), 0x9000u + 10);
  EXPECT_EQ(
      ut_txv_frame_offset_iova(ctx_, kFrameIova, kPageIova, 2, UT_TXV_IOVA_PAGE_LEN + 6),
      0x3000u + 6);
}

TEST_F(St20TxFrameIovaTest, OffsetPastThePageTableIsBadIova) {
  EXPECT_EQ(
      ut_txv_frame_offset_iova(ctx_, kFrameIova, kPageIova, 2, 2 * UT_TXV_IOVA_PAGE_LEN),
      MTL_BAD_IOVA);
}

}  // namespace
