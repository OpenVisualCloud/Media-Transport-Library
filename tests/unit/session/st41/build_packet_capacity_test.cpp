/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * tx_fastmetadata_session_build_packet() mbuf-capacity checks: the function
 * must reject buffers too small for the fmd header, and separately reject
 * buffers too small for header + RTP payload, without partially building
 * the packet.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest
 * --gtest_filter='St41TxBuildPacketCapacityTest.*'
 */

#include <gtest/gtest.h>

#include "session/st41_tx_harness.h"

class St41TxBuildPacketCapacityTest : public ::testing::Test {
 protected:
  ut_test_ctx* ctx_ = nullptr;

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

/* Buffer too small to even hold the fmd header (eth+ipv4+udp+rtp): the
 * function must return before writing anything. Note: a fresh mbuf always
 * has data_len == 0, so this alone can't distinguish the tailroom-based
 * check from the pre-fix data_len-based one (both reject here identically);
 * see RejectsTooSmallForHeaderDespiteStaleDataLen below for that. */
TEST_F(St41TxBuildPacketCapacityTest, RejectsTooSmallForHeader) {
  size_t room = ut41tx_fmd_hdr_len() - 1;
  struct rte_mbuf* pkt = ut41tx_alloc_mbuf(room);
  ASSERT_NE(pkt, nullptr);

  ut41tx_build_packet(ctx_, pkt);

  EXPECT_EQ(ut41tx_pkt_data_len(pkt), 0u);
  EXPECT_EQ(ut41tx_pkt_pkt_len(pkt), 0u);

  ut41tx_free_mbuf(pkt);
}

/* Same too-small-tailroom scenario, but data_len is pre-set to the full
 * header size (i.e. what the pre-fix `data_len < sizeof(*hdr)` check would
 * have read as "already big enough, proceed"). The tailroom-based check
 * must still reject *before* touching the buffer: tailroom, not data_len,
 * reflects real remaining capacity.
 *
 * data_len, not pkt_len, is the only reliable witness here: the function
 * unconditionally overwrites data_len to the eth+ipv4+udp size the instant
 * it gets past the header check (before the second, payload-capacity check
 * even runs), clobbering whatever value this test seeded. So under the
 * reverted (data_len-based) production code, the stale data_len fools the
 * *first* check into proceeding -- data_len flips from the seeded value to
 * 42 -- and only the *second* check (independently, always true here since
 * data_len(42) < sizeof(*hdr)) stops it from going further. Checking
 * pkt_len alone can't tell "rejected immediately" from "rejected one check
 * later after partially mutating the packet"; checking data_len can. */
TEST_F(St41TxBuildPacketCapacityTest, RejectsTooSmallForHeaderDespiteStaleDataLen) {
  size_t hdr_len = ut41tx_fmd_hdr_len();
  struct rte_mbuf* pkt = ut41tx_alloc_mbuf_stale_data_len(hdr_len - 1, hdr_len);
  ASSERT_NE(pkt, nullptr);

  ut41tx_build_packet(ctx_, pkt);

  /* Fixed (tailroom-based) code must reject at the very first check,
   * before ever assigning pkt->data_len -- it must still read back as the
   * seeded stale value, unchanged. (Reverted/data_len-based code fails
   * this: it proceeds past the first check and unconditionally overwrites
   * data_len to 42 before bailing out at the second check instead.) */
  EXPECT_EQ(ut41tx_pkt_data_len(pkt), hdr_len);
  EXPECT_EQ(ut41tx_pkt_pkt_len(pkt), 0u);

  ut41tx_free_mbuf(pkt);
}

/* Buffer holds the fmd header exactly but has no room for the RTP payload:
 * the function must get past the header check (data_len reflects the
 * eth+ipv4+udp prefix it already wrote) but stop before appending the
 * payload (pkt_len never gets set). */
TEST_F(St41TxBuildPacketCapacityTest, RejectsTooSmallForPayload) {
  const uint8_t payload[50] = {0};
  ut41tx_ctx_set_payload(ctx_, payload, sizeof(payload));

  size_t room = ut41tx_fmd_hdr_len();
  struct rte_mbuf* pkt = ut41tx_alloc_mbuf(room);
  ASSERT_NE(pkt, nullptr);

  ut41tx_build_packet(ctx_, pkt);

  EXPECT_EQ(ut41tx_pkt_data_len(pkt), ut41tx_l234_hdr_len());
  EXPECT_EQ(ut41tx_pkt_pkt_len(pkt), 0u);

  ut41tx_free_mbuf(pkt);
}

/* Ample buffer: the packet is actually built (both checks pass). */
TEST_F(St41TxBuildPacketCapacityTest, BuildsPacketWhenBufferSufficient) {
  const uint8_t payload[50] = {0};
  ut41tx_ctx_set_payload(ctx_, payload, sizeof(payload));

  size_t room = ut41tx_fmd_hdr_len() + sizeof(payload) + 64;
  struct rte_mbuf* pkt = ut41tx_alloc_mbuf(room);
  ASSERT_NE(pkt, nullptr);

  ut41tx_build_packet(ctx_, pkt);

  uint32_t data_item_length = (sizeof(payload) + 3) / 4;
  uint32_t expected = ut41tx_fmd_hdr_len() + data_item_length * 4;
  EXPECT_EQ(ut41tx_pkt_data_len(pkt), expected);
  EXPECT_EQ(ut41tx_pkt_pkt_len(pkt), expected);

  ut41tx_free_mbuf(pkt);
}
