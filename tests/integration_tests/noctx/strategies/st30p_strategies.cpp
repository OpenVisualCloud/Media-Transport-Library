/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st30p_strategies.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

#include "handlers/st30p_handler.hpp"
#include "tests.hpp"

St30pDefaultTimestamp::St30pDefaultTimestamp(St30pHandler* parentHandler)
    : FrameTestStrategy(parentHandler, false, true), lastTimestamp(0) {
  idx_tx = 0;
  idx_rx = 0;
}

void St30pDefaultTimestamp::rxTestFrameModifier(void* frame, size_t /*frame_size*/) {
  auto* f = static_cast<st30_frame*>(frame);
  auto* st30pParent = static_cast<St30pHandler*>(parent);
  uint64_t sampling = st30_get_sample_rate(st30pParent->sessionsOpsRx.sampling);
  uint64_t framebuffTime = st10_tai_to_media_clk(st30pParent->nsPacketTime, sampling);

  EXPECT_NEAR(f->timestamp,
              st10_tai_to_media_clk(idx_rx * st30pParent->nsPacketTime, sampling),
              framebuffTime)
      << " idx_rx: " << idx_rx;
  if (lastTimestamp != 0) {
    uint64_t diff = f->timestamp - lastTimestamp;
    EXPECT_TRUE(diff == framebuffTime) << " idx_rx: " << idx_rx << " diff: " << diff;
  }

  lastTimestamp = f->timestamp;
  idx_rx++;
}

St30pUserTimestamp::St30pUserTimestamp(St30pHandler* parentHandler)
    : St30pDefaultTimestamp(parentHandler) {
  enable_tx_modifier = true;
  enable_rx_modifier = true;
}

void St30pUserTimestamp::txTestFrameModifier(void* frame, size_t /*frame_size*/) {
  auto* f = static_cast<st30_frame*>(frame);
  auto* st30pParent = static_cast<St30pHandler*>(parent);
  ASSERT_NE(st30pParent, nullptr);
  ASSERT_TRUE(timingInitialized)
      << "Call St30pUserTimestamp::initializeTiming from the test before sending frames";
  f->tfmt = ST10_TIMESTAMP_FMT_TAI;
  f->timestamp = plannedTimestampNs(idx_tx);
  idx_tx++;
}

void St30pUserTimestamp::rxTestFrameModifier(void* frame, size_t /*frame_size*/) {
  auto* f = static_cast<st30_frame*>(frame);
  auto* st30pParent = static_cast<St30pHandler*>(parent);
  ASSERT_NE(st30pParent, nullptr);
  ASSERT_TRUE(timingInitialized) << "Call St30pUserTimestamp::initializeTiming from the "
                                    "test before validating frames";

  const uint64_t frame_idx = idx_rx++;
  const uint64_t expected_timestamp_ns = plannedTimestampNs(frame_idx);
  const uint64_t sampling = st30_get_sample_rate(st30pParent->sessionsOpsRx.sampling);
  const uint64_t expected_media_clk =
      st10_tai_to_media_clk(expected_timestamp_ns, sampling);

  verifyReceiveTiming(frame_idx, f->receive_timestamp, expected_timestamp_ns);
  verifyMediaClock(frame_idx, f->timestamp, expected_media_clk);
  verifyTimestampStep(frame_idx, f->timestamp, sampling);

  lastTimestamp = f->timestamp;
}

void St30pUserTimestamp::initializeTiming(St30pHandler* handler) {
  if (!handler) {
    throw std::invalid_argument("St30pUserTimestamp expects a valid handler");
  }
  if (timingInitialized) {
    return;
  }

  frameTimeNs = handler->nsPacketTime;
  if (!frameTimeNs) {
    auto& ops = handler->sessionsOpsTx;
    uint64_t packet_time = st30_get_packet_time(ops.ptime);
    uint64_t packet_size =
        st30_get_packet_size(ops.fmt, ops.ptime, ops.sampling, ops.channel);
    uint64_t packets_per_frame = 0;
    if (packet_size) {
      packets_per_frame = ops.framebuff_size / packet_size;
    }

    frameTimeNs = packet_time * packets_per_frame;
  }

  if (!frameTimeNs) {
    frameTimeNs = NS_PER_MS;
  }

  startingTime = static_cast<uint64_t>(frameTimeNs * 60);
  timingInitialized = true;
}

uint64_t St30pUserTimestamp::plannedTimestampNs(uint64_t frame_idx) const {
  double base = startingTime + frame_idx * frameTimeNs;
  return base <= 0.0 ? 0 : static_cast<uint64_t>(base);
}

void St30pUserTimestamp::verifyReceiveTiming(uint64_t frame_idx, uint64_t receive_time_ns,
                                             uint64_t expected_timestamp_ns) const {
  const int64_t delta_ns =
      static_cast<int64_t>(receive_time_ns) - static_cast<int64_t>(expected_timestamp_ns);
  int64_t expected_delta_ns = 40 * NS_PER_US;
  if (frame_idx == 0) {
    expected_delta_ns = 80 * NS_PER_US;
  }

  EXPECT_LE(delta_ns, expected_delta_ns)
      << " idx_rx: " << frame_idx << " delta(ns): " << delta_ns
      << " receive timestamp(ns): " << receive_time_ns
      << " expected timestamp(ns): " << expected_timestamp_ns;
}

void St30pUserTimestamp::verifyMediaClock(uint64_t frame_idx,
                                          uint64_t timestamp_media_clk,
                                          uint64_t expected_media_clk) const {
  EXPECT_EQ(timestamp_media_clk, expected_media_clk)
      << " idx_rx: " << frame_idx << " expected media clk: " << expected_media_clk
      << " received timestamp: " << timestamp_media_clk;
}

void St30pUserTimestamp::verifyTimestampStep(uint64_t frame_idx,
                                             uint64_t current_timestamp,
                                             uint64_t sampling_hz) {
  if (!lastTimestamp) {
    return;
  }

  double current_target = startingTime + frame_idx * frameTimeNs;
  double previous_target = startingTime + (frame_idx ? frame_idx - 1 : 0) * frameTimeNs;
  double expected_step_ns = current_target - previous_target;
  if (expected_step_ns < 0.0) {
    expected_step_ns = 0.0;
  }

  uint64_t expected_step_input = static_cast<uint64_t>(expected_step_ns);
  const uint64_t expected_step = st10_tai_to_media_clk(expected_step_input, sampling_hz);
  const uint64_t diff = current_timestamp - lastTimestamp;
  EXPECT_EQ(diff, expected_step) << " idx_rx: " << frame_idx << " diff: " << diff;
}

St30pRedundantLatency::St30pRedundantLatency(unsigned int latency,
                                             St30pHandler* parentHandler,
                                             int startingTime)
    : St30pUserTimestamp(parentHandler),
      latencyInMs(latency),
      startingTimeInMs(static_cast<unsigned int>(startingTime)) {
  startingTime = (50 + latencyInMs) * NS_PER_MS;
}

void St30pRedundantLatency::rxTestFrameModifier(void* /*frame*/, size_t /*frame_size*/) {
  idx_rx++;
}
