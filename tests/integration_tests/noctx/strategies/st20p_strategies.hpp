/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/* ST20p frame strategies and the strict-pacing topology check.
 * See README.md, "Timing model".
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/strategy.hpp"
#include "mtl_api.h"
#include "test_util.h"

class St20pHandler;
struct st_frame;

/* Strict ST20p pacing tolerances on packet 0 of every frame. */
/* Elapsed time is measured against frame zero, so drift is not forgiven per frame. */
constexpr int64_t kNoCtxPacingElapsedErrorMaxNs = 10 * NS_PER_US;
/* Packet 0 cannot arrive before its launch; this is only PHC cross-timestamp error. */
constexpr int64_t kNoCtxPacingEarlyMaxNs = 1 * NS_PER_US;
/* Wire and NIC latency of packet 0 plus launch jitter, like the elapsed bound. */
constexpr int64_t kNoCtxPacingLateMaxNs = 10 * NS_PER_US;
/* A user-paced plan starts this far past PTP now, rounded up to a frame. */
constexpr uint64_t kSt20pUserPacingLeadNs = 800 * NS_PER_MS;
/* St20pRedundantStreamPlan starts at PTP zero plus this plus its latency. */
constexpr int kSt20pRedundantStartMs = 50;

/* Empty if the ports suit the strict pacing tests, otherwise the reason they do not. */
std::string strictPacingTopologyError(const char* tx_port, const char* rx_port);

/* NOCTX_REQUIRE_STRICT=1: an unsuitable strict topology fails instead of skipping. */
bool strictPacingRequired();

/* Converts NIC RX timestamps from the RX port's PHC to CLOCK_MONOTONIC_RAW. */
class RxPhcClock {
 public:
  RxPhcClock() = default;
  RxPhcClock(const RxPhcClock&) = delete;
  RxPhcClock& operator=(const RxPhcClock&) = delete;
  ~RxPhcClock();
  /* False without a NIC timestamp; frame 0 skips unless strictPacingRequired(). */
  bool receiveTimeMonotonicRaw(uint64_t frame_idx, uint64_t receive_timestamp,
                               const char* port, mtl_handle mt, uint64_t* mono_ns);

 private:
  int fd = -1;
  bool skipped = false;
};

/* The NoCtx fake PTP clock is CLOCK_MONOTONIC_RAW minus a start offset. */
uint64_t monotonicRawToPtp(mtl_handle mt, uint64_t mono_ns);

struct PacingErrorSeries {
  int64_t min = INT64_MAX;
  int64_t max = INT64_MIN;
  int64_t sum = 0;
  uint64_t count = 0;
  void add(int64_t error_ns);
};

/* Prints min/avg/max of both packet-0 error series once per run, to calibrate bounds. */
struct PacingErrorLog {
  PacingErrorSeries abs_error_ns;
  PacingErrorSeries elapsed_error_ns;
  ~PacingErrorLog();
};

/* Default pacing oracle. Checks on every RX frame n:
 * 1. COMPLETE with the BPM packet count on P            expectCompletePrimaryFrame()
 * 2. timestamp is the media-clock RTP header value      expectRtpTimestamp()
 * 3. frame 0 RTP on the k*T + TR_offset - VRX*trs grid  expectRtpOnEpoch()
 * 4. RTP step == tick90k(T)                             rxTestFrameModifier()
 * With a NIC RX timestamp (RxPhcClock), packet 0 of frame n against frame 0 + n*T:
 * 5. launch within -kNoCtxPacingEarlyMaxNs..+kNoCtxPacingLateMaxNs  expectPacingLaunch()
 * 6. elapsed from frame 0 within +-kNoCtxPacingElapsedErrorMaxNs    expectPacingElapsed()
 */
class St20pDefaultPacingOracle : public FrameTestStrategy {
 protected:
  uint64_t lastTimestamp = 0;
  uint64_t receiveAnchorTimestamp = 0;
  uint64_t firstLaunchNs = 0;
  RxPhcClock rxPhc;
  PacingErrorLog pacingLog;

 public:
  explicit St20pDefaultPacingOracle(St20pHandler* parentHandler = nullptr);
  void rxTestFrameModifier(void* frame, size_t frame_size) override;
};

/* USER_PACING oracle. TX requests t_user(n) = start + (n + offset[n % size]) * T, start
 * = PTP now at construction + kSt20pUserPacingLeadNs, rounded up to T. The expected TX
 * is (the epoch nearest t_user(n)) + TR_offset - VRX*trs (expectedTransmitTimeNs()), so
 * the test must call getPacingParameters() before frame 0. Checks on every RX frame n:
 * 1. COMPLETE with the BPM packet count on P            expectCompletePrimaryFrame()
 * 2. launch within -kNoCtxPacingEarlyMaxNs..+kNoCtxPacingLateMaxNs  verifyReceiveTiming()
 * 3. elapsed from frame 0 within +-kNoCtxPacingElapsedErrorMaxNs    verifyReceiveTiming()
 * 4. timestamp is the media-clock RTP header value      expectRtpTimestamp()
 * 5. RTP == tick90k(expected TX)                        verifyMediaClock()
 * 6. RTP step == tick90k(T)                             verifyTimestampStep()
 * 2 and 3 need a NIC RX timestamp (RxPhcClock).
 */
class St20pUserPacingOracle : public FrameTestStrategy {
 public:
  explicit St20pUserPacingOracle(St20pHandler* parentHandler,
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
  void verifyReceiveTiming(uint64_t frame_idx, const st_frame* frame,
                           uint64_t expected_transmit_time_ns);
  void verifyMediaClock(uint64_t frame_idx, uint64_t timestamp_media_clk,
                        uint64_t expected_media_clk) const;
  virtual void verifyTimestampStep(uint64_t frame_idx, uint64_t current_timestamp);

  double frameTimeNs = 0.0;
  uint64_t startingTime = 0;
  uint64_t lastTimestamp = 0;
  uint64_t receiveAnchorTimestamp = 0;
  uint64_t expectedAnchorTime = 0;
  std::vector<double> timestampOffsetMultipliers;
  RxPhcClock rxPhc;
  PacingErrorLog pacingLog;
};

/* TX plan only: t_user(n) = (kSt20pRedundantStartMs + latency) ms + n * T from PTP
 * zero. RX counts frames in idx_rx and checks nothing. */
class St20pRedundantStreamPlan : public St20pUserPacingOracle {
 public:
  St20pRedundantStreamPlan(unsigned int latency, St20pHandler* parentHandler);
  void rxTestFrameModifier(void* frame, size_t frame_size) override;

 private:
  unsigned int latencyInMs;
};

/* EXACT_USER_PACING oracle: St20pUserPacingOracle with the expected TX = t_user(n)
 * itself, so check 5 is RTP == tick90k(t_user(n)); check 6 is not made. */
class St20pExactUserPacingOracle : public St20pUserPacingOracle {
 public:
  explicit St20pExactUserPacingOracle(St20pHandler* parentHandler = nullptr,
                                      std::vector<double> offsetMultipliers = {});

 protected:
  uint64_t expectedTransmitTimeNs(uint64_t frame_idx) const override;
  void verifyTimestampStep(uint64_t frame_idx, uint64_t current_timestamp) override;
};

/* Interlaced St20pUserPacingOracle, T = field period. The expected TX moves one field
 * later when its slot is off the ST 2110-21 frame grid (expectedTransmitTimeNs()).
 * Before checks 1-6 of every RX field n:
 * 0. interlaced, and second_field == (n odd)             rxTestFrameModifier()
 */
class St20pInterlacedUserPacingOracle : public St20pUserPacingOracle {
 public:
  using St20pUserPacingOracle::St20pUserPacingOracle;
  void rxTestFrameModifier(void* frame, size_t frame_size) override;

 protected:
  uint64_t expectedTransmitTimeNs(uint64_t frame_idx) const override;
};
