/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>
#include <mtl_api.h>
#include <rte_lcore.h>

#include <cerrno>
#include <string>
#include <vector>

#include "dev/mt_dev_harness.h"

/*
 * Pins dev_eal_init()'s argv assembly: the vector bound, and the "-l" corelist it
 * composes from main_lcore plus the user list. The harness stubs rte_eal_init() to fail,
 * which both keeps the builder re-runnable and makes "rte_eal_init() was reached" an
 * observable.
 */
class MtDevEalArgvTest : public testing::Test {
 protected:
  void SetUp() override {
    ctx_ = ut_dev_create_ctx();
    ASSERT_NE(ctx_, nullptr);
    ut_dev_set_port(ctx_, MTL_PORT_P, "0000:c9:01.0", 0);
    ut_dev_set_log_level(ctx_, MTL_LOG_LEVEL_ERR);
  }

  void TearDown() override {
    ut_dev_destroy_ctx(ctx_);
  }

  std::vector<std::string> Argv() {
    std::vector<std::string> argv;
    for (int i = 0; i < ut_dev_eal_argc(ctx_); i++) {
      const char* arg = ut_dev_eal_argv(ctx_, i);
      argv.push_back(arg ? arg : "<out of range>");
    }
    return argv;
  }

  std::string ValueAfter(const std::string& option) {
    std::vector<std::string> argv = Argv();
    for (size_t i = 0; i + 1 < argv.size(); i++) {
      if (argv[i] == option) return argv[i + 1];
    }
    return "<missing>";
  }

  ut_dev_ctx* ctx_ = nullptr;
};

/* Every documented maximum at once: the argv vector must still hold them all. */
TEST_F(MtDevEalArgvTest, WorstCaseParamsFitTheArgvVector) {
  ut_dev_set_num_ports(ctx_, MTL_PORT_MAX);
  for (int i = 0; i < MTL_PORT_MAX; i++) {
    ut_dev_set_port(ctx_, (enum mtl_port)i, ("0000:c9:01." + std::to_string(i)).c_str(),
                    2048);
  }
  ut_dev_set_dma_dev_ports(ctx_, MTL_DMA_DEV_MAX);
  ut_dev_set_lcores(ctx_, 1, "2,3,4,5");
  ut_dev_set_iova_mode(ctx_, MTL_IOVA_MODE_VA);
  ut_dev_set_log_level(ctx_, MTL_LOG_LEVEL_DEBUG);
  ut_dev_enable_rxtx_simd_512(ctx_);

  ut_dev_eal_init(ctx_);

  EXPECT_EQ(ut_dev_eal_init_calls(ctx_), 1);
  EXPECT_GT(ut_dev_eal_argc(ctx_), 0);
  EXPECT_LE(ut_dev_eal_argc(ctx_), ut_dev_eal_max_args());
}

/* The modest configuration that already overran a 32 entry vector. */
TEST_F(MtDevEalArgvTest, TwoPortsWithEightDmaDevicesFitTheArgvVector) {
  ut_dev_set_num_ports(ctx_, 2);
  ut_dev_set_port(ctx_, MTL_PORT_R, "0000:c9:01.1", 0);
  ut_dev_set_dma_dev_ports(ctx_, 8);
  ut_dev_set_lcores(ctx_, 1, "2,3,4,5");
  ut_dev_set_iova_mode(ctx_, MTL_IOVA_MODE_VA);

  ut_dev_eal_init(ctx_);

  EXPECT_EQ(ut_dev_eal_init_calls(ctx_), 1);
  EXPECT_GT(ut_dev_eal_argc(ctx_), 32);
  EXPECT_LE(ut_dev_eal_argc(ctx_), ut_dev_eal_max_args());
}

/* main_lcore is prepended to the user corelist, an otherwise undocumented contract. */
TEST_F(MtDevEalArgvTest, MainLcoreIsPrependedToTheCorelist) {
  ut_dev_set_lcores(ctx_, 0, "52,53,54,55");

  ut_dev_eal_init(ctx_);

  EXPECT_EQ(ut_dev_eal_init_calls(ctx_), 1);
  EXPECT_EQ(ValueAfter("-l"), "0,52,53,54,55");
}

TEST_F(MtDevEalArgvTest, CorelistNamingEveryLcoreIsNotTruncated) {
  std::string lcores;
  for (unsigned int lcore = 1; lcore < RTE_MAX_LCORE; lcore++) {
    if (!lcores.empty()) lcores += ",";
    lcores += std::to_string(lcore);
  }
  ut_dev_set_lcores(ctx_, 0, lcores.c_str());

  ut_dev_eal_init(ctx_);

  EXPECT_EQ(ut_dev_eal_init_calls(ctx_), 1);
  EXPECT_EQ(ValueAfter("-l"), "0," + lcores);
}

/* The "0," main_lcore prefix costs 2 of the buffer's bytes, leaving len - 3 for the user
 * list before the NUL. That last accepted byte and the first rejected one are what tell a
 * correct bound from an off by one. */
TEST_F(MtDevEalArgvTest, CorelistThatExactlyFillsTheArgumentIsAccepted) {
  std::string lcores(ut_dev_eal_lcores_max_len() - 3, '7');
  ut_dev_set_lcores(ctx_, 0, lcores.c_str());

  ut_dev_eal_init(ctx_);

  EXPECT_EQ(ut_dev_eal_init_calls(ctx_), 1);
  EXPECT_EQ(ValueAfter("-l"), "0," + lcores);
}

/* Too long to express: reject loudly instead of starting on a truncated core set. */
TEST_F(MtDevEalArgvTest, CorelistOneByteTooLongIsRejected) {
  std::string lcores(ut_dev_eal_lcores_max_len() - 2, '7');
  ut_dev_set_lcores(ctx_, 0, lcores.c_str());

  EXPECT_EQ(ut_dev_eal_init(ctx_), -EINVAL);
  EXPECT_EQ(ut_dev_eal_init_calls(ctx_), 0);
}
