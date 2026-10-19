/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The TX ring depth dev_config_port() gives a port without mtl_init_params.nb_tx_desc:
 * 2048 where a shaper paces the queue and the iavf or ice PMD frees the sent mbufs every
 * burst (DPDK 26.07), so a deep queue rides out a lost scheduler slice yet zero-copy
 * video gets its frames back; 512 on every other port, whose ring keeps what it sent. A
 * user nb_tx_desc wins on a DPDK port.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='MtDevTxDescTest*'
 */

#include <gtest/gtest.h>

#include "dev/mt_dev_harness.h"

namespace {

constexpr int kDeep = 2048;
constexpr int kShallow = 512;

struct TxDescCase {
  const char* name;
  const char* driver;
  enum st21_tx_pacing_way pacing;
  bool cleanup_frees;
  bool shared_txq;
  int expected;
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
  EXPECT_EQ(Config(c.driver, c.pacing, 0), c.expected);
  EXPECT_EQ(ut_dev_tx_deep(ctx_), c.expected == kDeep);
}

INSTANTIATE_TEST_SUITE_P(
    Cases, MtDevTxDescDefaultTest,
    testing::Values(
        TxDescCase{"IavfRl", "net_iavf", ST21_TX_PACING_WAY_RL, true, false, kDeep},
        TxDescCase{"IceRl", "net_ice", ST21_TX_PACING_WAY_RL, true, false, kDeep},
        TxDescCase{"IceTsn", "net_ice", ST21_TX_PACING_WAY_TSN, true, false, kDeep},
        TxDescCase{"IavfAuto", "net_iavf", ST21_TX_PACING_WAY_AUTO, true, false, kDeep},
        TxDescCase{"IceAuto", "net_ice", ST21_TX_PACING_WAY_AUTO, true, false, kDeep},
        TxDescCase{"IavfTsc", "net_iavf", ST21_TX_PACING_WAY_TSC, true, false, kShallow},
        TxDescCase{"IavfTscNarrow", "net_iavf", ST21_TX_PACING_WAY_TSC_NARROW, true,
                   false, kShallow},
        TxDescCase{"IcePtp", "net_ice", ST21_TX_PACING_WAY_PTP, true, false, kShallow},
        TxDescCase{"IceBe", "net_ice", ST21_TX_PACING_WAY_BE, true, false, kShallow},
        TxDescCase{"IavfRlLazyCleanup", "net_iavf", ST21_TX_PACING_WAY_RL, false, false,
                   kShallow},
        TxDescCase{"IceAutoLazyCleanup", "net_ice", ST21_TX_PACING_WAY_AUTO, false, false,
                   kShallow},
        TxDescCase{"IavfRlSharedTxq", "net_iavf", ST21_TX_PACING_WAY_RL, true, true,
                   kShallow},
        TxDescCase{"I40eRl", "net_i40e", ST21_TX_PACING_WAY_RL, true, false, kShallow},
        TxDescCase{"IgcTsn", "net_igc", ST21_TX_PACING_WAY_TSN, true, false, kShallow},
        TxDescCase{"IgcAuto", "net_igc", ST21_TX_PACING_WAY_AUTO, true, false, kShallow},
        TxDescCase{"Mlx5Auto", "mlx5_pci", ST21_TX_PACING_WAY_AUTO, true, false,
                   kShallow},
        TxDescCase{"DpdkAfXdpAuto", "net_af_xdp", ST21_TX_PACING_WAY_AUTO, true, false,
                   kShallow},
        TxDescCase{"KernelSocketAuto", "kernel_socket", ST21_TX_PACING_WAY_AUTO, true,
                   false, kShallow},
        TxDescCase{"NativeAfXdpAuto", "native_af_xdp", ST21_TX_PACING_WAY_AUTO, true,
                   false, kShallow}),
    [](const testing::TestParamInfo<TxDescCase>& info) { return info.param.name; });

TEST_F(MtDevTxDescTest, UserNbTxDescWinsOnADpdkPort) {
  EXPECT_EQ(Config("net_iavf", ST21_TX_PACING_WAY_TSC, 1024), 1024);
  EXPECT_EQ(Config("net_iavf", ST21_TX_PACING_WAY_RL, 4096), 4096);
}

/* the deep ring, reclaim and the bounded flush come with the default ring only */
TEST_F(MtDevTxDescTest, AUserRingOfTheDeepDepthIsNotDeep) {
  ut_dev_set_tx_cleanup_frees(ctx_, true);
  EXPECT_EQ(Config("net_iavf", ST21_TX_PACING_WAY_RL, kDeep), kDeep);
  EXPECT_FALSE(ut_dev_tx_deep(ctx_));
}

/* the depth is set before pacing is: an AUTO port whose RL init fails paces by TSC on a
 * deep ring, which is safe as its PMD frees each burst */
TEST_F(MtDevTxDescTest, AutoTmPortFallingBackToTscKeepsTheDeepRing) {
  ut_dev_set_tx_cleanup_frees(ctx_, true);
  ut_dev_set_pacing_port(ctx_, true, ST21_TX_PACING_WAY_AUTO);
  ASSERT_EQ(ut_dev_set_tx_ring(ctx_, ut_dev_nb_tx_queues(), kShallow, 32), 0);
  ASSERT_EQ(ut_dev_config_port_nb_tx_desc(ctx_, 0), kDeep);
  ut_dev_fail_tm_node_add(ctx_, -ENOTSUP);
  ASSERT_EQ(ut_dev_init_pacing(ctx_), 0);
  ASSERT_EQ(ut_dev_pacing_way(ctx_), ST21_TX_PACING_WAY_TSC);
  EXPECT_EQ(ut_dev_nb_tx_desc(ctx_), kDeep);
  ASSERT_EQ(ut_dev_start_port(ctx_), 0);
  EXPECT_EQ(ut_dev_tx_reclaim_rs_thresh(ctx_), 32);
  EXPECT_EQ(ut_dev_tx_free_thresh(ctx_, 0), kDeep - 2 * 32);
}

}  // namespace
