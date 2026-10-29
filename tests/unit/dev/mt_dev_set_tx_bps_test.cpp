/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "dev/mt_dev_harness.h"

namespace {

constexpr uint16_t kQueue = 0;
/* two rates apart from each other and from RL init's 1 Gbit/s default */
constexpr uint64_t kFirstBytesPerSec = 100ull * 1000 * 1000;
constexpr uint64_t kSecondBytesPerSec = 2 * kFirstBytesPerSec;
constexpr int kCommitError = -EIO;

}  // namespace

class MtDevSetTxBpsTest : public testing::Test {
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

/* mt_dev_set_tx_bps() returns the TM commit result of a rate change */
TEST_F(MtDevSetTxBpsTest, FailedCommitIsReturned) {
  ut_dev_set_pacing_port(ctx_, true, ST21_TX_PACING_WAY_RL);
  ASSERT_EQ(ut_dev_init_pacing(ctx_), 0);
  ASSERT_NE(ut_dev_tx_queue_bps(ctx_, kQueue), kFirstBytesPerSec);
  ASSERT_EQ(ut_dev_set_tx_bps(ctx_, kQueue, kFirstBytesPerSec), 0);
  ut_dev_fail_tm_commit(ctx_, kCommitError);

  EXPECT_EQ(ut_dev_set_tx_bps(ctx_, kQueue, kSecondBytesPerSec), kCommitError);
}
