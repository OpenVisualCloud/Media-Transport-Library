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

class St20pHandler;
struct st_frame;

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

/* Default pacing. Oracle: frame 0 RTP is on the epoch + tr_offset - vrx * trs grid,
 * packet 0 of frame n leaves at that grid point + n * T (-1/+10 us) and arrives n * T
 * after frame 0 (+-10 us), and RTP steps by exactly tick(T). */
class St20pDefaultTimestamp : public FrameTestStrategy {
 protected:
  uint64_t lastTimestamp = 0;
  uint64_t receiveAnchorTimestamp = 0;
  uint64_t firstLaunchNs = 0;
  RxPhcClock rxPhc;
  PacingErrorLog pacingLog;

 public:
  explicit St20pDefaultTimestamp(St20pHandler* parentHandler = nullptr);
  void rxTestFrameModifier(void* frame, size_t frame_size) override;
};

/* USER_PACING. Oracle: packet 0 at the epoch nearest the request plus
 * tr_offset - vrx * trs (-1/+10 us), elapsed from frame 0 within +-10 us,
 * RTP == tick(that TX). */
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

/* EXACT_USER_PACING. Oracle: packet 0 at the requested instant (-1/+10 us), elapsed
 * from frame 0 within +-10 us, RTP == tick(request); no fixed RTP step. */
class St20pExactUserPacing : public St20pUserTimestamp {
 public:
  explicit St20pExactUserPacing(St20pHandler* parentHandler = nullptr,
                                std::vector<double> offsetMultipliers = {});

 protected:
  uint64_t expectedTransmitTimeNs(uint64_t frame_idx) const override;
  void verifyTimestampStep(uint64_t frame_idx, uint64_t current_timestamp) override;
};

/* Interlaced St20pUserTimestamp at the field rate, with each field on its ST 2110-21
 * frame-grid slot; also checks that fields alternate first/second from a first field. */
class St20pInterlacedUserTimestamp : public St20pUserTimestamp {
 public:
  using St20pUserTimestamp::St20pUserTimestamp;
  void rxTestFrameModifier(void* frame, size_t frame_size) override;

 protected:
  uint64_t expectedTransmitTimeNs(uint64_t frame_idx) const override;
};

class St20pRedundantOddEvenLatency : public St20pRedundantLatency {
  uint8_t content = 0;

 public:
  St20pRedundantOddEvenLatency(unsigned int latency, St20pHandler* parentHandler);
  void rxTestFrameModifier(void* frame, size_t frame_size) override;

 private:
  unsigned int latencyInMs;
};
