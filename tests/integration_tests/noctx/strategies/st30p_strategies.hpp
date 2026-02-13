/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#pragma once

#include <cstdint>

#include "core/strategy.hpp"

class St30pHandler;

class St30pDefaultTimestamp : public FrameTestStrategy {
 public:
  explicit St30pDefaultTimestamp(St30pHandler* parentHandler = nullptr);
  void rxTestFrameModifier(void* frame, size_t frame_size) override;

 protected:
  uint64_t lastTimestamp;
};

class St30pUserTimestamp : public St30pDefaultTimestamp {
 public:
  explicit St30pUserTimestamp(St30pHandler* parentHandler = nullptr);
  void initializeTiming(St30pHandler* handler);
  void txTestFrameModifier(void* frame, size_t frame_size) override;
  void rxTestFrameModifier(void* frame, size_t frame_size) override;

 protected:
  uint64_t plannedTimestampNs(uint64_t frame_idx) const;
  void verifyReceiveTiming(uint64_t frame_idx, uint64_t receive_time_ns,
                           uint64_t expected_timestamp_ns) const;
  void verifyMediaClock(uint64_t frame_idx, uint64_t timestamp_media_clk,
                        uint64_t expected_media_clk) const;
  void verifyTimestampStep(uint64_t frame_idx, uint64_t current_timestamp,
                           uint64_t sampling_hz);

  double frameTimeNs = 0.0;
  uint64_t startingTime = 0;
  bool timingInitialized = false;
};

class St30pRedundantLatency : public St30pUserTimestamp {
 public:
  St30pRedundantLatency(unsigned int latency = 30, St30pHandler* parentHandler = nullptr,
                        int startingTime = 100);
  void rxTestFrameModifier(void* frame, size_t frame_size) override;

 private:
  unsigned int latencyInMs;
  unsigned int startingTimeInMs;
};
