/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pin tx_ancillary_corrupt_parity() (st_tx_ancillary_test.h, exercised via
 * ut40_tx_corrupt_parity()): the BAD_PARITY fault-injection post-pass that
 * strips parity bits from an already-encoded RFC 8331 sub-packet and
 * recomputes its checksum, so decode fails on parity specifically, not
 * checksum.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St40TxCorruptParityTest.*'
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "session/st40_tx_test_harness.h"
#include "st40_api.h"

namespace {

constexpr uint32_t kBufRoom = 1024;

std::vector<uint8_t> make_udw(uint16_t n, uint8_t seed) {
  std::vector<uint8_t> v(n);
  for (uint16_t i = 0; i < n; i++) v[i] = static_cast<uint8_t>(seed + i);
  return v;
}

struct st40_meta make_meta(uint16_t did, uint16_t sdid, uint16_t udw_size) {
  struct st40_meta m {};
  m.did = did;
  m.sdid = sdid;
  m.udw_size = udw_size;
  return m;
}

}  // namespace

class St40TxCorruptParityTest : public ::testing::Test {};

TEST_F(St40TxCorruptParityTest, DecodeFailsOnParityNotChecksum) {
  auto meta = make_meta(0x61, 0x02, 8);
  auto udw = make_udw(8, 5);
  std::vector<uint8_t> buf(kBufRoom, 0);
  uint32_t written = 0;
  ASSERT_EQ(st40_rfc8331_encode_packet(buf.data(), kBufRoom, &meta, udw.data(), &written),
            0);

  ut40_tx_corrupt_parity(buf.data(), meta.udw_size);

  struct st40_meta out {};
  std::vector<uint8_t> udw_out(8, 0);
  uint32_t consumed = 0;
  EXPECT_EQ(st40_rfc8331_decode_packet(buf.data(), written, &out, udw_out.data(),
                                       static_cast<uint32_t>(udw_out.size()), &consumed),
            ST40_RFC8331_DECODE_PARITY_FAIL);
}

TEST_F(St40TxCorruptParityTest, ChecksumStaysInternallyConsistent) {
  auto meta = make_meta(0x41, 0x01, 4);
  auto udw = make_udw(4, 1);
  std::vector<uint8_t> buf(kBufRoom, 0);
  uint32_t written = 0;
  ASSERT_EQ(st40_rfc8331_encode_packet(buf.data(), kBufRoom, &meta, udw.data(), &written),
            0);

  ut40_tx_corrupt_parity(buf.data(), meta.udw_size);

  auto* ph = reinterpret_cast<struct st40_rfc8331_payload_hdr*>(buf.data());
  uint8_t* udw_dst = reinterpret_cast<uint8_t*>(&ph->second_hdr_chunk);
  uint16_t stored = st40_get_udw(3 + meta.udw_size, udw_dst);
  uint16_t recomputed = st40_calc_checksum(3 + meta.udw_size, udw_dst);
  EXPECT_EQ(stored, recomputed);
}

TEST_F(St40TxCorruptParityTest, ZeroUdwSizeStillCorrupted) {
  auto meta = make_meta(0x08, 0x08, 0);
  std::vector<uint8_t> buf(kBufRoom, 0);
  uint32_t written = 0;
  ASSERT_EQ(st40_rfc8331_encode_packet(buf.data(), kBufRoom, &meta, nullptr, &written),
            0);

  ut40_tx_corrupt_parity(buf.data(), meta.udw_size);

  struct st40_meta out {};
  uint32_t consumed = 0;
  EXPECT_EQ(st40_rfc8331_decode_packet(buf.data(), written, &out, nullptr, 0, &consumed),
            ST40_RFC8331_DECODE_PARITY_FAIL);
}
