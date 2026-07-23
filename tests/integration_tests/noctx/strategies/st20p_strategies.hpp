/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#pragma once

#include <cstdint>
#include <vector>

#include "core/strategy.hpp"

class St20pHandler;

class St20pDefaultTimestamp : public FrameTestStrategy {
 protected:
  uint64_t lastTimestamp = 0;

 public:
  explicit St20pDefaultTimestamp(St20pHandler* parentHandler = nullptr);
  void rxTestFrameModifier(void* frame, size_t frame_size) override;

  /* Same zero-tolerance RX-vs-RTP-timestamp latency check as
   * St20pUserTimestamp::assertRlLatencyWithinBounds() -- see that method's
   * docstring for the full rationale. */
  void assertRlLatencyWithinBounds() const;

 protected:
  /* Same accounting as St20pUserTimestamp::recordRlLatencySample(); frame 0
   * is excluded (session warm-up, not steady-state pacing). */
  void recordRlLatencySample(uint64_t frame_idx, int64_t latency_ns);

  uint64_t rlLatencySampleCount = 0;
  int64_t rlLatencySumNs = 0;
  uint64_t rlLatencyNegativeCount = 0;
  int64_t rlLatencyWorstNegativeNs = 0;
  uint64_t rlLatencyWorstNegativeFrameIdx = 0;
  uint64_t rlLatencyExcessiveCount = 0;
  int64_t rlLatencyWorstExcessiveNs = 0;
  uint64_t rlLatencyWorstExcessiveFrameIdx = 0;
};

class St20pUserTimestamp : public FrameTestStrategy {
 public:
  explicit St20pUserTimestamp(St20pHandler* parentHandler,
                              std::vector<double> offsetMultipliers = {});

  int getPacingParameters();
  void txTestFrameModifier(void* frame, size_t frame_size) override;
  void rxTestFrameModifier(void* frame, size_t frame_size) override;

  /* Asserts that every received frame stayed within the per-frame timing
   * bound checked in verifyReceiveTiming(), for every frame after frame 0
   * (session warm-up, excluded -- see recordTimingSample()). Each frame's
   * bound is checked against its own independently-computed expected
   * arrival (see expectedTransmitTimeNs(), which derives the target purely
   * from frame_idx, never from a previous frame's actual timing), so
   * accumulating/compounding drift would show up here as a growing run of
   * outliers concentrated near the end -- no separate drift check needed.
   * There is no outlier budget: any single frame that breaches the bound
   * fails the test, but the failure message reports the average across all
   * frames so a one-off outlier is distinguishable from a systemic
   * regression. */
  void assertTimingWithinBudget() const;

  /* Asserts that, for every received frame after frame 0 (session warm-up,
   * excluded), latency = receive_timestamp - (RTP timestamp converted to
   * ns) is neither negative (the receiver cannot see a frame's first
   * packet before the frame's own declared RTP time -- see the RL
   * warm-up/target-TSC gating in _video_trs_rl_tasklet() in
   * st_video_transmitter.c, which guarantees the real packet never departs
   * before its target) nor implausibly large (see
   * kRlLatencyExcessiveThresholdNs, which would indicate the gating is
   * delaying transmission far past its target instead of tracking it).
   * Unlike assertTimingWithinBudget(), there is no outlier budget: any
   * single bad frame fails the test, but the failure message reports the
   * average across all frames so a one-off outlier is distinguishable from
   * a systemic regression. */
  void assertRlLatencyWithinBounds() const;

  double pacing_tr_offset_ns = 0.0;
  double pacing_trs_ns = 0.0;
  uint32_t pacing_vrx_pkts = 0;

 protected:
  void initializeTiming(St20pHandler* handler);
  uint64_t plannedTimestampNs(uint64_t frame_idx) const;
  uint64_t plannedTimestampBaseNs(uint64_t frame_idx) const;
  double offsetMultiplierForFrame(uint64_t frame_idx) const;
  virtual uint64_t expectedTransmitTimeNs(uint64_t frame_idx) const;
  virtual void verifyReceiveTiming(uint64_t frame_idx, uint64_t receive_time_ns,
                                   uint64_t expected_transmit_time_ns);
  /* Records a timing sample against [lower_bound_ns, upper_bound_ns] instead
   * of asserting immediately; see assertTimingWithinBudget(). Frame 0 is
   * excluded (session warm-up). Mutates accumulated stats, so it (and
   * verifyReceiveTiming()) are not const. */
  void recordTimingSample(uint64_t frame_idx, int64_t delta_ns, int64_t lower_bound_ns,
                          int64_t upper_bound_ns);
  void verifyMediaClock(uint64_t frame_idx, uint64_t timestamp_media_clk,
                        uint64_t expected_media_clk) const;
  virtual void verifyTimestampStep(uint64_t frame_idx, uint64_t current_timestamp);
  /* Records receive_timestamp - rtp_timestamp_ns for assertRlLatencyWithinBounds().
   * Frame 0 is excluded (session warm-up, not steady-state pacing). */
  void recordRlLatencySample(uint64_t frame_idx, int64_t latency_ns);

  double frameTimeNs = 0.0;
  uint64_t startingTime = 0;
  uint64_t lastTimestamp = 0;
  std::vector<double> timestampOffsetMultipliers;

  uint64_t timingSampleCount = 0;
  uint64_t timingOutlierCount = 0;
  int64_t timingWorstDeltaNs = 0;
  uint64_t timingWorstFrameIdx = 0;
  int64_t timingDeltaSumNs = 0;

  uint64_t rlLatencySampleCount = 0;
  int64_t rlLatencySumNs = 0;
  uint64_t rlLatencyNegativeCount = 0;
  int64_t rlLatencyWorstNegativeNs = 0;
  uint64_t rlLatencyWorstNegativeFrameIdx = 0;
  uint64_t rlLatencyExcessiveCount = 0;
  int64_t rlLatencyWorstExcessiveNs = 0;
  uint64_t rlLatencyWorstExcessiveFrameIdx = 0;
};

class St20pUserTimestampCustomStart : public St20pUserTimestamp {
 public:
  St20pUserTimestampCustomStart(St20pHandler* parentHandler,
                                std::vector<double> offsetsNs,
                                uint64_t customStartingTimeNs);
};

class St20pRedundantLatency : public St20pUserTimestamp {
 public:
  St20pRedundantLatency(unsigned int latency, St20pHandler* parentHandler);
  void rxTestFrameModifier(void* frame, size_t frame_size) override;

 private:
  unsigned int latencyInMs;
};

class St20pExactUserPacing : public St20pUserTimestamp {
 public:
  explicit St20pExactUserPacing(St20pHandler* parentHandler = nullptr,
                                std::vector<double> offsetMultipliers = {});

 protected:
  uint64_t expectedTransmitTimeNs(uint64_t frame_idx) const override;
  void verifyReceiveTiming(uint64_t frame_idx, uint64_t receive_time_ns,
                           uint64_t expected_transmit_time_ns) override;
  void verifyTimestampStep(uint64_t frame_idx, uint64_t current_timestamp) override;
};

class St20pRedundantOddEvenLatency : public St20pRedundantLatency {
  uint8_t content = 0;

 public:
  St20pRedundantOddEvenLatency(unsigned int latency, St20pHandler* parentHandler);
  void rxTestFrameModifier(void* frame, size_t frame_size) override;

 private:
  unsigned int latencyInMs;
};
