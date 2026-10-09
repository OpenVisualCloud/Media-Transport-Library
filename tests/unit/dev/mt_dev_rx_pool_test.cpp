/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * An AF_XDP queue's UMEM is its RX queue pool: the fill ring and every TX copy draw
 * from it, so a pool short of the TX ring starves the fill ring and the kernel drops RX.
 */

#include <gtest/gtest.h>

#include "dev/mt_dev_harness.h"

namespace {

constexpr uint16_t kNbRxDesc = 2048;
constexpr uint16_t kNbTxDesc = 2048;

class MtDevRxPoolTest : public testing::Test {
 protected:
  void SetUp() override {
    ctx_ = ut_dev_create_ctx();
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut_dev_destroy_ctx(ctx_);
  }

  ut_dev_ctx* ctx_ = nullptr;
};

TEST_F(MtDevRxPoolTest, AfXdpPoolCoversTheFillAndTxRings) {
  for (const char* driver : {"native_af_xdp", "net_af_xdp"}) {
    EXPECT_GE(ut_dev_rx_queue_pool_elements(ctx_, driver, kNbRxDesc, kNbTxDesc),
              kNbRxDesc + kNbTxDesc)
        << driver;
  }
}

TEST_F(MtDevRxPoolTest, PmdPoolCoversTheRxRingOnly) {
  EXPECT_EQ(ut_dev_rx_queue_pool_elements(ctx_, "net_iavf", kNbRxDesc, kNbTxDesc),
            kNbRxDesc + 1024);
}

/* MTL_FLAG_RX_MONO_POOL: one pool is the UMEM of every queue pair */
TEST_F(MtDevRxPoolTest, AfXdpMonoPoolCoversEveryQueuePair) {
  constexpr uint16_t kQueues = 4;
  EXPECT_GE(
      ut_dev_rx_mono_pool_elements(ctx_, "net_af_xdp", kQueues, kNbRxDesc, kNbTxDesc),
      kQueues * (kNbRxDesc + kNbTxDesc));
  EXPECT_EQ(ut_dev_rx_mono_pool_elements(ctx_, "net_iavf", kQueues, kNbRxDesc, kNbTxDesc),
            kQueues * kNbRxDesc + 1024);
}

}  // namespace
