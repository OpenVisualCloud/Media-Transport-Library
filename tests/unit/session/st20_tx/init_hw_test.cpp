/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins how tv_init_hw() sizes each session port's TX queue rate limit: the
 * trained result is looked up on the physical port the session port maps to,
 * the same port the queue is requested on.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxInitHwTest.*'
 */

#include <gtest/gtest.h>

#include <cstdint>

#include "session/st20_tx_harness.h"

namespace {

constexpr uint64_t kTrainedBytesPerSec = 7654321;

class St20TxInitHwTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

TEST_F(St20TxInitHwTest, RateLimitUsesTrainingResultOfMappedPhysicalPort) {
  enum mtl_port queue_port = MTL_PORT_P;
  uint64_t queue_bps = 0;

  ASSERT_EQ(ut_txv_run_init_hw_rl_lookup(ctx_, MTL_PORT_R, kTrainedBytesPerSec,
                                         &queue_port, &queue_bps),
            0);
  EXPECT_EQ(queue_port, MTL_PORT_R);
  EXPECT_EQ(queue_bps, kTrainedBytesPerSec);
}

}  // namespace
