/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Bounds tests for mt_rtcp_tx_parse_rtcp_packet(): a crafted RTCP NACK must
 * never make the FCI-walk count underflow (CWE-191) or walk past the received
 * bytes, and a large attacker-controlled "follow" must not size an unbounded
 * stack VLA in the retransmit path.
 *
 * With an empty ring, stat_rtp_retransmit_fail counts (clamped) bulk per parse
 * loop iteration, so it is a deterministic proxy for how many FCIs were walked.
 * Reject paths return <0 and never enter the loop (fail == 0). Accept paths
 * return 0 and walk exactly the intended number of FCIs.
 */

#include <gtest/gtest.h>

#include "session/rtcp_tx_harness.h"

namespace {

/* Fixed ring capacity for all cases; small so the VLA clamp is observable. */
constexpr int kRing = 8;
/* Mirror of MT_RTCP_MAX_FCIS in lib/src/mt_rtcp.h (internal header not pulled
 * into C++); the CapAtMaxFcis case fails loudly if the two ever diverge. */
constexpr uint32_t kMaxFcis = 256;

/* rtcp->len is length in 32-bit words minus one: len = 2 + num_fci for a
 * well-formed IMTL NACK (12-byte header = 3 words, each fci = 1 word). */
constexpr uint16_t LenForFci(uint32_t num_fci) {
  return (uint16_t)(2 + num_fci);
}
constexpr size_t BytesForFci(uint32_t num_fci) {
  return 12u + num_fci * 4u;
}

/* ── Reject paths: malformed len must be refused before walking FCIs ─────── */

TEST(MtRtcpTxParseNack, RejectLenUnderflowZero) {
  /* len=0 -> unpatched num_fcis = 0 + 1 - 3 = 65534 (uint16 underflow). */
  auto r = ut_rtcp_tx_feed_nack(kRing, /*len_field=*/0, /*fci_count=*/0,
                                /*follow=*/0, /*recv_len=*/20);
  EXPECT_LT(r.ret, 0);
  EXPECT_EQ(r.retransmit_fail, 0u);
  EXPECT_EQ(r.nack_received, 1u); /* still counted as a received NACK */
}

TEST(MtRtcpTxParseNack, RejectLenUnderflowOne) {
  /* len=1 -> unpatched num_fcis = 1 + 1 - 3 = 65535 (the reported CWE-191). */
  auto r = ut_rtcp_tx_feed_nack(kRing, /*len_field=*/1, /*fci_count=*/0,
                                /*follow=*/0, /*recv_len=*/20);
  EXPECT_LT(r.ret, 0);
  EXPECT_EQ(r.retransmit_fail, 0u);
}

TEST(MtRtcpTxParseNack, RejectDeclaredExceedsRecv) {
  /* len claims 4 FCIs (28 bytes) but only 1 FCI worth of bytes was received. */
  auto r = ut_rtcp_tx_feed_nack(kRing, /*len_field=*/LenForFci(4), /*fci_count=*/1,
                                /*follow=*/0, /*recv_len=*/BytesForFci(1));
  EXPECT_LT(r.ret, 0);
  EXPECT_EQ(r.retransmit_fail, 0u);
}

TEST(MtRtcpTxParseNack, RejectShorterThanHeader) {
  /* Fewer bytes received than the fixed 12-byte header: rejected before any
   * header field is inspected, so it is not even counted as a received NACK. */
  auto r = ut_rtcp_tx_feed_nack(kRing, /*len_field=*/LenForFci(1), /*fci_count=*/1,
                                /*follow=*/0, /*recv_len=*/8);
  EXPECT_LT(r.ret, 0);
  EXPECT_EQ(r.retransmit_fail, 0u);
  EXPECT_EQ(r.nack_received, 0u);
}

/* ── Accept paths: well-formed NACKs must still be processed (no false pos) ─ */

TEST(MtRtcpTxParseNack, AcceptZeroFci) {
  auto r = ut_rtcp_tx_feed_nack(kRing, /*len_field=*/LenForFci(0), /*fci_count=*/0,
                                /*follow=*/0, /*recv_len=*/BytesForFci(0));
  EXPECT_EQ(r.ret, 0);
  EXPECT_EQ(r.retransmit_fail, 0u);
}

TEST(MtRtcpTxParseNack, AcceptOneFci) {
  auto r = ut_rtcp_tx_feed_nack(kRing, /*len_field=*/LenForFci(1), /*fci_count=*/1,
                                /*follow=*/0, /*recv_len=*/BytesForFci(1));
  EXPECT_EQ(r.ret, 0);
  EXPECT_EQ(r.retransmit_fail, 1u); /* walked exactly one FCI */
}

TEST(MtRtcpTxParseNack, AcceptTwoFci) {
  auto r = ut_rtcp_tx_feed_nack(kRing, /*len_field=*/LenForFci(2), /*fci_count=*/2,
                                /*follow=*/0, /*recv_len=*/BytesForFci(2));
  EXPECT_EQ(r.ret, 0);
  EXPECT_EQ(r.retransmit_fail, 2u);
}

/* ── Caps: parse count is bounded by MT_RTCP_MAX_FCIS, matching produce path ─ */

TEST(MtRtcpTxParseNack, CapAtMaxFcis) {
  const uint32_t claimed = kMaxFcis + 44; /* 300 > 256 */
  auto r = ut_rtcp_tx_feed_nack(kRing, /*len_field=*/LenForFci(claimed),
                                /*fci_count=*/claimed, /*follow=*/0,
                                /*recv_len=*/BytesForFci(claimed));
  EXPECT_EQ(r.ret, 0);
  EXPECT_EQ(r.retransmit_fail, kMaxFcis); /* capped, not 300 */
}

/* ── VLA vector: a huge "follow" must be clamped to the ring capacity ─────── */

TEST(MtRtcpTxParseNack, ClampHugeFollowToRing) {
  /* One well-formed FCI with follow=0xFFFE -> bulk = follow + 1 = 65535.
   * Patched clamps bulk to the ring capacity before sizing the VLA, so the
   * single failed attempt adds exactly kRing to the fail counter. */
  auto r = ut_rtcp_tx_feed_nack(kRing, /*len_field=*/LenForFci(1), /*fci_count=*/1,
                                /*follow=*/0xFFFE, /*recv_len=*/BytesForFci(1));
  EXPECT_EQ(r.ret, 0);
  EXPECT_EQ(r.retransmit_fail, (uint32_t)kRing);
}

TEST(MtRtcpTxParseNack, FollowAllOnesWrapsToZeroBulk) {
  /* follow=0xFFFF -> bulk = 0x10000 truncates to 0; must fail cleanly. */
  auto r = ut_rtcp_tx_feed_nack(kRing, /*len_field=*/LenForFci(1), /*fci_count=*/1,
                                /*follow=*/0xFFFF, /*recv_len=*/BytesForFci(1));
  EXPECT_EQ(r.ret, 0);
  EXPECT_EQ(r.retransmit_fail, 1u); /* one attempt, bulk==0 counted as one fail */
}

}  // namespace
