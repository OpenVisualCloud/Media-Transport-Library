/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST 2110-21 RX timing parser compliance verdicts.
 *
 * The two false-FAILED defects that kept every nightly rx_timing job red, each
 * paired with negative controls that fail both before and after the fix, so the
 * suite cannot be satisfied by deleting a check instead of correcting it. The
 * session geometry and per-frame numbers are the ones the nightly suite measures
 * on real E810/E830/E835 hardware.
 */

#include <gtest/gtest.h>

#include <string>

#include "session/st20_rx_tp_harness.h"

namespace {

class St20RxTimingParser : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_GE(ut_rvtp_init(), 0);
    ctx_ = ut_rvtp_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_rvtp_destroy(ctx_);
  }

  /* One narrow-compliant frame at epoch UT_RVTP_EPOCH_BASE + n. */
  int FeedNominal(uint64_t n) {
    return ut_rvtp_feed_frame(ctx_, UT_RVTP_EPOCH_BASE + n, UT_RVTP_FPT_NOMINAL,
                              UT_RVTP_RTP_OFFSET_NOMINAL);
  }

  std::string LastCause() {
    return ut_rvtp_last_cause(ctx_);
  }

  ut_rvtp_ctx* ctx_ = nullptr;
};

/* Defect 1: the first frame of a stream has no previous RTP timestamp, so its
 * rtp_ts_delta is not a measurement -- yet it was judged against a one frame
 * time minimum. One failed frame per session, on every NIC. */
TEST_F(St20RxTimingParser, FirstFrameOfStreamIsNarrow) {
  EXPECT_EQ(FeedNominal(0), UT_RVTP_NARROW) << "cause: " << LastCause();
}

/* Negative control: once there is a previous timestamp the delta is a real
 * measurement, and a sender that stops advancing it must still fail. */
TEST_F(St20RxTimingParser, StalledSenderTimestampStillFails) {
  const uint32_t tmstamp = 90000;

  ut_rvtp_feed_frame_rtp(ctx_, UT_RVTP_EPOCH_BASE, UT_RVTP_FPT_NOMINAL, tmstamp);

  EXPECT_EQ(
      ut_rvtp_feed_frame_rtp(ctx_, UT_RVTP_EPOCH_BASE + 1, UT_RVTP_FPT_NOMINAL, tmstamp),
      UT_RVTP_FAILED);
  EXPECT_NE(LastCause().find("rtp_ts_delta"), std::string::npos)
      << "cause: " << LastCause();
}

/* Why the fix tracks validity instead of testing pre_rtp_tmstamp != 0: a real
 * 32-bit media clock wrap puts a legitimate timestamp of exactly 0 on the
 * wire, which the old test read as "no previous frame". Reachable in ~13.2 h
 * of streaming at 90 kHz. */
TEST_F(St20RxTimingParser, RtpTimestampWrapToZeroDoesNotFailTheNextFrame) {
  const uint64_t wrap = ut_rvtp_wrap_epoch();

  ASSERT_EQ(ut_rvtp_feed_frame(ctx_, wrap, UT_RVTP_FPT_NOMINAL, 0), UT_RVTP_NARROW)
      << "cause: " << LastCause();

  EXPECT_EQ(ut_rvtp_feed_frame(ctx_, wrap + 1, UT_RVTP_FPT_NOMINAL, 0), UT_RVTP_NARROW)
      << "cause: " << LastCause();
}

/* Defect 2: rv_tp_pkt_handle() withholds a whole rx burst from the parser as
 * untrusted, so the packet that anchors the frame can be a later one. fpt was
 * then back-extrapolated over the withheld packets at nominal spacing -- the
 * one spacing a burst is known not to have kept -- landing roughly
 * dropped * trs early and taking latency negative with it. On the nightly runs
 * this is the whole of the negative latency tail, and it fails the frame
 * wherever 2 * trs exceeds one media clock tick, i.e. at 1080p25 and 1080p30. */
TEST_F(St20RxTimingParser, FrameAnchoredAfterAnUntrustedBurstIsNarrow) {
  const int dropped = 3; /* an "untrusted 3 pkts" burst, as logged in CI */

  FeedNominal(0); /* supply the rtp_ts_delta reference */

  EXPECT_EQ(ut_rvtp_feed_frame_burst(ctx_, UT_RVTP_EPOCH_BASE + 1, UT_RVTP_FPT_NOMINAL,
                                     UT_RVTP_RTP_OFFSET_NOMINAL, dropped),
            UT_RVTP_NARROW)
      << "cause: " << LastCause();

  /* the extrapolation really did run, and really is off by about dropped * trs */
  EXPECT_LT(ut_rvtp_last_latency(ctx_), -(int32_t)ut_rvtp_tick_ns() / 2);
}

/* Negative control: a frame whose own first packet was parsed is measured, so a
 * first packet that arrives before the timestamp the sender stamped it with
 * must still fail. Stops the fix from being read as "drop the latency floor". */
TEST_F(St20RxTimingParser, EarlyFirstPacketStillFailsLatency) {
  const int32_t fpt = 619008; /* under the RTP timestamp, vrx still narrow */

  FeedNominal(0);

  EXPECT_EQ(
      ut_rvtp_feed_frame(ctx_, UT_RVTP_EPOCH_BASE + 1, fpt, UT_RVTP_RTP_OFFSET_NOMINAL),
      UT_RVTP_FAILED);
  EXPECT_LT(ut_rvtp_last_latency(ctx_), 0);
  EXPECT_EQ(LastCause(), "latency exceed min");
}

/* Negative control: fpt is skipped only when it was not measured. A frame whose
 * own first packet was parsed and arrived after TR-offset must still fail. */
TEST_F(St20RxTimingParser, LateFirstPacketStillFailsFpt) {
  const int32_t fpt = 640000; /* past the 1080p60 tr_offset of 637037 ns */

  FeedNominal(0);

  EXPECT_EQ(
      ut_rvtp_feed_frame(ctx_, UT_RVTP_EPOCH_BASE + 1, fpt, UT_RVTP_RTP_OFFSET_NOMINAL),
      UT_RVTP_FAILED);
  EXPECT_EQ(LastCause(), "fpt exceed tr_offset");
}

}  // namespace
