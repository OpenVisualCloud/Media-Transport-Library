/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright(c) 2025 Intel Corporation */

#pragma once

#include <mtl/st40_pipeline_api.h>

#include <cstdint>
#include <vector>

#include "core/strategy.hpp"
#include "test_util.h"

class St40pHandler;

/* ST 2110-40:2023 6.5 (the default absent TM=LLTM, per 7): a sender transmits no
 * later than T_EPO(j) + T_D, T_D = 1 ms. There is no ST 2110-21 style narrow
 * window for ANC, so no video-style microsecond bound. */
constexpr int64_t kSt40pMaxLateNs = 1 * NS_PER_MS;
/* The user-paced plan starts this many frames after PTP zero. */
constexpr double kSt40pUserPacingStartFrames = 70.0;

/* USER_PACING oracle. TX requests t_user(n) = (kSt40pUserPacingStartFrames + n +
 * offset[n % size]) * T from PTP zero; the expected TX is the epoch nearest t_user(n)
 * (expectedTransmitTimeNs()). getPacingParameters() always returns -ENOTSUP.
 * Checks on every RX frame n:
 * 1. software RX time - expected TX in [0, kSt40pMaxLateNs]  verifyReceiveTiming()
 * 2. RTP == tick90k(expected TX)                            verifyMediaClock()
 * 3. RTP step == tick90k(base(n)) - tick90k(base(n - 1)),
 *    base(n) = t_user(n) without its offset                verifyTimestampStep()
 */
class St40pUserPacingOracle : public FrameTestStrategy {
 public:
  explicit St40pUserPacingOracle(St40pHandler* parentHandler = nullptr,
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

/* EXACT_USER_PACING oracle: St40pUserPacingOracle with the expected TX = t_user(n)
 * itself; check 3 is not made. */
class St40pExactUserPacingOracle : public St40pUserPacingOracle {
 public:
  explicit St40pExactUserPacingOracle(St40pHandler* parentHandler = nullptr,
                                      std::vector<double> offsetMultipliers = {});

 protected:
  uint64_t expectedTransmitTimeNs(uint64_t frame_idx) const override;
  void verifyTimestampStep(uint64_t frame_idx, uint64_t current_timestamp) override;
};
