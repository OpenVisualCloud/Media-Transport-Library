/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins the TX ring model of common/ut_tx_ring.h to the DPDK 26.07 iavf code it ports, so
 * the tests built on it model the PMD, not a guess of it.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='UtTxRingTest.*'
 */

#include "common/ut_tx_ring.h"

#include <gtest/gtest.h>

#include "common/ut_common.h"

namespace {

constexpr uint16_t kNbDesc = 256;
constexpr uint16_t kRs = 32;
constexpr unsigned int kPoolSize = 511;

class UtTxRingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_eal_init(), 0);
    pool_ = rte_pktmbuf_pool_create("ut_txr_test", kPoolSize, 0, 0, 256,
                                    (int)rte_socket_id());
    ASSERT_NE(pool_, nullptr);
  }
  void TearDown() override {
    ut_txr_destroy(ring_);
    rte_mempool_free(pool_);
  }
  void Create(enum ut_txr_mode mode, uint16_t free_thresh, uint64_t drain_ns) {
    ring_ = ut_txr_create(mode, kNbDesc, kRs, free_thresh, drain_ns);
    ASSERT_NE(ring_, nullptr);
  }
  void Post(int n, uint64_t now_ns) {
    for (int i = 0; i < n; i++) {
      struct rte_mbuf* m = rte_pktmbuf_alloc(pool_);
      ASSERT_NE(m, nullptr);
      ASSERT_EQ(ut_txr_xmit(ring_, &m, 1, now_ns), 1);
    }
  }
  void BurstOfNone(uint64_t now_ns) {
    ut_txr_xmit(ring_, nullptr, 0, now_ns);
  }
  unsigned int Held() {
    return kPoolSize - rte_mempool_avail_count(pool_);
  }
  struct rte_mempool* pool_ = nullptr;
  struct ut_txr* ring_ = nullptr;
};

TEST_F(UtTxRingTest, XmitFreesOneDoneBatchWhileTwoAreInUse) {
  Create(UT_TXR_FREE_ON_CLEANUP, kNbDesc - 2 * kRs, 1);
  Post(2 * kRs - 1, 0);
  BurstOfNone(1000);
  EXPECT_EQ(Held(), 2u * kRs - 1);

  Post(1, 1000);
  BurstOfNone(2000);
  EXPECT_EQ(Held(), 1u * kRs);
  BurstOfNone(2000);
  EXPECT_EQ(Held(), 1u * kRs);
}

TEST_F(UtTxRingTest, BatchIsDoneWithItsRsDescriptor) {
  Create(UT_TXR_FREE_ON_CLEANUP, kNbDesc - 2 * kRs, 10);
  Post(3 * kRs, 0);

  BurstOfNone(10ull * kRs - 1);
  EXPECT_EQ(Held(), 3u * kRs);
  BurstOfNone(10ull * kRs);
  EXPECT_EQ(Held(), 2u * kRs);
}

TEST_F(UtTxRingTest, BucketHasNoRsUntilAPktCrossesIt) {
  Create(UT_TXR_FREE_ON_CLEANUP, kNbDesc - kRs + 1, 1); /* cleans from 1 in use */
  Post(kRs - 1, 0);
  BurstOfNone(1000);
  EXPECT_EQ(Held(), kRs - 1u);

  Post(1, 1000);
  BurstOfNone(2000);
  EXPECT_EQ(Held(), 0u);
}

TEST_F(UtTxRingTest, HungRingFreesNothing) {
  Create(UT_TXR_FREE_ON_CLEANUP, kNbDesc - 2 * kRs, UT_TXR_HUNG);
  Post(3 * kRs, 0);

  BurstOfNone(UINT64_MAX - 1);
  EXPECT_EQ(ut_txr_done_cleanup(ring_, 0, UINT64_MAX - 1), 0);
  EXPECT_EQ(Held(), 3u * kRs);
}

/* the walk starts at tx_tail over nb_tx_free slots, then over each batch it cleans */
TEST_F(UtTxRingTest, DoneCleanupFreesWhatTheCleanupFreedAgain) {
  Create(UT_TXR_FREE_ON_CLEANUP, kNbDesc - 2 * kRs, 1);
  Post(3 * kRs, 0);
  BurstOfNone(1000);

  EXPECT_EQ(ut_txr_done_cleanup(ring_, 0, 1000), 3 * kRs - 1);
  EXPECT_EQ(ut_txr_double_frees(ring_), 3u * kRs - 1);
  EXPECT_EQ(Held(), 0u);
}

TEST_F(UtTxRingTest, BeforeDpdk2607CleanupFreesNothing) {
  Create(UT_TXR_FREE_ON_REUSE, kRs, 1);
  Post(kNbDesc - 1, 0);
  for (int i = 0; i < 4; i++) BurstOfNone(1000);
  EXPECT_EQ(Held(), kNbDesc - 1u);
}

TEST_F(UtTxRingTest, BeforeDpdk2607XmitFreesTheMbufOfASlotItReuses) {
  Create(UT_TXR_FREE_ON_REUSE, kRs, 1);
  Post(kNbDesc - 1, 0);

  Post(kRs, 1000); /* into the unused slot and 31 reused ones */
  EXPECT_EQ(Held(), kNbDesc - 1u + kRs - (kRs - 1u));
  EXPECT_EQ(ut_txr_double_frees(ring_), 0u);
}

TEST_F(UtTxRingTest, BeforeDpdk2607DoneCleanupFreesTheDoneMbufsOnce) {
  Create(UT_TXR_FREE_ON_REUSE, kRs, 1);
  Post(3 * kRs, 0);

  EXPECT_EQ(ut_txr_done_cleanup(ring_, 0, 1000), 3 * kRs - 1);
  EXPECT_EQ(ut_txr_double_frees(ring_), 0u);
  EXPECT_EQ(Held(), 1u);
}

}  // namespace
