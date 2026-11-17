/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * mt_dev_tx_queue_fatal_error() marks a hung queue fatal, so it is never handed out
 * again, and does not stop it on any driver: no runtime TX queue stop is safe. The ixgbe
 * stop of an E610 frees the ring while the HW still holds descriptors, and the queue and
 * the port hang after it; an iavf stop breaks the TM tree of the port. The session
 * parks a pool the dead queue still holds.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*MtDevTxQueueFatalTest*'
 */

#include <gtest/gtest.h>

#include "dev/mt_dev_harness.h"

namespace {

class MtDevTxQueueFatalTest : public ::testing::TestWithParam<const char*> {
 protected:
  void SetUp() override {
    ctx_ = ut_dev_create_ctx();
    ASSERT_NE(ctx_, nullptr);
    ASSERT_EQ(ut_dev_set_tx_ring(ctx_, 1, 512, 32), 0);
    ut_dev_set_driver(ctx_, GetParam());
  }
  void TearDown() override {
    ut_dev_destroy_ctx(ctx_);
  }
  ut_dev_ctx* ctx_ = nullptr;
};

TEST_P(MtDevTxQueueFatalTest, FatalQueueIsMarkedAndNotStopped) {
  int stops = ut_dev_tx_queue_stop_calls();

  ASSERT_EQ(ut_dev_tx_queue_fatal_error(ctx_, 0), 0);

  EXPECT_TRUE(ut_dev_tx_queue_fatal(ctx_, 0));
  EXPECT_EQ(ut_dev_tx_queue_stop_calls(), stops);
}

INSTANTIATE_TEST_SUITE_P(Driver, MtDevTxQueueFatalTest,
                         ::testing::Values("net_ixgbe", "net_ice", "net_iavf", "net_i40e",
                                           "net_igc"));

}  // namespace
