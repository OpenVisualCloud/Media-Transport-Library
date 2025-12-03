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
};

class St20pUserTimestamp : public FrameTestStrategy {
 public:
  explicit St20pUserTimestamp(St20pHandler* parentHandler,
                              std::vector<double> offsetMultipliers = {});

  int getPacingParameters();
  void txTestFrameModifier(void* frame, size_t frame_size) override;
  void rxTestFrameModifier(void* frame, size_t frame_size) override;

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
                                   uint64_t expected_transmit_time_ns) const;
  void verifyMediaClock(uint64_t frame_idx, uint64_t timestamp_media_clk,
                        uint64_t expected_media_clk) const;
  virtual void verifyTimestampStep(uint64_t frame_idx, uint64_t current_timestamp);

  double frameTimeNs = 0.0;
  uint64_t startingTime = 0;
  uint64_t lastTimestamp = 0;
  std::vector<double> timestampOffsetMultipliers;
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
                           uint64_t expected_transmit_time_ns) const override;
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
