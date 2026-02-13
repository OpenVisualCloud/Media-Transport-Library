/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright(c) 2025 Intel Corporation */

#pragma once

#include <mtl/st40_pipeline_api.h>

#include <cstdint>
#include <vector>

#include "core/strategy.hpp"

class St40pHandler;

class St40pUserTimestamp : public FrameTestStrategy {
 public:
  explicit St40pUserTimestamp(St40pHandler* parentHandler = nullptr,
                              std::vector<double> offsetMultipliers = {});

  void txTestFrameModifier(void* frame, size_t frame_size) override;
  void rxTestFrameModifier(void* frame, size_t frame_size) override;
  int getPacingParameters();

  double pacing_tr_offset_ns = 0.0;
  double pacing_trs_ns = 0.0;
  uint32_t pacing_vrx_pkts = 0;

 protected:
  void initializeTiming(St40pHandler* handler);
  uint64_t plannedTimestampNs(uint64_t frame_idx) const;
  double plannedTimestampBaseNs(uint64_t frame_idx) const;
  double offsetMultiplierForFrame(uint64_t frame_idx) const;
  virtual uint64_t expectedTransmitTimeNs(uint64_t frame_idx) const;
  virtual void verifyReceiveTiming(uint64_t frame_idx, uint64_t receive_time_ns,
                                   uint64_t expected_transmit_time_ns) const;
  void verifyMediaClock(uint64_t frame_idx, uint64_t timestamp_media_clk,
                        uint64_t expected_media_clk) const;
  virtual void verifyTimestampStep(uint64_t frame_idx, uint64_t current_timestamp);

  long double frameTimeNs = 0.0;
  long double startingTime = 0;
  uint64_t lastTimestamp = 0;
  std::vector<double> timestampOffsetMultipliers;
};

class St40pExactUserPacing : public St40pUserTimestamp {
 public:
  explicit St40pExactUserPacing(St40pHandler* parentHandler = nullptr,
                                std::vector<double> offsetMultipliers = {});

 protected:
  uint64_t expectedTransmitTimeNs(uint64_t frame_idx) const override;
  void verifyTimestampStep(uint64_t frame_idx, uint64_t current_timestamp) override;
};
