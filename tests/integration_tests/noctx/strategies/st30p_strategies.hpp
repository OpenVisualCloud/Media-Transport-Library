/* SPDX-License-Identifier: BSD-3-Clause */
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
  void txTestFrameModifier(void* frame, size_t frame_size) override;
  void rxTestFrameModifier(void* frame, size_t frame_size) override;

 protected:
  uint64_t startingTime;
  uint64_t lastTimestamp;
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
