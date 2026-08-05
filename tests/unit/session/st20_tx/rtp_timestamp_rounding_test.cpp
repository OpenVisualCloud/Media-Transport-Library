/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "st_api.h"

namespace {

constexpr uint32_t kVideoClockHz = 90000; /* ST2110-20 media clock rate */

TEST(RtpTimestampRoundingTest, TickRoundedUpImpliesLaterTimeThanActualDeparture) {
  /* 17111 ns lies beyond the midpoint of a 90 kHz tick. */
  const uint64_t actual_departure_ns = 17111;

  uint32_t media_ts = st10_tai_to_media_clk(actual_departure_ns, kVideoClockHz);
  uint64_t implied_rtp_ns = st10_media_clk_to_ns(media_ts, kVideoClockHz);

  int64_t latency_ns = (int64_t)actual_departure_ns - (int64_t)implied_rtp_ns;

  /* Rounding up makes the implied RTP time later than the actual instant. */
  EXPECT_LT(latency_ns, 0) << "actual_departure_ns=" << actual_departure_ns
                           << " rounded to media_ts=" << media_ts
                           << " whose implied real time (" << implied_rtp_ns
                           << "ns) is expected to be LATER than the actual "
                              "departure instant for this sample";
}

TEST(RtpTimestampRoundingTest, SecondHalfOfTickAlwaysProducesNegativeLatency) {
  /* The rounding-error sign changes across each tick midpoint. */
  const uint64_t tick_ns_num = 1000000000ull; /* NS_PER_S */
  const uint64_t tick_ns_den = kVideoClockHz;
  int negative_count = 0;
  int sample_count = 0;

  for (uint64_t tick_idx = 1; tick_idx <= 4; tick_idx++) {
    for (uint64_t offset = 0; offset < 11111; offset += 500) {
      uint64_t actual_ns = (tick_idx * tick_ns_num) / tick_ns_den + offset;

      uint32_t media_ts = st10_tai_to_media_clk(actual_ns, kVideoClockHz);
      uint64_t implied_ns = st10_media_clk_to_ns(media_ts, kVideoClockHz);
      int64_t latency_ns = (int64_t)actual_ns - (int64_t)implied_ns;

      sample_count++;
      if (latency_ns < 0) negative_count++;
    }
  }

  EXPECT_GT(negative_count, 0);
  EXPECT_LT(negative_count, sample_count)
      << "expected a mix of positive and negative latencies across a full "
         "tick sweep, not an all-or-nothing result";
}

}  // namespace
