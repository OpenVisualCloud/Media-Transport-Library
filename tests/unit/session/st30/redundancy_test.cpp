/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * Timestamp-only redundancy filter:
 * normal redundancy, single-port baseline, same-vs-new timestamp acceptance,
 * burst switchover, threshold bypass and same-port duplicates.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St30RxRedundancyTest.*'
 */

#include <gtest/gtest.h>

#include "session/st30/st30_rx_test_base.h"

class St30RxRedundancyTest : public St30RxBaseTest {};

/* Same packets on both ports. ST30 filters by timestamp: R's packets
 * have timestamps not strictly greater than P's, so all are filtered.
 * feed_burst auto-increments ts per packet (real ST30 behavior). */
TEST_F(St30RxRedundancyTest, NormalRedundancy) {
  feed_burst(0, 4, 1000, MTL_SESSION_PORT_P);
  feed_burst(0, 4, 1000, MTL_SESSION_PORT_R);

  feed_burst(4, 4, 2000, MTL_SESSION_PORT_P);
  feed_burst(4, 4, 2000, MTL_SESSION_PORT_R);

  EXPECT_EQ(unrecovered(), 0u);
  EXPECT_EQ(redundant(), 8u);
  EXPECT_EQ(received(), 8u);
}

/* Timestamp-only filter: R's packets are filtered even with higher seq_ids,
 * because their timestamps are not strictly greater than P's latest.
 * This is the key ST30 difference from ST40 (which checks both ts and seq). */
TEST_F(St30RxRedundancyTest, TimestampOnlyFilter) {
  feed_burst(0, 4, 1000, MTL_SESSION_PORT_P);

  /* port 1 has HIGHER seq_ids but same/old timestamps → filtered */
  for (int i = 0; i < 4; i++) {
    int rc = feed(4 + i, 1000 + i, MTL_SESSION_PORT_R);
    EXPECT_LT(rc, 0) << "seq " << (4 + i)
                     << " should be filtered (ts not greater, ST30 ignores seq)";
  }

  EXPECT_EQ(redundant(), 4u);
  EXPECT_EQ(received(), 4u);
}

/* New timestamp always accepted even if seq goes backward.
 * R has lower seq_ids but strictly newer timestamps, so packets are accepted.
 * A source switch with new timestamps must not produce phantom unrecovered. */
TEST_F(St30RxRedundancyTest, NewTimestampAlwaysAccepted) {
  feed_burst(10, 4, 2000, MTL_SESSION_PORT_P);
  feed_burst(0, 4, 3000, MTL_SESSION_PORT_R);

  EXPECT_EQ(redundant(), 0u);
  EXPECT_EQ(received(), 8u);
  /* Session seq after P = 13. R brings seq 0. The seq gap wraps to 65522
   * via uint16_t arithmetic. This is phantom loss from source switch,
   * not real packet loss. */
  EXPECT_EQ(unrecovered(), 0u)
      << "Source switch with new timestamp should not produce phantom unrecovered";
}

/* Same timestamp, different seq on second port. Even though R has a
 * higher seq_id, it is filtered because the timestamp is not strictly
 * greater. */
TEST_F(St30RxRedundancyTest, SameTimestampDiffSeqFiltered) {
  feed(5, 1000, MTL_SESSION_PORT_P);

  int rc = feed(6, 1000, MTL_SESSION_PORT_R);
  EXPECT_LT(rc, 0) << "Same timestamp should be filtered even with newer seq";
  EXPECT_EQ(redundant(), 1u);
}

/* Burst switchover for audio: R has newer timestamps and is processed
 * first, then P arrives with old timestamps. P's late arrivals have
 * stale timestamps and must be filtered. */
TEST_F(St30RxRedundancyTest, BurstSwitchoverAudio) {
  /* establish history */
  feed_burst(0, 4, 1000, MTL_SESSION_PORT_P);

  /* port 1 arrives first with NEW timestamps */
  feed_burst(6, 4, 2000, MTL_SESSION_PORT_R);

  /* port 0 late arrivals with OLD timestamps */
  feed(4, 1004, MTL_SESSION_PORT_P);
  feed(5, 1005, MTL_SESSION_PORT_P);

  /* both late packets should be filtered (old timestamp < 2003) */
  EXPECT_EQ(redundant(), 2u);
  /* the 4 packets from port 1 advance seq from 3 to 6 — gap of 2 */
  EXPECT_EQ(unrecovered(), 2u);
}

/* Multiple packets with the same timestamp on the same port. Only the
 * first packet per timestamp is accepted; subsequent ones are filtered. */
TEST_F(St30RxRedundancyTest, SameTimestampFiltered) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  /* same ts again — filtered */
  int rc = feed(1, 1000, MTL_SESSION_PORT_P);
  EXPECT_LT(rc, 0) << "Same timestamp should be filtered";
  /* a third one too */
  rc = feed(5, 1000, MTL_SESSION_PORT_P);
  EXPECT_LT(rc, 0) << "Same timestamp should be filtered";
  EXPECT_EQ(redundant(), 2u);
  EXPECT_EQ(received(), 1u);
}

/* Redundancy error threshold bypass: 21 old-timestamp packets on both
 * ports should force-accept once both exceed threshold=20. */
TEST_F(St30RxRedundancyTest, ThresholdBypass) {
  /* establish state with a newer timestamp */
  feed_burst(0, 4, 5000, MTL_SESSION_PORT_P);
  /* also init port 1 with same timestamps so its error counter resets */
  feed_burst(0, 4, 5004, MTL_SESSION_PORT_R);

  uint64_t recv_before = received();

  /* send 21 old-timestamp packets on BOTH ports, alternating */
  for (int i = 0; i < 21; i++) {
    feed(50 + i, 1000 + i, MTL_SESSION_PORT_P);
    feed(50 + i, 1000 + i, MTL_SESSION_PORT_R);
  }

  /* After both ports exceed threshold (20), the 21st pair should be accepted.
   * Requires redundant_error_cnt to be incremented in the ST30 handler. */
  EXPECT_GT(received(), recv_before)
      << "After exceeding threshold on both ports, packets should be accepted";
}

/* Interleaved ports with increasing timestamps. P and R alternate, each
 * packet with a strictly newer timestamp. All packets accepted. */
TEST_F(St30RxRedundancyTest, InterleavedPortsIncreasingTs) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(1, 1001, MTL_SESSION_PORT_R);
  feed(2, 1002, MTL_SESSION_PORT_P);
  feed(3, 1003, MTL_SESSION_PORT_R);
  feed(4, 1004, MTL_SESSION_PORT_P);
  feed(5, 1005, MTL_SESSION_PORT_R);

  EXPECT_EQ(unrecovered(), 0u);
  EXPECT_EQ(redundant(), 0u);
  EXPECT_EQ(received(), 6u);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Per-port frame accounting (audio).
 *
 * ST30 credits exactly one port per completed frame: the port whose first
 * accepted packet of the new RTP timestamp allocated the frame buffer (see
 * `rx_audio_session_handle_frame_pkt` in
 * `lib/src/st2110/st_rx_audio_session.c`). Subsequent packets of the same
 * frame — whether on the same port or the other — never bump
 * `port[i].frames`. Redundant packets filtered by the timestamp check do
 * not even reach the credit branch.
 *
 * Invariant per session: port[P].frames + port[R].frames == frames_received
 * ───────────────────────────────────────────────────────────────────────── */

/* P delivers all frames; R is silent. P is credited once per frame. */
TEST_F(St30RxRedundancyTest, PerPortFramesPrimaryOnly) {
  feed_burst(0, ppf(), 1000, MTL_SESSION_PORT_P);
  feed_burst(ppf(), ppf(), 2000, MTL_SESSION_PORT_P);
  feed_burst(2 * ppf(), ppf(), 3000, MTL_SESSION_PORT_P);

  EXPECT_EQ(frames_done(), 3);
  EXPECT_EQ(port_frames(MTL_SESSION_PORT_P), 3u);
  EXPECT_EQ(port_frames(MTL_SESSION_PORT_R), 0u);
}

/* All frames originate on R; P is silent. R is credited once per frame. */
TEST_F(St30RxRedundancyTest, PerPortFramesSecondaryOnly) {
  feed_burst(0, ppf(), 1000, MTL_SESSION_PORT_R);
  feed_burst(ppf(), ppf(), 2000, MTL_SESSION_PORT_R);

  EXPECT_EQ(frames_done(), 2);
  EXPECT_EQ(port_frames(MTL_SESSION_PORT_P), 0u);
  EXPECT_EQ(port_frames(MTL_SESSION_PORT_R), 2u);
}

/* P delivers a full frame, R delivers the same frame's pkts after. R's pkts
 * have stale timestamps and are filtered by the timestamp guard before the
 * frame-credit branch — R must NOT be credited. */
TEST_F(St30RxRedundancyTest, PerPortFramesPrimaryFirstSecondaryFiltered) {
  feed_burst(0, ppf(), 1000, MTL_SESSION_PORT_P);
  feed_burst(0, ppf(), 1000, MTL_SESSION_PORT_R); /* all stale → redundant */

  EXPECT_EQ(frames_done(), 1);
  EXPECT_EQ(port_frames(MTL_SESSION_PORT_P), 1u);
  EXPECT_EQ(port_frames(MTL_SESSION_PORT_R), 0u)
      << "R's pkts were filtered before reaching the credit branch";
  EXPECT_EQ(redundant(), static_cast<uint64_t>(ppf()));
}

/* Frame N arrives on P; the first packet of frame N+1 arrives on R first
 * (with a strictly newer ts). R wins the credit for frame N+1; P keeps the
 * credit for frame N. */
TEST_F(St30RxRedundancyTest, PerPortFramesAlternatingWinner) {
  /* frame 1: P starts and finishes */
  feed_burst(0, ppf(), 1000, MTL_SESSION_PORT_P);
  /* frame 2: R sends the first pkt (new ts) — wins the credit */
  feed(ppf(), 2000, MTL_SESSION_PORT_R);
  /* finish frame 2 from R (or P, same outcome — credit already assigned) */
  for (int i = 1; i < ppf(); i++) {
    feed(ppf() + i, 2000 + i, MTL_SESSION_PORT_R);
  }

  EXPECT_EQ(frames_done(), 2);
  EXPECT_EQ(port_frames(MTL_SESSION_PORT_P), 1u) << "frame 1";
  EXPECT_EQ(port_frames(MTL_SESSION_PORT_R), 1u) << "frame 2";
  EXPECT_EQ(port_frames(MTL_SESSION_PORT_P) + port_frames(MTL_SESSION_PORT_R),
            static_cast<uint64_t>(frames_done()))
      << "audio invariant: per-port frames sum equals frames_received";
}
