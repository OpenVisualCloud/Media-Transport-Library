/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "dev/mt_dev_harness.h"

namespace {

constexpr uint16_t kQueue = 0;
/* not queue 0, the one RL init rates */
constexpr uint16_t kSessionQueue = 2;
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

  void InitRl(bool iavf, bool started) {
    ut_dev_set_pacing_port(ctx_, iavf, ST21_TX_PACING_WAY_RL);
    if (started) {
      ASSERT_EQ(ut_dev_start_port(ctx_), 0);
    }
    ASSERT_EQ(ut_dev_init_pacing(ctx_), 0);
    ASSERT_EQ(ut_dev_pacing_way(ctx_), ST21_TX_PACING_WAY_RL);
    commits_ = ut_dev_tm_commit_count(ctx_);
    events_ = ut_dev_event_count(ctx_);
  }

  int NewCommits() {
    return ut_dev_tm_commit_count(ctx_) - commits_;
  }

  std::vector<std::pair<ut_dev_event, uint16_t>> NewEvents() {
    std::vector<std::pair<ut_dev_event, uint16_t>> events;
    for (int i = events_; i < ut_dev_event_count(ctx_); i++)
      events.emplace_back(ut_dev_event_at(ctx_, i), ut_dev_event_queue_at(ctx_, i));
    return events;
  }

  bool QueueNodeShaped(uint16_t queue) {
    for (int i = 0; i < ut_dev_tm_node_count(ctx_); i++) {
      ut_dev_tm_node node = ut_dev_tm_node_at(ctx_, i);
      if (node.node_id == queue) return node.shaper_profile_id != ut_dev_tm_shaper_none();
    }
    return false;
  }

  ut_dev_ctx* ctx_ = nullptr;
  int commits_ = 0;
  int events_ = 0;
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

/* an ice commit would stop and restart the port under its streaming queues */
TEST_F(MtDevSetTxBpsTest, IcePfStartedRestartsOnlyThatQueue) {
  InitRl(false, true);

  ASSERT_EQ(ut_dev_set_tx_bps(ctx_, kSessionQueue, kFirstBytesPerSec), 0);

  EXPECT_EQ(NewCommits(), 0);
  std::vector<std::pair<ut_dev_event, uint16_t>> restart = {
      {UT_DEV_EVENT_TX_QUEUE_STOP, kSessionQueue},
      {UT_DEV_EVENT_TX_QUEUE_START, kSessionQueue}};
  EXPECT_EQ(NewEvents(), restart);
  EXPECT_TRUE(QueueNodeShaped(kSessionQueue));
  EXPECT_EQ(ut_dev_tx_queue_bps(ctx_, kSessionQueue), kFirstBytesPerSec);
}

TEST_F(MtDevSetTxBpsTest, IcePfNotStartedCommits) {
  InitRl(false, false);

  ASSERT_EQ(ut_dev_set_tx_bps(ctx_, kSessionQueue, kFirstBytesPerSec), 0);

  EXPECT_EQ(NewCommits(), 1);
  EXPECT_TRUE(NewEvents().empty());
}

TEST_F(MtDevSetTxBpsTest, IavfStartedCommits) {
  InitRl(true, true);

  ASSERT_EQ(ut_dev_set_tx_bps(ctx_, kSessionQueue, kFirstBytesPerSec), 0);

  EXPECT_EQ(NewCommits(), 1);
  EXPECT_TRUE(NewEvents().empty());
}
