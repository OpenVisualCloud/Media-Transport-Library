/* SPDX-License-Identifier: BSD-3-Clause */

#include "st30p_strategies.hpp"

#include <gtest/gtest.h>

#include "handlers/st30p_handler.hpp"
#include "tests.hpp"

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

  EXPECT_NEAR(f->timestamp,
              st10_tai_to_media_clk(idx_rx * st30pParent->nsPacketTime, sampling),
              framebuffTime)
      << " idx_rx: " << idx_rx;
  if (lastTimestamp != 0) {
    uint64_t diff = f->timestamp - lastTimestamp;
    EXPECT_TRUE(diff == framebuffTime) << " idx_rx: " << idx_rx << " diff: " << diff;
  }

  lastTimestamp = f->timestamp;
  idx_rx++;
}

St30pUserTimestamp::St30pUserTimestamp(St30pHandler* parentHandler)
    : St30pDefaultTimestamp(parentHandler),
      startingTime(10 * NS_PER_MS),
      lastTimestamp(0) {
  enable_tx_modifier = true;
  enable_rx_modifier = true;
}

void St30pUserTimestamp::txTestFrameModifier(void* frame, size_t /*frame_size*/) {
  auto* f = static_cast<st30_frame*>(frame);
  auto* st30pParent = static_cast<St30pHandler*>(parent);

  f->tfmt = ST10_TIMESTAMP_FMT_TAI;
  f->timestamp = startingTime + (st30pParent->nsPacketTime * idx_tx);
  idx_tx++;
}

void St30pUserTimestamp::rxTestFrameModifier(void* frame, size_t /*frame_size*/) {
  auto* f = static_cast<st30_frame*>(frame);
  auto* st30pParent = static_cast<St30pHandler*>(parent);
  uint64_t sampling = st30_get_sample_rate(st30pParent->sessionsOpsRx.sampling);
  idx_rx++;

  uint64_t expectedTimestamp = startingTime + (st30pParent->nsPacketTime * (idx_rx - 1));
  uint64_t expected_media_clk = st10_tai_to_media_clk(expectedTimestamp, sampling);

  EXPECT_EQ(f->timestamp, expected_media_clk)
      << " idx_rx: " << idx_rx << " tai difference: "
      << (int64_t)(st10_media_clk_to_ns(f->timestamp, sampling) - expectedTimestamp);

  if (lastTimestamp != 0) {
    uint64_t diff = f->timestamp - lastTimestamp;
    EXPECT_TRUE(diff == st10_tai_to_media_clk(st30pParent->nsPacketTime, sampling))
        << " idx_rx " << idx_rx << " diff: " << diff;
  }

  lastTimestamp = f->timestamp;
}

St30pRedundantLatency::St30pRedundantLatency(unsigned int latency,
                                             St30pHandler* parentHandler,
                                             int startingTime)
    : St30pUserTimestamp(parentHandler),
      latencyInMs(latency),
      startingTimeInMs(static_cast<unsigned int>(startingTime)) {
  startingTime = (50 + latencyInMs) * NS_PER_MS;
}

void St30pRedundantLatency::rxTestFrameModifier(void* /*frame*/, size_t /*frame_size*/) {
  idx_rx++;
}
