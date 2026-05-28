/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pin the wire-aligned byte size returned by st40_rfc8331_payload_bytes()
 * against hand-computed RFC 8331 §2.1 values. Any change to the helper's
 * arithmetic (10-bit packing, ceil-to-byte, 4-byte align, +hdr) breaks
 * wire compatibility with the TX/RX data path and the noctx test builder.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St40Rfc8331PayloadBytesTest.*'
 */

#include <gtest/gtest.h>

#include "st40_api.h"

class St40Rfc8331PayloadBytesTest : public ::testing::Test {};

/* Values computed by hand from (ceil((3+udw+1)*10 / 8) aligned to 4) + 4. */
TEST_F(St40Rfc8331PayloadBytesTest, MatchesRfc8331Formula) {
  EXPECT_EQ(st40_rfc8331_payload_bytes(0), 12u);
  EXPECT_EQ(st40_rfc8331_payload_bytes(1), 12u);
  EXPECT_EQ(st40_rfc8331_payload_bytes(8), 20u);
  EXPECT_EQ(st40_rfc8331_payload_bytes(9), 24u);
  EXPECT_EQ(st40_rfc8331_payload_bytes(100), 136u);
  EXPECT_EQ(st40_rfc8331_payload_bytes(255), 328u);
}
