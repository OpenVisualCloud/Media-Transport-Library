/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/* Oracles of the strict ST20p pacing tests: NIC RX timestamps converted from the
 * RX PHC to CLOCK_MONOTONIC_RAW, packet 0 against its planned launch, elapsed time
 * from frame 0 within +-10 us, exact RTP. See README.md, "Timing model".
 */

#include "st20p_strategies.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <linux/ethtool.h>
#include <linux/ptp_clock.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <pthread.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#include "core/constants.hpp"
#include "handlers/st20p_handler.hpp"
#include "tests.hpp"

namespace {
constexpr uint64_t kBpmPayloadBytes = 1260;
constexpr const char* kStrictTopology =
    "strict pacing needs TX and RX on different physical ports, a PHC reachable from "
    "the RX port, and NIC RX timestamps delivered: ";
constexpr int kPhcReadAttempts = 3;
constexpr int kPtpMappingSamples = 4;
constexpr int64_t kPhcFollowIntervalNs = 10 * NS_PER_MS;
constexpr int64_t kPhcFollowMaxSlopePpb = 100 * 1000;

/* PhcFollowingClock::now() - CLOCK_MONOTONIC_RAW is base_offset_ns at base_mono_ns and
 * changes by slope_ppb; a seqlock, so readers spin while the writer is mid-update. */
struct PhcFollowingOffset {
  std::atomic<uint32_t> seq{0};
  std::atomic<uint64_t> base_mono_ns{0};
  std::atomic<int64_t> base_offset_ns{0};
  std::atomic<int64_t> slope_ppb{0};
};
PhcFollowingOffset g_phc_offset;

int64_t phcOffsetAt(uint64_t mono_ns) {
  uint32_t seq;
  uint64_t base_mono_ns;
  int64_t base_offset_ns, slope_ppb;
  do {
    seq = g_phc_offset.seq.load(std::memory_order_acquire);
    base_mono_ns = g_phc_offset.base_mono_ns.load(std::memory_order_relaxed);
    base_offset_ns = g_phc_offset.base_offset_ns.load(std::memory_order_relaxed);
    slope_ppb = g_phc_offset.slope_ppb.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
  } while ((seq & 1) || seq != g_phc_offset.seq.load(std::memory_order_relaxed));
  return base_offset_ns +
         static_cast<int64_t>(mono_ns - base_mono_ns) * slope_ppb / (int64_t)NS_PER_S;
}

/* Only the follower thread writes, or the test thread while it does not run. */
void setPhcOffset(uint64_t base_mono_ns, int64_t base_offset_ns, int64_t slope_ppb) {
  const uint32_t seq = g_phc_offset.seq.load(std::memory_order_relaxed);
  g_phc_offset.seq.store(seq + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  g_phc_offset.base_mono_ns.store(base_mono_ns, std::memory_order_relaxed);
  g_phc_offset.base_offset_ns.store(base_offset_ns, std::memory_order_relaxed);
  g_phc_offset.slope_ppb.store(slope_ppb, std::memory_order_relaxed);
  g_phc_offset.seq.store(seq + 2, std::memory_order_release);
}

uint64_t monotonicRawNs() {
  timespec mono;
  clock_gettime(CLOCK_MONOTONIC_RAW, &mono);
  return static_cast<uint64_t>(mono.tv_sec) * NS_PER_S + mono.tv_nsec;
}

uint32_t expectedBpmPackets(const St20pHandler* handler) {
  const auto& ops = handler->sessionsOpsTx;
  st20_pgroup pg = {};
  EXPECT_EQ(st20_get_pgroup(ops.transport_fmt, &pg), 0);
  if (!pg.coverage) return 0;
  uint64_t frame_bytes = (uint64_t)ops.width * ops.height * pg.size / pg.coverage;
  if (ops.interlaced) frame_bytes /= 2;
  return (frame_bytes + kBpmPayloadBytes - 1) / kBpmPayloadBytes;
}

void expectCompletePrimaryFrame(uint64_t frame_idx, const st_frame* frame,
                                const St20pHandler* handler) {
  const uint32_t expected_packets = expectedBpmPackets(handler);
  EXPECT_EQ(frame->status, ST_FRAME_STATUS_COMPLETE)
      << "frame " << frame_idx << " is not complete";
  EXPECT_EQ(frame->pkts_total, expected_packets)
      << "frame " << frame_idx << " packet total differs from the BPM geometry";
  EXPECT_EQ(frame->pkts_recv[MTL_SESSION_PORT_P], expected_packets)
      << "frame " << frame_idx << " primary packet count differs from the BPM geometry";
}

void expectRtpTimestamp(uint64_t frame_idx, const st_frame* frame) {
  EXPECT_EQ(frame->tfmt, ST10_TIMESTAMP_FMT_MEDIA_CLK)
      << "frame " << frame_idx << " RX timestamp is not a media-clock value";
  EXPECT_EQ(frame->timestamp, static_cast<uint64_t>(frame->rtp_timestamp))
      << "frame " << frame_idx << " timestamp differs from the RTP header";
}

uint64_t ptpClockTimeNs(const ptp_clock_time& t) {
  return static_cast<uint64_t>(t.sec) * NS_PER_S + t.nsec;
}

/* The PF itself, or a VF's parent PF; empty if port is not a PCI device. */
std::string physicalPort(const char* port) {
  const std::string dev = std::string("/sys/bus/pci/devices/") + port;
  char path[PATH_MAX];
  if (realpath((dev + "/physfn").c_str(), path) || realpath(dev.c_str(), path))
    return path;
  return "";
}

/* A VF has no PHC of its own; its RX timestamps come from the parent PF's clock. */
int openPortPhc(const char* port) {
  const std::string pf = physicalPort(port);
  if (pf.empty()) return -1;
  DIR* dir = opendir((pf + "/net").c_str());
  if (!dir) return -1;
  std::string ifname;
  while (const dirent* entry = readdir(dir)) {
    if (entry->d_name[0] != '.') {
      ifname = entry->d_name;
      break;
    }
  }
  closedir(dir);
  if (ifname.empty() || ifname.size() >= IFNAMSIZ) return -1;

  const int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) return -1;
  ethtool_ts_info ts_info = {};
  ts_info.cmd = ETHTOOL_GET_TS_INFO;
  ifreq ifr = {};
  memcpy(ifr.ifr_name, ifname.c_str(), ifname.size());
  ifr.ifr_data = reinterpret_cast<char*>(&ts_info);
  const int ret = ioctl(sock, SIOCETHTOOL, &ifr);
  close(sock);
  if (ret < 0 || ts_info.phc_index < 0) return -1;
  return open(("/dev/ptp" + std::to_string(ts_info.phc_index)).c_str(), O_RDONLY);
}

uint64_t timespecNs(const timespec& t) {
  return static_cast<uint64_t>(t.tv_sec) * NS_PER_S + t.tv_nsec;
}

/* CLOCK_REALTIME minus CLOCK_MONOTONIC_RAW, to +-*half_width_ns. */
int64_t realtimeMinusMonotonicRaw(uint64_t* half_width_ns) {
  timespec before, real, after;
  clock_gettime(CLOCK_MONOTONIC_RAW, &before);
  clock_gettime(CLOCK_REALTIME, &real);
  clock_gettime(CLOCK_MONOTONIC_RAW, &after);
  *half_width_ns = (timespecNs(after) - timespecNs(before)) / 2;
  return static_cast<int64_t>(timespecNs(real) - timespecNs(before) - *half_width_ns);
}

/* A PHC reading and its CLOCK_MONOTONIC_RAW time, to +-*uncertainty_ns. The
 * PTP_SYS_OFFSET_PRECISE cross-timestamp (E830) is exact. Without one (E810,
 * EOPNOTSUPP) the tightest PTP_SYS_OFFSET_EXTENDED CLOCK_REALTIME bracket is mapped to
 * CLOCK_MONOTONIC_RAW; u is half that bracket, half the mapping bracket and half any
 * CLOCK_REALTIME change across the read. Empty on success, otherwise why not. */
std::string readPhc(int fd, uint64_t* phc_ns, uint64_t* mono_ns,
                    uint64_t* uncertainty_ns) {
  ptp_sys_offset_precise xts = {};
  if (ioctl(fd, PTP_SYS_OFFSET_PRECISE, &xts) == 0) {
    *phc_ns = ptpClockTimeNs(xts.device);
    *mono_ns = ptpClockTimeNs(xts.sys_monoraw);
    *uncertainty_ns = 0;
    return "";
  }
  if (errno != EOPNOTSUPP)
    return std::string("PTP_SYS_OFFSET_PRECISE: ") + strerror(errno);

  for (int attempt = 0; attempt < kPhcReadAttempts; attempt++) {
    uint64_t before_half_width, after_half_width;
    const int64_t before = realtimeMinusMonotonicRaw(&before_half_width);
    ptp_sys_offset_extended ext = {};
    ext.n_samples = PTP_MAX_SAMPLES;
    if (ioctl(fd, PTP_SYS_OFFSET_EXTENDED, &ext) < 0)
      return std::string("PTP_SYS_OFFSET_EXTENDED: ") + strerror(errno);
    const int64_t after = realtimeMinusMonotonicRaw(&after_half_width);

    uint64_t width = UINT64_MAX;
    for (unsigned i = 0; i < ext.n_samples; i++) {
      const uint64_t w = ptpClockTimeNs(ext.ts[i][2]) - ptpClockTimeNs(ext.ts[i][0]);
      if (w < width) {
        width = w;
        *phc_ns = ptpClockTimeNs(ext.ts[i][1]);
        *mono_ns = ptpClockTimeNs(ext.ts[i][0]) + w / 2 -
                   static_cast<uint64_t>((before + after) / 2);
      }
    }
    *uncertainty_ns = width / 2 + std::max(before_half_width, after_half_width) +
                      static_cast<uint64_t>(std::llabs(after - before)) / 2;
    if (*uncertainty_ns <= kPhcReadMaxUncertaintyNs) return "";
  }
  return "no PTP_SYS_OFFSET_EXTENDED read within +-" +
         std::to_string(kPhcReadMaxUncertaintyNs) + "ns (last +-" +
         std::to_string(*uncertainty_ns) + "ns)";
}

/* Default pacing stamps RTP at frame TX start: k * frame + tr_offset - vrx * trs. */
void expectRtpOnEpoch(const st_frame* frame, St20pHandler* handler, uint64_t* launch_ns) {
  double tr_offset_ns = 0, trs_ns = 0;
  uint32_t vrx_pkts = 0;
  ASSERT_EQ(st20p_tx_get_pacing_params(handler->sessionsHandleTx, &tr_offset_ns, &trs_ns,
                                       &vrx_pkts),
            0);
  const long double frame_ns =
      static_cast<long double>(NS_PER_S) / st_frame_rate(handler->sessionsOpsTx.fps);
  const long double offset_ns = tr_offset_ns - vrx_pkts * trs_ns;
  const uint64_t rtp_tai = st10_media_clk_to_tai(mtl_ptp_read_time(handler->ctx->handle),
                                                 frame->rtp_timestamp, VIDEO_CLOCK_HZ);
  const long double k = std::round((rtp_tai - offset_ns) / frame_ns);
  *launch_ns = static_cast<uint64_t>(k * frame_ns + offset_ns);
  const uint32_t expected = st10_tai_to_media_clk(*launch_ns, VIDEO_CLOCK_HZ);
  EXPECT_EQ(frame->rtp_timestamp, expected)
      << "frame 0 RTP timestamp is not on an epoch boundary (nearest epoch " << k << ")";
}

void skipOrFailWithoutNicRxTimestamps(const char* port, uint64_t receive_timestamp) {
  const std::string reason = std::string(kStrictTopology) + "the first frame on " + port +
                             " carries the software RX time " +
                             std::to_string(receive_timestamp);
  if (strictPacingRequired()) FAIL() << reason;
  GTEST_SKIP() << reason;
}

void expectPacingLaunch(uint64_t frame_idx, uint64_t receive_ptp_ns,
                        uint64_t expected_launch_ns, int64_t u, PacingErrorLog* log) {
  const int64_t abs_error_ns = static_cast<int64_t>(receive_ptp_ns - expected_launch_ns);
  log->abs_error_ns.add(abs_error_ns);

  EXPECT_GE(abs_error_ns - u, -kNoCtxPacingEarlyMaxNs)
      << "frame " << frame_idx << ": packet 0 abs_error=" << abs_error_ns << "+-" << u
      << "ns measured=" << receive_ptp_ns << "ns expected launch=" << expected_launch_ns
      << "ns";
  EXPECT_LE(abs_error_ns + u, kNoCtxPacingLateMaxNs)
      << "frame " << frame_idx << ": packet 0 abs_error=" << abs_error_ns << "+-" << u
      << "ns measured=" << receive_ptp_ns << "ns expected launch=" << expected_launch_ns
      << "ns";
}

void expectPacingElapsed(uint64_t frame_idx, const RxTime& rx, const PacingAnchor& anchor,
                         uint64_t expected_ns, PacingErrorLog* log) {
  ASSERT_GE(rx.ns, anchor.rx.ns)
      << "NIC RX timestamp moved backwards at frame " << frame_idx;
  ASSERT_GE(expected_ns, anchor.expected_ns)
      << "expected TX plan moved backwards at frame " << frame_idx;

  const int64_t measured_elapsed_ns = static_cast<int64_t>(rx.ns - anchor.rx.ns);
  const int64_t expected_elapsed_ns =
      static_cast<int64_t>(expected_ns - anchor.expected_ns);
  const int64_t elapsed_error_ns = measured_elapsed_ns - expected_elapsed_ns;
  const int64_t u = rx.u_ns + anchor.rx.u_ns;
  log->elapsed_error_ns.add(elapsed_error_ns);

  EXPECT_GE(elapsed_error_ns - u, -kNoCtxPacingElapsedErrorMaxNs)
      << "frame " << frame_idx << ": measured elapsed=" << measured_elapsed_ns << "+-"
      << u << "ns expected elapsed=" << expected_elapsed_ns << "ns";
  EXPECT_LE(elapsed_error_ns + u, kNoCtxPacingElapsedErrorMaxNs)
      << "frame " << frame_idx << ": measured elapsed=" << measured_elapsed_ns << "+-"
      << u << "ns expected elapsed=" << expected_elapsed_ns << "ns";
}

/* Packet-0 launch and elapsed checks; the first frame checked becomes the anchor. */
void expectPacing(uint64_t frame_idx, mtl_handle mt, const RxTime& rx,
                  uint64_t expected_ns, PacingAnchor* anchor, PacingErrorLog* log) {
  const RxTime ptp = monotonicRawToPtp(mt, rx);
  expectPacingLaunch(frame_idx, ptp.ns, expected_ns, ptp.u_ns, log);
  if (!anchor->rx.ns) {
    *anchor = {rx, expected_ns};
    return;
  }
  expectPacingElapsed(frame_idx, rx, *anchor, expected_ns, log);
}
}  // namespace

std::string strictPacingTopologyError(const char* tx_port, const char* rx_port) {
  const std::string tx_pf = physicalPort(tx_port);
  if (!tx_pf.empty() && tx_pf == physicalPort(rx_port))
    return std::string(kStrictTopology) + "TX " + tx_port + " and RX " + rx_port +
           " share one physical port";

  const int fd = openPortPhc(rx_port);
  if (fd < 0)
    return std::string(kStrictTopology) + "no PHC found for RX " + rx_port +
           " (its PF must stay bound to its kernel driver)";
  uint64_t phc_ns, mono_ns, uncertainty_ns;
  const std::string why = readPhc(fd, &phc_ns, &mono_ns, &uncertainty_ns);
  close(fd);
  if (!why.empty())
    return std::string(kStrictTopology) + "cannot read the PHC of RX " + rx_port + ": " +
           why;
  return "";
}

bool strictPacingRequired() {
  const char* required = getenv("NOCTX_REQUIRE_STRICT");
  return required && !strcmp(required, "1");
}

std::string sharedPhcError(const char* port_a, const char* port_b) {
  const int a = openPortPhc(port_a), b = openPortPhc(port_b);
  struct stat stat_a, stat_b;
  const bool shared = a >= 0 && b >= 0 && !fstat(a, &stat_a) && !fstat(b, &stat_b) &&
                      stat_a.st_rdev == stat_b.st_rdev;
  if (a >= 0) close(a);
  if (b >= 0) close(b);
  if (shared) return "";
  return std::string(kStrictTopology) + "RX " + port_a + " and RX " + port_b +
         " must timestamp on one PHC";
}

PhcFollowingClock::~PhcFollowingClock() {
  stop();
  if (fd >= 0) close(fd);
}

std::string PhcFollowingClock::open(const char* port, int64_t ptp_minus_phc) {
  ptp_minus_phc_ns = ptp_minus_phc;
  fd = openPortPhc(port);
  if (fd < 0) return std::string("no PHC found for ") + port;
  uint64_t phc_ns, mono_ns, uncertainty_ns;
  const std::string why = readPhc(fd, &phc_ns, &mono_ns, &uncertainty_ns);
  if (!why.empty()) return std::string("cannot read the PHC of ") + port + ": " + why;
  setPhcOffset(mono_ns, static_cast<int64_t>(phc_ns - mono_ns) + ptp_minus_phc_ns, 0);
  return "";
}

std::string PhcFollowingClock::follow() {
  const int cpu = sched_getcpu();
  if (cpu < 0) return std::string("sched_getcpu: ") + strerror(errno);
  follow_mono_ns = monotonicRawNs();
  std::promise<int> pinned;
  std::future<int> pin_ret = pinned.get_future();
  thread = std::thread(&PhcFollowingClock::run, this, cpu, std::move(pinned));
  const int ret = pin_ret.get();
  if (ret)
    return "cannot pin the PHC follower to CPU " + std::to_string(cpu) + ": " +
           strerror(ret);
  return "";
}

void PhcFollowingClock::stop() {
  stopping.store(true, std::memory_order_relaxed);
  if (!thread.joinable()) return;
  thread.join();
  stop_mono_ns = monotonicRawNs();
  setPhcOffset(stop_mono_ns, phcOffsetAt(stop_mono_ns), 0);
  fprintf(stderr,
          "NoCtx PHC follower: %llu reads in %.1f s, %llu failed, lag max %llu ns, gap "
          "max %.1f ms\n",
          (unsigned long long)reads, (double)(stop_mono_ns - follow_mono_ns) / NS_PER_S,
          (unsigned long long)failed_reads, (unsigned long long)max_lag_ns,
          (double)max_gap_ns / NS_PER_MS);
}

uint64_t PhcFollowingClock::now(void* /*priv*/) {
  const uint64_t mono_ns = monotonicRawNs();
  return mono_ns + phcOffsetAt(mono_ns);
}

std::string PhcFollowingClock::healthError() const {
  const uint64_t slots = (stop_mono_ns - follow_mono_ns) / kPhcFollowIntervalNs;
  if (max_lag_ns <= kPhcFollowMaxLagNs && max_gap_ns <= kPhcFollowMaxGapNs &&
      reads >= kPhcFollowMinReadShare * slots)
    return "";
  return "MTL's PTP time did not follow the RX PHC: " + std::to_string(reads) +
         " reads in " + std::to_string(slots) + " 10 ms slots, " +
         std::to_string(failed_reads) + " failed, lag max " + std::to_string(max_lag_ns) +
         " ns, gap max " + std::to_string(max_gap_ns) +
         " ns; the PHC was stepped or drifts > 100 ppm, its reads failed, or the "
         "follower thread was starved";
}

void PhcFollowingClock::run(int cpu, std::promise<int> pinned) {
  cpu_set_t cpus;
  CPU_ZERO(&cpus);
  CPU_SET(cpu, &cpus);
  const int pin_ret = pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
  pinned.set_value(pin_ret);
  if (pin_ret) return;
  const auto interval = std::chrono::nanoseconds(kPhcFollowIntervalNs);
  auto next = std::chrono::steady_clock::now();
  while (!stopping.load(std::memory_order_relaxed)) {
    next = std::max(next + interval, std::chrono::steady_clock::now());
    std::this_thread::sleep_until(next);
    uint64_t phc_ns, mono_ns, uncertainty_ns;
    if (!readPhc(fd, &phc_ns, &mono_ns, &uncertainty_ns).empty()) {
      failed_reads++;
      continue;
    }
    if (last_read_mono_ns) max_gap_ns = std::max(max_gap_ns, mono_ns - last_read_mono_ns);
    last_read_mono_ns = mono_ns;
    reads++;
    const int64_t offset = phcOffsetAt(mono_ns);
    const int64_t lag =
        static_cast<int64_t>(phc_ns - mono_ns) + ptp_minus_phc_ns - offset;
    max_lag_ns = std::max(max_lag_ns, static_cast<uint64_t>(std::llabs(lag)));
    const int64_t max_lag = kPhcFollowMaxSlopePpb * kPhcFollowIntervalNs / NS_PER_S;
    const int64_t slew = std::max(-max_lag, std::min(lag, max_lag));
    setPhcOffset(mono_ns, offset, slew * (int64_t)NS_PER_S / kPhcFollowIntervalNs);
  }
}

RxTime monotonicRawToPtp(mtl_handle mt, const RxTime& rx) {
  uint64_t width = UINT64_MAX, mono_minus_ptp = 0;
  for (int i = 0; i < kPtpMappingSamples; i++) {
    timespec before, after;
    clock_gettime(CLOCK_MONOTONIC_RAW, &before);
    const uint64_t ptp_now = mtl_ptp_read_time_raw(mt);
    clock_gettime(CLOCK_MONOTONIC_RAW, &after);
    const uint64_t w = timespecNs(after) - timespecNs(before);
    if (w < width) {
      width = w;
      mono_minus_ptp = timespecNs(before) + w / 2 - ptp_now;
    }
  }
  return {rx.ns - mono_minus_ptp, rx.u_ns + static_cast<int64_t>(width / 2)};
}

void PacingErrorSeries::add(int64_t error_ns) {
  min = std::min(min, error_ns);
  max = std::max(max, error_ns);
  sum += error_ns;
  count++;
}

PacingErrorLog::~PacingErrorLog() {
  if (!abs_error_ns.count) return;
  const PacingErrorSeries& a = abs_error_ns;
  const PacingErrorSeries& e = elapsed_error_ns;
  fprintf(stderr,
          "NoCtx pacing: %llu frames, abs_error_ns min/avg/max %lld/%lld/%lld, "
          "elapsed_error_ns min/avg/max %lld/%lld/%lld\n",
          (unsigned long long)a.count, (long long)a.min,
          (long long)(a.sum / (int64_t)a.count), (long long)a.max,
          (long long)(e.count ? e.min : 0),
          (long long)(e.count ? e.sum / (int64_t)e.count : 0),
          (long long)(e.count ? e.max : 0));
}

RxPhcClock::~RxPhcClock() {
  if (fd >= 0) close(fd);
  if (last_mono_ns > first_mono_ns)
    fprintf(stderr, "NoCtx RX PHC: rate %+.3f ppm over %.1f s, u max %llu ns\n",
            rateOffsetPpm(), (double)(last_mono_ns - first_mono_ns) / NS_PER_S,
            (unsigned long long)uncertainty_max_ns);
}

double RxPhcClock::rateOffsetPpm() const {
  const double mono = static_cast<double>(last_mono_ns - first_mono_ns);
  return (static_cast<double>(last_phc_ns - first_phc_ns) - mono) / mono * 1e6;
}

/* Like the other checks, the rate must hold at both ends of the two reads' +-u. */
void RxPhcClock::expectRate(const char* port, uint64_t uncertainty_ns) {
  const uint64_t mono = last_mono_ns - first_mono_ns;
  if (rate_failed || mono < NS_PER_S) return;
  const double u_ppm = (first_uncertainty_ns + uncertainty_ns) * 1e6 / mono;
  if (std::fabs(rateOffsetPpm()) + u_ppm <= kPhcRateMaxPpm) return;
  rate_failed = true;
  ADD_FAILURE() << "the PHC of " << port << " runs " << rateOffsetPpm() << "+-" << u_ppm
                << " ppm off CLOCK_MONOTONIC_RAW, over " << kPhcRateMaxPpm << " ppm";
}

bool RxPhcClock::receiveTimeMonotonicRaw(uint64_t frame_idx, uint64_t receive_timestamp,
                                         const char* port, mtl_handle mt, RxTime* rx) {
  if (skipped) return false;
  if (fd < 0) fd = openPortPhc(port);
  uint64_t phc_now, mono_now, uncertainty_ns;
  const std::string why =
      fd < 0 ? "no PHC found" : readPhc(fd, &phc_now, &mono_now, &uncertainty_ns);
  if (!why.empty()) {
    ADD_FAILURE() << "frame " << frame_idx << ": cannot read the PHC of " << port << ": "
                  << why;
    return false;
  }

  if (!first_mono_ns) {
    first_phc_ns = phc_now;
    first_mono_ns = mono_now;
    first_uncertainty_ns = uncertainty_ns;
  }
  last_phc_ns = phc_now;
  last_mono_ns = mono_now;
  uncertainty_max_ns = std::max(uncertainty_max_ns, uncertainty_ns);
  expectRate(port, uncertainty_ns);

  /* A VF never steers its PF's PHC, so a NIC timestamp is a recent PHC reading. */
  const uint64_t ts = receive_timestamp;
  if (ts <= phc_now && phc_now - ts < NS_PER_S) {
    *rx = {ts - (phc_now - mono_now), static_cast<int64_t>(uncertainty_ns)};
    return true;
  }

  const uint64_t ptp_now = mtl_ptp_read_time(mt);
  if (!frame_idx && ts <= ptp_now && ptp_now - ts < NS_PER_S) {
    skipped = true;
    skipOrFailWithoutNicRxTimestamps(port, ts);
  } else {
    ADD_FAILURE() << "frame " << frame_idx << ": receive_timestamp " << ts
                  << " is not a NIC RX timestamp from the last second of the PHC of "
                  << port << " (PHC " << phc_now << ", PTP " << ptp_now << ")";
  }
  return false;
}

St20pDefaultPacingOracle::St20pDefaultPacingOracle(St20pHandler* parentHandler)
    : FrameTestStrategy(parentHandler, false, true) {
}

void St20pDefaultPacingOracle::rxTestFrameModifier(void* frame, size_t /*frame_size*/) {
  auto* f = static_cast<st_frame*>(frame);
  auto* st20pParent = static_cast<St20pHandler*>(parent);
  uint64_t framebuffTime =
      st10_tai_to_media_clk(st20pParent->nsFrameTime, VIDEO_CLOCK_HZ);

  expectCompletePrimaryFrame(idx_rx, f, st20pParent);
  expectRtpTimestamp(idx_rx, f);
  if (idx_rx == 0) expectRtpOnEpoch(f, st20pParent, &firstLaunchNs);

  if (lastTimestamp != 0) {
    uint64_t diff = f->timestamp - lastTimestamp;
    EXPECT_TRUE(diff == framebuffTime) << " idx_rx: " << idx_rx << " diff: " << diff;
  }

  RxTime rx;
  if (rxPhc.receiveTimeMonotonicRaw(
          idx_rx, f->receive_timestamp,
          st20pParent->sessionsOpsRx.port.port[MTL_SESSION_PORT_P],
          st20pParent->ctx->handle, &rx))
    expectPacing(idx_rx, st20pParent->ctx->handle, rx,
                 firstLaunchNs + idx_rx * st20pParent->nsFrameTime, &anchor, &pacingLog);

  lastTimestamp = f->timestamp;
  idx_rx++;
}

St20pUserPacingOracle::St20pUserPacingOracle(St20pHandler* parentHandler,
                                             std::vector<double> offsetMultipliers)
    : FrameTestStrategy(parentHandler, true, true),
      timestampOffsetMultipliers(std::move(offsetMultipliers)) {
  initializeTiming(parentHandler);
}

int St20pUserPacingOracle::getPacingParameters() {
  auto* parentHandler = static_cast<St20pHandler*>(parent);
  if (parentHandler && parentHandler->sessionsHandleTx) {
    return st20p_tx_get_pacing_params(parentHandler->sessionsHandleTx,
                                      &pacing_tr_offset_ns, &pacing_trs_ns,
                                      &pacing_vrx_pkts);
  }

  return -1;
}

void St20pUserPacingOracle::txTestFrameModifier(void* frame, size_t /*frame_size*/) {
  auto* f = static_cast<st_frame*>(frame);
  f->tfmt = ST10_TIMESTAMP_FMT_TAI;
  f->timestamp = plannedTimestampNs(idx_tx);
  idx_tx++;
}

void St20pUserPacingOracle::rxTestFrameModifier(void* frame, size_t /*frame_size*/) {
  auto* f = static_cast<st_frame*>(frame);
  const uint64_t frame_idx = idx_rx++;

  const uint64_t expected_transmit_time_ns = expectedTransmitTimeNs(frame_idx);
  const uint64_t expected_media_clk =
      st10_tai_to_media_clk(expected_transmit_time_ns, VIDEO_CLOCK_HZ);

  expectCompletePrimaryFrame(frame_idx, f, static_cast<St20pHandler*>(parent));
  verifyReceiveTiming(frame_idx, f, expected_transmit_time_ns);
  expectRtpTimestamp(frame_idx, f);
  verifyMediaClock(frame_idx, f->timestamp, expected_media_clk);
  verifyTimestampStep(frame_idx, f->timestamp);

  lastTimestamp = f->timestamp;
}

uint64_t St20pUserPacingOracle::plannedTimestampNs(uint64_t frame_idx) const {
  uint64_t base = plannedTimestampBaseNs(frame_idx);
  int64_t offset = frameTimeNs * offsetMultiplierForFrame(frame_idx);
  int64_t adjusted = base + offset;
  return adjusted < 0 ? 0 : (adjusted);
}

uint64_t St20pUserPacingOracle::plannedTimestampBaseNs(uint64_t frame_idx) const {
  int64_t base = startingTime + frame_idx * frameTimeNs;
  return base < 0 ? 0 : base;
}

double St20pUserPacingOracle::offsetMultiplierForFrame(uint64_t frame_idx) const {
  if (timestampOffsetMultipliers.empty()) {
    return 0;
  }

  size_t loop_idx = frame_idx % timestampOffsetMultipliers.size();
  return timestampOffsetMultipliers[loop_idx];
}

uint64_t St20pUserPacingOracle::expectedTransmitTimeNs(uint64_t frame_idx) const {
  /* snap the requested TAI to the epoch the transmitter will pick */
  const double requested_ts = static_cast<double>(plannedTimestampNs(frame_idx));
  const double snapped_epoch =
      std::floor((requested_ts + frameTimeNs / 2.0) / frameTimeNs) * frameTimeNs;

  const double pacing_adjustment =
      pacing_tr_offset_ns - static_cast<double>(pacing_vrx_pkts) * pacing_trs_ns;

  const double expected = snapped_epoch + pacing_adjustment;
  return expected <= 0.0 ? 0 : static_cast<uint64_t>(expected);
}

void St20pUserPacingOracle::verifyReceiveTiming(uint64_t frame_idx, const st_frame* frame,
                                                uint64_t expected_transmit_time_ns) {
  RxTime rx;
  auto* handler = static_cast<St20pHandler*>(parent);
  if (rxPhc.receiveTimeMonotonicRaw(frame_idx, frame->receive_timestamp,
                                    handler->sessionsOpsRx.port.port[MTL_SESSION_PORT_P],
                                    handler->ctx->handle, &rx))
    expectPacing(frame_idx, handler->ctx->handle, rx, expected_transmit_time_ns, &anchor,
                 &pacingLog);
}

void St20pUserPacingOracle::verifyMediaClock(uint64_t frame_idx,
                                             uint64_t timestamp_media_clk,
                                             uint64_t expected_media_clk) const {
  EXPECT_EQ(timestamp_media_clk, expected_media_clk)
      << " idx_rx: " << frame_idx << "expected media clk: " << expected_media_clk
      << " received timestamp: " << timestamp_media_clk;
}

void St20pUserPacingOracle::verifyTimestampStep(uint64_t frame_idx,
                                                uint64_t current_timestamp) {
  if (!lastTimestamp) {
    return;
  }

  double current_target = plannedTimestampBaseNs(frame_idx);
  double previous_target = plannedTimestampBaseNs(frame_idx ? frame_idx - 1 : 0);
  double expected_step_ns = current_target - previous_target;
  if (expected_step_ns < 0.0) {
    expected_step_ns = 0.0;
  }

  uint64_t expected_step_input = static_cast<uint64_t>(expected_step_ns);
  const uint64_t expected_step =
      st10_tai_to_media_clk(expected_step_input, VIDEO_CLOCK_HZ);
  const uint64_t diff = current_timestamp - lastTimestamp;
  EXPECT_EQ(diff, expected_step) << " idx_rx: " << frame_idx << " diff: " << diff;
}

void St20pUserPacingOracle::initializeTiming(St20pHandler* handler) {
  if (!handler) {
    throw std::invalid_argument("St20pUserPacingOracle expects a valid handler");
  }

  frameTimeNs = handler->nsFrameTime;

  if (!frameTimeNs) {
    double framerate = st_frame_rate(handler->sessionsOpsTx.fps);
    if (framerate > 0.0) {
      long double frame_time = static_cast<long double>(NS_PER_S) / framerate;
      frameTimeNs = static_cast<uint64_t>(frame_time + 0.5L);
    }
  }

  if (!frameTimeNs) {
    frameTimeNs = NS_PER_S / 25;
  }

  /* The fake PTP clock starts at init, so plan from now rather than from zero. */
  const uint64_t frame_ns = static_cast<uint64_t>(frameTimeNs);
  const uint64_t now = mtl_ptp_read_time(handler->ctx->handle);
  startingTime = (now + kSt20pUserPacingLeadNs + frame_ns - 1) / frame_ns * frame_ns;
}

St20pRedundantStreamPlan::St20pRedundantStreamPlan(unsigned int latency,
                                                   St20pHandler* parentHandler)
    : St20pUserPacingOracle(parentHandler), latencyInMs(latency) {
  startingTime = (kSt20pRedundantStartMs + latencyInMs) * NS_PER_MS;
}

void St20pRedundantStreamPlan::rxTestFrameModifier(void* /*frame*/,
                                                   size_t /*frame_size*/) {
  idx_rx++;
}

St20pExactUserPacingOracle::St20pExactUserPacingOracle(
    St20pHandler* parentHandler, std::vector<double> offsetMultipliers)
    : St20pUserPacingOracle(parentHandler, std::move(offsetMultipliers)) {
}

uint64_t St20pExactUserPacingOracle::expectedTransmitTimeNs(uint64_t frame_idx) const {
  return plannedTimestampNs(frame_idx);
}

void St20pExactUserPacingOracle::verifyTimestampStep(uint64_t /*frame_idx*/,
                                                     uint64_t /*current_timestamp*/) {
  /* Exact pacing uses user-provided deltas; no fixed increment enforced here. */
}

/* Frame grid: a first field takes an even slot, a second field an odd one. */
uint64_t St20pInterlacedUserPacingOracle::expectedTransmitTimeNs(
    uint64_t frame_idx) const {
  const uint64_t frame_ns = static_cast<uint64_t>(frameTimeNs);
  const uint64_t epoch = (plannedTimestampNs(frame_idx) + frame_ns / 2) / frame_ns;
  const bool second_field = frame_idx & 1;
  const uint64_t expected = St20pUserPacingOracle::expectedTransmitTimeNs(frame_idx);
  return (epoch & 1) == second_field ? expected : expected + frame_ns;
}

void St20pInterlacedUserPacingOracle::rxTestFrameModifier(void* frame,
                                                          size_t frame_size) {
  auto* f = static_cast<st_frame*>(frame);
  EXPECT_TRUE(f->interlaced) << "received field was reported as progressive";
  EXPECT_EQ(f->second_field, (idx_rx & 1) != 0)
      << "field " << idx_rx << " has the wrong first/second-field identity";
  St20pUserPacingOracle::rxTestFrameModifier(frame, frame_size);
}

St20pVrxRecorder::St20pVrxRecorder(St20pHandler* parentHandler, int64_t ptp_minus_phc)
    : FrameTestStrategy(parentHandler, false, true), ptp_minus_phc_ns(ptp_minus_phc) {
}

void St20pVrxRecorder::rxTestFrameModifier(void* frame, size_t /*frame_size*/) {
  auto* f = static_cast<st_frame*>(frame);
  auto* handler = static_cast<St20pHandler*>(parent);
  if (!first_rx_ns) first_rx_ns = f->receive_timestamp;
  const uint64_t t = f->receive_timestamp - first_rx_ns;
  if (t < (uint64_t)kSt20pVrxWarmupS * NS_PER_S ||
      t >= (uint64_t)(kSt20pVrxWarmupS + kSt20pVrxWindowS) * NS_PER_S)
    return;
  delivery_ns.push_back(static_cast<int64_t>(mtl_ptp_read_time(handler->ctx->handle) -
                                             f->receive_timestamp));

  /* The parser's epochs come from its packet times: on the PHC they lag the TX epochs,
   * and so the RTP, by ptp_minus_phc_ns; software times are PTP times and do not. */
  const double frame_ticks = VIDEO_CLOCK_HZ / st_frame_rate(handler->sessionsOpsRx.fps);
  const double shift_ticks = (double)ptp_minus_phc_ns * VIDEO_CLOCK_HZ / NS_PER_S;
  for (int port = 0; port < MTL_SESSION_PORT_MAX; port++) {
    const st20_rx_tp_meta* tp = f->tp[port];
    if (!tp || !tp->pkts_cnt) continue;
    if (std::fabs(tp->rtp_offset) < frame_ticks) {
      sw_rx_frames[port]++;
      continue;
    }
    if (std::fabs(tp->rtp_offset - shift_ticks) >= frame_ticks / 2) {
      off_band_frames[port]++;
      continue;
    }
    vrx_min[port].push_back(tp->vrx_min);
    const uint32_t missing =
        f->pkts_total > tp->pkts_cnt ? f->pkts_total - tp->pkts_cnt : 0;
    max_missing_pkts[port] = std::max(max_missing_pkts[port], missing);
    if (missing > kSt20pVrxMaxLegLagPkts) partial_frames[port]++;
  }
}
