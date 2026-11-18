/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * A stats reader on another thread must not keep the TX video tasklets off its session:
 * the scheduler runs passes over a peer, which works each pass, and the read session.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*St20TxSessionLockStarvation*'
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <tuple>

#include "session/st20_tx_harness.h"

namespace {

enum Reader { kNone, kGet, kGetPaused, kReset };

using Load = std::tuple<ut_txv_tasklet, Reader>;

constexpr int kPasses = 10000; /* 50 ms behind the 5 us peer */
constexpr int kPauseUs = 1000;

class St20TxSessionLockStarvationTest : public ::testing::TestWithParam<Load> {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_txv_init());
    if (ut_txv_spare_cpus() < 2) GTEST_SKIP() << "needs two CPUs besides the EAL's";
    ctx_ = ut_txv_create();
    ASSERT_NE(nullptr, ctx_);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

TEST_P(St20TxSessionLockStarvationTest, TaskletsRunTheReadSessionEveryPass) {
  Reader reader = std::get<1>(GetParam());
  ut_txv_reader_load load = {};
  load.tasklet = std::get<0>(GetParam());
  load.passes = kPasses;
  load.getters = reader == kGet || reader == kGetPaused;
  load.resetters = reader == kReset;
  load.get_pause_us = reader == kGetPaused ? kPauseUs : 0;
  ut_txv_reader_result got = {};

  ASSERT_EQ(0, ut_txv_run_readers(ctx_, &load, &got));
  std::printf(
      "  ran %d/%d (%.1f%%), longest skip %d passes %.1f us, %.1f ms, %llu calls\n",
      got.ran, load.passes, 100.0 * got.ran / load.passes, got.longest_skip,
      static_cast<double>(got.longest_skip_ns) / 1e3,
      static_cast<double>(got.passes_ns) / 1e6,
      static_cast<unsigned long long>(got.gets + got.resets));
  EXPECT_FALSE(got.hung);
  EXPECT_EQ(0, got.call_err);
  EXPECT_EQ(load.passes, got.ran);
}

std::string LoadName(const ::testing::TestParamInfo<Load>& info) {
  static const char* readers[] = {"NoReader", "Get", "GetPaused", "Reset"};
  return std::string(std::get<0>(info.param) == UT_TXV_TASKLET_BUILDER ? "Builder"
                                                                       : "Transmitter") +
         readers[std::get<1>(info.param)];
}

INSTANTIATE_TEST_SUITE_P(Readers, St20TxSessionLockStarvationTest,
                         ::testing::Combine(::testing::Values(UT_TXV_TASKLET_BUILDER,
                                                              UT_TXV_TASKLET_TRANSMITTER),
                                            ::testing::Values(kGet, kReset, kGetPaused)),
                         LoadName);

INSTANTIATE_TEST_SUITE_P(Control, St20TxSessionLockStarvationTest,
                         ::testing::Values(Load(UT_TXV_TASKLET_BUILDER, kNone)),
                         LoadName);

}  // namespace
