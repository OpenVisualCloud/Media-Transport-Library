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
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
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
                        uint64_t expected_launch_ns, PacingErrorLog* log) {
  const int64_t abs_error_ns = static_cast<int64_t>(receive_ptp_ns - expected_launch_ns);
  log->abs_error_ns.add(abs_error_ns);

  EXPECT_GE(abs_error_ns, -kNoCtxPacingEarlyMaxNs)
      << "frame " << frame_idx << ": packet 0 abs_error=" << abs_error_ns
      << "ns measured=" << receive_ptp_ns << "ns expected launch=" << expected_launch_ns
      << "ns";
  EXPECT_LE(abs_error_ns, kNoCtxPacingLateMaxNs)
      << "frame " << frame_idx << ": packet 0 abs_error=" << abs_error_ns
      << "ns measured=" << receive_ptp_ns << "ns expected launch=" << expected_launch_ns
      << "ns";
}

void expectPacingElapsed(uint64_t frame_idx, uint64_t receive_time_ns,
                         uint64_t receive_anchor_ns, uint64_t expected_transmit_time_ns,
                         uint64_t expected_anchor_ns, PacingErrorLog* log) {
  ASSERT_GE(receive_time_ns, receive_anchor_ns)
      << "NIC RX timestamp moved backwards at frame " << frame_idx;
  ASSERT_GE(expected_transmit_time_ns, expected_anchor_ns)
      << "expected TX plan moved backwards at frame " << frame_idx;

  const int64_t measured_elapsed_ns =
      static_cast<int64_t>(receive_time_ns - receive_anchor_ns);
  const int64_t expected_elapsed_ns =
      static_cast<int64_t>(expected_transmit_time_ns - expected_anchor_ns);
  const int64_t elapsed_error_ns = measured_elapsed_ns - expected_elapsed_ns;
  log->elapsed_error_ns.add(elapsed_error_ns);

  EXPECT_GE(elapsed_error_ns, -kNoCtxPacingElapsedErrorMaxNs)
      << "frame " << frame_idx << ": measured elapsed=" << measured_elapsed_ns
      << "ns expected elapsed=" << expected_elapsed_ns << "ns";
  EXPECT_LE(elapsed_error_ns, kNoCtxPacingElapsedErrorMaxNs)
      << "frame " << frame_idx << ": measured elapsed=" << measured_elapsed_ns
      << "ns expected elapsed=" << expected_elapsed_ns << "ns";
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
  ptp_sys_offset_precise xts = {};
  const int ret = ioctl(fd, PTP_SYS_OFFSET_PRECISE, &xts);
  close(fd);
  if (ret < 0)
    return std::string(kStrictTopology) + "the PHC of RX " + rx_port +
           " does not support PTP_SYS_OFFSET_PRECISE";
  return "";
}

bool strictPacingRequired() {
  const char* required = getenv("NOCTX_REQUIRE_STRICT");
  return required && !strcmp(required, "1");
}

uint64_t monotonicRawToPtp(mtl_handle mt, uint64_t mono_ns) {
  timespec now;
  clock_gettime(CLOCK_MONOTONIC_RAW, &now);
  const uint64_t ptp_now = mtl_ptp_read_time_raw(mt);
  const uint64_t mono_now = (uint64_t)now.tv_sec * NS_PER_S + now.tv_nsec;
  return mono_ns - (mono_now - ptp_now);
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
}

bool RxPhcClock::receiveTimeMonotonicRaw(uint64_t frame_idx, uint64_t receive_timestamp,
                                         const char* port, mtl_handle mt,
                                         uint64_t* mono_ns) {
  if (skipped) return false;
  if (fd < 0) fd = openPortPhc(port);
  ptp_sys_offset_precise xts = {};
  if (fd < 0 || ioctl(fd, PTP_SYS_OFFSET_PRECISE, &xts) < 0) {
    ADD_FAILURE() << "frame " << frame_idx << ": cannot cross-timestamp the PHC of "
                  << port;
    return false;
  }

  /* A VF never steers its PF's PHC, so a NIC timestamp is a recent PHC reading. */
  const uint64_t phc_now = ptpClockTimeNs(xts.device);
  const uint64_t ts = receive_timestamp;
  if (ts <= phc_now && phc_now - ts < NS_PER_S) {
    *mono_ns = ts - (phc_now - ptpClockTimeNs(xts.sys_monoraw));
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

  uint64_t receive_time_ns;
  if (rxPhc.receiveTimeMonotonicRaw(
          idx_rx, f->receive_timestamp,
          st20pParent->sessionsOpsRx.port.port[MTL_SESSION_PORT_P],
          st20pParent->ctx->handle, &receive_time_ns)) {
    expectPacingLaunch(idx_rx,
                       monotonicRawToPtp(st20pParent->ctx->handle, receive_time_ns),
                       firstLaunchNs + idx_rx * st20pParent->nsFrameTime, &pacingLog);
    if (receiveAnchorTimestamp)
      expectPacingElapsed(idx_rx, receive_time_ns, receiveAnchorTimestamp,
                          idx_rx * st20pParent->nsFrameTime, 0, &pacingLog);
    else
      receiveAnchorTimestamp = receive_time_ns;
  }

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
  uint64_t receive_time_ns;
  auto* handler = static_cast<St20pHandler*>(parent);
  if (!rxPhc.receiveTimeMonotonicRaw(frame_idx, frame->receive_timestamp,
                                     handler->sessionsOpsRx.port.port[MTL_SESSION_PORT_P],
                                     handler->ctx->handle, &receive_time_ns))
    return;

  expectPacingLaunch(frame_idx, monotonicRawToPtp(handler->ctx->handle, receive_time_ns),
                     expected_transmit_time_ns, &pacingLog);
  if (receiveAnchorTimestamp) {
    expectPacingElapsed(frame_idx, receive_time_ns, receiveAnchorTimestamp,
                        expected_transmit_time_ns, expectedAnchorTime, &pacingLog);
  } else {
    receiveAnchorTimestamp = receive_time_ns;
    expectedAnchorTime = expected_transmit_time_ns;
  }
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
