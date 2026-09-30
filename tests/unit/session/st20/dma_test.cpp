/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * RX DMA frame boundary: frame N's copies are still queued (not submitted) when
 * frame N+1's first packet is parsed in the same burst. That packet must not be
 * dropped while the DMA engine can finish frame N.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St20RxDmaTest.*'
 */

#include <gtest/gtest.h>

#include <string>

#include "session/st20/st20_rx_test_base.h"
#include "session/stderr_capture.h"

class St20RxDmaTest : public St20RxBaseTest {
 protected:
  int num_port() const override {
    return 1;
  }
  int pkts_per_frame() const override {
    return 4;
  }

  void SetUp() override {
    St20RxBaseTest::SetUp();
    ut20_ctx_enable_dma(ctx_);
  }
};

/* Consecutive frames with no tasklet pass between them, as in one RX burst: each
 * frame completes when the next starts, the DMA engine gets every payload, and no
 * new-frame packet is dropped. */
TEST_F(St20RxDmaTest, NextFrameInSameBurstCompletesPrevious) {
  const uint64_t pkts = ut20_pkts_per_frame(ctx_);

  feed_full(1000, MTL_SESSION_PORT_P);
  EXPECT_EQ(frames_received(), 0) << "frame 1000 copies are not submitted yet";

  feed_full(2000, MTL_SESSION_PORT_P);
  EXPECT_EQ(frames_received(), 1);
  feed_full(3000, MTL_SESSION_PORT_P);
  EXPECT_EQ(frames_received(), 2);

  EXPECT_EQ(received(), 3 * pkts);
  EXPECT_EQ(no_slot(), 0u);
  EXPECT_EQ(ut20_dma_busy_drops(ctx_), 0);
  EXPECT_EQ(ut20_dma_flush_timeouts(ctx_), 0u);
  EXPECT_EQ(ut20_dma_copies(ctx_), 3 * pkts);
}

/* An engine that does not complete within the bound costs one bounded flush per
 * newer frame: the frame's packets are dropped and counted as DMA busy without
 * polling again, a newer frame retries, and the session recovers once the engine
 * completes. */
TEST_F(St20RxDmaTest, StalledEngineFlushesOncePerFrame) {
  const int pkts = ut20_pkts_per_frame(ctx_);

  feed_full(1000, MTL_SESSION_PORT_P);

  ut20_dma_set_stalled(ctx_, true);
  feed(0, 2000, MTL_SESSION_PORT_P);
  const uint64_t polls = ut20_dma_polls(ctx_);
  EXPECT_GT(polls, 0u);
  for (int i = 1; i < pkts; i++) feed(i, 2000, MTL_SESSION_PORT_P);
  EXPECT_EQ(ut20_dma_polls(ctx_), polls) << "later packets of the frame polled again";
  EXPECT_EQ(ut20_dma_busy_drops(ctx_), pkts);
  EXPECT_EQ(ut20_dma_flush_timeouts(ctx_), 1u);

  feed(0, 3000, MTL_SESSION_PORT_P);
  EXPECT_GT(ut20_dma_polls(ctx_), polls) << "a newer frame must retry the flush";
  EXPECT_EQ(ut20_dma_busy_drops(ctx_), pkts + 1);
  EXPECT_EQ(ut20_dma_flush_timeouts(ctx_), 2u);
  EXPECT_EQ(frames_received(), 0);

  ut20_dma_set_stalled(ctx_, false);
  feed_full(4000, MTL_SESSION_PORT_P);
  EXPECT_EQ(frames_received(), 1) << "frame 1000 completes once its copies do";
  feed_full(5000, MTL_SESSION_PORT_P);
  EXPECT_EQ(frames_received(), 2);
  EXPECT_EQ(ut20_dma_busy_drops(ctx_), pkts + 1);
  EXPECT_EQ(ut20_dma_flush_timeouts(ctx_), 2u);
}

/* A timed-out flush must not outlive an engine the tasklet drained: half an RTP wrap
 * later the next straddling burst would read as no newer than the timed-out frame. */
TEST_F(St20RxDmaTest, TaskletDrainClearsTimedOutFlush) {
  const uint32_t t0 = 2000;
  const uint32_t quarter_wrap = 0x40000000;

  feed_full(1000, MTL_SESSION_PORT_P);
  ut20_dma_set_stalled(ctx_, true);
  feed(0, t0, MTL_SESSION_PORT_P);
  ASSERT_EQ(ut20_dma_flush_timeouts(ctx_), 1u);

  ut20_dma_set_stalled(ctx_, false);
  ut20_rx_tasklet_pass(ctx_);
  ASSERT_EQ(frames_received(), 1);

  feed_full(t0 + quarter_wrap, MTL_SESSION_PORT_P);
  ut20_rx_tasklet_pass(ctx_);
  ASSERT_EQ(frames_received(), 2);

  const uint32_t straddled = t0 + 2 * quarter_wrap + 1000;
  feed_full(straddled, MTL_SESSION_PORT_P);
  const int busy_drops = ut20_dma_busy_drops(ctx_);
  const uint64_t polls = ut20_dma_polls(ctx_);
  feed_full(straddled + 1000, MTL_SESSION_PORT_P);
  EXPECT_GT(ut20_dma_polls(ctx_), polls) << "the straddling frame skipped the flush";
  EXPECT_EQ(ut20_dma_busy_drops(ctx_), busy_drops);
  EXPECT_EQ(frames_received(), 3);
}

/* The session stat dump reports the flush timeouts of its own interval only. */
TEST_F(St20RxDmaTest, StatDumpReportsFlushTimeoutsOfTheInterval) {
  feed_full(1000, MTL_SESSION_PORT_P);
  ut20_dma_set_stalled(ctx_, true);
  feed(0, 2000, MTL_SESSION_PORT_P);
  feed(0, 3000, MTL_SESSION_PORT_P);

  std::string first = ut_session::capture_stderr([&] { ut20_invoke_rv_stat(ctx_); });
  EXPECT_NE(first.find("flush timeouts 2\n"), std::string::npos) << first;
  std::string second = ut_session::capture_stderr([&] { ut20_invoke_rv_stat(ctx_); });
  EXPECT_NE(second.find("flush timeouts 0\n"), std::string::npos) << second;
}

class St20RxDmaRedundantTest : public St20RxDmaTest {
 protected:
  int num_port() const override {
    return 2;
  }
};

/* With inter-port skew and a stalled engine, the lagging port's older new frame
 * must not flush again once the leading port's newer frame did. */
TEST_F(St20RxDmaRedundantTest, StalledEngineSkewedPortsFlushOncePerNewerFrame) {
  feed_full(1000, MTL_SESSION_PORT_P);

  ut20_dma_set_stalled(ctx_, true);
  feed(0, 2000, MTL_SESSION_PORT_P);
  const uint64_t polls_2000 = ut20_dma_polls(ctx_);
  EXPECT_GT(polls_2000, 0u);
  feed(0, 3000, MTL_SESSION_PORT_P);
  const uint64_t polls_3000 = ut20_dma_polls(ctx_);
  EXPECT_GT(polls_3000, polls_2000);

  feed(0, 2000, MTL_SESSION_PORT_R);
  feed(1, 3000, MTL_SESSION_PORT_P);
  feed(1, 2000, MTL_SESSION_PORT_R);
  feed(0, 3000, MTL_SESSION_PORT_R);
  EXPECT_EQ(ut20_dma_polls(ctx_), polls_3000) << "an older or equal frame polled again";
  EXPECT_EQ(ut20_dma_busy_drops(ctx_), 6);
  EXPECT_EQ(ut20_dma_flush_timeouts(ctx_), 2u);

  ut20_dma_set_stalled(ctx_, false);
  feed(0, 4000, MTL_SESSION_PORT_P);
  EXPECT_EQ(frames_received(), 1) << "frame 1000 completes once its copies do";
}
