/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/* ST20p frame strategies and the strict-pacing topology check.
 * See README.md, "Timing model".
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "core/constants.hpp"
#include "core/strategy.hpp"
#include "mtl_api.h"
#include "st_api.h"
#include "test_util.h"

class St20pHandler;
struct st_frame;

/* Strict ST20p pacing tolerances on packet 0 of every frame. */
/* Elapsed time is measured against frame zero, so drift is not forgiven per frame. */
constexpr int64_t kNoCtxPacingElapsedErrorMaxNs = 10 * NS_PER_US;
/* How early packet 0 may arrive: never, with the PHC read error u counted against it. */
constexpr int64_t kNoCtxPacingEarlyMaxNs = 0;
/* Wire and NIC latency of packet 0 plus launch jitter, like the elapsed bound. */
constexpr int64_t kNoCtxPacingLateMaxNs = 10 * NS_PER_US;
/* Half a 90 kHz tick, rounded up: how far the tick nearest t_user can be from it. */
constexpr int64_t kNoCtxRtpHalfTickNs =
    (NS_PER_S + 2 * VIDEO_CLOCK_HZ - 1) / (2 * VIDEO_CLOCK_HZ);
/* A PHC read without a NIC cross-timestamp (E810) must place it to +-this. */
constexpr uint64_t kPhcReadMaxUncertaintyNs = 1 * NS_PER_US;
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

/* An ST20 frame (field) rate of mul/den per second, den 1001 for the x/1.001 rates. */
struct St20pFrameRate {
  uint64_t mul = 25;
  uint64_t den = 1;
};
St20pFrameRate st20pFrameRate(enum st_fps fps);
/* The library's epoch TAI, the last ns at or before k * 1e9 * den / mul. */
uint64_t st20pEpochTaiNs(const St20pFrameRate& rate, uint64_t epoch);
/* The epoch USER_PACING snaps t_ns to: the nearest, ties to the later one. */
uint64_t st20pNearestEpoch(const St20pFrameRate& rate, uint64_t t_ns);
/* Default and USER_PACING packet 0's target: the tick of epoch + TR_offset - VRX*trs. */
uint64_t st20pRoundedLaunchNs(const St20pFrameRate& rate, uint64_t epoch,
                              double tr_offset_ns, double trs_ns, uint32_t vrx_pkts);

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

/* Converts NIC RX timestamps from the RX port's PHC to CLOCK_MONOTONIC_RAW along the
 * line through a PHC read taken by start() and one taken per frame, to +-u
 * (kPhcReadMaxUncertaintyNs). Assumes the PHC rate is steady between the two reads,
 * so a PHC stepped or steered mid-run is not covered. Prints the rate when destroyed. */
class RxPhcClock {
 public:
  RxPhcClock() = default;
  RxPhcClock(const RxPhcClock&) = delete;
  RxPhcClock& operator=(const RxPhcClock&) = delete;
  ~RxPhcClock();
  /* Reads the PHC of port before the stream; a failure is reported by the first frame. */
  void start(const char* port);
  /* False without a NIC timestamp; frame 0 skips unless strictPacingRequired(). */
  bool receiveTimeMonotonicRaw(uint64_t frame_idx, uint64_t receive_timestamp,
                               const char* port, mtl_handle mt, RxTime* rx);

 private:
  double rateOffsetPpm() const;
  int fd = -1;
  bool skipped = false;
  std::string start_error = "start() was not called";
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
  /* Starts the thread that re-reads the PHC. */
  void follow();
  /* Stops following, freezes the offset and prints the read statistics. */
  void stop();
  /* For mtl_init_params.ptp_get_time_fn. */
  static uint64_t now(void* priv);
  /* After stop(): empty if the reads met kPhcFollowMaxLagNs, kPhcFollowMaxGapNs and
   * kPhcFollowMinReadShare, else why not. */
  std::string healthError() const;

 private:
  void run();
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

/* Prints min/avg/max of the packet-0 error series once per run, to calibrate bounds. */
struct PacingErrorLog {
  PacingErrorSeries abs_error_ns;
  PacingErrorSeries rtp_error_ns;
  PacingErrorSeries elapsed_error_ns;
  ~PacingErrorLog();
};

/* Default pacing oracle. Frame n goes out on epoch k0 + n, k0 the epoch frame 0's RTP
 * timestamp is on; its expected launch is st20pRoundedLaunchNs() of that epoch, with
 * the live st20p_tx_get_pacing_params(). Checks on every RX frame n:
 * 1. COMPLETE with the BPM packet count on P            expectCompletePrimaryFrame()
 * 2. timestamp is the media-clock RTP header value      expectRtpTimestamp()
 * 3. RTP == tick90k(expected launch), so the RTP step   expectRtpOnLaunch()
 *    follows the epochs (1501/1502 at 59.94)
 * With a NIC RX timestamp (RxPhcClock), at both ends of its +-u, packet 0 of frame n:
 * 4. launch within kNoCtxPacingEarlyMaxNs..kNoCtxPacingLateMaxNs   expectPacingLaunch()
 * 5. the same against the instant its RTP timestamp names      expectNearRtpTimestamp()
 * 6. elapsed from frame 0 within +-kNoCtxPacingElapsedErrorMaxNs    expectPacingElapsed()
 */
class St20pDefaultPacingOracle : public FrameTestStrategy {
 protected:
  St20pFrameRate rate;
  uint64_t firstEpoch = 0;
  double trOffsetNs = 0, trsNs = 0;
  uint32_t vrxPkts = 0;
  PacingAnchor anchor;
  RxPhcClock rxPhc;
  PacingErrorLog pacingLog;

 public:
  explicit St20pDefaultPacingOracle(St20pHandler* parentHandler = nullptr);
  void rxTestFrameModifier(void* frame, size_t frame_size) override;
};

/* USER_PACING oracle. TX requests t_user(n) = start + epoch n + offset[n % size] * T,
 * start = the epoch at or after PTP now at construction + kSt20pUserPacingLeadNs. The
 * expected launch is st20pRoundedLaunchNs() of the epoch nearest t_user(n)
 * (expectedTransmitTimeNs()), so the test must call getPacingParameters() before
 * frame 0. Checks on every RX frame n:
 * 1. COMPLETE with the BPM packet count on P            expectCompletePrimaryFrame()
 * 2. launch within kNoCtxPacingEarlyMaxNs..kNoCtxPacingLateMaxNs   verifyReceiveTiming()
 * 3. the same against the instant its RTP timestamp names, widened by
 *    rtpSlackNs() either way                            verifyReceiveTiming()
 * 4. elapsed from frame 0 within +-kNoCtxPacingElapsedErrorMaxNs    verifyReceiveTiming()
 * 5. timestamp is the media-clock RTP header value      expectRtpTimestamp()
 * 6. RTP == tick90k(expected launch), so the RTP step   verifyMediaClock()
 *    follows the epochs
 * 2-4 need a NIC RX timestamp (RxPhcClock) and hold at both ends of its +-u.
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
  virtual int64_t rtpSlackNs() const;
  void verifyReceiveTiming(uint64_t frame_idx, const st_frame* frame,
                           uint64_t expected_transmit_time_ns);
  void verifyMediaClock(uint64_t frame_idx, uint64_t timestamp_media_clk,
                        uint64_t expected_media_clk) const;

  St20pFrameRate rate;
  uint64_t startingTime = 0;
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

/* EXACT_USER_PACING oracle: St20pUserPacingOracle with the expected launch = t_user(n)
 * itself, so check 6 is RTP == tick90k(t_user(n)), and check 3 allows
 * kNoCtxRtpHalfTickNs either way, as that tick is the nearest one to t_user(n). */
class St20pExactUserPacingOracle : public St20pUserPacingOracle {
 public:
  explicit St20pExactUserPacingOracle(St20pHandler* parentHandler = nullptr,
                                      std::vector<double> offsetMultipliers = {});

 protected:
  uint64_t expectedTransmitTimeNs(uint64_t frame_idx) const override;
  int64_t rtpSlackNs() const override;
};

/* Interlaced St20pUserPacingOracle, T = field period. The expected launch moves one field
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
