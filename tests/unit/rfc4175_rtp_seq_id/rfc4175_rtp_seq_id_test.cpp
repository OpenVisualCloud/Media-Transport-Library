// SPDX-License-Identifier: BSD-3-Clause
// Copyright(c) 2026 Intel Corporation

#include <gtest/gtest.h>
#include <stdint.h>

extern "C" {
uint32_t test_seq_id(uint16_t base_seq, uint16_t ext_seq);
}

namespace {

TEST(Rfc4175RtpSeqId, BaseLandsInLowSixteenBits) {
  EXPECT_EQ(0x0000FFFFu, test_seq_id(0xFFFF, 0));
  EXPECT_EQ(0x00001234u, test_seq_id(0x1234, 0));
}

TEST(Rfc4175RtpSeqId, ExtLandsInHighSixteenBits) {
  EXPECT_EQ(0x00010000u, test_seq_id(0, 1));
  EXPECT_EQ(0xABCD0000u, test_seq_id(0, 0xABCD));
}

TEST(Rfc4175RtpSeqId, ExtAndBaseAreOrTogether) {
  EXPECT_EQ(0xABCD1234u, test_seq_id(0x1234, 0xABCD));
  EXPECT_EQ(0xFFFFFFFFu, test_seq_id(0xFFFF, 0xFFFF));
}

TEST(Rfc4175RtpSeqId, BaseWrapDoesNotBleedIntoExt) {
  // base == 0xFFFF, ext == 0 must produce 0x0000FFFF, not 0xFFFFFFFF.
  EXPECT_EQ(0x0000FFFFu, test_seq_id(0xFFFF, 0));
}

}  // namespace
