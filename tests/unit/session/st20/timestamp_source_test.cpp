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
