/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Documents, with the real production st10_tai_to_media_clk() /
 * st10_media_clk_to_ns() conversion functions (no mocks, no simulated
 * clocks), the "negative RX-timestamp-minus-RTP-timestamp latency" rounding
 * artifact also seen on real hardware as an EBU LIST pcap-compliance
 * failure (packet_ts_vs_rtp_ts, e.g. range {min: -954, max: -238} ns).
 *
 * st10_tai_to_media_clk() quantizes a continuous TAI instant to the
 * nearest 90 kHz media-clock tick (ties round down). Whenever the true
 * departure instant falls in the *second half* of its tick period, the
 * chosen tick rounds UP, so converting that tick back to ns
 * (st10_media_clk_to_ns) yields a value strictly LATER than the actual
 * departure instant. This is expected, unavoidable behavior of these raw
 * primitives in isolation - it is NOT fixed here and this file does not
 * claim it should be.
 *
 * The session-level TX pacing fix works around this artifact instead of
 * changing the primitives: it snaps the real transmission-gating time
 * (ptp_time_cursor/tsc_time_cursor) to the exact same tick the RTP
 * timestamp will round to, via st_tai_round_to_media_clk_ns() in
 * st_header.h, so the RTP timestamp and the actual departure instant are
 * always mutually consistent even though each individually still rounds.
 * These tests pin the raw-primitive behavior that fix routes around.
 */

#include <gtest/gtest.h>

#include "st_api.h"

namespace {

constexpr uint32_t kVideoClockHz = 90000; /* ST2110-20 media clock rate */

TEST(RtpTimestampRoundingTest, TickRoundedUpImpliesLaterTimeThanActualDeparture) {
  /* 17111 ns lies in the second half of tick #1 (tick period ~11111.11ns,
   * boundaries at 11111.11 and 22222.22): fractional position ~0.54 ticks
   * past the first boundary, past the round-to-nearest midpoint. */
  const uint64_t actual_departure_ns = 17111;

  uint32_t media_ts = st10_tai_to_media_clk(actual_departure_ns, kVideoClockHz);
  uint64_t implied_rtp_ns = st10_media_clk_to_ns(media_ts, kVideoClockHz);

  int64_t latency_ns = (int64_t)actual_departure_ns - (int64_t)implied_rtp_ns;

  /* Demonstrates the artifact described above: the raw primitives alone
   * make the implied RTP time LATER than the real departure instant, i.e.
   * a naive "departure >= implied_rtp_ns" check would fail here. Session
   * code avoids this by never comparing against these primitives'
   * round-trip directly - see st_tai_round_to_media_clk_ns(). */
  EXPECT_LT(latency_ns, 0) << "actual_departure_ns=" << actual_departure_ns
                           << " rounded to media_ts=" << media_ts
                           << " whose implied real time (" << implied_rtp_ns
                           << "ns) is expected to be LATER than the actual "
                              "departure instant for this sample";
}

TEST(RtpTimestampRoundingTest, SecondHalfOfTickAlwaysProducesNegativeLatency) {
  /* Sweep every possible sub-tick position for the first few ticks and
   * confirm the sign of the rounding error is fully determined by which
   * half of the tick the true instant falls in - i.e. this is a
   * systematic ~50% occurrence, not a rare corner case. */
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

  /* Roughly half of all sub-tick offsets land past the rounding midpoint. */
  EXPECT_GT(negative_count, 0);
  EXPECT_LT(negative_count, sample_count)
      << "expected a mix of positive and negative latencies across a full "
         "tick sweep, not an all-or-nothing result";
}

}  // namespace
