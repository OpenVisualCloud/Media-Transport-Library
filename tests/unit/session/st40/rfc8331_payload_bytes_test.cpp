/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pin the wire-aligned byte size returned by st40_rfc8331_payload_bytes()
 * against hand-computed RFC 8331 §2.1 values.
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

/* tests/tools/RxTxApp/src/{rx,tx}_ancillary_app.c's pre-fix formula: floor
 * division (missing the +7 before /8) then an align step that adds 4 bytes
 * even when already 4-byte aligned. Pinned here so this exact pattern is
 * never reintroduced: it diverges from the canonical helper (by 4 bytes,
 * always too large) whenever udw_size % 16 == 12. */
TEST_F(St40Rfc8331PayloadBytesTest, LegacyRxTxAppFormulaDivergedAtMod16Eq12) {
  for (int udw = 0; udw <= 255; udw++) {
    uint32_t total_size = (uint32_t)((3 + udw + 1) * 10) / 8;
    total_size = (4 - total_size % 4) + total_size;
    uint32_t legacy_len =
        (uint32_t)(sizeof(struct st40_rfc8331_payload_hdr) - 4) + total_size;
    uint32_t canonical_len = st40_rfc8331_payload_bytes((uint16_t)udw);
    if (udw % 16 == 12) {
      EXPECT_EQ(legacy_len, canonical_len + 4) << "udw_size=" << udw;
    } else {
      EXPECT_EQ(legacy_len, canonical_len) << "udw_size=" << udw;
    }
  }
}
