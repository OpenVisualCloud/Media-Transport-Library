/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST40 RX in ST40_RX_FLAG_FAST_METADATA mode: SMPTE ST 2110-41 packets, zero-item
 * keepalives included, reach the app untouched and without Ethernet padding. Payload
 * bits that would read as RFC 8331 F bits must not drop packets, drive interlace
 * detection or count cross-port mismatches.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St40RxFmdTest.*'
 */

#include <arpa/inet.h>
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "session/st40/st40_rx_test_base.h"
#include "st40_api.h"

namespace {
constexpr uint32_t kDit = 0x123456;
constexpr uint32_t kTs = 1000;
constexpr size_t kRtpBaseLen = 12;

/* An FMD data item whose first word reads as an RFC 8331 header with `f_bits`. */
std::vector<uint8_t> rfc8331_lookalike_item(uint8_t f_bits) {
  struct st40_rfc8331_payload_hdr_common hdr;
  memset(&hdr, 0, sizeof(hdr));
  hdr.first_hdr_chunk.anc_count = 1;
  hdr.first_hdr_chunk.f = f_bits;
  uint32_t word = htonl(hdr.swapped_handle);

  std::vector<uint8_t> item(8, 0xc3);
  memcpy(item.data(), &word, sizeof(word));
  return item;
}

std::vector<uint8_t> fmd_packet(uint16_t seq, uint32_t ts,
                                const std::vector<uint8_t>& item, uint8_t pt = 0) {
  struct st40_fmd_rtp_hdr hdr;
  memset(&hdr, 0, sizeof(hdr));
  hdr.base.version = 2;
  hdr.base.payload_type = pt;
  hdr.base.seq_number = htons(seq);
  hdr.base.tmstamp = htonl(ts);
  hdr.fmd_hdr_chunk.data_item_type = kDit;
  hdr.fmd_hdr_chunk.data_item_k_bit = 0;
  hdr.fmd_hdr_chunk.data_item_length = item.size() / 4;
  hdr.swapped_fmd_hdr_chunk = htonl(hdr.swapped_fmd_hdr_chunk);

  std::vector<uint8_t> pkt(sizeof(hdr) + item.size());
  memcpy(pkt.data(), &hdr, sizeof(hdr));
  memcpy(pkt.data() + sizeof(hdr), item.data(), item.size());
  return pkt;
}

std::vector<uint8_t> zero_item_packet(uint16_t seq, uint32_t ts) {
  auto pkt = fmd_packet(seq, ts, {});
  pkt.resize(kRtpBaseLen);
  return pkt;
}

void append_be32(std::vector<uint8_t>& pkt, uint32_t word) {
  for (int shift = 24; shift >= 0; shift -= 8)
    pkt.push_back(static_cast<uint8_t>(word >> shift));
}

void append_item(std::vector<uint8_t>& pkt, uint32_t dit, uint8_t k, uint32_t words) {
  append_be32(pkt, (dit << 10) | (static_cast<uint32_t>(k) << 9) | words);
  for (uint32_t i = 0; i < words; i++) append_be32(pkt, 0xc0c1c2c3 + i);
}
}  // namespace

class St40RxFmdTest : public St40RxBaseTest {
 protected:
  void SetUp() override {
    St40RxBaseTest::SetUp();
    ut40_ctx_set_fmd(ctx_);
  }

  int feed_fmd(uint16_t seq, uint32_t ts, const std::vector<uint8_t>& item,
               enum mtl_session_port port) {
    auto pkt = fmd_packet(seq, ts, item);
    return ut40_feed_rtp_bytes(ctx_, pkt.data(), static_cast<uint16_t>(pkt.size()), port);
  }

  /* `pkt` fed on port P and read back through st40_rx_get_mbuf(). */
  std::vector<uint8_t> round_trip(const std::vector<uint8_t>& pkt, uint16_t eth_pad = 0) {
    ut40_drain_paused guard;
    std::vector<uint8_t> got(2048);
    if (ut40_feed_rtp_bytes_padded(ctx_, pkt.data(), static_cast<uint16_t>(pkt.size()),
                                   eth_pad, MTL_SESSION_PORT_P) != 0)
      return {};
    int len = ut40_ring_dequeue_rtp(ctx_, got.data(), static_cast<uint16_t>(got.size()));
    got.resize(len < 0 ? 0 : len);
    return got;
  }
};

TEST_F(St40RxFmdTest, PayloadLookingLikeInvalidFBitsIsAccepted) {
  EXPECT_EQ(feed_fmd(0, kTs, rfc8331_lookalike_item(0x1), MTL_SESSION_PORT_P), 0);

  EXPECT_EQ(wrong_interlace(), 0u);
  EXPECT_EQ(received(), 1u);
}

TEST_F(St40RxFmdTest, EnqueuedPacketIsByteIdentical) {
  ut40_drain_paused guard;
  auto sent = fmd_packet(0, kTs, rfc8331_lookalike_item(0x2));
  ASSERT_EQ(ut40_feed_rtp_bytes(ctx_, sent.data(), static_cast<uint16_t>(sent.size()),
                                MTL_SESSION_PORT_P),
            0);

  std::vector<uint8_t> got(sent.size() + 16);
  int len = ut40_ring_dequeue_rtp(ctx_, got.data(), static_cast<uint16_t>(got.size()));

  ASSERT_EQ(len, static_cast<int>(sent.size()));
  EXPECT_EQ(memcmp(got.data(), sent.data(), sent.size()), 0);
}

/* §5.1: a zero-item packet is a valid keepalive, delivered as its 12 RTP bytes. */
TEST_F(St40RxFmdTest, ZeroItemPacketIsDelivered) {
  auto sent = zero_item_packet(0, kTs);

  EXPECT_EQ(round_trip(sent), sent);
  EXPECT_EQ(received(), 1u);
}

/* The 54-byte zero-item frame is padded to 60 on the wire; len comes from UDP. */
TEST_F(St40RxFmdTest, EthernetPaddingIsNotDelivered) {
  auto sent = zero_item_packet(0, kTs);

  EXPECT_EQ(round_trip(sent, 6), sent);
}

/* §5.1/§5.4: one packet carries several items of several DITs. */
TEST_F(St40RxFmdTest, MultiItemMultiDitPacketIsByteIdentical) {
  auto sent = zero_item_packet(0, kTs);
  append_item(sent, 0x000001, 0, 2);
  append_item(sent, 0x200002, 0, 1);
  append_item(sent, 0x3ff000, 1, 3);

  EXPECT_EQ(round_trip(sent), sent);
}

/* §5.2: CSRCs and an RFC 8285 header extension (X=1) follow the SSRC. */
TEST_F(St40RxFmdTest, CsrcAndExtensionPacketIsByteIdentical) {
  auto sent = zero_item_packet(0, kTs);
  sent[0] = 0x80 | 0x10 | 2; /* V=2, X=1, CC=2 */
  append_be32(sent, 0xa1a2a3a4);
  append_be32(sent, 0xb1b2b3b4);
  append_be32(sent, 0xbede0001); /* one-byte header extension, one word */
  append_be32(sent, 0x10aa0000);
  append_item(sent, kDit, 1, 2);

  EXPECT_EQ(round_trip(sent), sent);
}

TEST_F(St40RxFmdTest, WrongPayloadTypeIsDroppedAndCounted) {
  ut40_ctx_set_pt(ctx_, 115);
  auto item = rfc8331_lookalike_item(0x0);
  auto wrong = fmd_packet(0, kTs, item, 96);
  auto right = fmd_packet(1, kTs + 1, item, 115);

  EXPECT_EQ(ut40_feed_rtp_bytes(ctx_, wrong.data(), static_cast<uint16_t>(wrong.size()),
                                MTL_SESSION_PORT_P),
            -EINVAL);
  EXPECT_EQ(ut40_feed_rtp_bytes(ctx_, right.data(), static_cast<uint16_t>(right.size()),
                                MTL_SESSION_PORT_P),
            0);

  EXPECT_EQ(wrong_pt(), 1u);
  EXPECT_EQ(received(), 1u);
}

/* §5.2: the 16-bit seq has no extension; a gap counts the missing packets. */
TEST_F(St40RxFmdTest, SeqGapCountsLostPackets) {
  auto item = rfc8331_lookalike_item(0x0);

  ASSERT_EQ(feed_fmd(0xfffe, kTs, item, MTL_SESSION_PORT_P), 0);
  ASSERT_EQ(feed_fmd(0x0001, kTs + 1, item, MTL_SESSION_PORT_P), 0);

  EXPECT_EQ(port_ooo(MTL_SESSION_PORT_P), 2u);
  EXPECT_EQ(ooo(), 2u);
}

TEST_F(St40RxFmdTest, PayloadFBitsDoNotDriveInterlace) {
  ut40_ctx_set_interlace_auto(ctx_, true);

  ASSERT_EQ(feed_fmd(0, kTs, rfc8331_lookalike_item(0x2), MTL_SESSION_PORT_P), 0);
  ASSERT_EQ(feed_fmd(1, kTs + 1, rfc8331_lookalike_item(0x3), MTL_SESSION_PORT_P), 0);

  EXPECT_EQ(interlace_first(), 0u);
  EXPECT_EQ(interlace_second(), 0u);
  EXPECT_FALSE(ut40_ctx_interlaced(ctx_));
}

TEST_F(St40RxFmdTest, PayloadFBitsDoNotCountCrossPortMismatch) {
  feed_fmd(0, kTs, rfc8331_lookalike_item(0x2), MTL_SESSION_PORT_P);
  feed_fmd(0, kTs, rfc8331_lookalike_item(0x3), MTL_SESSION_PORT_R);

  EXPECT_EQ(field_bit_mismatch(), 0u);
}

TEST_F(St40RxFmdTest, DuplicateOnRedundantPortIsFiltered) {
  auto item = rfc8331_lookalike_item(0x0);

  EXPECT_EQ(feed_fmd(0, kTs, item, MTL_SESSION_PORT_P), 0);
  EXPECT_EQ(feed_fmd(0, kTs, item, MTL_SESSION_PORT_R), -EAGAIN);

  EXPECT_EQ(redundant(), 1u);
  EXPECT_EQ(received(), 1u);
  EXPECT_EQ(unrecovered(), 0u);
}

TEST_F(St40RxFmdTest, OpsCheckAcceptsRtpLevel) {
  EXPECT_EQ(ut40_ops_check(ST40_TYPE_RTP_LEVEL, ST40_RX_FLAG_FAST_METADATA), 0);
}

TEST_F(St40RxFmdTest, OpsCheckRejectsFrameLevel) {
  ASSERT_EQ(ut40_ops_check(ST40_TYPE_FRAME_LEVEL, 0), 0);

  EXPECT_EQ(ut40_ops_check(ST40_TYPE_FRAME_LEVEL, ST40_RX_FLAG_FAST_METADATA), -EINVAL);
}
