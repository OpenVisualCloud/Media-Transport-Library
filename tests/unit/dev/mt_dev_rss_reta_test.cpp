/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <cerrno>

#include "dev/mt_dev_harness.h"

namespace {

class MtDevRssRetaTest : public testing::Test {
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

/* Shared RSS with no rx queue (MTL_FLAG_DISABLE_SYSTEM_RX_QUEUES): each reta entry is
 * the entry index modulo nb_rx_q, a division by zero. */
TEST_F(MtDevRssRetaTest, NoRxQueueFailsInsteadOfDividingByZero) {
  EXPECT_EQ(ut_dev_config_rss_reta(ctx_, 0, 512), -EINVAL);
}

}  // namespace
