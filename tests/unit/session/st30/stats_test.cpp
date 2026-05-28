/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * Per-port and session-wide stats invariants:
 * per-port packet/OOO/reorder/duplicate counters, frame completion, and
 * the global lost == sum(per-port-lost) invariant.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St30RxStatsTest.*'
 */

#include <gtest/gtest.h>

#include <functional>
#include <regex>
#include <string>

#include "session/st30/st30_rx_test_base.h"
#include "session/stderr_capture.h"

class St30RxStatsTest : public St30RxBaseTest {};

/* Per-port packet counters track packets received on each port independently. */
TEST_F(St30RxStatsTest, PortPacketCount) {
  /* feed 5 pkts on P with increasing ts */
  for (int i = 0; i < 5; i++) feed(i, 1000 + i, MTL_SESSION_PORT_P);
  /* feed 3 pkts on R with further increasing ts */
  for (int i = 0; i < 3; i++) feed(5 + i, 2000 + i, MTL_SESSION_PORT_R);

  EXPECT_EQ(port_pkts(MTL_SESSION_PORT_P), 5u);
  EXPECT_EQ(port_pkts(MTL_SESSION_PORT_R), 3u);
}

/* Per-port OOO: gap on P only, R is sequential. OOO must be isolated. */
TEST_F(St30RxStatsTest, PortOOOPerPort) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(5, 1005, MTL_SESSION_PORT_P); /* gap of 4 */
  feed(6, 2000, MTL_SESSION_PORT_R);
  feed(7, 2001, MTL_SESSION_PORT_R);

  EXPECT_EQ(port_ooo(MTL_SESSION_PORT_P), 4u);
  EXPECT_EQ(port_ooo(MTL_SESSION_PORT_R), 0u);
}

/* Three complete frames: 3×pkts_per_frame packets produce 3 frames. */
TEST_F(St30RxStatsTest, MultipleFrames) {
  ut30_ctx_destroy(ctx_);
  ctx_ = ut30_ctx_create(1);
  ASSERT_NE(ctx_, nullptr);

  int total = ppf();
  uint16_t seq = 0;
  uint32_t ts = 1000;
  for (int f = 0; f < 3; f++) {
    for (int i = 0; i < total; i++) {
      feed(seq++, ts++, MTL_SESSION_PORT_P);
    }
  }

  EXPECT_EQ(frames_done(), 3);
}

/* Sequence gap produces exact unrecovered count with per-packet timestamps. */
TEST_F(St30RxStatsTest, SeqGapExactCount) {
  ut30_ctx_destroy(ctx_);
  ctx_ = ut30_ctx_create(1);
  ASSERT_NE(ctx_, nullptr);

  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(1, 1001, MTL_SESSION_PORT_P);
  feed(5, 1005, MTL_SESSION_PORT_P); /* gap of 3 in session_seq */

  EXPECT_EQ(unrecovered(), 3u);
}

/* Backward sequence arrival on the same port must not inflate the per-port
 * OOO counter due to unsigned 16-bit wrapping. */
TEST_F(St30RxStatsTest, PerPortOOOBackwardSeq) {
  /* establish port P latest_seq_id = 5 */
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(5, 1005, MTL_SESSION_PORT_P);

  uint64_t ooo_before = port_ooo(MTL_SESSION_PORT_P);

  /* feed seq 3 (backward) with newer timestamp so it passes redundancy */
  feed(3, 2000, MTL_SESSION_PORT_P);

  /* Backward seq should not add ~65533 phantom OOO events */
  EXPECT_LE(port_ooo(MTL_SESSION_PORT_P), ooo_before + 10u)
      << "Backward seq arrival should not produce phantom OOO";
}

/* Duplicate seq_id on the same port must not inflate per-port OOO counter
 * due to unsigned 16-bit wrapping (gap should be 0, not 65535). */
TEST_F(St30RxStatsTest, DuplicateSeqPortOOOWrapping) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(5, 1005, MTL_SESSION_PORT_P);

  uint64_t ooo_before = port_ooo(MTL_SESSION_PORT_P);

  /* re-feed seq 5 with same ts — filtered as redundant */
  feed(5, 1005, MTL_SESSION_PORT_P);

  /* Duplicate seq should not inflate OOO counter */
  EXPECT_EQ(port_ooo(MTL_SESSION_PORT_P), ooo_before)
      << "Duplicate seq on same port should not inflate OOO counter";
}

/* stat_lost_packets == port[0].lost + port[1].lost invariant. */
TEST_F(St30RxStatsTest, LostPacketsInvariant) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(2, 1001, MTL_SESSION_PORT_P);
  feed(0, 1002, MTL_SESSION_PORT_R);
  feed(3, 1003, MTL_SESSION_PORT_R);

  EXPECT_EQ(ooo(), port_ooo(MTL_SESSION_PORT_P) + port_ooo(MTL_SESSION_PORT_R));
}

/* Gap across the 16-bit RTP seq wrap boundary. With latest=65534 and seq=1,
 * the true forward gap is 2 (seq 65535 and 0 missed). Both per-port OOO and
 * session-level unrecovered must reflect the true gap, not ~65535. */
TEST_F(St30RxStatsTest, PortLostNotInflatedAtSeqWrap) {
  /* establish latest_seq_id[P] = 65534 */
  feed(65533, 1000, MTL_SESSION_PORT_P);
  feed(65534, 1001, MTL_SESSION_PORT_P);
  uint64_t lost_before = port_ooo(MTL_SESSION_PORT_P);
  uint64_t unrec_before = unrecovered();

  /* forward jump across the wrap: gap = 2 (seq 65535 and 0 missing) */
  feed(1, 1002, MTL_SESSION_PORT_P);

  EXPECT_EQ(port_ooo(MTL_SESSION_PORT_P) - lost_before, 2u)
      << "per-port lost gap across seq wrap must be the true forward gap (2),"
         " not a phantom ~65535 from naive subtraction";
  EXPECT_EQ(unrecovered() - unrec_before, 2u)
      << "session-seq gap across wrap must equal the true forward gap (2)";
}

/* Disjoint per-wire losses whose union covers the full stream: P drops
 * seqs 0..2 but delivers 3..5; R drops 3..5 but delivers 0..2. Each pkt
 * has its own monotonic timestamp. Reconstruction must succeed — every
 * packet accepted exactly once, no unrecovered, no redundant. */
TEST_F(St30RxStatsTest, DisjointPortLossesUnionRecovers) {
  feed(0, 1000, MTL_SESSION_PORT_R);
  feed(1, 1001, MTL_SESSION_PORT_R);
  feed(2, 1002, MTL_SESSION_PORT_R);
  feed(3, 1003, MTL_SESSION_PORT_P);
  feed(4, 1004, MTL_SESSION_PORT_P);
  feed(5, 1005, MTL_SESSION_PORT_P);

  EXPECT_EQ(received(), 6u);
  EXPECT_EQ(redundant(), 0u);
  EXPECT_EQ(unrecovered(), 0u) << "every gap on one wire was filled by the other wire";
}

/* Documented invariant stat_pkts_unrecovered <= sum(port[].lost_packets):
 * both wires drop the same seq, so the hole is unrecoverable. Per-port
 * accounting bumps lost on each wire (it runs before the redundancy filter),
 * while the session-wide unrecovered counter records the unfilled hole.
 * The invariant must hold strictly. */
TEST_F(St30RxStatsTest, UnrecoveredNotGreaterThanSummedPortLost) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(0, 1000, MTL_SESSION_PORT_R); /* filtered as redundant by ts */
  /* Both wires skip seq 1. */
  feed(2, 1002, MTL_SESSION_PORT_P); /* P jumped 0 -> 2: own gap of 1 */
  feed(2, 1002, MTL_SESSION_PORT_R); /* R also jumped 0 -> 2 (then filtered) */

  EXPECT_GE(unrecovered(), 1u) << "the missing seq is post-redundancy loss";
  EXPECT_LE(unrecovered(), port_ooo(MTL_SESSION_PORT_P) + port_ooo(MTL_SESSION_PORT_R))
      << "documented invariant: stat_pkts_unrecovered <= sum(port[].lost)";
}

/* Documented invariant: every accepted packet is counted as exactly one of
 * {received, redundant} — never both, never neither. Mixed-traffic scenario
 * with own-stream forward delivery plus cross-port duplicates pins the
 * partition. */
TEST_F(St30RxStatsTest, ReceivedPlusRedundantEqualsAcceptedPackets) {
  feed_burst(0, 4, 1000, MTL_SESSION_PORT_P); /* 4 accepted as received */
  feed_burst(0, 4, 1000, MTL_SESSION_PORT_R); /* 4 accepted as redundant */
  feed_burst(4, 4, 2000, MTL_SESSION_PORT_P); /* 4 more received */

  EXPECT_EQ(received(), 8u);
  EXPECT_EQ(redundant(), 4u);
  EXPECT_EQ(received() + redundant(), 12u)
      << "every accepted packet is counted in exactly one bucket";
}

/* Mirrors ST20 BackpressureWarnLineEmitted; single-axis (no pkts_no_slot);
 * single-port pool of 2. */
class St30RxFramePoolTest : public ::testing::Test {
 protected:
  ut30_test_ctx* ctx_ = nullptr;
  uint16_t next_seq_ = 0;
  uint32_t next_ts_ = 1000;

  void SetUp() override {
    ASSERT_EQ(ut30_init(), 0) << "EAL init failed";
    ctx_ = ut30_ctx_create(1); /* single port = framebuff pool of 2 */
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    if (ctx_) {
      ut30_set_hold_frames(ctx_, false);
      ut30_ctx_destroy(ctx_);
      ctx_ = nullptr;
    }
  }

  void feed_full() {
    int pkts = ut30_pkts_per_frame(ctx_);
    ut30_feed_full_frame(ctx_, next_seq_, next_ts_, MTL_SESSION_PORT_P);
    next_seq_ = (uint16_t)(next_seq_ + pkts);
    next_ts_ += (uint32_t)pkts;
  }

  void feed_first_pkt() {
    ut30_feed_pkt(ctx_, next_seq_, next_ts_, MTL_SESSION_PORT_P);
    next_seq_ = (uint16_t)(next_seq_ + 1);
    next_ts_ += 1;
  }

  uint64_t slot_fail() {
    return ut30_stat_slot_get_frame_fail(ctx_);
  }
};

TEST_F(St30RxFramePoolTest, BackpressureWarnLineEmitted) {
  auto run_stage = ut_session::capture_stderr;

  const std::string kWarnPrefix = "back-pressure: framebuff pool empty (";
  const std::string kFreeMid = " free), dropped ";
  const std::string kRemediation = "raise framebuff_cnt";
  const std::string kAudioDrain = "st30_rx_put_framebuff";

  /* Healthy: no slot_get_frame_fail deltas → no warn line. */
  std::string s1 = run_stage([&] {
    feed_full();
    feed_full();
    ut30_invoke_rx_audio_session_stat(ctx_);
  });
  EXPECT_EQ(s1.find(kWarnPrefix), std::string::npos);
  EXPECT_EQ(s1.find("slot get frame fail"), std::string::npos);
  EXPECT_EQ(s1.find("framebuff pool empty"), std::string::npos);

  /* Starved interval 1: hold the two completed frames, then start a third
   * frame → get_frame fails on its first packet. Warn line fires with
   * gauge + remediation; legacy per-counter line demoted to info. No
   * sustained suffix yet (counter == 1). */
  ut30_set_hold_frames(ctx_, true);
  feed_full();
  feed_full();
  std::string s2 = run_stage([&] {
    feed_first_pkt();
    ut30_invoke_rx_audio_session_stat(ctx_);
  });
  EXPECT_GE(slot_fail(), 1u) << "back-pressure must have bumped slot_get_frame_fail";
  EXPECT_NE(s2.find(kWarnPrefix), std::string::npos) << "warn line missing: " << s2;
  EXPECT_NE(s2.find(kFreeMid), std::string::npos) << s2;
  EXPECT_NE(s2.find(kRemediation), std::string::npos) << s2;
  EXPECT_NE(s2.find(kAudioDrain), std::string::npos) << s2;
  EXPECT_EQ(s2.find("(sustained "), std::string::npos)
      << "sustained suffix must not appear until 3 consecutive intervals";

  /* Starved intervals 2 and 3 → "(sustained 3x)" appears. */
  run_stage([&] {
    feed_first_pkt();
    ut30_invoke_rx_audio_session_stat(ctx_);
  });
  std::string s3 = run_stage([&] {
    feed_first_pkt();
    ut30_invoke_rx_audio_session_stat(ctx_);
  });
  EXPECT_NE(s3.find(kWarnPrefix), std::string::npos) << s3;
  std::regex sustained_re(R"(\(sustained [0-9]+x\))");
  EXPECT_TRUE(std::regex_search(s3, sustained_re))
      << "expected (sustained Nx) suffix after 3 consecutive starved intervals: " << s3;

  /* Recovery: drain, no deltas this interval → no warn, hysteresis resets. */
  ut30_set_hold_frames(ctx_, false);
  const uint64_t slot_fail_after_recovery = slot_fail();
  std::string s4 = run_stage([&] {
    feed_full();
    ut30_invoke_rx_audio_session_stat(ctx_);
  });
  EXPECT_EQ(s4.find(kWarnPrefix), std::string::npos);
  EXPECT_EQ(s4.find("framebuff pool empty"), std::string::npos);
  EXPECT_EQ(slot_fail(), slot_fail_after_recovery);

  /* Single starved interval after recovery: warn without sustained suffix. */
  ut30_set_hold_frames(ctx_, true);
  feed_full();
  feed_full();
  std::string s5 = run_stage([&] {
    feed_first_pkt();
    ut30_invoke_rx_audio_session_stat(ctx_);
  });
  EXPECT_NE(s5.find(kWarnPrefix), std::string::npos) << s5;
  EXPECT_EQ(s5.find("(sustained "), std::string::npos)
      << "hysteresis must have reset on recovery; got: " << s5;
}

/* RTP-mode coverage: in ST30_TYPE_RTP_LEVEL the rtps_ring-full path bumps
 * stat_slot_get_frame_fail while s->st30_frames stays NULL. The stat
 * callback must not deref the NULL pool and must not emit the frame-mode
 * back-pressure warn line. The notice() above remains as the RTP signal. */
TEST(St30RxRtpRingTest, BackpressureNoCrash) {
  ASSERT_EQ(ut30_init(), 0);
  ut30_test_ctx* ctx = ut30_ctx_create(1);
  ASSERT_NE(ctx, nullptr);

  void* saved = ut30_detach_frames(ctx);
  ut30_bump_slot_get_frame_fail(ctx, 5);

  testing::internal::CaptureStderr();
  ut30_invoke_rx_audio_session_stat(ctx);
  fflush(stderr);
  std::string out = testing::internal::GetCapturedStderr();
  fprintf(stderr, "%s", out.c_str());

  EXPECT_EQ(out.find("back-pressure: framebuff pool empty"), std::string::npos)
      << "frame-mode warn line must not fire in RTP mode: " << out;
  EXPECT_NE(out.find("slot get frame fail"), std::string::npos)
      << "notice() must still surface in RTP mode: " << out;

  ut30_reattach_frames(ctx, saved);
  ut30_ctx_destroy(ctx);
}
