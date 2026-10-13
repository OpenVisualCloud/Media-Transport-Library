/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST40 RX in ST40_RX_FLAG_FAST_METADATA mode: SMPTE ST 2110-41 packets, zero-item
 * keepalives included, reach the app untouched and without Ethernet padding. Payload
 * bits that would read as RFC 8331 F bits must not drop packets, drive interlace
 * detection or count cross-port mismatches. FRAME_LEVEL delivers each Data Item
 * Package as its own frame.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St40RxFmd*'
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

std::vector<uint8_t> item_words(uint32_t words) {
  std::vector<uint8_t> contents;
  for (uint32_t i = 0; i < words; i++) append_be32(contents, 0xc0c1c2c3 + i);
  return contents;
}

void append_item(std::vector<uint8_t>& pkt, uint32_t dit, uint8_t k, uint32_t words) {
  append_be32(pkt, (dit << 10) | (static_cast<uint32_t>(k) << 9) | words);
  auto contents = item_words(words);
  pkt.insert(pkt.end(), contents.begin(), contents.end());
}

std::vector<uint8_t> one_item_packet(uint16_t seq, uint32_t ts) {
  auto pkt = zero_item_packet(seq, ts);
  append_item(pkt, kDit, 0, 1);
  return pkt;
}

/* `contents` zero-padded to a word, as a sender must. */
void append_contents(std::vector<uint8_t>& pkt, uint32_t dit, uint8_t k,
                     std::vector<uint8_t> contents) {
  contents.resize((contents.size() + 3) & ~size_t(3), 0);
  append_be32(pkt, (dit << 10) | (static_cast<uint32_t>(k) << 9) | (contents.size() / 4));
  pkt.insert(pkt.end(), contents.begin(), contents.end());
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

TEST_F(St40RxFmdTest, UdpLengthPastDataIsCappedByDataLen) {
  ut40_drain_paused guard;
  auto sent = one_item_packet(0, kTs);
  ASSERT_EQ(ut40_feed_rtp_bytes_udp_len(
                ctx_, sent.data(), static_cast<uint16_t>(sent.size()),
                static_cast<uint16_t>(sent.size() + 64), MTL_SESSION_PORT_P),
            0);

  std::vector<uint8_t> got(sent.size() + 128);
  int len = ut40_ring_dequeue_rtp(ctx_, got.data(), static_cast<uint16_t>(got.size()));

  EXPECT_EQ(len, static_cast<int>(sent.size()));
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

TEST_F(St40RxFmdTest, OpsCheckAcceptsFrameLevel) {
  EXPECT_EQ(ut40_ops_check(ST40_TYPE_FRAME_LEVEL, ST40_RX_FLAG_FAST_METADATA), 0);
}

class St40RxFmdFrameTest : public St40RxFmdTest {
 protected:
  static constexpr uint16_t kSlots = 4;
  static constexpr uint32_t kSlotSize = 256;

  void SetUp() override {
    St40RxFmdTest::SetUp();
    ut40_captured_reset();
    ut40_setup_frame_pool(ctx_, kSlots, kSlotSize);
  }

  void TearDown() override {
    ut40_teardown_frame_pool(ctx_);
    St40RxFmdTest::TearDown();
  }

  int feed_pkt(const std::vector<uint8_t>& pkt, uint16_t eth_pad = 0,
               enum mtl_session_port port = MTL_SESSION_PORT_P) {
    return ut40_feed_rtp_bytes_padded(ctx_, pkt.data(), static_cast<uint16_t>(pkt.size()),
                                      eth_pad, port);
  }

  std::vector<uint8_t> frame_bytes(int i) {
    std::vector<uint8_t> bytes(ut40_captured_udw_fill(i));
    for (uint32_t off = 0; off < bytes.size(); off++)
      bytes[off] = ut40_captured_udw_byte(i, off);
    return bytes;
  }

  uint64_t parse_err() {
    return ut40_stat_anc_pkt_parse_err(ctx_);
  }
  uint64_t frames_dropped() {
    return ut40_stat_anc_frames_dropped(ctx_);
  }

  struct item_seq {
    uint32_t dit;
    bool discont;
    uint32_t lost;
  };
  std::vector<item_seq> items_;

  /* Feed `pkt`, keep the seq stats of each item it delivers, and give the slot back
   * as an app does, so a long stream fits in the pool and the capture table. */
  int feed_released(const std::vector<uint8_t>& pkt,
                    enum mtl_session_port port = MTL_SESSION_PORT_P) {
    int ret = feed_pkt(pkt, 0, port);
    for (int i = 0; i < ut40_captured_count(); i++) {
      items_.push_back({ut40_captured_fmd_dit(i), ut40_captured_seq_discont(i),
                        ut40_captured_seq_lost(i)});
      ut40_release_frame(ctx_, ut40_captured_addr(i));
    }
    ut40_captured_reset();
    return ret;
  }

  /* One-item packet whose DIT is its seq, to tell the delivered items apart. */
  static std::vector<uint8_t> seq_item_packet(uint16_t seq, uint32_t ts) {
    auto pkt = zero_item_packet(seq, ts);
    append_item(pkt, seq, 0, 1);
    return pkt;
  }
};

/* §5.4: one item is one frame holding its contents, zero padding included. */
TEST_F(St40RxFmdFrameTest, SingleItemIsDeliveredAsOneFrame) {
  std::vector<uint8_t> contents = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  auto pkt = zero_item_packet(0, kTs);
  append_contents(pkt, kDit, 1, contents);

  ASSERT_EQ(feed_pkt(pkt), 0);

  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_fmd_dit(0), kDit);
  EXPECT_EQ(ut40_captured_fmd_k_bit(0), 1);
  EXPECT_EQ(ut40_captured_meta_num(0), 0);
  EXPECT_EQ(ut40_captured_rtp_ts(0), kTs);
  contents.resize(12, 0);
  EXPECT_EQ(frame_bytes(0), contents);
}

/* §5.1/§5.3: the items of one packet become frames in order, on its timestamp. */
TEST_F(St40RxFmdFrameTest, MultiItemPacketDeliversOneFramePerItemInOrder) {
  const uint32_t dits[] = {0x000001, 0x200002, 0x3ff000};
  const uint8_t k_bits[] = {0, 1, 0};
  const uint32_t words[] = {2, 1, 3};
  auto pkt = zero_item_packet(0, kTs);
  for (int i = 0; i < 3; i++) append_item(pkt, dits[i], k_bits[i], words[i]);

  ASSERT_EQ(feed_pkt(pkt), 0);

  ASSERT_EQ(ut40_captured_count(), 3);
  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(ut40_captured_fmd_dit(i), dits[i]) << "item " << i;
    EXPECT_EQ(ut40_captured_fmd_k_bit(i), k_bits[i]) << "item " << i;
    EXPECT_EQ(frame_bytes(i), item_words(words[i])) << "item " << i;
    EXPECT_EQ(ut40_captured_rtp_ts(i), kTs) << "item " << i;
  }
}

/* §5.1: a zero-item keepalive carries no data item, so no frame. */
TEST_F(St40RxFmdFrameTest, ZeroItemPacketDeliversNoFrame) {
  auto item_pkt = zero_item_packet(1, kTs + 1);
  append_item(item_pkt, kDit, 0, 1);

  ASSERT_EQ(feed_pkt(zero_item_packet(0, kTs)), 0);
  ASSERT_EQ(feed_pkt(item_pkt), 0);

  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_rtp_ts(0), kTs + 1);
  EXPECT_EQ(parse_err(), 0u);
}

/* The 54-byte zero-item frame is padded to 60 on the wire; the pad is not an item. */
TEST_F(St40RxFmdFrameTest, EthernetPaddingIsNotParsedAsItem) {
  auto item_pkt = zero_item_packet(1, kTs + 1);
  append_item(item_pkt, kDit, 0, 1);

  ASSERT_EQ(feed_pkt(zero_item_packet(0, kTs), 6), 0);
  ASSERT_EQ(feed_pkt(item_pkt), 0);

  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_rtp_ts(0), kTs + 1);
  EXPECT_EQ(parse_err(), 0u);
}

/* §5.4: a Data Item Length of 0 is illegal; the rest of the packet is not trusted. */
TEST_F(St40RxFmdFrameTest, ZeroLengthItemStopsParsingAndCountsParseError) {
  auto pkt = zero_item_packet(0, kTs);
  append_item(pkt, 0x000001, 0, 1);
  append_be32(pkt, kDit << 10);
  append_item(pkt, 0x000002, 0, 1);

  ASSERT_EQ(feed_pkt(pkt), 0);

  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_fmd_dit(0), 0x000001u);
  EXPECT_EQ(parse_err(), 1u);
}

TEST_F(St40RxFmdFrameTest, TrailingZeroLengthItemCountsParseError) {
  auto pkt = zero_item_packet(0, kTs);
  append_item(pkt, kDit, 0, 1);
  append_be32(pkt, kDit << 10);

  ASSERT_EQ(feed_pkt(pkt), 0);

  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(parse_err(), 1u);
}

/* §5.4: items are whole words, so a tail too short for an item header is malformed. */
TEST_F(St40RxFmdFrameTest, TrailingBytesShortOfItemHeaderCountParseError) {
  auto pkt = one_item_packet(0, kTs);
  pkt.push_back(0);
  pkt.push_back(0);

  ASSERT_EQ(feed_pkt(pkt), 0);

  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(parse_err(), 1u);
}

/* §5.4: an item must sit wholly inside the packet. */
TEST_F(St40RxFmdFrameTest, ItemOverrunningPacketCountsParseError) {
  auto pkt = zero_item_packet(0, kTs);
  append_item(pkt, 0x000001, 0, 1);
  append_item(pkt, 0x000002, 0, 3);
  pkt.resize(pkt.size() - 4);

  ASSERT_EQ(feed_pkt(pkt), 0);

  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_fmd_dit(0), 0x000001u);
  EXPECT_EQ(parse_err(), 1u);
}

/* §5.2: CSRCs and an RFC 8285 header extension sit between the SSRC and the items. */
TEST_F(St40RxFmdFrameTest, CsrcAndExtensionAreSkipped) {
  auto pkt = zero_item_packet(0, kTs);
  pkt[0] = 0x80 | 0x10 | 2; /* V=2, X=1, CC=2 */
  append_be32(pkt, 0xa1a2a3a4);
  append_be32(pkt, 0xb1b2b3b4);
  append_be32(pkt, 0xbede0001); /* one-byte header extension, one word */
  append_be32(pkt, 0x10aa0000);
  append_item(pkt, kDit, 1, 2);

  ASSERT_EQ(feed_pkt(pkt), 0);

  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_fmd_dit(0), kDit);
  EXPECT_EQ(ut40_captured_fmd_k_bit(0), 1);
  EXPECT_EQ(frame_bytes(0), item_words(2));
  EXPECT_EQ(parse_err(), 0u);
}

TEST_F(St40RxFmdFrameTest, ExtensionOverrunningPacketCountsParseError) {
  auto pkt = zero_item_packet(0, kTs);
  pkt[0] = 0x80 | 0x10;         /* V=2, X=1 */
  append_be32(pkt, 0xbede0201); /* 513 words; the low 9 bits read as a 1-word item */
  append_be32(pkt, 0x11223344);
  auto item_pkt = zero_item_packet(1, kTs + 1);
  append_item(item_pkt, kDit, 0, 1);

  ASSERT_EQ(feed_pkt(pkt), 0);
  ASSERT_EQ(feed_pkt(item_pkt), 0);

  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_rtp_ts(0), kTs + 1);
  EXPECT_EQ(parse_err(), 1u);
}

TEST_F(St40RxFmdFrameTest, TruncatedExtensionHeaderCountsParseError) {
  auto pkt = zero_item_packet(0, kTs);
  pkt[0] = 0x80 | 0x10; /* V=2, X=1 */
  pkt.push_back(0xbe);
  pkt.push_back(0xde);

  ASSERT_EQ(feed_pkt(pkt), 0);

  EXPECT_EQ(ut40_captured_count(), 0);
  EXPECT_EQ(parse_err(), 1u);
}

TEST_F(St40RxFmdFrameTest, CsrcListOverrunningPacketCountsParseError) {
  auto pkt = one_item_packet(0, kTs);
  pkt[0] = 0x80 | 15; /* V=2, CC=15, none present */

  ASSERT_EQ(feed_pkt(pkt), 0);

  EXPECT_EQ(ut40_captured_count(), 0);
  EXPECT_EQ(parse_err(), 1u);
}

TEST_F(St40RxFmdFrameTest, RtpPaddingCountOfZeroCountsParseError) {
  auto pkt = one_item_packet(0, kTs);
  pkt[0] |= 0x20; /* P=1 */
  append_be32(pkt, 0x00000100);

  ASSERT_EQ(feed_pkt(pkt), 0);

  EXPECT_EQ(ut40_captured_count(), 0);
  EXPECT_EQ(parse_err(), 1u);
}

TEST_F(St40RxFmdFrameTest, RtpPaddingPastPayloadCountsParseError) {
  auto pkt = one_item_packet(0, kTs);
  pkt[0] |= 0x20; /* P=1 */
  append_be32(pkt, 0x000000ff);

  ASSERT_EQ(feed_pkt(pkt), 0);

  EXPECT_EQ(ut40_captured_count(), 0);
  EXPECT_EQ(parse_err(), 1u);
}

/* Past data_len the harness writes words that parse as items. */
TEST_F(St40RxFmdFrameTest, UdpLengthPastDataIsCappedByDataLen) {
  auto pkt = one_item_packet(0, kTs);

  ASSERT_EQ(ut40_feed_rtp_bytes_udp_len(
                ctx_, pkt.data(), static_cast<uint16_t>(pkt.size()),
                static_cast<uint16_t>(pkt.size() + 64), MTL_SESSION_PORT_P),
            0);

  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(frame_bytes(0), item_words(1));
  EXPECT_EQ(parse_err(), 0u);
}

/* RFC 3550 §5.1: with P set, the last octet counts the trailing octets to ignore. */
TEST_F(St40RxFmdFrameTest, RtpPaddingIsNotParsedAsItem) {
  auto pkt = zero_item_packet(0, kTs);
  pkt[0] |= 0x20; /* P=1 */
  append_item(pkt, kDit, 0, 1);
  append_be32(pkt, 0x00000004);

  ASSERT_EQ(feed_pkt(pkt), 0);

  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(frame_bytes(0), item_words(1));
  EXPECT_EQ(parse_err(), 0u);
}

/* RFC 3550 §5.1: the padding can take the whole payload, which then holds no item. */
TEST_F(St40RxFmdFrameTest, RtpPaddingFillingPayloadIsZeroItemPacket) {
  auto pkt = zero_item_packet(0, kTs);
  pkt[0] |= 0x20; /* P=1 */
  append_be32(pkt, 0x00000004);

  ASSERT_EQ(feed_pkt(pkt), 0);
  ASSERT_EQ(feed_pkt(one_item_packet(1, kTs + 1)), 0);

  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_FALSE(ut40_captured_seq_discont(0));
  EXPECT_EQ(parse_err(), 0u);
}

/* §5.4: the Data Item Length has 9 bits; the largest item, 359 words, fills a packet. */
TEST_F(St40RxFmdFrameTest, MaxDataItemUsesAllNineLengthBits) {
  constexpr uint32_t kMaxWords = 359;
  ut40_teardown_frame_pool(ctx_);
  ut40_setup_frame_pool(ctx_, kSlots, kMaxWords * 4);
  auto pkt = zero_item_packet(0, kTs);
  append_item(pkt, kDit, 1, kMaxWords);

  ASSERT_EQ(feed_pkt(pkt), 0);

  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_udw_fill(0), kMaxWords * 4);
  EXPECT_EQ(ut40_captured_fmd_dit(0), kDit);
  EXPECT_EQ(parse_err(), 0u);
}

TEST_F(St40RxFmdFrameTest, ItemLargerThanFramebuffIsDroppedAndNextDelivered) {
  auto pkt = zero_item_packet(0, kTs);
  append_item(pkt, 0x000001, 0, kSlotSize / 4 + 1);
  append_item(pkt, 0x000002, 0, kSlotSize / 4);

  ASSERT_EQ(feed_pkt(pkt), 0);

  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_fmd_dit(0), 0x000002u);
  EXPECT_EQ(ut40_captured_udw_fill(0), static_cast<uint32_t>(kSlotSize));
  EXPECT_EQ(frames_dropped(), 1u);
  EXPECT_EQ(parse_err(), 0u);
}

TEST_F(St40RxFmdFrameTest, PoolExhaustionCountsEachDroppedItem) {
  auto pkt = zero_item_packet(0, kTs);
  for (uint32_t i = 0; i < kSlots + 2; i++) append_item(pkt, i, 0, 1);

  ASSERT_EQ(feed_pkt(pkt), 0);

  EXPECT_EQ(ut40_captured_count(), static_cast<int>(kSlots));
  EXPECT_EQ(frames_dropped(), 2u);
  EXPECT_EQ(parse_err(), 0u);
}

TEST_F(St40RxFmdFrameTest, FramesReportTheirPacketStats) {
  auto pkt = zero_item_packet(7, kTs);
  append_item(pkt, kDit, 0, 1);
  append_item(pkt, kDit, 0, 1);

  ASSERT_EQ(feed_pkt(pkt), 0);

  ASSERT_EQ(ut40_captured_count(), 2);
  for (int i = 0; i < 2; i++) {
    EXPECT_EQ(ut40_captured_pkts_total(i), 1u) << "item " << i;
    EXPECT_EQ(ut40_captured_port_pkts_recv(i, MTL_SESSION_PORT_P), 1u) << "item " << i;
    EXPECT_EQ(ut40_captured_seq_lost(i), 0u) << "item " << i;
    EXPECT_EQ(ut40_captured_status(i), ST_FRAME_STATUS_COMPLETE) << "item " << i;
    EXPECT_FALSE(ut40_captured_marker(i)) << "item " << i;
    EXPECT_FALSE(ut40_captured_interlaced(i)) << "item " << i;
  }
}

/* timestamp_first_pkt is the receive time of the packet that carried the item. */
TEST_F(St40RxFmdFrameTest, FramesReportTheirPacketReceiveTime) {
  ASSERT_EQ(feed_pkt(one_item_packet(0, kTs)), 0);
  ASSERT_EQ(feed_pkt(one_item_packet(1, kTs + 1)), 0);

  ASSERT_EQ(ut40_captured_count(), 2);
  EXPECT_GT(ut40_captured_timestamp_first_pkt(0), 0u);
  EXPECT_LT(ut40_captured_timestamp_first_pkt(0), ut40_captured_timestamp_first_pkt(1));
}

/* §5.2 requires marker 0; a frame still reports what the wire carried. */
TEST_F(St40RxFmdFrameTest, MarkerReportsTheWireBit) {
  auto pkt = zero_item_packet(0, kTs);
  pkt[1] |= 0x80;
  append_item(pkt, kDit, 0, 1);

  ASSERT_EQ(feed_pkt(pkt), 0);

  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_fmd_dit(0), kDit);
  EXPECT_TRUE(ut40_captured_marker(0));
}

TEST_F(St40RxFmdFrameTest, RedundantCopyDeliversNoDuplicateFrame) {
  auto pkt = zero_item_packet(0, kTs);
  append_item(pkt, kDit, 0, 1);

  ASSERT_EQ(feed_pkt(pkt, 0, MTL_SESSION_PORT_P), 0);
  EXPECT_EQ(feed_pkt(pkt, 0, MTL_SESSION_PORT_R), -EAGAIN);

  EXPECT_EQ(ut40_captured_count(), 1);
}

TEST_F(St40RxFmdFrameTest, ItemFromRedundantPortCountsThatPort) {
  ASSERT_EQ(feed_pkt(one_item_packet(0, kTs), 0, MTL_SESSION_PORT_R), 0);

  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_port_pkts_recv(0, MTL_SESSION_PORT_R), 1u);
  EXPECT_EQ(ut40_captured_port_pkts_recv(0, MTL_SESSION_PORT_P), 0u);
}

TEST_F(St40RxFmdFrameTest, FirstPacketIsNotADiscontinuity) {
  ASSERT_EQ(feed_pkt(one_item_packet(100, kTs)), 0);

  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_FALSE(ut40_captured_seq_discont(0));
  EXPECT_EQ(ut40_captured_seq_lost(0), 0u);
}

TEST_F(St40RxFmdFrameTest, ConsecutivePacketsAreNotADiscontinuity) {
  ASSERT_EQ(feed_pkt(one_item_packet(0xffff, kTs)), 0);
  ASSERT_EQ(feed_pkt(one_item_packet(0, kTs + 1)), 0);

  ASSERT_EQ(ut40_captured_count(), 2);
  EXPECT_FALSE(ut40_captured_seq_discont(1));
  EXPECT_EQ(ut40_captured_seq_lost(1), 0u);
}

/* A lost packet may have carried items; the item after the gap is itself intact. */
TEST_F(St40RxFmdFrameTest, SeqGapFlagsNextItemWithLostCount) {
  ASSERT_EQ(feed_pkt(one_item_packet(0, kTs)), 0);
  ASSERT_EQ(feed_pkt(one_item_packet(3, kTs + 1)), 0);

  ASSERT_EQ(ut40_captured_count(), 2);
  EXPECT_TRUE(ut40_captured_seq_discont(1));
  EXPECT_EQ(ut40_captured_seq_lost(1), 2u);
  EXPECT_EQ(ut40_captured_status(1), ST_FRAME_STATUS_COMPLETE);
}

/* §5.2: the 16-bit seq wraps; a gap across the wrap counts the same lost packets. */
TEST_F(St40RxFmdFrameTest, SeqGapAcrossWrapFlagsNextItemWithLostCount) {
  ASSERT_EQ(feed_pkt(one_item_packet(0xfffe, kTs)), 0);
  ASSERT_EQ(feed_pkt(one_item_packet(0x0001, kTs + 1)), 0);

  ASSERT_EQ(ut40_captured_count(), 2);
  EXPECT_TRUE(ut40_captured_seq_discont(1));
  EXPECT_EQ(ut40_captured_seq_lost(1), 2u);
}

/* In arrival order a late packet and the packet after it both break the sequence. */
TEST_F(St40RxFmdFrameTest, ReorderFlagsLatePacketAndItsSuccessor) {
  ASSERT_EQ(feed_pkt(one_item_packet(0, kTs)), 0);
  ASSERT_EQ(feed_pkt(one_item_packet(2, kTs + 1)), 0);
  ASSERT_EQ(feed_pkt(one_item_packet(1, kTs + 1)), 0);
  ASSERT_EQ(feed_pkt(one_item_packet(3, kTs + 2)), 0);

  ASSERT_EQ(ut40_captured_count(), 4);
  EXPECT_TRUE(ut40_captured_seq_discont(1));
  EXPECT_EQ(ut40_captured_seq_lost(1), 1u);
  EXPECT_TRUE(ut40_captured_seq_discont(2));
  EXPECT_EQ(ut40_captured_seq_lost(2), 0u);
  EXPECT_TRUE(ut40_captured_seq_discont(3));
}

TEST_F(St40RxFmdFrameTest, SeqGapFlagsOnlyFirstItemOfPacket) {
  ASSERT_EQ(feed_pkt(one_item_packet(0, kTs)), 0);
  auto pkt = zero_item_packet(2, kTs + 1);
  for (int i = 0; i < 3; i++) append_item(pkt, kDit, 0, 1);
  ASSERT_EQ(feed_pkt(pkt), 0);

  ASSERT_EQ(ut40_captured_count(), 4);
  EXPECT_TRUE(ut40_captured_seq_discont(1));
  EXPECT_EQ(ut40_captured_seq_lost(1), 1u);
  for (int i = 2; i < 4; i++) {
    EXPECT_FALSE(ut40_captured_seq_discont(i)) << "item " << i;
    EXPECT_EQ(ut40_captured_seq_lost(i), 0u) << "item " << i;
  }
}

/* A gap is reported on the next item even when the packet after it carries none. */
TEST_F(St40RxFmdFrameTest, SeqGapBeforeZeroItemPacketFlagsNextItem) {
  ASSERT_EQ(feed_pkt(one_item_packet(0, kTs)), 0);
  ASSERT_EQ(feed_pkt(zero_item_packet(2, kTs + 1)), 0);
  ASSERT_EQ(feed_pkt(one_item_packet(3, kTs + 2)), 0);

  ASSERT_EQ(ut40_captured_count(), 2);
  EXPECT_TRUE(ut40_captured_seq_discont(1));
  EXPECT_EQ(ut40_captured_seq_lost(1), 1u);
}

/* An item this receiver could not hold is a gap the app must see too. */
TEST_F(St40RxFmdFrameTest, LocallyDroppedItemFlagsNextItem) {
  auto pkt = zero_item_packet(0, kTs);
  append_item(pkt, 0x000001, 0, 1);
  append_item(pkt, 0x000002, 0, kSlotSize / 4 + 1);
  append_item(pkt, 0x000003, 0, 1);

  ASSERT_EQ(feed_pkt(pkt), 0);

  ASSERT_EQ(ut40_captured_count(), 2);
  EXPECT_FALSE(ut40_captured_seq_discont(0));
  EXPECT_TRUE(ut40_captured_seq_discont(1));
  EXPECT_EQ(ut40_captured_seq_lost(1), 0u);
}

TEST_F(St40RxFmdFrameTest, MalformedPacketFlagsNextItem) {
  auto bad = zero_item_packet(0, kTs);
  append_be32(bad, kDit << 10); /* Data Item Length 0 */

  ASSERT_EQ(feed_pkt(bad), 0);
  ASSERT_EQ(feed_pkt(one_item_packet(1, kTs + 1)), 0);

  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_TRUE(ut40_captured_seq_discont(0));
  EXPECT_EQ(ut40_captured_seq_lost(0), 0u);
}

/* A malformed packet arrived, so it breaks the stream but is not a lost packet. */
TEST_F(St40RxFmdFrameTest, MalformedHeaderPacketIsNotCountedLost) {
  auto bad = one_item_packet(1, kTs + 1);
  bad[0] = 0x80 | 15; /* V=2, CC=15, none present */

  ASSERT_EQ(feed_pkt(one_item_packet(0, kTs)), 0);
  ASSERT_EQ(feed_pkt(bad), 0);
  ASSERT_EQ(feed_pkt(one_item_packet(2, kTs + 2)), 0);

  ASSERT_EQ(ut40_captured_count(), 2);
  EXPECT_TRUE(ut40_captured_seq_discont(1));
  EXPECT_EQ(ut40_captured_seq_lost(1), 0u);
}

/* Items the app rejected never reached it: their slots come back, the next is flagged. */
TEST_F(St40RxFmdFrameTest, RejectedItemsFreeTheirSlotsAndFlagNextItem) {
  auto pkt = zero_item_packet(0, kTs);
  for (uint32_t i = 0; i < kSlots + 2; i++) append_item(pkt, kDit, 0, 1);

  ut40_set_notify_frame_fail_after(1);
  ASSERT_EQ(feed_pkt(pkt), 0);
  ut40_set_notify_frame_fail_after(-1);
  ASSERT_EQ(feed_pkt(one_item_packet(1, kTs + 1)), 0);

  ASSERT_EQ(ut40_captured_count(), 2);
  EXPECT_FALSE(ut40_captured_seq_discont(0));
  EXPECT_TRUE(ut40_captured_seq_discont(1));
  EXPECT_EQ(frames_dropped(), 0u);
}

/* A new source is a new seq space: its first item is not a discontinuity. */
TEST_F(St40RxFmdFrameTest, SourceUpdateRestartsSeqTracking) {
  ASSERT_EQ(feed_pkt(one_item_packet(100, kTs)), 0);
  ASSERT_EQ(ut40_update_source(ctx_), 0);
  ASSERT_EQ(feed_pkt(one_item_packet(5, kTs + 1)), 0);

  ASSERT_EQ(ut40_captured_count(), 2);
  EXPECT_FALSE(ut40_captured_seq_discont(1));
  EXPECT_EQ(ut40_captured_seq_lost(1), 0u);
}

/* A gap of the old source that no item reported yet is not reported on the new one. */
TEST_F(St40RxFmdFrameTest, SourceUpdateDropsPendingGap) {
  ASSERT_EQ(feed_pkt(one_item_packet(0, kTs)), 0);
  ASSERT_EQ(feed_pkt(zero_item_packet(3, kTs + 1)), 0);
  ASSERT_EQ(ut40_update_source(ctx_), 0);
  ASSERT_EQ(feed_pkt(one_item_packet(5, kTs + 2)), 0);

  ASSERT_EQ(ut40_captured_count(), 2);
  EXPECT_FALSE(ut40_captured_seq_discont(1));
  EXPECT_EQ(ut40_captured_seq_lost(1), 0u);
}

/* RFC 3550 §5.1: an RTP packet holds at least the 12-byte fixed header. */
TEST_F(St40RxFmdTest, DatagramShorterThanRtpHeaderIsDropped) {
  ut40_drain_paused guard;
  auto pkt = zero_item_packet(0, kTs);

  /* 8 RTP bytes, the Ethernet frame padded to 60 bytes */
  EXPECT_LT(ut40_feed_rtp_bytes_padded(ctx_, pkt.data(), 8, 10, MTL_SESSION_PORT_P), 0);

  EXPECT_EQ(received(), 0u);
}

/* §5.3: the item's document defines its timestamp; the next seq is a new packet. */
TEST_F(St40RxFmdTest, NextSeqWithOlderTimestampIsDelivered) {
  auto item = rfc8331_lookalike_item(0x0);

  EXPECT_EQ(feed_fmd(0, kTs + 100, item, MTL_SESSION_PORT_P), 0);
  EXPECT_EQ(feed_fmd(1, kTs, item, MTL_SESSION_PORT_P), 0);

  EXPECT_EQ(received(), 2u);
  EXPECT_EQ(redundant(), 0u);
}

/* ST 2022-7: R fills the packet P lost, also when R is one packet behind P. */
TEST_F(St40RxFmdFrameTest, RedundantPortFillsGapAfterNextPrimaryPacket) {
  feed_pkt(one_item_packet(0, kTs), 0, MTL_SESSION_PORT_P);
  feed_pkt(one_item_packet(2, kTs + 2), 0, MTL_SESSION_PORT_P);
  feed_pkt(one_item_packet(0, kTs), 0, MTL_SESSION_PORT_R);
  feed_pkt(one_item_packet(1, kTs + 1), 0, MTL_SESSION_PORT_R);

  EXPECT_EQ(ut40_captured_count(), 3);
}

/* No packet is lost, so the item after one with an older timestamp is no break. */
TEST_F(St40RxFmdFrameTest, NextSeqWithOlderTimestampIsNoLoss) {
  feed_pkt(one_item_packet(0, kTs + 100));
  feed_pkt(one_item_packet(1, kTs));
  feed_pkt(one_item_packet(2, kTs + 200));

  int last = ut40_captured_count() - 1;
  EXPECT_EQ(ut40_captured_count(), 3);
  EXPECT_FALSE(ut40_captured_seq_discont(last));
  EXPECT_EQ(ut40_captured_seq_lost(last), 0u);
}

/* RFC 3550 §5.1: a restarted sender takes a new random seq, here below the last one.
 * Whether the lib delivers or drops the items of the new run, the first item the app
 * gets after the restart must report the break. The R leg of the session is down. */
TEST_F(St40RxFmdFrameTest, SeqRestartBelowLastSeqFlagsNextDeliveredItem) {
  ASSERT_EQ(feed_released(seq_item_packet(1000, kTs)), 0);
  ASSERT_EQ(feed_released(seq_item_packet(1001, kTs + 1)), 0);
  for (uint16_t seq = 900; seq <= 1002; seq++)
    feed_released(seq_item_packet(seq, kTs + 100 + seq));

  ASSERT_GE(items_.size(), 3u);
  EXPECT_TRUE(items_[2].discont)
      << "first item after the restart is seq " << items_[2].dit << ", " << items_.size()
      << " items delivered of 105";
}

/* The same restart on a single-port session: no packet of the new run is lost. */
TEST_F(St40RxFmdFrameTest, SeqRestartBelowLastSeqSinglePortReportsNoLoss) {
  ut40_teardown_frame_pool(ctx_);
  ut40_ctx_destroy(ctx_);
  ctx_ = ut40_ctx_create(1);
  ASSERT_NE(ctx_, nullptr);
  ut40_ctx_set_fmd(ctx_);
  ut40_setup_frame_pool(ctx_, kSlots, kSlotSize);

  ASSERT_EQ(feed_released(seq_item_packet(1000, kTs)), 0);
  ASSERT_EQ(feed_released(seq_item_packet(1001, kTs + 1)), 0);
  for (uint16_t seq = 900; seq < 960; seq++)
    feed_released(seq_item_packet(seq, kTs + 100 + seq));

  uint32_t lost = 0;
  for (const auto& item : items_) lost += item.lost;
  EXPECT_EQ(lost, 0u) << items_.size() << " items delivered of 62";
}

/* In arrival order packet 1 is late; it comes outside the timestamp window that lets a
 * late packet in, so the lib drops it. Either way the next item reports a break. */
TEST_F(St40RxFmdFrameTest, LatePacketOutsideWindowFlagsNextItem) {
  ASSERT_EQ(feed_pkt(one_item_packet(0, kTs)), 0);
  ASSERT_EQ(feed_pkt(one_item_packet(2, kTs + 2)), 0);
  ASSERT_EQ(feed_pkt(one_item_packet(3, kTs + 3)), 0);
  feed_pkt(one_item_packet(1, kTs + 1));
  ASSERT_EQ(feed_pkt(one_item_packet(4, kTs + 4)), 0);

  int last = ut40_captured_count() - 1;
  ASSERT_GE(last, 0);
  EXPECT_EQ(ut40_captured_fmd_dit(last), kDit);
  EXPECT_TRUE(ut40_captured_seq_discont(last));
}

/* seq_lost is a uint32_t sum; past UINT32_MAX lost packets it must not wrap to a few. */
TEST_F(St40RxFmdFrameTest, SeqLostDoesNotWrapPastUint32Max) {
  const uint32_t jumps = 131081; /* 131081 * 32766 > UINT32_MAX */
  uint16_t seq = 0;
  ASSERT_EQ(feed_pkt(one_item_packet(seq, kTs)), 0);
  ut40_set_notify_frame_fail_after(0);
  for (uint32_t i = 1; i <= jumps; i++) {
    seq = static_cast<uint16_t>(seq + 32767); /* the largest forward gap, 32766 */
    feed_pkt(one_item_packet(seq, kTs + i));
  }
  ut40_set_notify_frame_fail_after(-1);
  ut40_captured_reset();
  ASSERT_EQ(feed_pkt(one_item_packet(static_cast<uint16_t>(seq + 1), kTs + jumps + 1)),
            0);

  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_TRUE(ut40_captured_seq_discont(0));
  EXPECT_EQ(ut40_captured_seq_lost(0), UINT32_MAX);
}

/* ST 2022-7: the R copy of each packet is not a break in the stream. */
TEST_F(St40RxFmdFrameTest, RedundantCopyIsNotADiscontinuity) {
  auto p0 = one_item_packet(0, kTs);
  auto p1 = one_item_packet(1, kTs + 1);

  ASSERT_EQ(feed_pkt(p0, 0, MTL_SESSION_PORT_P), 0);
  feed_pkt(p0, 0, MTL_SESSION_PORT_R);
  ASSERT_EQ(feed_pkt(p1, 0, MTL_SESSION_PORT_P), 0);
  feed_pkt(p1, 0, MTL_SESSION_PORT_R);

  ASSERT_EQ(ut40_captured_count(), 2);
  EXPECT_FALSE(ut40_captured_seq_discont(1));
  EXPECT_EQ(ut40_captured_seq_lost(1), 0u);
}

/* The gap before a dropped item is still reported on the next delivered item. */
TEST_F(St40RxFmdFrameTest, SeqGapIsReportedPastADroppedItem) {
  ASSERT_EQ(feed_pkt(one_item_packet(0, kTs)), 0);
  auto pkt = zero_item_packet(3, kTs + 1);
  append_item(pkt, 0x000001, 0, kSlotSize / 4 + 1);
  append_item(pkt, 0x000002, 0, 1);
  ASSERT_EQ(feed_pkt(pkt), 0);

  ASSERT_EQ(ut40_captured_count(), 2);
  EXPECT_EQ(ut40_captured_fmd_dit(1), 0x000002u);
  EXPECT_TRUE(ut40_captured_seq_discont(1));
  EXPECT_EQ(ut40_captured_seq_lost(1), 2u);
}

TEST_F(St40RxFmdFrameTest, NoFreeFrameFlagsNextItem) {
  auto pkt = zero_item_packet(0, kTs);
  for (uint32_t i = 0; i < kSlots + 1; i++) append_item(pkt, i, 0, 1);
  ASSERT_EQ(feed_released(pkt), 0);
  ASSERT_EQ(feed_released(one_item_packet(1, kTs + 1)), 0);

  ASSERT_EQ(items_.size(), kSlots + 1u);
  EXPECT_TRUE(items_[kSlots].discont);
  EXPECT_EQ(items_[kSlots].lost, 0u);
}

/* ST 2022-7: the legs take turns to lead; each item reaches the app once, in order. */
TEST_F(St40RxFmdFrameTest, AlternatingLegsDeliverEachItemOnceInOrder) {
  ASSERT_EQ(feed_pkt(one_item_packet(0, kTs), 0, MTL_SESSION_PORT_P), 0);
  feed_pkt(one_item_packet(0, kTs), 0, MTL_SESSION_PORT_R);
  ASSERT_EQ(feed_pkt(one_item_packet(1, kTs + 1), 0, MTL_SESSION_PORT_R), 0);
  feed_pkt(one_item_packet(1, kTs + 1), 0, MTL_SESSION_PORT_P);
  ASSERT_EQ(feed_pkt(one_item_packet(2, kTs + 2), 0, MTL_SESSION_PORT_P), 0);
  feed_pkt(one_item_packet(2, kTs + 2), 0, MTL_SESSION_PORT_R);

  ASSERT_EQ(ut40_captured_count(), 3);
  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(ut40_captured_rtp_ts(i), kTs + i) << "item " << i;
    EXPECT_FALSE(ut40_captured_seq_discont(i)) << "item " << i;
  }
}

/* ST 2022-7: R lags by a timestamp; its copy fills the one hole P left, nothing twice. */
TEST_F(St40RxFmdFrameTest, LaggingLegFillsHoleInPreviousTimestampOnce) {
  ASSERT_EQ(feed_pkt(one_item_packet(10, kTs), 0, MTL_SESSION_PORT_P), 0);
  ASSERT_EQ(feed_pkt(one_item_packet(12, kTs), 0, MTL_SESSION_PORT_P), 0);
  ASSERT_EQ(feed_pkt(one_item_packet(13, kTs + 1), 0, MTL_SESSION_PORT_P), 0);
  for (uint16_t seq = 10; seq < 13; seq++)
    feed_pkt(one_item_packet(seq, kTs), 0, MTL_SESSION_PORT_R);
  feed_pkt(one_item_packet(13, kTs + 1), 0, MTL_SESSION_PORT_R);

  ASSERT_EQ(ut40_captured_count(), 4);
  EXPECT_EQ(ut40_captured_rtp_ts(3), kTs);
}

/* A held frame stays the app's across a source update and comes back on put. */
TEST_F(St40RxFmdFrameTest, FrameHeldAcrossSourceUpdateIsReusedAfterPut) {
  for (uint16_t seq = 0; seq < kSlots; seq++)
    ASSERT_EQ(feed_pkt(one_item_packet(seq, kTs + seq)), 0);
  ASSERT_EQ(ut40_update_source(ctx_), 0);
  ASSERT_EQ(feed_pkt(one_item_packet(100, kTs + 100)), 0);
  ASSERT_EQ(ut40_captured_count(), static_cast<int>(kSlots));
  void* held = ut40_captured_addr(1);
  ut40_release_frame(ctx_, held);
  ASSERT_EQ(feed_pkt(one_item_packet(101, kTs + 101)), 0);

  ASSERT_EQ(ut40_captured_count(), static_cast<int>(kSlots) + 1);
  EXPECT_EQ(ut40_captured_addr(kSlots), held);
  EXPECT_EQ(ut40_captured_rtp_ts(kSlots), kTs + 101);
  EXPECT_TRUE(ut40_captured_seq_discont(kSlots));
}

/* A packet per frame has its own timestamp; the doc flags the late one too. */
TEST_F(St40RxFmdFrameTest, ReorderAcrossTimestampsFlagsLatePacket) {
  ASSERT_EQ(feed_pkt(one_item_packet(0, kTs)), 0);
  ASSERT_EQ(feed_pkt(one_item_packet(2, kTs + 2)), 0);
  EXPECT_EQ(feed_pkt(one_item_packet(1, kTs + 1)), 0);
  ASSERT_EQ(feed_pkt(one_item_packet(3, kTs + 3)), 0);

  ASSERT_EQ(ut40_captured_count(), 4);
  EXPECT_TRUE(ut40_captured_seq_discont(2));
  EXPECT_TRUE(ut40_captured_seq_discont(3));
}

/* seq_lost sums the forward gaps since the previous delivered item. */
TEST_F(St40RxFmdFrameTest, SeqLostSumsGapsAcrossZeroItemPackets) {
  ASSERT_EQ(feed_pkt(one_item_packet(0, kTs)), 0);
  ASSERT_EQ(feed_pkt(zero_item_packet(2, kTs + 1)), 0);
  ASSERT_EQ(feed_pkt(one_item_packet(5, kTs + 2)), 0);

  ASSERT_EQ(ut40_captured_count(), 2);
  EXPECT_TRUE(ut40_captured_seq_discont(1));
  EXPECT_EQ(ut40_captured_seq_lost(1), 3u);
}
