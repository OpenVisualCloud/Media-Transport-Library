/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Proves that mt_mbuf_time_stamp() derives a correct RX hw-timestamp from
 * the generic RTE_ETH_RX_OFFLOAD_TIMESTAMP mbuf dynfield alone, even when
 * none of the ice IEEE1588-specific markers (packet_type L2_ETHER_TIMESYNC,
 * mb->timesync, RX_IEEE1588_PTP/_TMST ol_flags) are present — i.e. exactly
 * the state produced by unpatched upstream ice.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='PtpDynfieldTimestamp.*'
 */

#include <gtest/gtest.h>

#include "ptp/ptp_harness.h"

class PtpDynfieldTimestamp : public ::testing::Test {
 protected:
  ut_ptp_test_ctx* ctx_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(ut_ptp_init(), 0) << "EAL init failed";
    ctx_ = ut_ptp_ctx_create();
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut_ptp_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }
};

TEST_F(PtpDynfieldTimestamp, CorrectWithoutIeee1588Markers) {
  const uint64_t kTestNs = 123456789000ULL;

  void* mbuf = ut_ptp_alloc_mbuf(ctx_, kTestNs);
  ASSERT_NE(mbuf, nullptr);
  ASSERT_TRUE(ut_ptp_mbuf_is_unpatched_ice_state(mbuf));

  uint64_t ts = ut_ptp_mbuf_time_stamp(ctx_, mbuf);
  ASSERT_EQ(ts, kTestNs);

  ut_ptp_free_mbuf(mbuf);
}

TEST_F(PtpDynfieldTimestamp, ZeroWithoutOffloadTimestampFeature) {
  const uint64_t kTestNs = 123456789000ULL;

  ut_ptp_ctx_destroy(ctx_);
  ctx_ = ut_ptp_ctx_create_no_offload_timestamp();
  ASSERT_NE(ctx_, nullptr);

  void* mbuf = ut_ptp_alloc_mbuf(ctx_, kTestNs);
  ASSERT_NE(mbuf, nullptr);

  uint64_t ts = ut_ptp_mbuf_time_stamp(ctx_, mbuf);
  ASSERT_EQ(ts, 0u);

  ut_ptp_free_mbuf(mbuf);
}
