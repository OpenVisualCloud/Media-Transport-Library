/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/* Oracles of the ST30p pacing and redundancy tests. See README.md, "Test catalogue".
 */

#include "st30p_strategies.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <stdexcept>

#include "handlers/st30p_handler.hpp"
#include "tests.hpp"

namespace {
/* Normal ST30 labels frame 0 with the packet-grid point it is sent at, not PTP zero. */
void expectFirstFrameOnPacketGrid(const st30_frame* f, const St30pHandler* handler) {
  const uint32_t sampling = st30_get_sample_rate(handler->sessionsOpsRx.sampling);
  const uint64_t packet_ns = st30_get_packet_time(handler->sessionsOpsRx.ptime);
  const uint64_t rtp_tai =
      st10_media_clk_to_tai(f->receive_timestamp, f->timestamp, sampling);
  EXPECT_EQ(rtp_tai % packet_ns, 0u)
      << "frame 0 RTP time " << rtp_tai << " is off the " << packet_ns << " ns grid";
  ASSERT_LE(rtp_tai, f->receive_timestamp)
      << "frame 0 RTP time " << rtp_tai << " is after its receive time";
  EXPECT_LT(f->receive_timestamp - rtp_tai, handler->nsPacketTime)
      << "frame 0 RTP time " << rtp_tai << " is a frame or more before its receive time "
      << f->receive_timestamp;
}
}  // namespace

St30pDefaultTimestamp::St30pDefaultTimestamp(St30pHandler* parentHandler)
    : FrameTestStrategy(parentHandler, false, true), lastTimestamp(0) {
  idx_tx = 0;
  idx_rx = 0;
}

void St30pDefaultTimestamp::rxTestFrameModifier(void* frame, size_t /*frame_size*/) {
  auto* f = static_cast<st30_frame*>(frame);
  auto* st30pParent = static_cast<St30pHandler*>(parent);
  uint64_t sampling = st30_get_sample_rate(st30pParent->sessionsOpsRx.sampling);
  uint64_t framebuffTime = st10_tai_to_media_clk(st30pParent->nsPacketTime, sampling);

  if (idx_rx == 0) expectFirstFrameOnPacketGrid(f, st30pParent);
  if (lastTimestamp != 0) {
    uint64_t diff = f->timestamp - lastTimestamp;
    EXPECT_TRUE(diff == framebuffTime) << " idx_rx: " << idx_rx << " diff: " << diff;
  }

  lastTimestamp = f->timestamp;
  idx_rx++;
}

St30pUserTimestamp::St30pUserTimestamp(St30pHandler* parentHandler)
    : St30pDefaultTimestamp(parentHandler) {
  enable_tx_modifier = true;
  enable_rx_modifier = true;
}

void St30pUserTimestamp::txTestFrameModifier(void* frame, size_t /*frame_size*/) {
  auto* f = static_cast<st30_frame*>(frame);
  auto* st30pParent = static_cast<St30pHandler*>(parent);
  ASSERT_NE(st30pParent, nullptr);
  ASSERT_TRUE(timingInitialized)
      << "Call St30pUserTimestamp::initializeTiming from the test before sending frames";
  f->tfmt = ST10_TIMESTAMP_FMT_TAI;
  f->timestamp = plannedTimestampNs(idx_tx);
  idx_tx++;
}

void St30pUserTimestamp::rxTestFrameModifier(void* frame, size_t /*frame_size*/) {
  auto* f = static_cast<st30_frame*>(frame);
  auto* st30pParent = static_cast<St30pHandler*>(parent);
  ASSERT_NE(st30pParent, nullptr);
  ASSERT_TRUE(timingInitialized) << "Call St30pUserTimestamp::initializeTiming from the "
                                    "test before validating frames";

  const uint64_t frame_idx = idx_rx++;
  const uint64_t expected_timestamp_ns = plannedTimestampNs(frame_idx);
  const uint64_t sampling = st30_get_sample_rate(st30pParent->sessionsOpsRx.sampling);
  const uint64_t expected_media_clk =
      st10_tai_to_media_clk(expected_timestamp_ns, sampling);

  verifyReceiveTiming(frame_idx, f, expected_timestamp_ns);
  verifyMediaClock(frame_idx, f->timestamp, expected_media_clk);
  verifyTimestampStep(frame_idx, f->timestamp, sampling);

  lastTimestamp = f->timestamp;
}

void St30pUserTimestamp::initializeTiming(St30pHandler* handler) {
  if (!handler) {
    throw std::invalid_argument("St30pUserTimestamp expects a valid handler");
  }
  if (timingInitialized) {
    return;
  }

  frameTimeNs = handler->nsPacketTime;
  if (!frameTimeNs) {
    auto& ops = handler->sessionsOpsTx;
    uint64_t packet_time = st30_get_packet_time(ops.ptime);
    uint64_t packet_size =
        st30_get_packet_size(ops.fmt, ops.ptime, ops.sampling, ops.channel);
    uint64_t packets_per_frame = 0;
    if (packet_size) {
      packets_per_frame = ops.framebuff_size / packet_size;
    }

    frameTimeNs = packet_time * packets_per_frame;
  }

  if (!frameTimeNs) {
    frameTimeNs = NS_PER_MS;
  }

  startingTime = static_cast<uint64_t>(frameTimeNs * 60);
  timingInitialized = true;
}

uint64_t St30pUserTimestamp::plannedTimestampNs(uint64_t frame_idx) const {
  double base = startingTime + frame_idx * frameTimeNs;
  return base <= 0.0 ? 0 : static_cast<uint64_t>(base);
}

void St30pUserTimestamp::verifyReceiveTiming(uint64_t frame_idx, const st30_frame* frame,
                                             uint64_t expected_timestamp_ns) {
  auto* handler = static_cast<St30pHandler*>(parent);
  const mtl_handle mt = handler->ctx->handle;
  uint64_t receive_mono_ns;
  if (!rxPhc.receiveTimeMonotonicRaw(frame_idx, frame->receive_timestamp,
                                     handler->sessionsOpsRx.port.port[MTL_SESSION_PORT_P],
                                     mt, &receive_mono_ns))
    return;

  const uint64_t receive_time_ns = monotonicRawToPtp(mt, receive_mono_ns);
  const int64_t delta_ns =
      static_cast<int64_t>(receive_time_ns) - static_cast<int64_t>(expected_timestamp_ns);
  int64_t expected_delta_ns = 40 * NS_PER_US;
  if (frame_idx == 0) {
    expected_delta_ns = 80 * NS_PER_US;
  }

  EXPECT_LE(std::abs(delta_ns), expected_delta_ns)
      << " idx_rx: " << frame_idx << " delta(ns): " << delta_ns
      << " receive timestamp(ns): " << receive_time_ns
      << " expected timestamp(ns): " << expected_timestamp_ns;
}

void St30pUserTimestamp::verifyMediaClock(uint64_t frame_idx,
                                          uint64_t timestamp_media_clk,
                                          uint64_t expected_media_clk) const {
  EXPECT_EQ(timestamp_media_clk, expected_media_clk)
      << " idx_rx: " << frame_idx << " expected media clk: " << expected_media_clk
      << " received timestamp: " << timestamp_media_clk;
}

void St30pUserTimestamp::verifyTimestampStep(uint64_t frame_idx,
                                             uint64_t current_timestamp,
                                             uint64_t sampling_hz) {
  if (!lastTimestamp) {
    return;
  }

  double current_target = startingTime + frame_idx * frameTimeNs;
  double previous_target = startingTime + (frame_idx ? frame_idx - 1 : 0) * frameTimeNs;
  double expected_step_ns = current_target - previous_target;
  if (expected_step_ns < 0.0) {
    expected_step_ns = 0.0;
  }

  uint64_t expected_step_input = static_cast<uint64_t>(expected_step_ns);
  const uint64_t expected_step = st10_tai_to_media_clk(expected_step_input, sampling_hz);
  const uint64_t diff = current_timestamp - lastTimestamp;
  EXPECT_EQ(diff, expected_step) << " idx_rx: " << frame_idx << " diff: " << diff;
}

St30pRedundantLatency::St30pRedundantLatency(unsigned int /*latency*/,
                                             St30pHandler* parentHandler,
                                             int /*startingTime*/)
    : St30pUserTimestamp(parentHandler) {
}

void St30pRedundantLatency::rxTestFrameModifier(void* /*frame*/, size_t /*frame_size*/) {
  idx_rx++;
}
