/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins the UDP ports st20_tx_update_destination() writes into the session's
 * packet header template: the new destination port, and the source port chosen
 * by the st20_tx_create() rules (ops.udp_src_port, else the destination port,
 * unless MTL_FLAG_RANDOM_SRC_PORT picked one).
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxUpdateDestinationTest.*'
 */

#include <gtest/gtest.h>

#include <cstdint>

#include "session/st20_tx_harness.h"

namespace {

constexpr uint16_t kUdpSrcPort = 5004;
constexpr uint16_t kCreatedUdpPort = 10000;
constexpr uint16_t kRandomSrcPort = 43210;
constexpr uint16_t kNewUdpPort = 20000;

class St20TxUpdateDestinationTest : public ::testing::Test {
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

TEST_F(St20TxUpdateDestinationTest, DestinationPortIsNotReplacedByUserSourcePort) {
  uint16_t src_port = 0, dst_port = 0;

  ASSERT_EQ(ut_txv_update_dst(ctx_, kUdpSrcPort, kUdpSrcPort, kNewUdpPort, &src_port,
                              &dst_port),
            0);
  EXPECT_EQ(dst_port, kNewUdpPort);
  EXPECT_EQ(src_port, kUdpSrcPort);
}

TEST_F(St20TxUpdateDestinationTest, DefaultSourcePortFollowsNewDestinationPort) {
  uint16_t src_port = 0, dst_port = 0;

  ASSERT_EQ(
      ut_txv_update_dst(ctx_, 0, kCreatedUdpPort, kNewUdpPort, &src_port, &dst_port), 0);
  EXPECT_EQ(dst_port, kNewUdpPort);
  EXPECT_EQ(src_port, kNewUdpPort);
}

TEST_F(St20TxUpdateDestinationTest, RandomSourcePortIsKept) {
  uint16_t src_port = 0, dst_port = 0;

  ut_txv_set_random_src_port(ctx_, true);
  ASSERT_EQ(ut_txv_update_dst(ctx_, 0, kRandomSrcPort, kNewUdpPort, &src_port, &dst_port),
            0);
  EXPECT_EQ(dst_port, kNewUdpPort);
  EXPECT_EQ(src_port, kRandomSrcPort);
}

}  // namespace
