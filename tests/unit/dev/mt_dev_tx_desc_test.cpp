/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The ports dev_config_port() marks deep: a shaper paces the queue and the iavf or ice
 * PMD frees the sent mbufs every burst (DPDK 26.07), on the default TX ring. Only a deep
 * port reclaims. Every port keeps the 512-descriptor default, and a user nb_tx_desc wins
 * on a DPDK port.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='MtDevTxDescTest*'
 */

#include <gtest/gtest.h>

#include "dev/mt_dev_harness.h"

namespace {

constexpr int kDefault = 512;

struct TxDescCase {
  const char* name;
  const char* driver;
  enum st21_tx_pacing_way pacing;
  bool cleanup_frees;
  bool shared_txq;
  bool deep;
};

class MtDevTxDescTest : public testing::Test {
 protected:
  void SetUp() override {
    ctx_ = ut_dev_create_ctx();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_dev_destroy_ctx(ctx_);
  }
  int Config(const char* driver, enum st21_tx_pacing_way pacing, uint16_t user) {
    ut_dev_set_driver(ctx_, driver);
    ut_dev_set_tx_pacing(ctx_, pacing);
    return ut_dev_config_port_nb_tx_desc(ctx_, user);
  }
  ut_dev_ctx* ctx_ = nullptr;
};

class MtDevTxDescDefaultTest : public MtDevTxDescTest,
                               public testing::WithParamInterface<TxDescCase> {};

TEST_P(MtDevTxDescDefaultTest, DeepOnlyWhereTheShapedQueueFreesEachBurst) {
  const TxDescCase& c = GetParam();
  ut_dev_set_tx_cleanup_frees(ctx_, c.cleanup_frees);
  if (c.shared_txq) ut_dev_set_shared_txq(ctx_);
  EXPECT_EQ(Config(c.driver, c.pacing, 0), kDefault);
  EXPECT_EQ(ut_dev_tx_deep(ctx_), c.deep);
}

INSTANTIATE_TEST_SUITE_P(
    Cases, MtDevTxDescDefaultTest,
    testing::Values(
        TxDescCase{"IavfRl", "net_iavf", ST21_TX_PACING_WAY_RL, true, false, true},
        TxDescCase{"IceRl", "net_ice", ST21_TX_PACING_WAY_RL, true, false, true},
        TxDescCase{"IceTsn", "net_ice", ST21_TX_PACING_WAY_TSN, true, false, true},
        TxDescCase{"IavfAuto", "net_iavf", ST21_TX_PACING_WAY_AUTO, true, false, true},
        TxDescCase{"IceAuto", "net_ice", ST21_TX_PACING_WAY_AUTO, true, false, true},
        TxDescCase{"IavfTsc", "net_iavf", ST21_TX_PACING_WAY_TSC, true, false, false},
        TxDescCase{"IavfTscNarrow", "net_iavf", ST21_TX_PACING_WAY_TSC_NARROW, true,
                   false, false},
        TxDescCase{"IcePtp", "net_ice", ST21_TX_PACING_WAY_PTP, true, false, false},
        TxDescCase{"IceBe", "net_ice", ST21_TX_PACING_WAY_BE, true, false, false},
        TxDescCase{"IavfRlLazyCleanup", "net_iavf", ST21_TX_PACING_WAY_RL, false, false,
                   false},
        TxDescCase{"IceAutoLazyCleanup", "net_ice", ST21_TX_PACING_WAY_AUTO, false, false,
                   false},
        TxDescCase{"IavfRlSharedTxq", "net_iavf", ST21_TX_PACING_WAY_RL, true, true,
                   false},
        TxDescCase{"I40eRl", "net_i40e", ST21_TX_PACING_WAY_RL, true, false, false},
        TxDescCase{"IgcTsn", "net_igc", ST21_TX_PACING_WAY_TSN, true, false, false},
        TxDescCase{"IgcAuto", "net_igc", ST21_TX_PACING_WAY_AUTO, true, false, false},
        TxDescCase{"Mlx5Auto", "mlx5_pci", ST21_TX_PACING_WAY_AUTO, true, false, false},
        TxDescCase{"DpdkAfXdpAuto", "net_af_xdp", ST21_TX_PACING_WAY_AUTO, true, false,
                   false},
        TxDescCase{"KernelSocketAuto", "kernel_socket", ST21_TX_PACING_WAY_AUTO, true,
                   false, false},
        TxDescCase{"NativeAfXdpAuto", "native_af_xdp", ST21_TX_PACING_WAY_AUTO, true,
                   false, false}),
    [](const testing::TestParamInfo<TxDescCase>& info) { return info.param.name; });

TEST_F(MtDevTxDescTest, UserNbTxDescWinsOnADpdkPort) {
  EXPECT_EQ(Config("net_iavf", ST21_TX_PACING_WAY_TSC, 1024), 1024);
  EXPECT_EQ(Config("net_iavf", ST21_TX_PACING_WAY_RL, 4096), 4096);
}

/* reclaim comes with the default ring only */
TEST_F(MtDevTxDescTest, AUserRingIsNotDeep) {
  ut_dev_set_tx_cleanup_frees(ctx_, true);
  EXPECT_EQ(Config("net_iavf", ST21_TX_PACING_WAY_RL, kDefault), kDefault);
  EXPECT_FALSE(ut_dev_tx_deep(ctx_));
}

/* deep is set before pacing is: an AUTO port whose RL init fails paces by TSC and
 * reclaims, which is safe as its PMD frees each burst */
TEST_F(MtDevTxDescTest, AutoTmPortFallingBackToTscStaysDeep) {
  ut_dev_set_tx_cleanup_frees(ctx_, true);
  ut_dev_set_pacing_port(ctx_, true, ST21_TX_PACING_WAY_AUTO);
  ASSERT_EQ(ut_dev_set_tx_ring(ctx_, ut_dev_nb_tx_queues(), kDefault, 32), 0);
  ASSERT_EQ(ut_dev_config_port_nb_tx_desc(ctx_, 0), kDefault);
  ut_dev_fail_tm_node_add(ctx_, -ENOTSUP);
  ASSERT_EQ(ut_dev_init_pacing(ctx_), 0);
  ASSERT_EQ(ut_dev_pacing_way(ctx_), ST21_TX_PACING_WAY_TSC);
  EXPECT_TRUE(ut_dev_tx_deep(ctx_));
  ASSERT_EQ(ut_dev_start_port(ctx_), 0);
  EXPECT_EQ(ut_dev_tx_reclaim_rs_thresh(ctx_), 32);
  EXPECT_EQ(ut_dev_tx_free_thresh(ctx_, 0), kDefault - 2 * 32);
}

}  // namespace
