/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * timestamp_first_pkt sourcing: the RX video path must report the HW
 * RX-timestamp dynfield when the interface advertises RX timestamp
 * offload, not the software PTP clock.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St20RxTimestampSourceTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20/st20_rx_test_base.h"

class St20RxTimestampSourceTest : public St20RxBaseTest {
 protected:
  int num_port() const override {
    return 1;
  }
};

TEST_F(St20RxTimestampSourceTest, ReceiveTimestampSourcedFromHwOffload) {
  constexpr uint64_t kHwRawNs = 987654321000ull;
  ut20_ctx_enable_hw_timestamp(ctx_, MTL_SESSION_PORT_P);

  feed_full(1000, MTL_SESSION_PORT_P);
  uint64_t sw_only = ut20_last_timestamp_first_pkt(ctx_);
  ASSERT_EQ(frames_received(), 1);
  EXPECT_NE(sw_only, kHwRawNs) << "sanity: SW ptp stub must not already equal kHwRawNs";

  for (int i = 0; i < pkts_per_frame(); i++) {
    ut20_feed_frame_pkt_hw_ts(ctx_, i, 2000, MTL_SESSION_PORT_P, kHwRawNs);
  }

  EXPECT_EQ(frames_received(), 2);
  EXPECT_EQ(ut20_last_timestamp_first_pkt(ctx_), kHwRawNs)
      << "timestamp_first_pkt must come from the HW RX-timestamp dynfield "
         "(mt_mbuf_time_stamp), not the SW PTP clock fallback";
}

/* st20_api.h documents timestamp_first_pkt as "ST10_TIMESTAMP_FMT_TAI in ns,
 * PTP". On a no-timesync port -- any VF -- the software PTP correction is a
 * TSC-domain offset roughly the size of the TSC-to-TAI gap. If that leaks into
 * the HW RX timestamp the reported value is not TAI at all, which is what the
 * ST 2110-21 RX timing parser was tripping over. The case above cannot catch it:
 * its harness leaves every PTP accumulator at zero, so it passes whether or not
 * the offset is applied. Park a real correction and pin the contract. */
TEST_F(St20RxTimestampSourceTest, ReceiveTimestampUnpollutedBySoftwarePtpOffset) {
  constexpr uint64_t kHwRawNs = 987654321000ull;
  constexpr int64_t kTscToTaiGapNs = 850000000000000000ll;

  ut20_ctx_enable_hw_timestamp(ctx_, MTL_SESSION_PORT_P);
  ut20_ctx_set_ptp_no_timesync_delta(ctx_, kTscToTaiGapNs);

  for (int i = 0; i < pkts_per_frame(); i++) {
    ut20_feed_frame_pkt_hw_ts(ctx_, i, 1000, MTL_SESSION_PORT_P, kHwRawNs);
  }

  ASSERT_EQ(frames_received(), 1);
  EXPECT_EQ(ut20_last_timestamp_first_pkt(ctx_), kHwRawNs)
      << "a software PTP offset leaked into timestamp_first_pkt; the public "
         "value is documented as TAI and must be the NIC clock alone";
}
