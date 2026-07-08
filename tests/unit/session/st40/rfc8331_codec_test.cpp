/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * Pin st40_rfc8331_encode_packet() / st40_rfc8331_decode_packet() against
 * the RFC 8331 wire format, and the bswap helpers' host/network symmetry.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St40Rfc8331*.*'
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <vector>

#include "st40_api.h"

namespace {

constexpr uint32_t kBufRoom = 1024;

std::vector<uint8_t> make_udw(uint16_t n, uint8_t seed) {
  std::vector<uint8_t> v(n);
  for (uint16_t i = 0; i < n; i++) v[i] = static_cast<uint8_t>(seed + i);
  return v;
}

struct st40_meta make_meta(uint16_t did, uint16_t sdid, uint16_t line, uint16_t c,
                           uint16_t s, uint16_t stream, uint16_t udw_size) {
  struct st40_meta m {};
  m.c = c;
  m.line_number = line;
  m.hori_offset = 0x2CD;
  m.s = s;
  m.stream_num = stream;
  m.did = did;
  m.sdid = sdid;
  m.udw_size = udw_size;
  m.udw_offset = 0;
  return m;
}

}  // namespace

class St40Rfc8331CodecTest : public ::testing::Test {};

TEST_F(St40Rfc8331CodecTest, RoundTripMatrix) {
  struct Case {
    uint16_t did, sdid, line, c, s, stream, udw_size;
  };
  const Case cases[] = {
      {0x61, 0x02, 0, 0, 0, 0, 0},   {0x61, 0x02, 0x1AB, 1, 1, 7, 1},
      {0x41, 0x01, 260, 0, 1, 1, 8}, {0x08, 0x08, 0x1FF, 1, 0, 127, 9},
      {0x00, 0x00, 1, 1, 1, 1, 100}, {0xFF, 0xFF, 0x7FF, 1, 1, 0x7F, 255},
  };
  uint8_t seed = 0;
  for (const auto& tc : cases) {
    auto in = make_meta(tc.did, tc.sdid, tc.line, tc.c, tc.s, tc.stream, tc.udw_size);
    auto udw = make_udw(tc.udw_size, seed++);

    std::vector<uint8_t> buf(kBufRoom, 0);
    uint32_t written = 0;
    ASSERT_EQ(st40_rfc8331_encode_packet(buf.data(), kBufRoom, &in, udw.data(), &written),
              0)
        << "encode udw_size=" << tc.udw_size;
    EXPECT_EQ(written, st40_rfc8331_payload_bytes(tc.udw_size));

    struct st40_meta out {};
    std::vector<uint8_t> udw_out(tc.udw_size ? tc.udw_size : 1, 0xFF);
    uint32_t consumed = 0;
    ASSERT_EQ(
        st40_rfc8331_decode_packet(buf.data(), written, &out, udw_out.data(),
                                   static_cast<uint32_t>(udw_out.size()), &consumed),
        ST40_RFC8331_DECODE_OK)
        << "decode udw_size=" << tc.udw_size;
    EXPECT_EQ(consumed, written);
    EXPECT_EQ(out.c, in.c);
    EXPECT_EQ(out.line_number, in.line_number);
    EXPECT_EQ(out.hori_offset, in.hori_offset);
    EXPECT_EQ(out.s, in.s);
    EXPECT_EQ(out.stream_num, in.stream_num);
    EXPECT_EQ(out.did, in.did);
    EXPECT_EQ(out.sdid, in.sdid);
    EXPECT_EQ(out.udw_size, tc.udw_size);
    for (uint16_t i = 0; i < tc.udw_size; i++) {
      EXPECT_EQ(udw_out[i], udw[i]) << "udw mismatch sz=" << tc.udw_size << " i=" << i;
    }
  }
}

TEST_F(St40Rfc8331CodecTest, DecodeShortBufferHeader) {
  uint8_t tiny[3] = {0};
  struct st40_meta out {};
  uint32_t consumed = 0;
  EXPECT_EQ(st40_rfc8331_decode_packet(tiny, sizeof(tiny), &out, nullptr, 0, &consumed),
            ST40_RFC8331_DECODE_SHORT_BUFFER);
}

TEST_F(St40Rfc8331CodecTest, DecodeShortBufferBody) {
  auto in = make_meta(0x61, 0x02, 0x1AB, 1, 1, 7, 20);
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
  auto in = make_meta(0x61, 0x02, 0x1AB, 1, 1, 7, 10);
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
  auto in = make_meta(0x61, 0x02, 0x1AB, 1, 1, 7, 4);
  auto udw = make_udw(4, 4);
  std::vector<uint8_t> buf(kBufRoom, 0);
  uint32_t written = 0;
  ASSERT_EQ(st40_rfc8331_encode_packet(buf.data(), kBufRoom, &in, udw.data(), &written),
            0);
  /* Corrupt a DID/SDID/DC parity bit in the second header chunk (first 4 bytes
   * of the body). */
  buf[4] ^= 0x01;
  struct st40_meta out {};
  std::vector<uint8_t> udw_out(4, 0);
  uint32_t consumed = 0;
  EXPECT_EQ(
      st40_rfc8331_decode_packet(buf.data(), written, &out, udw_out.data(), 4, &consumed),
      ST40_RFC8331_DECODE_PARITY_FAIL);
}

TEST_F(St40Rfc8331CodecTest, DecodeChecksumFail) {
  auto in = make_meta(0x61, 0x02, 0x1AB, 1, 1, 7, 4);
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

/* RFC 8331 requires the checksum to be validated regardless of udw_size;
 * a udw_size=0 sub-packet with a corrupted checksum must still be rejected. */
TEST_F(St40Rfc8331CodecTest, DecodeChecksumFailZeroUdw) {
  auto in = make_meta(0x61, 0x02, 0x1AB, 1, 1, 7, 0);
  std::vector<uint8_t> buf(kBufRoom, 0);
  uint32_t written = 0;
  ASSERT_EQ(st40_rfc8331_encode_packet(buf.data(), kBufRoom, &in, nullptr, &written), 0);
  /* checksum word (idx=3) occupies body-relative bytes 3-4; corrupt the second byte. */
  buf[8] ^= 0x40;
  struct st40_meta out {};
  uint32_t consumed = 0;
  EXPECT_EQ(st40_rfc8331_decode_packet(buf.data(), written, &out, nullptr, 0, &consumed),
            ST40_RFC8331_DECODE_CHECKSUM_FAIL);
}

TEST_F(St40Rfc8331CodecTest, EncodeNoSpace) {
  auto in = make_meta(0x61, 0x02, 0x1AB, 1, 1, 7, 10);
  auto udw = make_udw(10, 6);
  uint8_t small[8] = {0};
  uint32_t written = 0xDEAD;
  EXPECT_EQ(st40_rfc8331_encode_packet(small, sizeof(small), &in, udw.data(), &written),
            -ENOSPC);
}

/* data_count is an 8-bit wire field; udw_size > 255 must be rejected rather
 * than silently wrapping in the encoded header. */
TEST_F(St40Rfc8331CodecTest, EncodeUdwSizeTooLarge) {
  auto in = make_meta(0x61, 0x02, 0x1AB, 1, 1, 7, 256);
  auto udw = make_udw(256, 7);
  std::vector<uint8_t> buf(kBufRoom, 0);
  uint32_t written = 0xDEAD;
  EXPECT_EQ(st40_rfc8331_encode_packet(buf.data(), kBufRoom, &in, udw.data(), &written),
            -EINVAL);
}

TEST_F(St40Rfc8331CodecTest, RtpHdrBswapRoundTrip) {
  struct st40_rfc8331_rtp_hdr hdr {};
  hdr.swapped_first_hdr_chunk = 0x12345678u;
  uint32_t orig = hdr.swapped_first_hdr_chunk;
  st40_rfc8331_rtp_hdr_bswap(&hdr);
  EXPECT_NE(hdr.swapped_first_hdr_chunk, orig);
  st40_rfc8331_rtp_hdr_bswap(&hdr);
  EXPECT_EQ(hdr.swapped_first_hdr_chunk, orig);
}

TEST_F(St40Rfc8331CodecTest, PayloadHdrBswapRoundTrip) {
  struct st40_rfc8331_payload_hdr hdr {};
  hdr.swapped_first_hdr_chunk = 0x12345678u;
  hdr.swapped_second_hdr_chunk = 0x9ABCDEF0u;
  uint32_t orig_first = hdr.swapped_first_hdr_chunk;
  uint32_t orig_second = hdr.swapped_second_hdr_chunk;
  st40_rfc8331_payload_hdr_bswap(&hdr);
  EXPECT_NE(hdr.swapped_first_hdr_chunk, orig_first);
  EXPECT_NE(hdr.swapped_second_hdr_chunk, orig_second);
  st40_rfc8331_payload_hdr_bswap(&hdr);
  EXPECT_EQ(hdr.swapped_first_hdr_chunk, orig_first);
  EXPECT_EQ(hdr.swapped_second_hdr_chunk, orig_second);
}
