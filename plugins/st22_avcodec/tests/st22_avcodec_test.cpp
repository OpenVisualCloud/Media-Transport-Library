/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "st22_avcodec_harness.h"

class St22AvcodecEncode : public ::testing::Test {
 protected:
  /* A new encoder codes the same first frame to the same size: take it from a buffer
   * the packet fits in. */
  void SetUp() override {
    std::vector<uint8_t> dst(1 << 16);
    ASSERT_EQ(ut_avcodec_encode_frame(dst.data(), dst.size(), &packet_size_), 0);
  }

  size_t packet_size_ = 0;
};

TEST_F(St22AvcodecEncode, CopiesAPacketThatExactlyFillsTheCodestreamBuffer) {
  std::vector<uint8_t> dst(packet_size_);
  size_t data_size;

  EXPECT_EQ(ut_avcodec_encode_frame(dst.data(), packet_size_, &data_size), 0);
  EXPECT_EQ(data_size, packet_size_);
  /* an Annex B access unit: a start code, and a stop bit in its last byte */
  EXPECT_EQ(std::vector<uint8_t>(dst.begin(), dst.begin() + 4),
            (std::vector<uint8_t>{0, 0, 0, 1}));
  EXPECT_NE(dst.back(), 0);
}

TEST_F(St22AvcodecEncode, RejectsAPacketOneByteLargerThanTheCodestreamBuffer) {
  /* dst has room for the whole packet, so a copy shows in it rather than past it */
  std::vector<uint8_t> dst(packet_size_);
  size_t data_size;

  EXPECT_EQ(ut_avcodec_encode_frame(dst.data(), packet_size_ - 1, &data_size), -EIO);
  EXPECT_EQ(data_size, 0u);
  EXPECT_EQ(dst, std::vector<uint8_t>(packet_size_));
}
