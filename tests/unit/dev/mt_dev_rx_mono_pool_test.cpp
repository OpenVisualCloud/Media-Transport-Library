/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * With MTL_FLAG_RX_MONO_POOL every RX queue bursts from its own queue of its own DPDK
 * port and is put back to its own MTL port.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='MtDevRxMonoPoolTest.*'
 */

#include <gtest/gtest.h>

#include "dev/mt_dev_harness.h"

class MtDevRxMonoPoolTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ctx_ = ut_dev_create_ctx();
    ASSERT_NE(nullptr, ctx_);
  }
  void TearDown() override {
    ut_dev_destroy_ctx(ctx_);
  }
  ut_dev_ctx* ctx_ = nullptr;
};

TEST_F(MtDevRxMonoPoolTest, EveryQueueKeepsItsOwnIds) {
  constexpr uint16_t kQueues = 8, kPortId = 3;
  uint16_t queue_ids[kQueues], port_ids[kQueues];
  enum mtl_port ports[kQueues];

  ASSERT_EQ(
      0, ut_dev_init_mono_rx_queues(ctx_, kQueues, kPortId, queue_ids, port_ids, ports));
  for (uint16_t q = 0; q < kQueues; q++) {
    EXPECT_EQ(q, queue_ids[q]) << "queue " << q;
    EXPECT_EQ(kPortId, port_ids[q]) << "queue " << q;
    EXPECT_EQ(MTL_PORT_R, ports[q]) << "queue " << q;
  }
}
