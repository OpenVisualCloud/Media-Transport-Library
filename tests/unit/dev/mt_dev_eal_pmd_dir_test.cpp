/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <climits>
#include <cstdlib>
#include <string>

#include "dev/mt_dev_harness.h"

/* A DPDK built into <base>/old and moved to <base>/new: librte_eal now loads from new. */
class MtDevEalPmdDirTest : public testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/mt_dev_pmd_dir_test.XXXXXX";
    ASSERT_NE(mkdtemp(tmpl), nullptr);
    base_ = tmpl;
    compiled_ = base_ + "/old/lib/x86_64-linux-gnu/dpdk/pmds-26.2";
    eal_dir_ = base_ + "/new/lib/x86_64-linux-gnu";
    derived_ = eal_dir_ + "/dpdk/pmds-26.2";
  }

  void TearDown() override {
    std::string cmd = "rm -rf '" + base_ + "'";
    ASSERT_EQ(system(cmd.c_str()), 0);
  }

  void MkdirP(const std::string& dir) {
    std::string cmd = "mkdir -p '" + dir + "'";
    ASSERT_EQ(system(cmd.c_str()), 0);
  }

  int PmdDir(const std::string& compiled) {
    out_[0] = '\0';
    return ut_dev_eal_pmd_dir(compiled.c_str(), eal_dir_.c_str(), out_, sizeof(out_));
  }

  std::string base_, compiled_, eal_dir_, derived_;
  char out_[PATH_MAX];
};

TEST_F(MtDevEalPmdDirTest, MissingCompiledDirGivesTheDirBesideTheLoadedEal) {
  MkdirP(derived_);

  EXPECT_EQ(PmdDir(compiled_), 0);
  EXPECT_EQ(std::string(out_), derived_);
}

TEST_F(MtDevEalPmdDirTest, ExistingCompiledDirGivesNothing) {
  MkdirP(compiled_);
  MkdirP(derived_);

  EXPECT_LT(PmdDir(compiled_), 0);
}

TEST_F(MtDevEalPmdDirTest, MissingDerivedDirGivesNothing) {
  MkdirP(eal_dir_);

  EXPECT_LT(PmdDir(compiled_), 0);
}

TEST_F(MtDevEalPmdDirTest, EmptyCompiledDirGivesNothing) {
  MkdirP(derived_);

  EXPECT_LT(PmdDir(""), 0);
}

TEST_F(MtDevEalPmdDirTest, SubdirOfAnyDepthAfterTheEalDirNameIsKept) {
  std::string derived = eal_dir_ + "/dpdk/extra/pmds-26.2";
  MkdirP(derived);

  EXPECT_EQ(PmdDir(base_ + "/old/lib/x86_64-linux-gnu/dpdk/extra/pmds-26.2"), 0);
  EXPECT_EQ(std::string(out_), derived);
}

TEST_F(MtDevEalPmdDirTest, EalDirNameNotInTheCompiledDirKeepsTheLastTwoComponents) {
  MkdirP(derived_);

  EXPECT_EQ(PmdDir(base_ + "/old/lib64/dpdk/pmds-26.2"), 0);
  EXPECT_EQ(std::string(out_), derived_);
}
