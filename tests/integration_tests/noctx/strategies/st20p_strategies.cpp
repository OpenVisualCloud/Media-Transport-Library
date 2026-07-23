/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st20p_strategies.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <utility>

#include "core/constants.hpp"
#include "handlers/st20p_handler.hpp"
#include "tests.hpp"

namespace {
/* Steady-state RX-timestamp-minus-RTP-timestamp latency measured on real HW
 * is ~8-13us (excluding frame 0 warm-up, see recordRlLatencySample()); 20us
 * leaves margin for run-to-run variance while still failing on a real
 * regression -- there is no outlier budget for this check. */
constexpr int64_t kRlLatencyExcessiveThresholdNs = 20 * NS_PER_US;
}  // namespace

St20pDefaultTimestamp::St20pDefaultTimestamp(St20pHandler* parentHandler)
    : FrameTestStrategy(parentHandler, false, true) {
}

void St20pDefaultTimestamp::rxTestFrameModifier(void* frame, size_t /*frame_size*/) {
  auto* f = static_cast<st_frame*>(frame);
  auto* st20pParent = static_cast<St20pHandler*>(parent);
  uint64_t framebuffTime =
      st10_tai_to_media_clk(st20pParent->nsFrameTime, VIDEO_CLOCK_HZ);

  EXPECT_NEAR(f->timestamp, framebuffTime * (idx_rx + 1), framebuffTime / 20)
      << " idx_rx: " << idx_rx;

  if (lastTimestamp != 0) {
    uint64_t diff = f->timestamp - lastTimestamp;
    EXPECT_TRUE(diff == framebuffTime) << " idx_rx: " << idx_rx << " diff: " << diff;
  }

  const uint64_t rtp_timestamp_ns =
      st10_media_clk_to_ns(static_cast<uint32_t>(f->timestamp), VIDEO_CLOCK_HZ);
  const int64_t rl_latency_ns =
      static_cast<int64_t>(f->receive_timestamp) - static_cast<int64_t>(rtp_timestamp_ns);
  recordRlLatencySample(idx_rx, rl_latency_ns);

  lastTimestamp = f->timestamp;
  idx_rx++;
}

void St20pDefaultTimestamp::recordRlLatencySample(uint64_t frame_idx,
                                                  int64_t latency_ns) {
  if (frame_idx == 0) {
    return;
  }

  rlLatencySampleCount++;
  rlLatencySumNs += latency_ns;

  if (latency_ns < 0) {
    rlLatencyNegativeCount++;
    if (latency_ns < rlLatencyWorstNegativeNs || rlLatencyNegativeCount == 1) {
      rlLatencyWorstNegativeNs = latency_ns;
      rlLatencyWorstNegativeFrameIdx = frame_idx;
    }
  } else if (latency_ns >= kRlLatencyExcessiveThresholdNs) {
    rlLatencyExcessiveCount++;
    if (latency_ns > rlLatencyWorstExcessiveNs || rlLatencyExcessiveCount == 1) {
      rlLatencyWorstExcessiveNs = latency_ns;
      rlLatencyWorstExcessiveFrameIdx = frame_idx;
    }
  }
}

void St20pDefaultTimestamp::assertRlLatencyWithinBounds() const {
  if (rlLatencySampleCount == 0) {
    return;
  }

  const int64_t average_latency_ns =
      rlLatencySumNs / static_cast<int64_t>(rlLatencySampleCount);

  EXPECT_EQ(rlLatencyNegativeCount, 0u)
      << rlLatencyNegativeCount << " of " << rlLatencySampleCount
      << " frames (excl. frame 0) had negative RX-timestamp-minus-RTP-timestamp "
         "latency (worst offender: frame "
      << rlLatencyWorstNegativeFrameIdx << ", latency_ns=" << rlLatencyWorstNegativeNs
      << ", average over all " << rlLatencySampleCount << " frames=" << average_latency_ns
      << "ns) -- a frame cannot be received before its own RTP timestamp";

  EXPECT_EQ(rlLatencyExcessiveCount, 0u)
      << rlLatencyExcessiveCount << " of " << rlLatencySampleCount
      << " frames (excl. frame 0) had RX-timestamp-minus-RTP-timestamp latency >= "
      << kRlLatencyExcessiveThresholdNs / static_cast<int64_t>(NS_PER_US)
      << "us (worst offender: frame " << rlLatencyWorstExcessiveFrameIdx
      << ", latency_ns=" << rlLatencyWorstExcessiveNs << ", average over all "
      << rlLatencySampleCount << " frames=" << average_latency_ns
      << "ns) -- RL warm-up gating should not delay transmission this far past target";
}

St20pUserTimestamp::St20pUserTimestamp(St20pHandler* parentHandler,
                                       std::vector<double> offsetMultipliers)
    : FrameTestStrategy(parentHandler, true, true),
      timestampOffsetMultipliers(std::move(offsetMultipliers)) {
  initializeTiming(parentHandler);
}

int St20pUserTimestamp::getPacingParameters() {
  auto* parentHandler = static_cast<St20pHandler*>(parent);
  if (parentHandler && parentHandler->sessionsHandleTx) {
    return st20p_tx_get_pacing_params(parentHandler->sessionsHandleTx,
                                      &pacing_tr_offset_ns, &pacing_trs_ns,
                                      &pacing_vrx_pkts);
  }

  return -1;
}

void St20pUserTimestamp::txTestFrameModifier(void* frame, size_t /*frame_size*/) {
  auto* f = static_cast<st_frame*>(frame);
  f->tfmt = ST10_TIMESTAMP_FMT_TAI;
  f->timestamp = plannedTimestampNs(idx_tx);
  idx_tx++;
}

void St20pUserTimestamp::rxTestFrameModifier(void* frame, size_t /*frame_size*/) {
  auto* f = static_cast<st_frame*>(frame);
  const uint64_t frame_idx = idx_rx++;

  const uint64_t expected_transmit_time_ns = expectedTransmitTimeNs(frame_idx);
  const uint64_t expected_media_clk =
      st10_tai_to_media_clk(expected_transmit_time_ns, VIDEO_CLOCK_HZ);

  verifyReceiveTiming(frame_idx, f->receive_timestamp, expected_transmit_time_ns);
  verifyMediaClock(frame_idx, f->timestamp, expected_media_clk);
  verifyTimestampStep(frame_idx, f->timestamp);

  const uint64_t rtp_timestamp_ns =
      st10_media_clk_to_ns(static_cast<uint32_t>(f->timestamp), VIDEO_CLOCK_HZ);
  const int64_t rl_latency_ns =
      static_cast<int64_t>(f->receive_timestamp) - static_cast<int64_t>(rtp_timestamp_ns);
  recordRlLatencySample(frame_idx, rl_latency_ns);

  lastTimestamp = f->timestamp;
}

uint64_t St20pUserTimestamp::plannedTimestampNs(uint64_t frame_idx) const {
  uint64_t base = plannedTimestampBaseNs(frame_idx);
  int64_t offset = frameTimeNs * offsetMultiplierForFrame(frame_idx);
  int64_t adjusted = base + offset;
  return adjusted < 0 ? 0 : (adjusted);
}

uint64_t St20pUserTimestamp::plannedTimestampBaseNs(uint64_t frame_idx) const {
  int64_t base = startingTime + frame_idx * frameTimeNs;
  return base < 0 ? 0 : base;
}

double St20pUserTimestamp::offsetMultiplierForFrame(uint64_t frame_idx) const {
  if (timestampOffsetMultipliers.empty()) {
    return 0;
  }

  size_t loop_idx = frame_idx % timestampOffsetMultipliers.size();
  return timestampOffsetMultipliers[loop_idx];
}

uint64_t St20pUserTimestamp::expectedTransmitTimeNs(uint64_t frame_idx) const {
  /* snap the requested TAI to the epoch the transmitter will pick */
  const double requested_ts = static_cast<double>(plannedTimestampNs(frame_idx));
  const double snapped_epoch =
      std::floor((requested_ts + frameTimeNs / 2.0) / frameTimeNs) * frameTimeNs;

  const double pacing_adjustment =
      pacing_tr_offset_ns - static_cast<double>(pacing_vrx_pkts) * pacing_trs_ns;

  const double expected = snapped_epoch + pacing_adjustment;
  return expected <= 0.0 ? 0 : static_cast<uint64_t>(expected);
}

void St20pUserTimestamp::verifyReceiveTiming(uint64_t frame_idx, uint64_t receive_time_ns,
                                             uint64_t expected_transmit_time_ns) {
  const int64_t delta_ns = static_cast<int64_t>(receive_time_ns) -
                           static_cast<int64_t>(expected_transmit_time_ns);
  /* Steady-state receive jitter measured on real HW is ~5-10us (excluding
   * frame 0 warm-up, absorbed by the outlier budget in
   * assertTimingWithinBudget()); 15us keeps headroom without masking a real
   * pacing regression. */
  const int64_t tolerance_ns = 15 * NS_PER_US;

  recordTimingSample(frame_idx, delta_ns, INT64_MIN, tolerance_ns);
}

void St20pUserTimestamp::recordTimingSample(uint64_t frame_idx, int64_t delta_ns,
                                            int64_t lower_bound_ns,
                                            int64_t upper_bound_ns) {
  /* Frame 0 carries one-time session/RL warm-up latency (consistently
   * elevated on real HW, see recordRlLatencySample()), not a pacing
   * regression; excluding it keeps the zero-tolerance check below
   * meaningful. */
  if (frame_idx == 0) {
    return;
  }

  timingSampleCount++;
  timingDeltaSumNs += delta_ns;

  const bool out_of_bounds = (delta_ns < lower_bound_ns) || (delta_ns > upper_bound_ns);
  if (out_of_bounds) {
    timingOutlierCount++;
    if (std::abs(delta_ns) > std::abs(timingWorstDeltaNs)) {
      timingWorstDeltaNs = delta_ns;
      timingWorstFrameIdx = frame_idx;
    }
  }
}

void St20pUserTimestamp::assertTimingWithinBudget() const {
  if (timingSampleCount == 0) {
    return;
  }

  const int64_t average_delta_ns =
      timingDeltaSumNs / static_cast<int64_t>(timingSampleCount);

  /* No outlier budget: any frame past frame 0 that breaches the pacing
   * timing bound fails the test. The average is always reported so a
   * failure caused by one or two bad frames is distinguishable at a glance
   * from a systemic regression (every frame off target). */
  EXPECT_EQ(timingOutlierCount, 0u)
      << timingOutlierCount << " of " << timingSampleCount
      << " frames (excl. frame 0) exceeded the pacing timing bound (worst "
         "offender: frame "
      << timingWorstFrameIdx << ", delta_ns=" << timingWorstDeltaNs
      << ", average over all " << timingSampleCount << " frames=" << average_delta_ns
      << "ns)";
}

void St20pUserTimestamp::verifyMediaClock(uint64_t frame_idx,
                                          uint64_t timestamp_media_clk,
                                          uint64_t expected_media_clk) const {
  EXPECT_EQ(timestamp_media_clk, expected_media_clk)
      << " idx_rx: " << frame_idx << "expected media clk: " << expected_media_clk
      << " received timestamp: " << timestamp_media_clk;
}

void St20pUserTimestamp::recordRlLatencySample(uint64_t frame_idx, int64_t latency_ns) {
  /* Frame 0 carries one-time session/RL warm-up latency (consistently
   * ~2-3x steady-state on real HW), not a pacing regression; excluding it
   * keeps the zero-tolerance check below meaningful. */
  if (frame_idx == 0) {
    return;
  }

  rlLatencySampleCount++;
  rlLatencySumNs += latency_ns;

  if (latency_ns < 0) {
    rlLatencyNegativeCount++;
    if (latency_ns < rlLatencyWorstNegativeNs || rlLatencyNegativeCount == 1) {
      rlLatencyWorstNegativeNs = latency_ns;
      rlLatencyWorstNegativeFrameIdx = frame_idx;
    }
  } else if (latency_ns >= kRlLatencyExcessiveThresholdNs) {
    rlLatencyExcessiveCount++;
    if (latency_ns > rlLatencyWorstExcessiveNs || rlLatencyExcessiveCount == 1) {
      rlLatencyWorstExcessiveNs = latency_ns;
      rlLatencyWorstExcessiveFrameIdx = frame_idx;
    }
  }
}

void St20pUserTimestamp::assertRlLatencyWithinBounds() const {
  if (rlLatencySampleCount == 0) {
    return;
  }

  const int64_t average_latency_ns =
      rlLatencySumNs / static_cast<int64_t>(rlLatencySampleCount);

  /* No outlier budget here (unlike assertTimingWithinBudget()): any frame
   * past frame 0 that breaches the bound fails the test. The average is
   * always reported so a failure caused by one or two bad frames is
   * distinguishable at a glance from a systemic regression. */
  EXPECT_EQ(rlLatencyNegativeCount, 0u)
      << rlLatencyNegativeCount << " of " << rlLatencySampleCount
      << " frames (excl. frame 0) had negative RX-timestamp-minus-RTP-timestamp "
         "latency (worst offender: frame "
      << rlLatencyWorstNegativeFrameIdx << ", latency_ns=" << rlLatencyWorstNegativeNs
      << ", average over all " << rlLatencySampleCount << " frames=" << average_latency_ns
      << "ns) -- a frame cannot be received before its own RTP timestamp";

  EXPECT_EQ(rlLatencyExcessiveCount, 0u)
      << rlLatencyExcessiveCount << " of " << rlLatencySampleCount
      << " frames (excl. frame 0) had RX-timestamp-minus-RTP-timestamp latency >= "
      << kRlLatencyExcessiveThresholdNs / static_cast<int64_t>(NS_PER_US)
      << "us (worst offender: frame " << rlLatencyWorstExcessiveFrameIdx
      << ", latency_ns=" << rlLatencyWorstExcessiveNs << ", average over all "
      << rlLatencySampleCount << " frames=" << average_latency_ns
      << "ns) -- RL warm-up gating should not delay transmission this far past target";
}

void St20pUserTimestamp::verifyTimestampStep(uint64_t frame_idx,
                                             uint64_t current_timestamp) {
  if (!lastTimestamp) {
    return;
  }

  double current_target = plannedTimestampBaseNs(frame_idx);
  double previous_target = plannedTimestampBaseNs(frame_idx ? frame_idx - 1 : 0);
  double expected_step_ns = current_target - previous_target;
  if (expected_step_ns < 0.0) {
    expected_step_ns = 0.0;
  }

  uint64_t expected_step_input = static_cast<uint64_t>(expected_step_ns);
  const uint64_t expected_step =
      st10_tai_to_media_clk(expected_step_input, VIDEO_CLOCK_HZ);
  const uint64_t diff = current_timestamp - lastTimestamp;
  EXPECT_EQ(diff, expected_step) << " idx_rx: " << frame_idx << " diff: " << diff;
}

void St20pUserTimestamp::initializeTiming(St20pHandler* handler) {
  if (!handler) {
    throw std::invalid_argument("St20pUserTimestamp expects a valid handler");
  }

  frameTimeNs = handler->nsFrameTime;

  if (!frameTimeNs) {
    double framerate = st_frame_rate(handler->sessionsOpsTx.fps);
    if (framerate > 0.0) {
      long double frame_time = static_cast<long double>(NS_PER_S) / framerate;
      frameTimeNs = static_cast<uint64_t>(frame_time + 0.5L);
    }
  }

  if (!frameTimeNs) {
    frameTimeNs = NS_PER_S / 25;
  }

  startingTime = frameTimeNs * 20;
}

St20pUserTimestampCustomStart::St20pUserTimestampCustomStart(
    St20pHandler* parentHandler, std::vector<double> offsetsNs,
    uint64_t customStartingTimeNs)
    : St20pUserTimestamp(parentHandler, std::move(offsetsNs)) {
  startingTime = customStartingTimeNs;
}

St20pRedundantLatency::St20pRedundantLatency(unsigned int latency,
                                             St20pHandler* parentHandler)
    : St20pUserTimestamp(parentHandler), latencyInMs(latency) {
  startingTime = (50 + latencyInMs) * NS_PER_MS;
}

void St20pRedundantLatency::rxTestFrameModifier(void* /*frame*/, size_t /*frame_size*/) {
  idx_rx++;
}

St20pExactUserPacing::St20pExactUserPacing(St20pHandler* parentHandler,
                                           std::vector<double> offsetMultipliers)
    : St20pUserTimestamp(parentHandler, std::move(offsetMultipliers)) {
}

uint64_t St20pExactUserPacing::expectedTransmitTimeNs(uint64_t frame_idx) const {
  return plannedTimestampNs(frame_idx);
}

void St20pExactUserPacing::verifyReceiveTiming(uint64_t frame_idx,
                                               uint64_t receive_time_ns,
                                               uint64_t expected_transmit_time_ns) {
  const int64_t delta_ns = static_cast<int64_t>(receive_time_ns) -
                           static_cast<int64_t>(expected_transmit_time_ns);
  /* Steady-state receive jitter measured on real HW is ~5-9.5us (excluding
   * frame 0 warm-up, absorbed by the outlier budget in
   * assertTimingWithinBudget()); 15us keeps headroom without masking a real
   * pacing regression. */
  const int64_t tolerance_ns = 15 * NS_PER_US;
  /* Exact mode's tv_sync_pacing() sets start_time_tai = required_tai verbatim
   * (st_tx_video_session.c) -- it never reads pacing->tr_offset or
   * pacing->vrx for the actual wall-clock schedule. RL pacing gates the
   * first real packet on its own target TSC (_video_trs_rl_tasklet() in
   * st_video_transmitter.c), so no early-arrival allowance is needed here. */
  recordTimingSample(frame_idx, delta_ns, /*lower_bound_ns=*/0, tolerance_ns);
}

void St20pExactUserPacing::verifyTimestampStep(uint64_t /*frame_idx*/,
                                               uint64_t /*current_timestamp*/) {
  /* Exact pacing uses user-provided deltas; no fixed increment enforced here. */
}
