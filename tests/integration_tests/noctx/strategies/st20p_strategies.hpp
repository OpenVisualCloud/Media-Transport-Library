/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/* ST20p frame strategies and the strict-pacing topology check.
 * See README.md, "Timing model".
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "core/strategy.hpp"
#include "mtl_api.h"
#include "test_util.h"

class St20pHandler;
struct st_frame;

/* Strict ST20p pacing tolerances on packet 0 of every frame. */
/* Elapsed time is measured against frame zero, so drift is not forgiven per frame. */
constexpr int64_t kNoCtxPacingElapsedErrorMaxNs = 10 * NS_PER_US;
/* Packet 0 cannot arrive before its launch; this is only PHC read error, u included. */
constexpr int64_t kNoCtxPacingEarlyMaxNs = 1 * NS_PER_US;
/* Wire and NIC latency of packet 0 plus launch jitter, like the elapsed bound. */
constexpr int64_t kNoCtxPacingLateMaxNs = 10 * NS_PER_US;
/* A PHC read without a NIC cross-timestamp (E810) must place it to +-this. */
constexpr uint64_t kPhcReadMaxUncertaintyNs = 1 * NS_PER_US;
/* The PHC is read about a frame after the NIC timestamp: 5 ppm is 0.2 us at 25 fps. */
constexpr double kPhcRateMaxPpm = 5.0;
/* A user-paced plan starts this far past PTP now, rounded up to a frame. */
constexpr uint64_t kSt20pUserPacingLeadNs = 800 * NS_PER_MS;
/* St20pRedundantStreamPlan starts at PTP zero plus this plus its latency. */
constexpr int kSt20pRedundantStartMs = 50;
/* PhcFollowingClock health: the largest lag of MTL's PTP time behind its target. */
constexpr uint64_t kPhcFollowMaxLagNs = 5 * NS_PER_US;
/* PhcFollowingClock health: the longest time between two successful PHC reads. */
constexpr uint64_t kPhcFollowMaxGapNs = 100 * NS_PER_MS;
/* PhcFollowingClock health: the least share of its 10 ms read slots that read the PHC. */
constexpr double kPhcFollowMinReadShare = 0.9;
/* The most packets a 2022-7 leg may finish behind the other; the parser does not time
 * the lagging leg's packets after the frame completed. */
constexpr uint32_t kSt20pVrxMaxLegLagPkts = 4;
/* St20pVrxRecorder skips the start-up underflows of the first kSt20pVrxWarmupS. */
constexpr int kSt20pVrxWarmupS = 20;
/* St20pVrxRecorder then records for this long. */
constexpr int kSt20pVrxWindowS = 30;

/* Empty if the ports suit the strict pacing tests, otherwise the reason they do not. */
std::string strictPacingTopologyError(const char* tx_port, const char* rx_port);

/* NOCTX_REQUIRE_STRICT=1: an unsuitable strict topology fails instead of skipping. */
bool strictPacingRequired();

/* Empty if both ports' RX timestamps come from one PHC, otherwise why not. */
std::string sharedPhcError(const char* port_a, const char* port_b);

/* A time known to +-u_ns. */
struct RxTime {
  uint64_t ns = 0;
  int64_t u_ns = 0;
};

/* The first frame of a strict check, which elapsed time is measured from. */
struct PacingAnchor {
  RxTime rx;
  uint64_t expected_ns = 0;
};

/* Converts NIC RX timestamps from the RX port's PHC to CLOCK_MONOTONIC_RAW, to +-u
 * (kPhcReadMaxUncertaintyNs), fails a PHC rate off by more than kPhcRateMaxPpm and
 * prints the rate when destroyed. */
class RxPhcClock {
 public:
  RxPhcClock() = default;
  RxPhcClock(const RxPhcClock&) = delete;
  RxPhcClock& operator=(const RxPhcClock&) = delete;
  ~RxPhcClock();
  /* False without a NIC timestamp; frame 0 skips unless strictPacingRequired(). */
  bool receiveTimeMonotonicRaw(uint64_t frame_idx, uint64_t receive_timestamp,
                               const char* port, mtl_handle mt, RxTime* rx);

 private:
  double rateOffsetPpm() const;
  void expectRate(const char* port, uint64_t uncertainty_ns);
  int fd = -1;
  bool skipped = false;
  bool rate_failed = false;
  uint64_t first_phc_ns = 0, first_mono_ns = 0, first_uncertainty_ns = 0;
  uint64_t last_phc_ns = 0, last_mono_ns = 0;
  uint64_t uncertainty_max_ns = 0;
};

/* MTL PTP time as an RX port's PHC plus a fixed offset, for tests whose oracle is the
 * library's RX timing parser. now() is CLOCK_MONOTONIC_RAW plus an offset that a thread
 * re-measures every 10 ms and slews toward over the next 10 ms at up to 100 ppm; each
 * update can move now() by ~10-20 ns, either way. now() costs a vDSO clock read, as
 * FakePtpClockNow, but spins while the follower is mid-update, so a follower preempted
 * there stalls every caller, TX tasklets included. One instance per process. */
class PhcFollowingClock {
 public:
  PhcFollowingClock() = default;
  PhcFollowingClock(const PhcFollowingClock&) = delete;
  PhcFollowingClock& operator=(const PhcFollowingClock&) = delete;
  ~PhcFollowingClock();
  /* Reads the PHC of port's PF once, so now() is PHC + ptp_minus_phc_ns; empty on
   * success, else why not. */
  std::string open(const char* port, int64_t ptp_minus_phc);
  /* Starts the thread that re-reads the PHC, pinned to the calling thread's CPU;
   * empty on success, else why not. */
  std::string follow();
  /* Stops following, freezes the offset and prints the read statistics. */
  void stop();
  /* For mtl_init_params.ptp_get_time_fn. */
  static uint64_t now(void* priv);
  /* After stop(): empty if the reads met kPhcFollowMaxLagNs, kPhcFollowMaxGapNs and
   * kPhcFollowMinReadShare, else why not. */
  std::string healthError() const;

 private:
  void run(int cpu, std::promise<int> pinned);
  int fd = -1;
  int64_t ptp_minus_phc_ns = 0;
  std::thread thread;
  std::atomic<bool> stopping{false};
  uint64_t follow_mono_ns = 0, stop_mono_ns = 0, last_read_mono_ns = 0;
  uint64_t reads = 0, failed_reads = 0, max_lag_ns = 0, max_gap_ns = 0;
};

/* The NoCtx fake PTP clock is CLOCK_MONOTONIC_RAW minus a start offset. The offset is
 * read against MTL's PTP time in the tightest of a few CLOCK_MONOTONIC_RAW brackets,
 * whose half-width adds to u. */
RxTime monotonicRawToPtp(mtl_handle mt, const RxTime& rx);

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
 * With a NIC RX timestamp (RxPhcClock), at both ends of its +-u, packet 0 of frame n
 * against frame 0 + n*T:
 * 5. launch within -kNoCtxPacingEarlyMaxNs..+kNoCtxPacingLateMaxNs  expectPacingLaunch()
 * 6. elapsed from frame 0 within +-kNoCtxPacingElapsedErrorMaxNs    expectPacingElapsed()
 */
class St20pDefaultPacingOracle : public FrameTestStrategy {
 protected:
  uint64_t lastTimestamp = 0;
  PacingAnchor anchor;
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
 * 2 and 3 need a NIC RX timestamp (RxPhcClock) and hold at both ends of its +-u.
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
  PacingAnchor anchor;
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

/* For the frames received kSt20pVrxWarmupS to kSt20pVrxWarmupS + kSt20pVrxWindowS after
 * the session's first, by receive_timestamp, with MTL's PTP time = RX PHC +
 * ptp_minus_phc_ns (a whole number of frames), records PTP now - receive_timestamp and
 * per port with packets:
 * - sw_rx_frames: rtp_offset not shifted by ptp_minus_phc_ns, so the parser timed the
 *   port's packets in software
 * - off_band_frames: rtp_offset neither within a frame of 0 nor within half a frame
 *   of the shift, e.g. a frame one period late or across an RTP wrap
 * - otherwise vrx_min, and the frame's packets the port's tp missed: the most in
 *   max_missing_pkts, and partial_frames if more than kSt20pVrxMaxLegLagPkts. */
class St20pVrxRecorder : public FrameTestStrategy {
 public:
  St20pVrxRecorder(St20pHandler* parentHandler, int64_t ptp_minus_phc);
  void rxTestFrameModifier(void* frame, size_t frame_size) override;

  std::vector<int64_t> delivery_ns;
  std::vector<int32_t> vrx_min[MTL_SESSION_PORT_MAX];
  uint32_t max_missing_pkts[MTL_SESSION_PORT_MAX] = {};
  uint64_t partial_frames[MTL_SESSION_PORT_MAX] = {};
  uint64_t sw_rx_frames[MTL_SESSION_PORT_MAX] = {};
  uint64_t off_band_frames[MTL_SESSION_PORT_MAX] = {};

 private:
  int64_t ptp_minus_phc_ns;
  uint64_t first_rx_ns = 0;
};
