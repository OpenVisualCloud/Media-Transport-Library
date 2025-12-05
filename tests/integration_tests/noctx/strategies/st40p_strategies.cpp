/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright(c) 2025 Intel Corporation */

#include "st40p_strategies.hpp"

#include <gtest/gtest.h>
#include <mtl/st_api.h>

#include <cerrno>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "core/constants.hpp"
#include "handlers/st40p_handler.hpp"
#include "tests.hpp"

St40pUserTimestamp::St40pUserTimestamp(St40pHandler* parentHandler,
                                       std::vector<double> offsetMultipliers)
    : FrameTestStrategy(parentHandler, true, true),
      timestampOffsetMultipliers(std::move(offsetMultipliers)) {
  initializeTiming(parentHandler);
}

int St40pUserTimestamp::getPacingParameters() {
  /* ST40 pipeline lacks a public pacing query; keep defaults for now. */
  return -ENOTSUP;
}

void St40pUserTimestamp::txTestFrameModifier(void* frame, size_t /*frame_size*/) {
  auto* info = static_cast<st40_frame_info*>(frame);
  info->tfmt = ST10_TIMESTAMP_FMT_TAI;
  info->timestamp = plannedTimestampNs(idx_tx);
  idx_tx++;
}

void St40pUserTimestamp::rxTestFrameModifier(void* frame, size_t /*frame_size*/) {
  auto* info = static_cast<st40_frame_info*>(frame);
  const uint64_t frame_idx = idx_rx++;

  const uint64_t expected_transmit_time_ns = expectedTransmitTimeNs(frame_idx);
  const uint64_t expected_media_clk =
      st10_tai_to_media_clk(expected_transmit_time_ns, VIDEO_CLOCK_HZ);

  verifyReceiveTiming(frame_idx, info->receive_timestamp, expected_transmit_time_ns);
  verifyMediaClock(frame_idx, info->timestamp, expected_media_clk);
  verifyTimestampStep(frame_idx, info->timestamp);

  lastTimestamp = info->timestamp;
}

uint64_t St40pUserTimestamp::plannedTimestampNs(uint64_t frame_idx) const {
  double base = plannedTimestampBaseNs(frame_idx);
  double offset = frameTimeNs * offsetMultiplierForFrame(frame_idx);
  double adjusted = base + offset;
  return adjusted <= 0.0 ? 0 : static_cast<uint64_t>(adjusted);
}

double St40pUserTimestamp::plannedTimestampBaseNs(uint64_t frame_idx) const {
  double base = startingTime + frame_idx * frameTimeNs;
  return base < 0.0 ? 0.0 : base;
}

double St40pUserTimestamp::offsetMultiplierForFrame(uint64_t frame_idx) const {
  if (timestampOffsetMultipliers.empty()) {
    return 0;
  }

  size_t loop_idx = frame_idx % timestampOffsetMultipliers.size();
  return timestampOffsetMultipliers[loop_idx];
}

uint64_t St40pUserTimestamp::expectedTransmitTimeNs(uint64_t frame_idx) const {
  double target_ns = plannedTimestampBaseNs(frame_idx);
  double pacing_adjustment = pacing_tr_offset_ns - pacing_vrx_pkts * pacing_trs_ns;

  double expected = target_ns + pacing_adjustment;
  return expected <= 0.0 ? 0 : static_cast<uint64_t>(expected);
}

void St40pUserTimestamp::verifyReceiveTiming(uint64_t frame_idx, uint64_t receive_time_ns,
                                             uint64_t expected_transmit_time_ns) const {
  const int64_t delta_ns = static_cast<int64_t>(receive_time_ns) -
                           static_cast<int64_t>(expected_transmit_time_ns);
  int64_t expected_delta_ns = 55 * NS_PER_US;
  if (frame_idx == 0) {
    expected_delta_ns = 80 * NS_PER_US;
  }

  EXPECT_LE(delta_ns, expected_delta_ns)
      << " idx_rx: " << frame_idx << " delta(ns): " << delta_ns
      << " receive timestamp(ns): " << receive_time_ns
      << " expected timestamp(ns): " << expected_transmit_time_ns;
}

void St40pUserTimestamp::verifyMediaClock(uint64_t frame_idx,
                                          uint64_t timestamp_media_clk,
                                          uint64_t expected_media_clk) const {
  EXPECT_EQ(timestamp_media_clk, expected_media_clk)
      << " idx_rx: " << frame_idx << " expected media clk: " << expected_media_clk
      << " received timestamp: " << timestamp_media_clk;
}

void St40pUserTimestamp::verifyTimestampStep(uint64_t frame_idx,
                                             uint64_t current_timestamp) {
  if (!lastTimestamp) {
    return;
  }

  double current_target = plannedTimestampBaseNs(frame_idx);
  double previous_target = plannedTimestampBaseNs(frame_idx ? frame_idx - 1 : 0);
  const uint64_t expected_step = st10_tai_to_media_clk(current_target, VIDEO_CLOCK_HZ) -
                                 st10_tai_to_media_clk(previous_target, VIDEO_CLOCK_HZ);

  const uint64_t diff = current_timestamp - lastTimestamp;
  EXPECT_EQ(diff, expected_step) << " idx_rx: " << frame_idx << " diff: " << diff;
}

void St40pUserTimestamp::initializeTiming(St40pHandler* handler) {
  if (!handler) {
    throw std::invalid_argument("St40pUserTimestamp expects a valid handler");
  }

  double framerate = st_frame_rate(handler->sessionsOpsTx.fps);
  if (framerate <= 0.0) {
    framerate = 60.0;
  }

  frameTimeNs = static_cast<long double>(NS_PER_S) / framerate;

  if (frameTimeNs <= 0.0) {
    frameTimeNs = static_cast<long double>(NS_PER_S) / 25.0;
  }

  startingTime = frameTimeNs * 70.0;
}
