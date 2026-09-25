/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/* ST30p frame strategies. See README.md, "Test catalogue".
 */

#pragma once

#include <cstdint>

#include "core/strategy.hpp"
#include "strategies/st20p_strategies.hpp"

class St30pHandler;
struct st30_frame;

/* ST30p user pacing: |NIC RX time - t_user| per buffer, looser for buffer 0. */
constexpr int64_t kSt30pRxToleranceNs = 40 * NS_PER_US;
constexpr int64_t kSt30pFirstBufferRxToleranceNs = 80 * NS_PER_US;
/* The user-paced plan starts this many buffers after PTP zero. */
constexpr int kSt30pUserPacingStartBuffers = 60;

/* Default pacing oracle. Checks on every RX buffer n:
 * 1. buffer 0 RTP, as TAI, on the packet-time grid, not after its (software) RX time
 *    and less than one buffer before it           expectFirstFrameOnPacketGrid()
 * 2. RTP step == tick(nsFramebuffTime)            rxTestFrameModifier()
 */
class St30pDefaultPacingOracle : public FrameTestStrategy {
 public:
  explicit St30pDefaultPacingOracle(St30pHandler* parentHandler = nullptr);
  void rxTestFrameModifier(void* frame, size_t frame_size) override;

 protected:
  uint64_t lastTimestamp;
};

/* USER_PACING oracle; replaces the default checks. TX requests t_user(n) =
 * (kSt30pUserPacingStartBuffers + n) * B from PTP zero, B = nsFramebuffTime, once the
 * test calls initializeTiming(). Checks on every RX buffer n:
 * 1. |NIC RX time - t_user(n)| <= kSt30pRxToleranceNs
 *    (kSt30pFirstBufferRxToleranceNs for buffer 0)   verifyReceiveTiming()
 * 2. RTP == tick(t_user(n)) at the sample rate      verifyMediaClock()
 * 3. RTP step == tick(B)                            verifyTimestampStep()
 */
class St30pUserPacingOracle : public St30pDefaultPacingOracle {
 public:
  explicit St30pUserPacingOracle(St30pHandler* parentHandler = nullptr);
  void initializeTiming(St30pHandler* handler);
  void txTestFrameModifier(void* frame, size_t frame_size) override;
  void rxTestFrameModifier(void* frame, size_t frame_size) override;

 protected:
  uint64_t plannedTimestampNs(uint64_t frame_idx) const;
  void verifyReceiveTiming(uint64_t frame_idx, const st30_frame* frame,
                           uint64_t expected_timestamp_ns);
  void verifyMediaClock(uint64_t frame_idx, uint64_t timestamp_media_clk,
                        uint64_t expected_media_clk) const;
  void verifyTimestampStep(uint64_t frame_idx, uint64_t current_timestamp,
                           uint64_t sampling_hz);

  double frameTimeNs = 0.0;
  uint64_t startingTime = 0;
  bool timingInitialized = false;
  RxPhcClock rxPhc;
};

/* TX plan only: the St30pUserPacingOracle plan. RX counts buffers in idx_rx and checks
 * nothing. */
class St30pRedundantStreamPlan : public St30pUserPacingOracle {
 public:
  explicit St30pRedundantStreamPlan(St30pHandler* parentHandler = nullptr);
  void rxTestFrameModifier(void* frame, size_t frame_size) override;
};
