/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins tv_mempool_free()'s ownership contract. st20_tx_queue_fatal_error()
 * relies on it to decide whether a recovery cycle may replace the session's
 * pools: mt_mempool_free() reports success even when it declines to free an
 * in-use pool, so tv_mempool_free() has to detect that itself and keep the
 * pointer, otherwise recovery leaks the pool and loses the session's only handle
 * to it. Pools the session merely borrows must be cleared but never freed.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxMempoolReleaseTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20_tx_harness.h"

namespace {

class St20TxMempoolReleaseTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
    ASSERT_EQ(ut_txv_install_hdr_mempool(ctx_), 0);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

TEST_F(St20TxMempoolReleaseTest, IdlePoolIsFreedAndCleared) {
  ASSERT_TRUE(ut_txv_hdr_mempool_alive(ctx_));

  EXPECT_EQ(ut_txv_mempool_free(ctx_), 0);
  EXPECT_FALSE(ut_txv_hdr_mempool_installed(ctx_));
  EXPECT_FALSE(ut_txv_hdr_mempool_alive(ctx_));
}

TEST_F(St20TxMempoolReleaseTest, PoolWithOutstandingMbufIsReportedBusyAndRetained) {
  ASSERT_EQ(ut_txv_hold_hdr_mbuf(ctx_), 0);

  EXPECT_LT(ut_txv_mempool_free(ctx_), 0);
  EXPECT_TRUE(ut_txv_hdr_mempool_installed(ctx_))
      << "a pool that was not actually freed must stay reachable, otherwise "
         "the session drops its only handle to it";
}

TEST_F(St20TxMempoolReleaseTest, PoolBecomesFreeableOnceMbufIsReturned) {
  ASSERT_EQ(ut_txv_hold_hdr_mbuf(ctx_), 0);
  ASSERT_LT(ut_txv_mempool_free(ctx_), 0);

  ut_txv_release_hdr_mbuf(ctx_);

  EXPECT_EQ(ut_txv_mempool_free(ctx_), 0);
  EXPECT_FALSE(ut_txv_hdr_mempool_installed(ctx_));
}

TEST_F(St20TxMempoolReleaseTest, BorrowedMonoPoolIsClearedButNotFreed) {
  ut_txv_set_tx_mono_pool(ctx_, true);

  EXPECT_EQ(ut_txv_mempool_free(ctx_), 0);
  EXPECT_FALSE(ut_txv_hdr_mempool_installed(ctx_));
  EXPECT_TRUE(ut_txv_hdr_mempool_alive(ctx_))
      << "the mono pool is owned by the interface, freeing it here would pull it "
         "out from under every other session sharing it";
}

TEST_F(St20TxMempoolReleaseTest, BorrowedReuseRxPoolIsClearedButNotFreed) {
  ut_txv_set_hdr_mempool_reuse_rx(ctx_, true);

  EXPECT_EQ(ut_txv_mempool_free(ctx_), 0);
  EXPECT_FALSE(ut_txv_hdr_mempool_installed(ctx_));
  EXPECT_TRUE(ut_txv_hdr_mempool_alive(ctx_))
      << "a reused RX pool belongs to the RX queue, not to this session";
}

}  // namespace
