/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * TX hang recovery frees the done mbufs of the failed queue before it replaces the
 * session pools. Since DPDK 26.07 the iavf, ice and i40e xmit cleanup frees a done batch
 * and leaves it in the sw_ring, and their rte_eth_tx_done_cleanup() frees it again: on a
 * deep port mt_dev_tx_done_cleanup() never calls it, and runs the bounded flush instead
 * where the PMD frees a batch each burst, never the lazy pad loop. Every other port calls
 * it.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*MtDevTxDoneCleanup*'
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <vector>

#include "common/ut_common.h"
#include "common/ut_known_bug.h"
#include "common/ut_tx_ring.h"
#include "dev/mt_dev_harness.h"

namespace {

constexpr uint64_t kStartNs = 1000000000;
constexpr uint64_t kDoneNs = kStartNs + 2000000;
constexpr uint16_t kRs = 32;

class MtDevTxDoneCleanupTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_eal_init(), 0);
    pool_ = rte_pktmbuf_pool_create("ut_cleanup_session", 2047, 0, 0, 128,
                                    (int)rte_socket_id());
    ASSERT_NE(pool_, nullptr);
    ctx_ = ut_dev_create_ctx();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_clock_stop();
    ut_eth_reset();
    ut_txr_destroy(ring_);
    ut_dev_destroy_ctx(ctx_);
    rte_mempool_free(pool_);
  }
  /* a port of driver started on that DPDK, its queue 0 a ring of 1000 pkts sent and
   * done, 4 batches of them cleaned by bursts since */
  void StartPort(const char* driver, bool cleanup_frees, uint16_t nb_desc,
                 enum st21_tx_pacing_way pacing = ST21_TX_PACING_WAY_RL) {
    ut_dev_set_driver(ctx_, driver);
    ut_dev_set_tx_cleanup_frees(ctx_, cleanup_frees);
    ut_dev_set_tx_pacing(ctx_, pacing);
    ut_dev_init_tx_deep(ctx_);
    ut_dev_set_link_speed(ctx_, 25000);
    ASSERT_EQ(ut_dev_set_tx_ring(ctx_, 1, nb_desc, kRs), 0);
    ASSERT_EQ(ut_dev_start_port(ctx_), 0);
    uint16_t free_thresh = ut_dev_tx_free_thresh(ctx_, 0);
    ring_ = ut_txr_create(cleanup_frees ? UT_TXR_FREE_ON_CLEANUP : UT_TXR_FREE_ON_REUSE,
                          nb_desc, kRs, free_thresh ? free_thresh : kRs, 1000);
    ASSERT_NE(ring_, nullptr);
    ASSERT_EQ(ut_dev_attach_tx_ring(ctx_, 0, ring_), 0);
    unsigned int queued = RTE_MIN(1000, nb_desc - 1);
    for (unsigned int i = 0; i < queued; i++) {
      struct rte_mbuf* m = rte_pktmbuf_alloc(pool_);
      ASSERT_NE(m, nullptr);
      ASSERT_EQ(ut_txr_xmit(ring_, &m, 1, kStartNs), 1);
    }
    for (int i = 0; i < 4; i++) ut_txr_xmit(ring_, nullptr, 0, kDoneNs);
    descs_ = ut_txr_descs(ring_);
    ut_clock_start(kDoneNs, 100);
  }
  struct rte_mempool* pool_ = nullptr;
  ut_dev_ctx* ctx_ = nullptr;
  struct ut_txr* ring_ = nullptr;
  uint64_t descs_ = 0;
};

TEST_F(MtDevTxDoneCleanupTest, OtherPmdCallsThePmdCleanup) {
  StartPort("net_igc", true, 2048);

  ut_dev_tx_done_cleanup(ctx_, 0);

  EXPECT_EQ(ut_eth_tx_done_cleanup_calls(), 1);
}

TEST_F(MtDevTxDoneCleanupTest, BeforeDpdk2607IavfCallsThePmdCleanup) {
  StartPort("net_iavf", false, 2048);

  ut_dev_tx_done_cleanup(ctx_, 0);

  EXPECT_EQ(ut_eth_tx_done_cleanup_calls(), 1);
  EXPECT_EQ(ut_txr_double_frees(ring_), 0u);
}

class MtDevTxDoneCleanupReclaimTest : public MtDevTxDoneCleanupTest,
                                      public ::testing::WithParamInterface<const char*> {
};

/* the flush pushes them out instead: every mbuf back once */
TEST_P(MtDevTxDoneCleanupReclaimTest, TxDoneCleanupSkippedOnReclaimPort) {
  StartPort(GetParam(), true, 2048);
  ASSERT_EQ(ut_dev_tx_reclaim_rs_thresh(ctx_), kRs);

  ut_dev_tx_done_cleanup(ctx_, 0);

  EXPECT_EQ(ut_eth_tx_done_cleanup_calls(), 0);
  EXPECT_EQ(ut_txr_double_frees(ring_), 0u);
  EXPECT_EQ(rte_mempool_in_use_count(pool_), 0u);
  EXPECT_EQ(ut_txr_descs(ring_) - descs_, 1u + 2 * kRs);
}

/* With no sys TX mbuf for the mark, recovery gets the error, not the lazy loop's
 * 2 * nb_tx_desc pads of up to 2 ms each in the tasklet. */
TEST_P(MtDevTxDoneCleanupReclaimTest, ReclaimPortWithoutACopyReturnsTheError) {
  StartPort(GetParam(), true, 2048);
  std::vector<struct rte_mbuf*> sys;
  for (struct rte_mbuf* m; (m = rte_pktmbuf_alloc(ut_dev_tx_sys_pool(ctx_)));)
    sys.push_back(m);

  EXPECT_EQ(ut_dev_tx_done_cleanup(ctx_, 0), -ENOMEM);

  EXPECT_EQ(ut_eth_tx_done_cleanup_calls(), 0);
  EXPECT_EQ(ut_txr_descs(ring_), descs_);
  for (struct rte_mbuf* m : sys) rte_pktmbuf_free(m);
}

INSTANTIATE_TEST_SUITE_P(Driver, MtDevTxDoneCleanupReclaimTest,
                         ::testing::Values("net_iavf", "net_ice"));

/* no reclaim below 3 batches, the cleanup frees mbufs all the same */
TEST_F(MtDevTxDoneCleanupTest, TxDoneCleanupSkippedOnSmallRingIavf) {
  StartPort("net_iavf", true, 2 * kRs);
  ASSERT_EQ(ut_dev_tx_reclaim_rs_thresh(ctx_), 0);

  ut_dev_tx_done_cleanup(ctx_, 0);

  EXPECT_EQ(ut_eth_tx_done_cleanup_calls(), 0);
  EXPECT_EQ(ut_txr_descs(ring_), descs_);
}

class MtDevTxDoneCleanupNotDeepTest : public MtDevTxDoneCleanupTest,
                                      public ::testing::WithParamInterface<const char*> {
};

TEST_P(MtDevTxDoneCleanupNotDeepTest, CallsThePmdCleanup) {
  StartPort(GetParam(), true, 512, ST21_TX_PACING_WAY_TSC);
  ASSERT_FALSE(ut_dev_tx_deep(ctx_));

  ut_dev_tx_done_cleanup(ctx_, 0);

  EXPECT_EQ(ut_eth_tx_done_cleanup_calls(), 1);
}

TEST_P(MtDevTxDoneCleanupNotDeepTest, FreesNoMbufTwice) {
  UT_KNOWN_BUG(
      "the 26.07 iavf, ice and i40e done cleanup frees the batches their xmit "
      "cleanup freed again");
  StartPort(GetParam(), true, 512, ST21_TX_PACING_WAY_TSC);

  ut_dev_tx_done_cleanup(ctx_, 0);

  EXPECT_EQ(ut_txr_double_frees(ring_), 0u);
}

INSTANTIATE_TEST_SUITE_P(Driver, MtDevTxDoneCleanupNotDeepTest,
                         ::testing::Values("net_iavf", "net_ice", "net_i40e"));

}  // namespace
