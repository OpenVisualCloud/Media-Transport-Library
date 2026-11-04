/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * A frame level audio session on the rx queue of its manager has no rx queue of its
 * own: st30_rx_get_queue_meta() reports the queue of the manager, and the manager gives
 * each packet of the queue to its session by the key of the udp port.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St30RxMgrQueue*'
 */

#include <gtest/gtest.h>

#include "session/st30/st30_rx_test_base.h"

class St30RxMgrQueueTest : public St30RxBaseTest {};

TEST_F(St30RxMgrQueueTest, QueueIdIsTheQueueOfTheManager) {
  EXPECT_EQ(ut30_mq_queue_id(ctx_), 9);
}

/* The key holds the session index and the session port; the session takes only a packet
 * of its phy port and group, and a held slot lock makes the packet wait. */
TEST_F(St30RxMgrQueueTest, DispatchGivesThePacketToTheSessionOfTheKey) {
  const uint32_t key = 2u << 1 | MTL_SESSION_PORT_R;

  EXPECT_EQ(ut30_mq_dispatch(ctx_, MTL_PORT_R, key, 1, false), 0);
  EXPECT_EQ(port_pkts(MTL_SESSION_PORT_R), 1u);
  EXPECT_EQ(ut30_mq_dispatch(ctx_, MTL_PORT_R, key, 2, false), -ENOENT) << "other group";
  EXPECT_EQ(ut30_mq_dispatch(ctx_, MTL_PORT_P, key, 1, false), -ENOENT)
      << "other phy port";
  EXPECT_EQ(ut30_mq_dispatch(ctx_, MTL_PORT_R, key, 1, true), -EBUSY) << "slot lock held";
  EXPECT_EQ(port_pkts(MTL_SESSION_PORT_R), 1u);
}
