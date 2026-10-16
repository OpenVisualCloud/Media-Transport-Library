/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "main/mt_pcap_harness.h"

namespace {
constexpr uint64_t kPtpNs = 1700000000123456789ULL;
}

class MtPcapTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ctx_ = ut_pcap_create(kPtpNs);
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut_pcap_destroy(ctx_);
  }

  ut_pcap_ctx* ctx_ = nullptr;
};

TEST_F(MtPcapTest, DumpWritesPtpTimeAsPacketTimestamp) {
  ASSERT_EQ(ut_pcap_open(ctx_), 0);
  ASSERT_EQ(ut_pcap_dump_one(ctx_), 1);

  uint64_t ts = 0;
  ASSERT_EQ(ut_pcap_close_and_read_ts(ctx_, &ts), 0);
  EXPECT_EQ(ts, kPtpNs);
}
