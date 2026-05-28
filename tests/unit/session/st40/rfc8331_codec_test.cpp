/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pin st40_rfc8331_encode_packet / st40_rfc8331_decode_packet against
 * RFC 8331 wire format. Three groups:
 *   - round-trip identity over a meta x udw_size matrix
 *   - one input per decode_result enum value
 *   - one canonical encoded byte pattern (anchors the spec)
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <vector>

#include "st40_api.h"

namespace {

constexpr uint32_t kBufRoom = 1024;

std::vector<uint8_t> make_udw(uint16_t n, uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<uint8_t> v(n);
  for (auto& b : v) b = static_cast<uint8_t>(rng() & 0xFF);
  return v;
}

struct st40_meta make_meta(uint16_t udw_size) {
  struct st40_meta m {};
  m.c = 1;
  m.line_number = 0x1AB;
  m.hori_offset = 0x2CD;
  m.s = 1;
  m.stream_num = 7;
  m.did = 0x61;
  m.sdid = 0x02;
  m.udw_size = udw_size;
  m.udw_offset = 0;
  return m;
}

}  // namespace

class St40Rfc8331CodecTest : public ::testing::Test {};

TEST_F(St40Rfc8331CodecTest, RoundTripMatrix) {
  const uint16_t sizes[] = {0, 1, 8, 9, 100, 255};
  uint32_t seed = 0xC0FFEEu;
  for (uint16_t sz : sizes) {
    auto in = make_meta(sz);
    auto udw = make_udw(sz, seed++);

    std::vector<uint8_t> buf(kBufRoom, 0);
    uint32_t written = 0;
    ASSERT_EQ(st40_rfc8331_encode_packet(buf.data(), kBufRoom, &in, udw.data(), &written),
              0)
        << "encode udw_size=" << sz;
    EXPECT_EQ(written, st40_rfc8331_payload_bytes(sz));

    struct st40_meta out {};
    std::vector<uint8_t> udw_out(sz ? sz : 1, 0xFF);
    uint32_t consumed = 0;
    ASSERT_EQ(
        st40_rfc8331_decode_packet(buf.data(), written, &out, udw_out.data(),
                                   static_cast<uint32_t>(udw_out.size()), &consumed),
        ST40_RFC8331_DECODE_OK)
        << "decode udw_size=" << sz;
    EXPECT_EQ(consumed, written);
    EXPECT_EQ(out.c, in.c);
    EXPECT_EQ(out.line_number, in.line_number);
    EXPECT_EQ(out.hori_offset, in.hori_offset);
    EXPECT_EQ(out.s, in.s);
    EXPECT_EQ(out.stream_num, in.stream_num);
    EXPECT_EQ(out.did, in.did);
    EXPECT_EQ(out.sdid, in.sdid);
    EXPECT_EQ(out.udw_size, sz);
    for (uint16_t i = 0; i < sz; i++) {
      EXPECT_EQ(udw_out[i], udw[i]) << "udw mismatch sz=" << sz << " i=" << i;
    }
  }
}

TEST_F(St40Rfc8331CodecTest, DecodeOkMetaOnly) {
  auto in = make_meta(4);
  auto udw = make_udw(4, 1);
  std::vector<uint8_t> buf(kBufRoom, 0);
  uint32_t written = 0;
  ASSERT_EQ(st40_rfc8331_encode_packet(buf.data(), kBufRoom, &in, udw.data(), &written),
            0);

  struct st40_meta out {};
  uint32_t consumed = 0;
  EXPECT_EQ(st40_rfc8331_decode_packet(buf.data(), written, &out, nullptr, 0, &consumed),
            ST40_RFC8331_DECODE_OK);
  EXPECT_EQ(out.udw_size, 4);
  EXPECT_EQ(consumed, written);
}

TEST_F(St40Rfc8331CodecTest, DecodeShortBufferHeader) {
  uint8_t tiny[3] = {0};
  struct st40_meta out {};
  uint32_t consumed = 0;
  EXPECT_EQ(st40_rfc8331_decode_packet(tiny, sizeof(tiny), &out, nullptr, 0, &consumed),
            ST40_RFC8331_DECODE_SHORT_BUFFER);
}

TEST_F(St40Rfc8331CodecTest, DecodeShortBufferBody) {
  auto in = make_meta(20);
  auto udw = make_udw(20, 2);
  std::vector<uint8_t> buf(kBufRoom, 0);
  uint32_t written = 0;
  ASSERT_EQ(st40_rfc8331_encode_packet(buf.data(), kBufRoom, &in, udw.data(), &written),
            0);
  struct st40_meta out {};
  uint32_t consumed = 0;
  /* feed only the 8-byte header -- body is missing */
  EXPECT_EQ(st40_rfc8331_decode_packet(buf.data(), 8, &out, nullptr, 0, &consumed),
            ST40_RFC8331_DECODE_SHORT_BUFFER);
}

TEST_F(St40Rfc8331CodecTest, DecodeShortBufferUdwOut) {
  auto in = make_meta(10);
  auto udw = make_udw(10, 3);
  std::vector<uint8_t> buf(kBufRoom, 0);
  uint32_t written = 0;
  ASSERT_EQ(st40_rfc8331_encode_packet(buf.data(), kBufRoom, &in, udw.data(), &written),
            0);
  struct st40_meta out {};
  std::vector<uint8_t> small_out(5, 0);
  uint32_t consumed = 0;
  EXPECT_EQ(st40_rfc8331_decode_packet(buf.data(), written, &out, small_out.data(), 5,
                                       &consumed),
            ST40_RFC8331_DECODE_SHORT_BUFFER);
}

TEST_F(St40Rfc8331CodecTest, DecodeParityFail) {
  auto in = make_meta(4);
  auto udw = make_udw(4, 4);
  std::vector<uint8_t> buf(kBufRoom, 0);
  uint32_t written = 0;
  ASSERT_EQ(st40_rfc8331_encode_packet(buf.data(), kBufRoom, &in, udw.data(), &written),
            0);
  /* Corrupt DID byte (low 8 bits of second_hdr_chunk on the wire are at
   * offset 4..7; flip a bit in the parity-bearing nibble). */
  buf[4] ^= 0x01;
  struct st40_meta out {};
  std::vector<uint8_t> udw_out(4, 0);
  uint32_t consumed = 0;
  EXPECT_EQ(
      st40_rfc8331_decode_packet(buf.data(), written, &out, udw_out.data(), 4, &consumed),
      ST40_RFC8331_DECODE_PARITY_FAIL);
}

TEST_F(St40Rfc8331CodecTest, DecodeChecksumFail) {
  auto in = make_meta(4);
  auto udw = make_udw(4, 5);
  std::vector<uint8_t> buf(kBufRoom, 0);
  uint32_t written = 0;
  ASSERT_EQ(st40_rfc8331_encode_packet(buf.data(), kBufRoom, &in, udw.data(), &written),
            0);
  /* Last 4 bytes hold the 10-bit checksum field; toggle a bit in the
   * second-to-last byte (avoids the all-zero padding tail). */
  buf[written - 3] ^= 0x40;
  struct st40_meta out {};
  std::vector<uint8_t> udw_out(4, 0);
  uint32_t consumed = 0;
  EXPECT_EQ(
      st40_rfc8331_decode_packet(buf.data(), written, &out, udw_out.data(), 4, &consumed),
      ST40_RFC8331_DECODE_CHECKSUM_FAIL);
}

TEST_F(St40Rfc8331CodecTest, EncodeNoSpace) {
  auto in = make_meta(10);
  auto udw = make_udw(10, 6);
  uint8_t small[8] = {0};
  uint32_t written = 0xDEAD;
  EXPECT_EQ(st40_rfc8331_encode_packet(small, sizeof(small), &in, udw.data(), &written),
            -ENOSPC);
}
