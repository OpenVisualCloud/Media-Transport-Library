/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>
#include <mtl_api.h>
#include <rte_lcore.h>

#include <algorithm>
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
    if (!tmp_.empty()) {
      std::string cmd = "rm -rf '" + tmp_ + "'";
      ASSERT_EQ(system(cmd.c_str()), 0);
    }
  }

  /* A DPDK built under <tmp>/old and moved to <tmp>/new; returns the PMD dir in new. */
  std::string RelocateDpdk() {
    char tmpl[] = "/tmp/mt_dev_eal_argv_test.XXXXXX";
    if (!mkdtemp(tmpl)) return "<mkdtemp failed>";
    tmp_ = tmpl;
    std::string pmd_dir = tmp_ + "/new/lib/x86_64-linux-gnu/dpdk/pmds-26.2";
    std::string cmd = "mkdir -p '" + pmd_dir + "'";
    if (system(cmd.c_str())) return "<mkdir failed>";
    ut_dev_set_eal_pmd_paths(
        ctx_, (tmp_ + "/old/lib/x86_64-linux-gnu/dpdk/pmds-26.2").c_str(),
        (tmp_ + "/new/lib/x86_64-linux-gnu/librte_eal.so.26").c_str());
    return pmd_dir;
  }

  void SetAffinity(std::vector<unsigned int> cpus) {
    ut_dev_set_affinity(ctx_, cpus.data(), static_cast<int>(cpus.size()));
  }

  std::string Logs() {
    char logs[4096];
    ut_dev_eal_init_logs(ctx_, logs, sizeof(logs));
    return logs;
  }

  static constexpr const char* kRebuildAdvice = "./script/build_dpdk.sh -f";

  bool HasOption(const std::string& option) {
    std::vector<std::string> argv = Argv();
    return std::find(argv.begin(), argv.end(), option) != argv.end();
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
  std::string tmp_;
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
  std::string pmd_dir = RelocateDpdk();

  ut_dev_eal_init(ctx_);

  EXPECT_EQ(ut_dev_eal_init_calls(ctx_), 1);
  EXPECT_EQ(ValueAfter("-d"), pmd_dir);
  EXPECT_LE(ut_dev_eal_argc(ctx_), ut_dev_eal_max_args());
}

TEST_F(MtDevEalArgvTest, RelocatedDpdkAddsThePmdDirBeforeTheSeparator) {
  std::string pmd_dir = RelocateDpdk();

  ut_dev_eal_init(ctx_);

  std::vector<std::string> argv = Argv();
  ASSERT_GE(argv.size(), 3u);
  EXPECT_EQ(argv[argv.size() - 3], "-d");
  EXPECT_EQ(argv[argv.size() - 2], pmd_dir);
  EXPECT_EQ(argv.back(), "--");
}

/* Adding -d next to a compiled-in dir that EAL loads anyway double-loads every driver. */
TEST_F(MtDevEalArgvTest, ExistingCompiledPmdDirAddsNoPmdOption) {
  std::string pmd_dir = RelocateDpdk();
  ut_dev_set_eal_pmd_paths(ctx_, pmd_dir.c_str(),
                           (tmp_ + "/new/lib/x86_64-linux-gnu/librte_eal.so.26").c_str());

  ut_dev_eal_init(ctx_);

  EXPECT_EQ(ut_dev_eal_init_calls(ctx_), 1);
  EXPECT_FALSE(HasOption("-d"));
}

/* A static DPDK is linked into the library that holds rte_eal_init, drivers included. */
TEST_F(MtDevEalArgvTest, StaticDpdkAddsNoPmdOption) {
  RelocateDpdk();
  ut_dev_set_eal_pmd_paths(ctx_,
                           (tmp_ + "/old/lib/x86_64-linux-gnu/dpdk/pmds-26.2").c_str(),
                           (tmp_ + "/new/lib/x86_64-linux-gnu/libmtl.so.26").c_str());

  ut_dev_eal_init(ctx_);

  EXPECT_EQ(ut_dev_eal_init_calls(ctx_), 1);
  EXPECT_FALSE(HasOption("-d"));
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

/* An lcore id is the CPU id: EAL must not renumber the CPUs the user lists. */
TEST_F(MtDevEalArgvTest, CorelistIsNotRemapped) {
  ut_dev_set_lcores(ctx_, 0, "60,61");

  ut_dev_eal_init(ctx_);

  EXPECT_EQ(ut_dev_eal_init_calls(ctx_), 1);
  EXPECT_FALSE(HasOption("--remap-lcore-ids"));
}

/* Without a corelist EAL takes every affinity CPU, numbered by its CPU id. */
TEST_F(MtDevEalArgvTest, AutomaticAssignmentLeavesTheAffinityToEal) {
  SetAffinity({1, 2});

  ut_dev_eal_init(ctx_);

  EXPECT_EQ(ut_dev_eal_init_calls(ctx_), 1);
  EXPECT_FALSE(HasOption("-l"));
  EXPECT_FALSE(HasOption("--remap-lcore-ids"));
}

/* An affinity CPU at or past RTE_MAX_LCORE fails EAL init, so pass only the CPUs below.
 */
TEST_F(MtDevEalArgvTest, AutomaticAssignmentDropsCpusPastRteMaxLcore) {
  if (RTE_MAX_LCORE + 3 >= CPU_SETSIZE) GTEST_SKIP() << "no CPU id above RTE_MAX_LCORE";
  SetAffinity({1, 2, RTE_MAX_LCORE, RTE_MAX_LCORE + 3});

  ut_dev_eal_init(ctx_);

  EXPECT_EQ(ut_dev_eal_init_calls(ctx_), 1);
  EXPECT_EQ(ValueAfter("-l"), "1,2");
}

TEST_F(MtDevEalArgvTest, AutomaticAssignmentWithoutACpuBelowRteMaxLcoreIsRejected) {
  if (RTE_MAX_LCORE + 3 >= CPU_SETSIZE) GTEST_SKIP() << "no CPU id above RTE_MAX_LCORE";
  SetAffinity({RTE_MAX_LCORE});

  EXPECT_EQ(ut_dev_eal_init(ctx_), -EINVAL);
  EXPECT_EQ(ut_dev_eal_init_calls(ctx_), 0);
}

/* A DPDK too small for the CPUs asked for: the log names the rebuild that fixes it. */
TEST_F(MtDevEalArgvTest, AutomaticAssignmentPastRteMaxLcoreNamesTheDpdkToBuild) {
  if (RTE_MAX_LCORE + 3 >= CPU_SETSIZE) GTEST_SKIP() << "no CPU id above RTE_MAX_LCORE";
  SetAffinity({1, RTE_MAX_LCORE});

  EXPECT_NE(Logs().find(kRebuildAdvice), std::string::npos) << Logs();
}

TEST_F(MtDevEalArgvTest, CorelistPastRteMaxLcoreNamesTheDpdkToBuild) {
  ut_dev_set_lcores(ctx_, 0, ("1," + std::to_string(RTE_MAX_LCORE)).c_str());

  EXPECT_NE(Logs().find(kRebuildAdvice), std::string::npos) << Logs();
}

TEST_F(MtDevEalArgvTest, CorelistRangePastRteMaxLcoreNamesTheDpdkToBuild) {
  ut_dev_set_lcores(ctx_, 0, ("1-" + std::to_string(RTE_MAX_LCORE)).c_str());

  EXPECT_NE(Logs().find(kRebuildAdvice), std::string::npos) << Logs();
}

TEST_F(MtDevEalArgvTest, CorelistWithAnOverflowingCpuIdNamesTheDpdkToBuild) {
  ut_dev_set_lcores(ctx_, 0, "1,99999999999999999999999");

  EXPECT_NE(Logs().find("DPDK_MAX_LCORES=1024 "), std::string::npos) << Logs();
}

/* With '@' a CPU at or past RTE_MAX_LCORE is valid: only the lcore id must be below. */
TEST_F(MtDevEalArgvTest, MappedCorelistPastRteMaxLcoreIsNotWarned) {
  ut_dev_set_lcores(ctx_, 0, ("1@" + std::to_string(RTE_MAX_LCORE)).c_str());

  EXPECT_EQ(Logs().find(kRebuildAdvice), std::string::npos) << Logs();
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
