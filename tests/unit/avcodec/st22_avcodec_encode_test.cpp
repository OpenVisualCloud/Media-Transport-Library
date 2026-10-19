/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "avcodec/st22_avcodec_harness.h"

namespace {

constexpr size_t kBufferSize = 64;
constexpr size_t kGuardSize = 32;
constexpr uint8_t kGuardByte = 0xa5;

class St22AvcodecEncodeTest : public testing::Test {
 protected:
  void SetUp() override {
    ut_avcodec_reset();
    mem_.assign(kBufferSize + kGuardSize, kGuardByte);
    dst_ = {};
    dst_.addr[0] = mem_.data();
    dst_.fmt = ST_FRAME_FMT_H264_CODESTREAM;
    dst_.buffer_size = kBufferSize;
    dst_.data_size = kBufferSize;
  }

  int Encode(size_t pkt_size) {
    pkt_.resize(pkt_size);
    for (size_t i = 0; i < pkt_size; i++) pkt_[i] = static_cast<uint8_t>(i % kGuardByte);
    ut_avcodec_set_packet(pkt_.data(), static_cast<int>(pkt_size));
    return ut_avcodec_encode_frame(&dst_);
  }

  bool GuardsIntact() const {
    for (size_t i = kBufferSize; i < mem_.size(); i++)
      if (mem_[i] != kGuardByte) return false;
    return true;
  }

  std::vector<uint8_t> Copied(size_t size) const {
    return std::vector<uint8_t>(mem_.data(), mem_.data() + size);
  }

  std::vector<uint8_t> mem_;
  std::vector<uint8_t> pkt_;
  st_frame dst_;
};

TEST_F(St22AvcodecEncodeTest, RejectsAPacketLargerThanTheCodestreamBuffer) {
  EXPECT_LT(Encode(kBufferSize + 1), 0);
  EXPECT_EQ(dst_.data_size, 0u);
  EXPECT_TRUE(GuardsIntact());
  EXPECT_EQ(ut_avcodec_packet_refs(), 0);
}

TEST_F(St22AvcodecEncodeTest, AcceptsAPacketThatExactlyFillsTheCodestreamBuffer) {
  EXPECT_EQ(Encode(kBufferSize), 0);
  EXPECT_EQ(dst_.data_size, kBufferSize);
  EXPECT_EQ(Copied(kBufferSize), pkt_);
  EXPECT_TRUE(GuardsIntact());
}

TEST_F(St22AvcodecEncodeTest, CopiesAPacketSmallerThanTheCodestreamBuffer) {
  EXPECT_EQ(Encode(kBufferSize / 2), 0);
  EXPECT_EQ(dst_.data_size, kBufferSize / 2);
  EXPECT_EQ(Copied(kBufferSize / 2), pkt_);
}

}  // namespace
