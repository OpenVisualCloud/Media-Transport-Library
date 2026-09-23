/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * tx_fastmetadata_session_build_packet() mbuf-capacity checks: a buffer
 * that exactly fits header + payload must be built, and one that is a byte
 * short of the header or of the payload must be rejected. The only caller
 * passes a freshly allocated mbuf, so data_len is always 0 on entry.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest
 * --gtest_filter='St41TxBuildPacketCapacityTest.*'
 */

#include <gtest/gtest.h>

#include "session/st41_tx_harness.h"

namespace {
/* A whole number of 4-byte words, so the rounded-up payload write equals the
 * unrounded length the room check tests (see the build asserts in
 * tx_fastmetadata_sessions_mgr_init). */
constexpr uint16_t kPayloadLen = 48;
}  // namespace

class St41TxBuildPacketCapacityTest : public ::testing::Test {
 protected:
  ut41tx_ctx* ctx_ = nullptr;
  uint8_t payload_[kPayloadLen] = {};

  void SetUp() override {
    ASSERT_EQ(ut41tx_init(), 0) << "EAL init failed";
    ctx_ = ut41tx_ctx_create();
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut41tx_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }
};

/* Must return before writing the header, leaving the mbuf untouched. */
TEST_F(St41TxBuildPacketCapacityTest, RejectsBufferTooSmallForHeader) {
  struct rte_mbuf* pkt = ut41tx_alloc_mbuf(ut41tx_fmd_hdr_len() - 1);
  ASSERT_NE(pkt, nullptr);

  ut41tx_build_packet(ctx_, pkt);

  EXPECT_EQ(ut41tx_pkt_data_len(pkt), 0u);
  EXPECT_EQ(ut41tx_pkt_pkt_len(pkt), 0u);

  ut41tx_free_mbuf(pkt);
}

/* An exact fit must be built. Checks against data_len instead of tailroom
 * reject every fresh mbuf, and an off-by-one payload check rejects this one. */
TEST_F(St41TxBuildPacketCapacityTest, BuildsPacketThatExactlyFits) {
  ut41tx_ctx_set_payload(ctx_, payload_, kPayloadLen);
  size_t room = ut41tx_fmd_hdr_len() + kPayloadLen;
  struct rte_mbuf* pkt = ut41tx_alloc_mbuf(room);
  ASSERT_NE(pkt, nullptr);

  ut41tx_build_packet(ctx_, pkt);

  EXPECT_EQ(ut41tx_pkt_data_len(pkt), room);
  EXPECT_EQ(ut41tx_pkt_pkt_len(pkt), room);

  ut41tx_free_mbuf(pkt);
}

/* The header fits, the payload does not: the packet must not be finalized. */
TEST_F(St41TxBuildPacketCapacityTest, RejectsBufferOneByteShortOfPayload) {
  ut41tx_ctx_set_payload(ctx_, payload_, kPayloadLen);
  struct rte_mbuf* pkt = ut41tx_alloc_mbuf(ut41tx_fmd_hdr_len() + kPayloadLen - 1);
  ASSERT_NE(pkt, nullptr);

  ut41tx_build_packet(ctx_, pkt);

  EXPECT_EQ(ut41tx_pkt_pkt_len(pkt), 0u);

  ut41tx_free_mbuf(pkt);
}
