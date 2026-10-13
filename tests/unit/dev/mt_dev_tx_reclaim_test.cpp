/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Since DPDK 26.07 the iavf and ice scalar xmit frees the mbufs of one done rs batch a
 * burst while at least nb_tx_desc - 1 - tx_free_thresh descriptors are in use.
 * dev_start_port() sets tx_free_thresh to nb_tx_desc - 2 * tx_rs_thresh, so zero-copy
 * video gets its frames back as their pkts leave, not a TX ring later. Before 26.07 the
 * cleanup only marks the batch free, so the ports keep the PMD defaults.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='MtDevTxReclaimTest.*'
 */

#include <gtest/gtest.h>
#include <rte_version.h>

#include "dev/mt_dev_harness.h"

namespace {

constexpr bool kCleanupFrees = RTE_VERSION >= RTE_VERSION_NUM(26, 7, 0, 0);
constexpr uint16_t kNbTxQ = 4;
constexpr uint16_t kNbTxDesc = 2048;
constexpr uint16_t kRsThresh = 32; /* the iavf and ice default */

class MtDevTxReclaimTest : public testing::Test {
 protected:
  void SetUp() override {
    ctx_ = ut_dev_create_ctx();
    ASSERT_NE(ctx_, nullptr);
    ASSERT_EQ(ut_dev_set_tx_ring(ctx_, kNbTxQ, kNbTxDesc, kRsThresh), 0);
  }
  void TearDown() override {
    ut_dev_destroy_ctx(ctx_);
  }
  void ExpectEveryQueueFreeThresh(uint16_t thresh) {
    for (uint16_t q = 0; q < kNbTxQ; q++)
      EXPECT_EQ(ut_dev_tx_free_thresh(ctx_, q), thresh) << "queue " << q;
  }
  void ExpectDefaults() {
    ExpectEveryQueueFreeThresh(0);
    EXPECT_EQ(ut_dev_tx_reclaim_rs_thresh(ctx_), 0);
  }
  /* what a port with rs batches of rs gets: reclaim only where the cleanup frees */
  void ExpectReclaim(uint16_t nb_tx_desc, uint16_t rs) {
    if (!kCleanupFrees) return ExpectDefaults();
    ExpectEveryQueueFreeThresh(nb_tx_desc - 2 * rs);
    EXPECT_EQ(ut_dev_tx_reclaim_rs_thresh(ctx_), rs);
  }
  ut_dev_ctx* ctx_ = nullptr;
};

TEST_F(MtDevTxReclaimTest, IavfAndIceQueuesFreeSentMbufsEveryBurst) {
  for (const char* driver : {"net_iavf", "net_ice"}) {
    SCOPED_TRACE(driver);
    ut_dev_set_driver(ctx_, driver);
    ASSERT_EQ(ut_dev_start_port(ctx_), 0);
    ExpectReclaim(kNbTxDesc, kRsThresh);
  }
}

TEST_F(MtDevTxReclaimTest, ThresholdFollowsTheReportedRsThresh) {
  ut_dev_set_driver(ctx_, "net_ice");
  ASSERT_EQ(ut_dev_set_tx_ring(ctx_, kNbTxQ, kNbTxDesc, 64), 0);
  ASSERT_EQ(ut_dev_start_port(ctx_), 0);
  ExpectReclaim(kNbTxDesc, 64);
}

TEST_F(MtDevTxReclaimTest, OtherPmdKeepsItsDefaults) {
  ASSERT_EQ(ut_dev_start_port(ctx_), 0); /* igc */
  ExpectDefaults();
}

TEST_F(MtDevTxReclaimTest, NoReportedRsThreshKeepsTheDefaults) {
  ut_dev_set_driver(ctx_, "net_iavf");
  ASSERT_EQ(ut_dev_set_tx_ring(ctx_, kNbTxQ, kNbTxDesc, 0), 0);
  ASSERT_EQ(ut_dev_start_port(ctx_), 0);
  ExpectDefaults();
}

/* the PMDs take a tx_free_thresh of at least tx_rs_thresh: 3 batches is the least ring */
TEST_F(MtDevTxReclaimTest, RingOfThreeRsBatchesReclaims) {
  ut_dev_set_driver(ctx_, "net_iavf");
  ASSERT_EQ(ut_dev_set_tx_ring(ctx_, kNbTxQ, 3 * kRsThresh, kRsThresh), 0);
  ASSERT_EQ(ut_dev_start_port(ctx_), 0);
  ExpectReclaim(3 * kRsThresh, kRsThresh);
}

TEST_F(MtDevTxReclaimTest, RingTooSmallForTheThresholdKeepsTheDefaults) {
  ut_dev_set_driver(ctx_, "net_iavf");
  ASSERT_EQ(ut_dev_set_tx_ring(ctx_, kNbTxQ, 3 * kRsThresh - 1, kRsThresh), 0);
  ASSERT_EQ(ut_dev_start_port(ctx_), 0);
  ExpectDefaults();
}

}  // namespace
