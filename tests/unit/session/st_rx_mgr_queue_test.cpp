/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "session/st_rx_mgr_queue_harness.h"

class StRxMgrQueueTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_mq_init(), 0);
    ut_mq_reset();
    mbufs_ = ut_mq_mbufs_in_use();
  }
  void TearDown() override {
    ut_mq_teardown();
    EXPECT_EQ(ut_mq_mbufs_in_use(), mbufs_) << "a packet was not freed";
  }
  unsigned int mbufs_ = 0;
};

/* Each packet goes to the session of its udp port, in the order of the queue. */
TEST_F(StRxMgrQueueTest, DispatchesEachPacketByUdpPort) {
  ut_mq_map(30000, 6);
  ut_mq_map(30002, 9);
  ASSERT_EQ(ut_mq_feed(30002), 0);
  ASSERT_EQ(ut_mq_feed(30000), 0);

  EXPECT_EQ(ut_mq_tasklet(8), 0);
  ASSERT_EQ(ut_mq_got_n(), 2);
  EXPECT_EQ(ut_mq_got(0), 9u);
  EXPECT_EQ(ut_mq_got(1), 6u);
}

/* A packet for a udp port of no session is freed, not delivered. */
TEST_F(StRxMgrQueueTest, PacketOfNoSessionIsFreed) {
  ASSERT_EQ(ut_mq_feed(40000), 0);

  ut_mq_tasklet(8);
  EXPECT_EQ(ut_mq_got_n(), 0);
  EXPECT_EQ(ut_mq_deferred(), 0);
}

/* The stat thread holds the session lock: the packet waits for the next call. */
TEST_F(StRxMgrQueueTest, PacketOfBusySessionWaitsForTheNextCall) {
  ut_mq_map(30000, 6);
  ASSERT_EQ(ut_mq_feed(30000), 0);
  ut_mq_set_busy(6, -1);

  EXPECT_GT(ut_mq_tasklet(8), 0) << "a kept packet is pending work";
  EXPECT_EQ(ut_mq_got_n(), 0);
  EXPECT_EQ(ut_mq_deferred(), 1);

  ut_mq_set_busy(-1, 0);
  EXPECT_EQ(ut_mq_tasklet(8), 0);
  ASSERT_EQ(ut_mq_got_n(), 1);
  EXPECT_EQ(ut_mq_got(0), 6u);
  EXPECT_EQ(ut_mq_deferred(), 0);
}

/* A full defer list drops the packet and counts it. */
TEST_F(StRxMgrQueueTest, FullDeferListDropsAndCounts) {
  ut_mq_map(30000, 6);
  ut_mq_set_busy(6, -1);
  for (int i = 0; i < 70; i++) ASSERT_EQ(ut_mq_feed(30000), 0);

  for (int i = 0; i < 10; i++) ut_mq_tasklet(32);
  EXPECT_EQ(ut_mq_deferred(), 64);
  EXPECT_EQ(ut_mq_dropped(), 6u);
}

/* One call takes at most burst packets, so the other tasklets run in between. */
TEST_F(StRxMgrQueueTest, OneCallTakesAtMostBurstPackets) {
  ut_mq_map(30000, 6);
  for (int i = 0; i < 20; i++) ASSERT_EQ(ut_mq_feed(30000), 0);

  EXPECT_GT(ut_mq_tasklet(8), 0);
  EXPECT_EQ(ut_mq_got_n(), 8);
  EXPECT_GT(ut_mq_tasklet(8), 0);
  EXPECT_EQ(ut_mq_got_n(), 16);
  EXPECT_EQ(ut_mq_tasklet(8), 0);
  EXPECT_EQ(ut_mq_got_n(), 20);
}

/* A vector rx path reads nothing for a burst below 8, so a smaller burst reads 8. */
TEST_F(StRxMgrQueueTest, SmallBurstReadsEightPackets) {
  ut_mq_map(30000, 6);
  for (int i = 0; i < 10; i++) ASSERT_EQ(ut_mq_feed(30000), 0);

  ut_mq_tasklet(4);
  EXPECT_EQ(ut_mq_got_n(), 8);
}

/* A second session with the same udp port gets a queue of its own. */
TEST_F(StRxMgrQueueTest, AddOfTakenUdpPortIsRefused) {
  ut_mq_map(30000, 6);

  EXPECT_EQ(ut_mq_add(30000, 9), -EEXIST);
}

/* A packet after a kept packet of the same session waits behind it, also when the lock
 * is free again: the session gets its packets in the order of the queue. */
TEST_F(StRxMgrQueueTest, LaterPacketOfAKeptSessionWaitsBehindIt) {
  ut_mq_map(30000, 6);
  ut_mq_map(30002, 9);
  ASSERT_EQ(ut_mq_feed(30000), 0); /* seq 0 */
  ASSERT_EQ(ut_mq_feed(30000), 0); /* seq 1 */
  ASSERT_EQ(ut_mq_feed(30002), 0); /* seq 2 */
  ut_mq_set_busy(6, 1);

  EXPECT_GT(ut_mq_tasklet(8), 0);
  ASSERT_EQ(ut_mq_got_n(), 1) << "the other session does not wait";
  EXPECT_EQ(ut_mq_got_seq(0), 2u);
  EXPECT_EQ(ut_mq_deferred(), 2);

  EXPECT_EQ(ut_mq_tasklet(8), 0);
  ASSERT_EQ(ut_mq_got_n(), 3);
  EXPECT_EQ(ut_mq_got_seq(1), 0u);
  EXPECT_EQ(ut_mq_got_seq(2), 1u);
}

/* After a long lock hold, one call gives at most burst kept packets to the session, and
 * does not read the queue in that call. */
TEST_F(StRxMgrQueueTest, OneCallGivesAtMostBurstKeptPackets) {
  ut_mq_map(30000, 6);
  ut_mq_set_busy(6, -1);
  for (int i = 0; i < 20; i++) ASSERT_EQ(ut_mq_feed(30000), 0);
  for (int i = 0; i < 3; i++) ut_mq_tasklet(8);
  ASSERT_EQ(ut_mq_deferred(), 20);
  ASSERT_EQ(ut_mq_feed(30000), 0);

  ut_mq_set_busy(-1, 0);
  EXPECT_GT(ut_mq_tasklet(8), 0);
  EXPECT_EQ(ut_mq_got_n(), 8);
  EXPECT_EQ(ut_mq_pending(), 1) << "the queue is not read in this call";
  ut_mq_tasklet(8);
  ut_mq_tasklet(8);
  ut_mq_tasklet(8);
  ASSERT_EQ(ut_mq_got_n(), 21);
  for (int i = 0; i < 21; i++) EXPECT_EQ(ut_mq_got_seq(i), (uint32_t)i);
}

/* A call that gives a full burst of kept packets does not read the queue, so it reports
 * work left: with MTL_FLAG_TASKLET_SLEEP the scheduler must not sleep on the queue. */
TEST_F(StRxMgrQueueTest, FullBurstOfKeptPacketsReportsWorkLeft) {
  ut_mq_map(30000, 6);
  ut_mq_set_busy(6, -1);
  for (int i = 0; i < 8; i++) ASSERT_EQ(ut_mq_feed(30000), 0);
  ut_mq_tasklet(8);
  ASSERT_EQ(ut_mq_deferred(), 8);
  ASSERT_EQ(ut_mq_feed(30000), 0);

  ut_mq_set_busy(-1, 0);
  EXPECT_GT(ut_mq_tasklet(8), 0) << "the queue still holds a packet";
  EXPECT_EQ(ut_mq_got_n(), 8);
  EXPECT_EQ(ut_mq_pending(), 1);
}

/* The lock is taken again before the kept packets go: they keep their order. */
TEST_F(StRxMgrQueueTest, KeptPacketsKeepTheirOrderWhenTheLockIsTakenAgain) {
  ut_mq_map(30000, 6);
  ASSERT_EQ(ut_mq_feed(30000), 0); /* seq 0 */
  ASSERT_EQ(ut_mq_feed(30000), 0); /* seq 1 */
  ut_mq_set_busy(6, 1);
  ut_mq_tasklet(8);
  ut_mq_set_busy(6, 1);
  ut_mq_tasklet(8);
  EXPECT_EQ(ut_mq_got_n(), 0);

  ut_mq_tasklet(8);
  ASSERT_EQ(ut_mq_got_n(), 2);
  EXPECT_EQ(ut_mq_got_seq(0), 0u);
  EXPECT_EQ(ut_mq_got_seq(1), 1u);
}

/* An add whose flow rule fails gives the udp port back. */
TEST_F(StRxMgrQueueTest, FailedAddFreesTheUdpPort) {
  EXPECT_EQ(ut_mq_add(30004, 9), -EIO);
  EXPECT_EQ(ut_mq_map_get(30004), 0u);
}

/* A burst above the stack array reads at most 32 packets. */
TEST_F(StRxMgrQueueTest, LargeBurstReadsAtMost32Packets) {
  ut_mq_map(30000, 6);
  for (int i = 0; i < 40; i++) ASSERT_EQ(ut_mq_feed(30000), 0);

  ut_mq_tasklet(64);
  EXPECT_EQ(ut_mq_got_n(), 32);
}

/* A packet shorter than the udp headers is freed, not given to a session. */
TEST_F(StRxMgrQueueTest, ShortPacketIsFreed) {
  ut_mq_map(0, 6);
  ASSERT_EQ(ut_mq_feed_short(), 0);

  EXPECT_EQ(ut_mq_tasklet(8), 0);
  EXPECT_EQ(ut_mq_got_n(), 0);
  EXPECT_EQ(ut_mq_deferred(), 0);
}
