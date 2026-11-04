/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * A frame level ancillary session on the rx queue of its manager: the manager tasklet
 * reads the queue and rx_ancillary_sessions_queue_dispatch() gives each packet to the
 * session under its slot lock, after the same port and IP check as mt_udp_matched().
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St40RxMgrQueue*'
 */

#include <gtest/gtest.h>

#include "mtl_sch_api.h"
#include "session/st40_harness.h"

namespace {
constexpr uint16_t kUdpPort = 40000;
}  // namespace

class St40RxMgrQueueTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut40_init(), 0);
    ut40_drain_ring();
    ctx_ = ut40_ctx_create(1);
    ASSERT_NE(ctx_, nullptr);
    ut40_setup_frame_pool(ctx_, 3, 1024);
    ut40_mq_setup(ctx_, kUdpPort);
    mbufs_ = ut40_mbufs_in_use();
  }

  void TearDown() override {
    ut40_mq_clear(ctx_);
    EXPECT_EQ(ut40_mbufs_in_use(), mbufs_) << "a packet was not freed";
    ut40_teardown_frame_pool(ctx_);
    ut40_ctx_destroy(ctx_);
  }

  /* the frames the session gave to the app go back, so the slots stay free */
  void ReleaseFrames() {
    for (int i = 0; i < ut40_captured_count(); i++)
      ut40_release_frame(ctx_, ut40_captured_addr(i));
    ut40_captured_reset();
  }

  ut_test_ctx* ctx_ = nullptr;
  unsigned int mbufs_ = 0;
};

/* Each packet of the group reaches the session, and a frame level session frees it. */
TEST_F(St40RxMgrQueueTest, PacketOfTheGroupReachesTheSession) {
  /* one frame per packet, as many as the frame pool of 3 holds */
  for (uint16_t seq = 0; seq < 3; seq++) ASSERT_EQ(ut40_mq_push(ctx_, seq, 1), 0);

  ut40_mgr_tasklet(ctx_);
  EXPECT_EQ(ut40_mq_left(), 0);
  EXPECT_EQ(ut40_stat_received(ctx_), 3u);
  EXPECT_EQ(ut40_captured_count(), 3);
  ReleaseFrames();
}

/* A packet of another group on the same udp port is not for the session. */
TEST_F(St40RxMgrQueueTest, PacketOfAnotherGroupIsDropped) {
  ASSERT_EQ(ut40_mq_push(ctx_, 0, 2), 0);

  ut40_mgr_tasklet(ctx_);
  EXPECT_EQ(ut40_stat_received(ctx_), 0u);
  EXPECT_EQ(ut40_mq_deferred(ctx_), 0);
}

/* After a change of the session port, the packets of the old physical port are not for
 * the session. */
TEST_F(St40RxMgrQueueTest, PacketOfAnotherPhysicalPortIsDropped) {
  ut40_mq_set_phy_port(ctx_, MTL_PORT_R);
  ASSERT_EQ(ut40_mq_push(ctx_, 0, 1), 0);

  ut40_mgr_tasklet(ctx_);
  EXPECT_EQ(ut40_stat_received(ctx_), 0u);
}

/* The stat thread holds the slot lock: the packet waits and reaches the session in a
 * later call. */
TEST_F(St40RxMgrQueueTest, PacketOfALockedSessionReachesItLater) {
  ASSERT_EQ(ut40_mq_push(ctx_, 0, 1), 0);

  ut40_mq_lock(ctx_);
  EXPECT_EQ(ut40_mgr_tasklet(ctx_), MTL_TASKLET_HAS_PENDING);
  EXPECT_EQ(ut40_mq_deferred(ctx_), 1);
  EXPECT_EQ(ut40_stat_received(ctx_), 0u);
  ut40_mq_unlock(ctx_);

  ut40_mgr_tasklet(ctx_);
  EXPECT_EQ(ut40_mq_deferred(ctx_), 0);
  EXPECT_EQ(ut40_stat_received(ctx_), 1u);
  ReleaseFrames();
}

/* A session port on the queue of the manager reports the queue of the manager, as
 * st40_rx_get_queue_meta() does. */
TEST_F(St40RxMgrQueueTest, QueueIdIsTheQueueOfTheManager) {
  EXPECT_EQ(ut40_mq_queue_id(ctx_), 9);
}
