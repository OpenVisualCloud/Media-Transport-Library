/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Per-port and session-wide stats invariants for ST 2110-20:
 * per-port packet counters, lost-packets composition invariant, and the
 * received/redundant accounting invariant under cross-port redundancy.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St20RxStatsTest.*'
 */

#include <gtest/gtest.h>

#include <functional>
#include <regex>
#include <string>

#include "session/st20/st20_rx_test_base.h"
#include "session/stderr_capture.h"

class St20RxStatsTest : public St20RxBaseTest {};

/* Per-port packet counters (port_user_stats.common.port[].packets) are
 * bumped by the _handle_mbuf wrapper. Tests below feed via the wrapper
 * to exercise that accounting path; the non-wrapper helpers in the base
 * fixture skip the wrapper layer and are therefore not used here. */

/* Bitmap-redundant arrivals on the secondary port still bump per-port
 * counters: per-port stats represent wire-level RX, not the merged stream. */
TEST_F(St20RxStatsTest, RedundantStillCountsPerPort) {
  ut20_feed_frame_pkt_via_wrapper(ctx_, 0, 1000, MTL_SESSION_PORT_P);
  ut20_feed_frame_pkt_via_wrapper(ctx_, 1, 1000, MTL_SESSION_PORT_P);
  ut20_feed_frame_pkt_via_wrapper(ctx_, 0, 1000, MTL_SESSION_PORT_R);
  ut20_feed_frame_pkt_via_wrapper(ctx_, 1, 1000, MTL_SESSION_PORT_R);

  EXPECT_EQ(received(), 2u);
  EXPECT_EQ(redundant(), 2u);
  EXPECT_EQ(port_pkts(MTL_SESSION_PORT_P), 2u);
  EXPECT_EQ(port_pkts(MTL_SESSION_PORT_R), 2u);
}

/* Each accepted packet is counted as EITHER received OR redundant — never
 * both. Holds across redundancy, reorder, and frame-gone paths. */
TEST_F(St20RxStatsTest, ReceivedPlusRedundantInvariant) {
  /* frame 1: P delivers fully; R duplicates → all R pkts hit frame-gone */
  ut20_feed_frame_pkt_via_wrapper(ctx_, 0, 1000, MTL_SESSION_PORT_P);
  ut20_feed_frame_pkt_via_wrapper(ctx_, 1, 1000, MTL_SESSION_PORT_P);
  ut20_feed_frame_pkt_via_wrapper(ctx_, 0, 1000, MTL_SESSION_PORT_R);
  ut20_feed_frame_pkt_via_wrapper(ctx_, 1, 1000, MTL_SESSION_PORT_R);
  /* frame 2: clean P-only */
  ut20_feed_frame_pkt_via_wrapper(ctx_, 0, 2000, MTL_SESSION_PORT_P);
  ut20_feed_frame_pkt_via_wrapper(ctx_, 1, 2000, MTL_SESSION_PORT_P);

  /* total accepted into the session: 2 (P f1) + 2 (R f1 dup) + 2 (P f2) = 6 */
  EXPECT_EQ(received() + redundant(), 6u)
      << "every accepted packet must land in exactly one of {received, redundant}";
  EXPECT_EQ(frames_received(), 2);
}

/* lost_packets invariant: stat_lost_packets equals the sum of per-port
 * lost counters. P delivers {0,2}, R delivers {1,3}; the frame fully
 * reconstructs (4 unique pkts) but each port carries a per-frame deficit
 * of 2 charged at finalisation. */
TEST_F(St20RxStatsTest, LostPacketsInvariant) {
  ut20_test_ctx* wide = ut20_ctx_create_geom(2, 4);
  ASSERT_NE(wide, nullptr);

  ut20_feed_frame_pkt(wide, 0, 1000, MTL_SESSION_PORT_P);
  ut20_feed_frame_pkt(wide, 1, 1000, MTL_SESSION_PORT_R);
  ut20_feed_frame_pkt(wide, 3, 1000, MTL_SESSION_PORT_R);
  ut20_feed_frame_pkt(wide, 2, 1000, MTL_SESSION_PORT_P);
  /* Recycle the slot (slot_max==2) so the completed frame's deferred per-port
   * loss is accounted; flush frames are fully redundant (zero deficit). */
  for (uint32_t ts = 90000; ts <= 91000; ts += 1000) {
    for (int i = 0; i < 4; i++) {
      ut20_feed_frame_pkt(wide, i, ts, MTL_SESSION_PORT_P);
      ut20_feed_frame_pkt(wide, i, ts, MTL_SESSION_PORT_R);
    }
  }

  EXPECT_EQ(ut20_frames_received(wide), 3);
  EXPECT_EQ(ut20_stat_port_lost(wide, MTL_SESSION_PORT_P), 2u);
  EXPECT_EQ(ut20_stat_port_lost(wide, MTL_SESSION_PORT_R), 2u);
  EXPECT_EQ(ut20_stat_lost_pkts(wide), ut20_stat_port_lost(wide, MTL_SESSION_PORT_P) +
                                           ut20_stat_port_lost(wide, MTL_SESSION_PORT_R))
      << "stat_lost_packets must equal the sum of per-port lost_packets";

  ut20_ctx_destroy(wide);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Wide-frame stat invariants.
 *
 * Per-port loss isolation and the unrecovered-packets counter cannot be
 * properly exercised with the default 2-pkt geometry because intra-frame
 * gaps collapse into the trivial first/last cases. These tests use a
 * 4-pkt geometry to express real intra-frame gaps on each wire.
 * ───────────────────────────────────────────────────────────────────────── */

class St20RxStatsWideTest : public ::testing::Test {
 protected:
  static constexpr int kPktsPerFrame = 4;
  ut20_test_ctx* ctx_ = nullptr;
  void SetUp() override {
    ASSERT_EQ(ut20_init(), 0);
    ctx_ = ut20_ctx_create_geom(2, kPktsPerFrame);
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    if (ctx_) ut20_ctx_destroy(ctx_);
  }
  void feed(int pkt_idx, uint32_t ts, enum mtl_session_port port) {
    ut20_feed_frame_pkt(ctx_, pkt_idx, ts, port);
  }
  int frames_received() {
    return ut20_frames_received(ctx_);
  }
  uint64_t frames_incomplete() {
    return ut20_stat_frames_incomplete(ctx_);
  }
  uint64_t lost_session() {
    return ut20_stat_lost_pkts(ctx_);
  }
  uint64_t pkts_unrecovered() {
    return ut20_stat_pkts_unrecovered(ctx_);
  }
  uint64_t port_lost(enum mtl_session_port p) {
    return ut20_stat_port_lost(ctx_, p);
  }
  /* Completed-frame per-port loss is charged when the slot is recycled; with
   * redundancy slot_max==2, so two fresh fully-redundant (zero-deficit) frames
   * rotate the slot window and finalise the frame under test. */
  void flush() {
    for (int f = 0; f < 2; f++) {
      uint32_t ts = 90000 + (uint32_t)f * 1000u;
      for (int i = 0; i < kPktsPerFrame; i++) {
        feed(i, ts, MTL_SESSION_PORT_P);
        feed(i, ts, MTL_SESSION_PORT_R);
      }
    }
  }
};

/* P delivers {0,3}, R delivers {1,2}; the frame reconstructs from the
 * union of both wires. Each port carries a per-frame deficit equal to the
 * two packets the OTHER port supplied. */
TEST_F(St20RxStatsWideTest, PortLossCountsOwnGapsOnly) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(1, 1000, MTL_SESSION_PORT_R);
  feed(2, 1000, MTL_SESSION_PORT_R);
  feed(3, 1000, MTL_SESSION_PORT_P);
  flush();

  EXPECT_EQ(frames_received(), 3);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), 2u) << "P missed pkts 1 and 2";
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_R), 2u) << "R missed pkts 0 and 3";
}

/* P delivers {0,3}, R delivers {2,1} (reordered on its own wire). Reorder
 * does not affect the loss count — only the per-frame delivery count at
 * finalisation matters, so each port carries a deficit of 2. */
TEST_F(St20RxStatsWideTest, PerPortLossIsIndependent) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(2, 1000, MTL_SESSION_PORT_R);
  feed(1, 1000, MTL_SESSION_PORT_R); /* reorder on R */
  feed(3, 1000, MTL_SESSION_PORT_P);
  flush();

  EXPECT_EQ(frames_received(), 3);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_R), 2u);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), 2u);
}

/* Per-wire loss with full cross-wire recovery. P skips indices that R
 * supplies. stat_lost_packets reflects the wire-level loss, but
 * stat_pkts_unrecovered must stay zero because reconstruction succeeded.
 * Pins the documented invariant
 *     stat_pkts_unrecovered <= stat_lost_packets. */
TEST_F(St20RxStatsWideTest, UnrecoveredZeroWhenReconstructionSucceeds) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(1, 1000, MTL_SESSION_PORT_R); /* recovers P's gap */
  feed(2, 1000, MTL_SESSION_PORT_R);
  feed(3, 1000, MTL_SESSION_PORT_P);
  flush();

  EXPECT_EQ(frames_received(), 3);
  EXPECT_GE(lost_session(), 1u) << "P's wire really skipped packets";
  EXPECT_EQ(pkts_unrecovered(), 0u)
      << "every gap on one wire was filled by the other wire";
  EXPECT_LE(pkts_unrecovered(), lost_session()) << "documented invariant";
}

/* Frame-pool exhaustion: app stops releasing frames → both counters bump. */
class St20RxFramePoolTest : public St20RxBaseTest {
 protected:
  /* Single port → slot_max == 1, the minimal geometry that exhibits the bug. */
  int num_port() const override {
    return 1;
  }

  void TearDown() override {
    if (ctx_) {
      ut20_set_hold_frames(ctx_, false);
    }
    St20RxBaseTest::TearDown();
  }

  void feed_first_pkt(uint32_t ts) {
    ut20_feed_frame_pkt(ctx_, 0, ts, MTL_SESSION_PORT_P);
  }
};

/* Pool exhaustion: app stops releasing frames → both counters bump,
 * warn line fires, "(sustained 3x)" appears, resets on recovery. */
TEST_F(St20RxFramePoolTest, BackpressureWarnLineEmitted) {
  auto run_stage = ut_session::capture_stderr;

  const std::string kWarnPrefix = "back-pressure: framebuff pool empty (";
  const std::string kFreeMid = " free), dropped ";
  const std::string kRemediation = "raise framebuff_cnt";

  /* Healthy: no deltas → no warn line, no info/notice for back-pressure. */
  std::string s1 = run_stage([&] {
    feed_full(1000, MTL_SESSION_PORT_P);
    feed_full(2000, MTL_SESSION_PORT_P);
    ut20_invoke_rv_stat(ctx_);
  });
  EXPECT_EQ(s1.find(kWarnPrefix), std::string::npos);
  EXPECT_EQ(s1.find("tmstamp outside OFO window"), std::string::npos);
  EXPECT_EQ(s1.find("framebuff pool empty"), std::string::npos);

  /* Starved interval 1: both deltas > 0 → unified warn line with gauge,
   * dropped counts, remediation; legacy lines emit at info. No sustained
   * suffix yet (counter == 1). */
  ut20_set_hold_frames(ctx_, true);
  feed_full(3000, MTL_SESSION_PORT_P);
  feed_full(4000, MTL_SESSION_PORT_P);
  std::string s2 = run_stage([&] {
    feed_first_pkt(5000);
    ut20_invoke_rv_stat(ctx_);
  });
  EXPECT_NE(s2.find(kWarnPrefix), std::string::npos) << "warn line missing: " << s2;
  EXPECT_NE(s2.find(kFreeMid), std::string::npos) << s2;
  EXPECT_NE(s2.find(kRemediation), std::string::npos) << s2;
  EXPECT_EQ(s2.find("(sustained "), std::string::npos)
      << "sustained suffix must not appear until 3 consecutive intervals";

  /* Starved intervals 2 and 3: counter increments each call. After the
   * third consecutive starved interval the "(sustained 3x)" suffix
   * appears on the warn line. */
  run_stage([&] {
    feed_first_pkt(6000);
    ut20_invoke_rv_stat(ctx_);
  });
  std::string s3 = run_stage([&] {
    feed_first_pkt(7000);
    ut20_invoke_rv_stat(ctx_);
  });
  EXPECT_NE(s3.find(kWarnPrefix), std::string::npos) << s3;
  std::regex sustained_re(R"(\(sustained [0-9]+x\))");
  EXPECT_TRUE(std::regex_search(s3, sustained_re))
      << "expected (sustained Nx) suffix after 3 consecutive starved intervals: " << s3;

  /* Recovery: pool drained → no deltas this interval → no warn, no
   * legacy lines, hysteresis counter resets so a future single-interval
   * starvation will again start at "no suffix". */
  ut20_set_hold_frames(ctx_, false);
  const uint64_t no_slot_after_recovery = no_slot();
  std::string s4 = run_stage([&] {
    feed_full(9000, MTL_SESSION_PORT_P);
    ut20_invoke_rv_stat(ctx_);
  });
  EXPECT_EQ(s4.find(kWarnPrefix), std::string::npos);
  EXPECT_EQ(s4.find("tmstamp outside OFO window"), std::string::npos);
  EXPECT_EQ(s4.find("framebuff pool empty"), std::string::npos);
  EXPECT_EQ(no_slot(), no_slot_after_recovery); /* freeze after drain */

  /* Single starved interval after recovery: warn appears WITHOUT
   * sustained suffix, proving the hysteresis counter reset. */
  ut20_set_hold_frames(ctx_, true);
  feed_full(10000, MTL_SESSION_PORT_P);
  feed_full(11000, MTL_SESSION_PORT_P);
  std::string s5 = run_stage([&] {
    feed_first_pkt(12000);
    ut20_invoke_rv_stat(ctx_);
  });
  EXPECT_NE(s5.find(kWarnPrefix), std::string::npos) << s5;
  EXPECT_EQ(s5.find("(sustained "), std::string::npos)
      << "hysteresis must have reset on recovery; got: " << s5;
}

/* The warn line's "M pkts" number must reflect ONLY pool-empty drops
 * (stat_pkts_pool_empty delta), not the wider stat_pkts_no_slot total
 * which is also bumped by past-tmstamp and DMA-busy paths. Drives a
 * concurrent non-pool-empty no_slot bump alongside a real pool-empty
 * interval and asserts the reported number isolates the pool-empty
 * cause. */
TEST_F(St20RxFramePoolTest, BackpressureReportsOnlyPoolEmptyPkts) {
  auto run_stage = ut_session::capture_stderr;

  /* Inject a non-pool-empty no_slot bump (simulating past-ts drops). */
  const uint64_t kPastTsBump = 7;
  ut20_bump_pkts_no_slot_past_ts(ctx_, kPastTsBump);

  /* Drive a real pool-empty interval: one frame attempt with the pool
   * held → exactly UT20_PKTS_PER_FRAME-worth of pool-empty pkts will
   * land in stat_pkts_pool_empty. */
  ut20_set_hold_frames(ctx_, true);
  feed_full(1000, MTL_SESSION_PORT_P);
  feed_full(2000, MTL_SESSION_PORT_P);
  std::string s = run_stage([&] {
    feed_first_pkt(3000);
    ut20_invoke_rv_stat(ctx_);
  });

  ASSERT_NE(s.find("back-pressure: framebuff pool empty ("), std::string::npos)
      << "warn line did not fire: " << s;

  /* Extract "dropped F frames / M pkts" — M must NOT include the
   * kPastTsBump past-ts pkts. */
  std::smatch m;
  std::regex dropped_re(R"(dropped ([0-9]+) frames / ([0-9]+) pkts)");
  ASSERT_TRUE(std::regex_search(s, m, dropped_re))
      << "warn line missing 'dropped F frames / M pkts': " << s;
  const uint64_t reported_pkts = std::stoull(m[2].str());

  /* Exact-equal makes this an oracle, not a bounded sanity check: any future
   * regression that sources the M-pkts from a counter other than
   * stat_pkts_pool_empty will mismatch. */
  EXPECT_EQ(reported_pkts, ut20_stat_pkts_pool_empty(ctx_))
      << "reported pkts must equal stat_pkts_pool_empty delta exactly: " << s;
  EXPECT_GT(reported_pkts, 0u) << "expected real pool-empty pkts: " << s;
}

/* Genuine post-redundancy loss: both wires drop the same packet, so the
 * missing index cannot be filled from either source. The frame must be
 * reported as incomplete and stat_pkts_unrecovered must record the
 * unrecoverable packet. Two further complete frames are fed so the
 * incomplete slot is reclaimed and finalised by the slot-window
 * manager. */
TEST_F(St20RxStatsWideTest, UnrecoveredWhenBothPortsMissSamePacket) {
  /* Neither wire delivers pkt 2 for ts = 1000. */
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(0, 1000, MTL_SESSION_PORT_R);
  feed(1, 1000, MTL_SESSION_PORT_P);
  feed(1, 1000, MTL_SESSION_PORT_R);
  feed(3, 1000, MTL_SESSION_PORT_P);
  feed(3, 1000, MTL_SESSION_PORT_R);
  /* Drive two more complete frames so the incomplete slot is reclaimed. */
  for (int i = 0; i < kPktsPerFrame; i++) feed(i, 2000, MTL_SESSION_PORT_P);
  for (int i = 0; i < kPktsPerFrame; i++) feed(i, 3000, MTL_SESSION_PORT_P);

  EXPECT_GE(frames_incomplete(), 1u) << "no port delivered pkt 2";
  EXPECT_GE(pkts_unrecovered(), 1u) << "the missing pkt is post-redundancy loss";
  EXPECT_LE(pkts_unrecovered(), lost_session())
      << "documented invariant: unrecovered <= lost";
}

/* ─────────────────────────────────────────────────────────────────────────
 * Per-port loss contract.
 *
 * Contract pinned by these cases:
 *   port[i].lost_packets += max(0, expected_this_frame - unique_arrivals_on_i)
 *                          charged at frame finalisation.
 *   expected_this_frame   = pkts_received + this frame's all-port holes.
 *   stat_lost_packets      = sum over ports of port[i].lost_packets.
 *
 * A per-packet forward-gap-on-each-port's-own-sequence model is blind to
 * frame-edge losses and double-counts packets dropped on BOTH ports. The
 * tests below isolate those two failure modes.
 * ───────────────────────────────────────────────────────────────────────── */

class St20RxStatsSinglePortTest : public ::testing::Test {
 protected:
  static constexpr int kPktsPerFrame = 4;
  ut20_test_ctx* ctx_ = nullptr;
  void SetUp() override {
    ASSERT_EQ(ut20_init(), 0);
    ctx_ = ut20_ctx_create_geom(1, kPktsPerFrame);
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    if (ctx_) ut20_ctx_destroy(ctx_);
  }
  void feed(int pkt_idx, uint32_t ts) {
    ut20_feed_frame_pkt(ctx_, pkt_idx, ts, MTL_SESSION_PORT_P);
  }
  uint64_t port_lost_p() {
    return ut20_stat_port_lost(ctx_, MTL_SESSION_PORT_P);
  }
  uint64_t lost_session() {
    return ut20_stat_lost_pkts(ctx_);
  }
  uint64_t pkts_unrecovered() {
    return ut20_stat_pkts_unrecovered(ctx_);
  }
  int frames_received() {
    return ut20_frames_received(ctx_);
  }
};

/* Single-port session: a packet dropped at the LEADING edge of the frame
 * has no prior packet on this port to compare against, so the per-packet
 * forward-gap detector cannot see it. The new contract recognises it as
 * a per-frame deficit at finalisation. */
TEST_F(St20RxStatsSinglePortTest, GapBeforeFirstSeenPktCountedAsPerPortLoss) {
  /* Frame ts=1000: pkt 0 dropped on wire; deliver pkts 1,2,3. */
  feed(1, 1000);
  feed(2, 1000);
  feed(3, 1000);
  /* Drive a clean second frame so the partial slot is reclaimed/finalised. */
  for (int i = 0; i < kPktsPerFrame; i++) feed(i, 2000);

  EXPECT_EQ(port_lost_p(), 1u)
      << "leading-edge loss on single-port session must charge port[P]";
  EXPECT_EQ(lost_session(), pkts_unrecovered())
      << "single-port: stat_lost_packets == stat_pkts_unrecovered";
}

/* P delivers contiguous prefix (pkts 0..2); R delivers only the trailing
 * pkt (3). The frame fully reconstructs but each port carries a deficit
 * equal to what the OTHER port covered, even though neither port observed
 * a per-packet forward gap in its own sequence stream. */
TEST_F(St20RxStatsWideTest, LastPacketDroppedOnPortCountsAsPerPortDeficit) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(1, 1000, MTL_SESSION_PORT_P);
  feed(2, 1000, MTL_SESSION_PORT_P);
  feed(3, 1000, MTL_SESSION_PORT_R); /* R's only contribution */
  flush();

  EXPECT_EQ(frames_received(), 3);
  EXPECT_EQ(pkts_unrecovered(), 0u) << "frame recovered through redundancy";
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), 1u) << "P missed pkt 3";
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_R), 3u) << "R missed pkts 0,1,2";
  EXPECT_EQ(lost_session(), port_lost(MTL_SESSION_PORT_P) + port_lost(MTL_SESSION_PORT_R))
      << "stat_lost_packets equals the sum of per-port deficits";
}

/* Both ports drop the same packet index: hole is unrecoverable. The new
 * contract charges the hole to EACH port's per-frame deficit, so
 * stat_lost_packets (= sum of per-port deficits) counts it once per port,
 * while stat_pkts_unrecovered counts the all-port hole exactly once. */
TEST_F(St20RxStatsWideTest, BothPortsMissSamePacketNoDoubleCount) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(0, 1000, MTL_SESSION_PORT_R);
  feed(1, 1000, MTL_SESSION_PORT_P);
  feed(1, 1000, MTL_SESSION_PORT_R);
  feed(3, 1000, MTL_SESSION_PORT_P);
  feed(3, 1000, MTL_SESSION_PORT_R);
  /* Drive two complete subsequent frames so the partial slot is reclaimed. */
  for (int i = 0; i < kPktsPerFrame; i++) {
    feed(i, 2000, MTL_SESSION_PORT_P);
    feed(i, 2000, MTL_SESSION_PORT_R);
  }
  for (int i = 0; i < kPktsPerFrame; i++) {
    feed(i, 3000, MTL_SESSION_PORT_P);
    feed(i, 3000, MTL_SESSION_PORT_R);
  }

  EXPECT_EQ(pkts_unrecovered(), 1u);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), 1u);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_R), 1u);
  EXPECT_EQ(lost_session(), port_lost(MTL_SESSION_PORT_P) + port_lost(MTL_SESSION_PORT_R))
      << "stat_lost_packets equals the sum of per-port deficits";
}

/* The very first frame can be corrupted — there is no prior complete
 * frame from which to latch "expected pkts per frame". Accounting must
 * derive `expected` from the in-flight frame's own geometry, otherwise
 * startup losses are silently dropped. */
TEST_F(St20RxStatsWideTest, FirstFrameCorruptedCountsLossCorrectly) {
  /* First frame: only pkts 0,1 delivered (both ports); pkts 2,3 dropped. */
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(0, 1000, MTL_SESSION_PORT_R);
  feed(1, 1000, MTL_SESSION_PORT_P);
  feed(1, 1000, MTL_SESSION_PORT_R);
  /* Drive two complete subsequent frames so the slot is reclaimed. */
  for (int i = 0; i < kPktsPerFrame; i++) {
    feed(i, 2000, MTL_SESSION_PORT_P);
    feed(i, 2000, MTL_SESSION_PORT_R);
  }
  for (int i = 0; i < kPktsPerFrame; i++) {
    feed(i, 3000, MTL_SESSION_PORT_P);
    feed(i, 3000, MTL_SESSION_PORT_R);
  }

  EXPECT_GE(frames_incomplete(), 1u);
  EXPECT_EQ(pkts_unrecovered(), 2u) << "pkts 2 and 3 unrecoverable on first frame";
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), 2u);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_R), 2u);
  EXPECT_EQ(lost_session(), port_lost(MTL_SESSION_PORT_P) + port_lost(MTL_SESSION_PORT_R))
      << "stat_lost_packets equals the sum of per-port deficits";
}

/* Opposite-edge drops: P provides contiguous prefix, R provides contiguous
 * suffix. Frame fully reconstructs but each port carries the deficit the
 * other port covered. */
TEST_F(St20RxStatsWideTest, ContiguousPrefixSuffixPerPortDeficit) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(1, 1000, MTL_SESSION_PORT_P);
  feed(2, 1000, MTL_SESSION_PORT_R);
  feed(3, 1000, MTL_SESSION_PORT_R);
  flush();

  EXPECT_EQ(frames_received(), 3);
  EXPECT_EQ(pkts_unrecovered(), 0u);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), 2u);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_R), 2u);
  EXPECT_EQ(lost_session(), port_lost(MTL_SESSION_PORT_P) + port_lost(MTL_SESSION_PORT_R))
      << "stat_lost_packets equals the sum of per-port deficits";
}

/* A completed-but-port-short frame whose slot is never recycled: P delivers
 * {0,3}, R delivers {1,2}; the frame finalises (loss_pending set) but no later
 * frame rotates its slot, so the deferred per-port deficit stays pending until
 * the detach flush runs, mirroring the production teardown path. */
TEST_F(St20RxStatsWideTest, FinalSlotDeficitChargedAtDetach) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(1, 1000, MTL_SESSION_PORT_R);
  feed(2, 1000, MTL_SESSION_PORT_R);
  feed(3, 1000, MTL_SESSION_PORT_P);

  EXPECT_EQ(frames_received(), 1);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), 0u)
      << "deficit deferred: the slot has not been recycled";
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_R), 0u);

  ut20_session_detach(ctx_);

  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), 2u) << "detach flush charges P's deficit";
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_R), 2u) << "detach flush charges R's deficit";
  EXPECT_EQ(lost_session(),
            port_lost(MTL_SESSION_PORT_P) + port_lost(MTL_SESSION_PORT_R));
}

/* Known, tolerated limitation: a same-wire duplicate/retransmit of an
 * already-received packet inflates pkts_recv_per_port, so the expected <= recv
 * guard in rv_slot_account_per_port_loss zeroes the genuine per-port deficit.
 * This is accepted to keep the redundant common-path twin credit correct
 * (unconditional ++ on the bitmap-set branch) and the per-packet handler cheap;
 * the precise fix would need per-port per-index delivery tracking. This test
 * PINS the masked behaviour so a future change to it is a conscious decision. */
TEST(St20Stats, same_port_dup_masks_deficit_known_limitation) {
  ASSERT_EQ(ut20_init(), 0);
  ut20_test_ctx* ctx = ut20_ctx_create_geom(1, 4);
  ASSERT_NE(ctx, nullptr);

  ut20_feed_frame_pkt(ctx, 0, 1000, MTL_SESSION_PORT_P);
  ut20_feed_frame_pkt(ctx, 1, 1000, MTL_SESSION_PORT_P);
  ut20_feed_frame_pkt(ctx, 3, 1000, MTL_SESSION_PORT_P); /* pkt 2 lost on wire */
  ut20_feed_frame_pkt(ctx, 0, 1000, MTL_SESSION_PORT_P); /* same-port duplicate */

  /* Recycle the incomplete slot so its deferred deficit is finalised. */
  for (int i = 0; i < 4; i++) ut20_feed_frame_pkt(ctx, i, 2000, MTL_SESSION_PORT_P);

  EXPECT_EQ(ut20_stat_port_lost(ctx, MTL_SESSION_PORT_P), 0u)
      << "same-port dup inflates recv; deficit guard masks the per-port hole";
  EXPECT_EQ(ut20_stat_lost_pkts(ctx), 0u);

  ut20_ctx_destroy(ctx);
}

/* Control for the regression above: the identical 0,1,3 gap WITHOUT the masking
 * duplicate must charge the loss. If this also reads 0, incomplete-frame
 * charging is broken more broadly than the duplicate-masking case. */
TEST(St20Stats, single_port_gap_charges_deficit_control) {
  ASSERT_EQ(ut20_init(), 0);
  ut20_test_ctx* ctx = ut20_ctx_create_geom(1, 4);
  ASSERT_NE(ctx, nullptr);

  ut20_feed_frame_pkt(ctx, 0, 1000, MTL_SESSION_PORT_P);
  ut20_feed_frame_pkt(ctx, 1, 1000, MTL_SESSION_PORT_P);
  ut20_feed_frame_pkt(ctx, 3, 1000, MTL_SESSION_PORT_P); /* pkt 2 lost on wire */

  for (int i = 0; i < 4; i++) ut20_feed_frame_pkt(ctx, i, 2000, MTL_SESSION_PORT_P);

  EXPECT_EQ(ut20_stat_port_lost(ctx, MTL_SESSION_PORT_P), 1u);
  EXPECT_EQ(ut20_stat_lost_pkts(ctx), 1u);

  ut20_ctx_destroy(ctx);
}

/* RTP-passthrough mode (ST20_TYPE_RTP_LEVEL) has no frame-completion event to
 * defer per-port loss to, so rv_handle_rtp_pkt INTENTIONALLY keeps the inline
 * forward-gap charge that frame mode dropped. This pins that a sequence gap is
 * still counted in RTP mode. Feed seq 0,1,3 (gap at 2); the loss must surface
 * inline in stat_lost_packets without any frame finalisation. */
TEST(St20Stats, rtp_mode_loss_accounting_regression) {
  ASSERT_EQ(ut20_init(), 0);
  ut20_test_ctx* ctx = ut20_ctx_create_geom(1, 4);
  ASSERT_NE(ctx, nullptr);
  ut20_ctx_enable_rtp(ctx);

  ut20_feed_rtp_pkt(ctx, 0, 0, 1000, MTL_SESSION_PORT_P);
  ut20_feed_rtp_pkt(ctx, 1, 1, 1000, MTL_SESSION_PORT_P);
  ut20_feed_rtp_pkt(ctx, 3, 3, 1000, MTL_SESSION_PORT_P); /* seq 2 lost on wire */

  EXPECT_GE(ut20_stat_lost_pkts(ctx), 1u)
      << "RTP-mode sequence gap at seq 2 must charge at least one lost packet";

  ut20_ctx_destroy(ctx);
}
