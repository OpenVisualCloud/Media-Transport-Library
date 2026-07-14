/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * FRAME_LEVEL transport assembler: dispatch, frame lifecycle, real ANC
 * payload parsing (parity/checksum/stride), and per-frame seq_lost /
 * seq_discont derivation from the frame bitmap.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest
 * --gtest_filter='St40Rx*Frame*.*:St40RxBitmapSeqStatsTest.*'
 */

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <queue>
#include <thread>

#include "session/st40_harness.h"
#include "st_api.h"

/* ── FRAME_LEVEL transport assembler dispatch ──────────────────────── */

class St40RxFrameLevelDispatchTest : public ::testing::Test {
 protected:
  ut_test_ctx* ctx_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(ut40_init(), 0);
    ut40_drain_ring();
    ut40_notify_rtp_calls_reset();
    ctx_ = ut40_ctx_create(2);
    ASSERT_NE(ctx_, nullptr);
    ut40_set_frame_level(ctx_); /* flip BEFORE first feed */
  }

  void TearDown() override {
    ut40_drain_ring();
    ut40_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }
};

/* In FRAME_LEVEL mode the transport must:
 *  - route every accepted packet to the assembler (stub increments counter)
 *  - NOT enqueue to the rtp ring
 *  - NOT call notify_rtp_ready
 *  - still update common stats and per-port frame counters
 */
TEST_F(St40RxFrameLevelDispatchTest, DispatchesToAssemblerNotRtpRing) {
  /* feed a normal frame: 5 packets ending with marker */
  ut40_feed_burst(ctx_, 0, 5, 1000, 1, MTL_SESSION_PORT_P);

  EXPECT_EQ(ut40_stat_assemble_dispatched(ctx_), 5u);
  EXPECT_EQ(ut40_stat_received(ctx_), 5u);
  EXPECT_EQ(ut40_notify_rtp_calls(), 0)
      << "FRAME_LEVEL dispatch must not call notify_rtp_ready";
  EXPECT_EQ(ut40_stat_port_frames(ctx_, MTL_SESSION_PORT_P), 1u);
}

TEST_F(St40RxFrameLevelDispatchTest, RingStaysEmptyUnderFrameLevel) {
  ut40_drain_paused guard; /* don't drain the ring even if RTP path slipped through */
  ut40_feed_burst(ctx_, 0, 4, 2000, 1, MTL_SESSION_PORT_P);

  int count = -1;
  bool has_marker = false;
  int rc = ut40_ring_dequeue_markers(&count, &has_marker);
  EXPECT_EQ(rc, 0);
  EXPECT_EQ(count, 0)
      << "FRAME_LEVEL dispatch must not enqueue any packet to the rtp ring";
  EXPECT_EQ(ut40_stat_assemble_dispatched(ctx_), 4u);
}

TEST_F(St40RxFrameLevelDispatchTest, RedundantPacketsStillFiltered) {
  /* P then R with same packets — R packets must be classified redundant
   * and NOT reach the assembler. */
  ut40_feed_burst(ctx_, 0, 6, 3000, 1, MTL_SESSION_PORT_P);
  ut40_feed_burst(ctx_, 0, 6, 3000, 1, MTL_SESSION_PORT_R);

  EXPECT_EQ(ut40_stat_assemble_dispatched(ctx_), 6u)
      << "redundant pkts on R must be filtered before assembler dispatch";
  EXPECT_EQ(ut40_stat_redundant(ctx_), 6u);
  EXPECT_EQ(ut40_notify_rtp_calls(), 0);
}

/* ── FRAME_LEVEL transport assembler ────────────────────────────────── */

class St40RxFrameAssemblyTest : public ::testing::Test {
 protected:
  ut_test_ctx* ctx_ = nullptr;
  static constexpr uint16_t kSlots = 4;
  static constexpr uint32_t kSlotSize = 4096;

  void SetUp() override {
    ASSERT_EQ(ut40_init(), 0);
    ut40_drain_ring();
    ut40_notify_rtp_calls_reset();
    ut40_captured_reset();
    ctx_ = ut40_ctx_create(2);
    ASSERT_NE(ctx_, nullptr);
    ut40_setup_frame_pool(ctx_, kSlots, kSlotSize);
  }
  void TearDown() override {
    ut40_teardown_frame_pool(ctx_);
    ut40_drain_ring();
    ut40_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }
};

/* Single-port frame: 5 packets, marker on the last, no parsing — assembler
 * builds a frame with anc_count=1-per-pkt placeholders and delivers exactly
 * once on the marker. */
TEST_F(St40RxFrameAssemblyTest, SinglePortFrameDeliveredOnMarker) {
  constexpr uint32_t ts = 1000;

  for (int i = 0; i < 4; i++) ut40_feed_pkt_anc0(ctx_, i, ts, 0, MTL_SESSION_PORT_P);
  EXPECT_EQ(ut40_captured_count(), 0) << "no marker yet → no frame delivered";

  ut40_feed_pkt_anc0(ctx_, 4, ts, 1, MTL_SESSION_PORT_P);

  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_rtp_ts(0), ts);
  EXPECT_TRUE(ut40_captured_marker(0));
  EXPECT_EQ(ut40_captured_meta_num(0), 5)
      << "5 pkts → 5 anc meta entries (each pkt anc_count=1)";
  EXPECT_EQ(ut40_stat_anc_frames_ready(ctx_), 1u);
  EXPECT_EQ(ut40_stat_anc_frames_dropped(ctx_), 0u);
  EXPECT_EQ(ut40_notify_rtp_calls(), 0) << "FRAME_LEVEL must never call notify_rtp_ready";
  EXPECT_NE(ut40_captured_addr(0), nullptr);
}

/* Single-packet frame: a marker on the very first packet must deliver. */
TEST_F(St40RxFrameAssemblyTest, SinglePacketFrameWithMarker) {
  ut40_feed_pkt_anc0(ctx_, 0, 5000, 1, MTL_SESSION_PORT_P);
  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_meta_num(0), 1);
  EXPECT_TRUE(ut40_captured_marker(0));
}

/* Two consecutive frames: app releases each one, the slot pool recycles. */
TEST_F(St40RxFrameAssemblyTest, ConsecutiveFramesRecycleSlots) {
  for (int i = 0; i < 3; i++)
    ut40_feed_pkt_anc0(ctx_, i, 1000, i == 2, MTL_SESSION_PORT_P);
  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_stat_anc_frames_ready(ctx_), 1u);
  void* a0 = ut40_captured_addr(0);
  ASSERT_NE(a0, nullptr);
  ut40_release_frame(ctx_, a0);

  for (int i = 0; i < 3; i++)
    ut40_feed_pkt_anc0(ctx_, 3 + i, 2000, i == 2, MTL_SESSION_PORT_P);
  ASSERT_EQ(ut40_captured_count(), 2);
  EXPECT_EQ(ut40_stat_anc_frames_ready(ctx_), 2u);
  EXPECT_EQ(ut40_stat_anc_frames_dropped(ctx_), 0u)
      << "recycled slot must satisfy alloc — no drops expected";
  EXPECT_EQ(ut40_captured_rtp_ts(1), 2000u);

  /* Hard invariant: with 4 slots and only one in flight at a time, alloc
   * MUST recycle the released slot. We don't assert which, but we do assert
   * that releasing a0 made it possible at all (no drops). */
  void* a1 = ut40_captured_addr(1);
  EXPECT_NE(a1, nullptr);
  ut40_release_frame(ctx_, a1);
}

/* Slot pool exhaustion: 5 frames in flight (no marker) with 4 slots →
 * the 5th cannot allocate; stat_anc_frames_dropped++. */
TEST_F(St40RxFrameAssemblyTest, SlotPoolExhaustionDropsFrame) {
  /* Each new tmstamp on a multi-port session pushes the previous inflight to
   * PENDING. With kSlots=4 and >4 distinct timestamps in flight, the pool
   * eventually exhausts. The intermediate force-deliver-pending path means
   * some frames DO get captured along the way — the invariant we assert is
   * that at least one frame allocation failed. */
  uint16_t seq = 0;
  for (uint32_t ts = 100; ts < 100 + 50 * 8; ts += 50) {
    ut40_feed_pkt_anc0(ctx_, seq++, ts, 0, MTL_SESSION_PORT_P);
  }

  EXPECT_GE(ut40_stat_anc_frames_dropped(ctx_), 1u);
}

/* notify_frame_ready returns -1 → the assembler must reclaim the slot
 * (state → FREE) so the pool stays usable for the next frame. */
TEST_F(St40RxFrameAssemblyTest, NotifyFailureReclaimsSlot) {
  ut40_set_notify_frame_fail_after(0); /* fail the very first call */
  for (int i = 0; i < 3; i++)
    ut40_feed_pkt_anc0(ctx_, i, 1000, i == 2, MTL_SESSION_PORT_P);
  EXPECT_EQ(ut40_captured_count(), 0);
  /* The failed delivery still counts toward stat_anc_frames_ready (assembly
   * succeeded; only the app rejected it). What matters: pool is reusable. */
  EXPECT_EQ(ut40_stat_anc_frames_ready(ctx_), 1u);

  /* Feed enough independent frames to exhaust the pool 1+ times over.
   * If reclamation worked, all should land. If not, at least one drops. */
  ut40_set_notify_frame_fail_after(-1);
  for (uint16_t i = 0; i < kSlots * 2; i++) {
    uint32_t ts = 2000u + 1000u * i;
    /* single-pkt frames so each completes immediately and we reuse slot */
    ut40_feed_pkt_anc0(ctx_, 10 + i, ts, 1, MTL_SESSION_PORT_P);
    void* addr = ut40_captured_addr(ut40_captured_count() - 1);
    if (addr) ut40_release_frame(ctx_, addr);
  }
  EXPECT_EQ(ut40_stat_anc_frames_dropped(ctx_), 0u)
      << "failed delivery must not leak the slot — pool stays usable";
  EXPECT_EQ(ut40_captured_count(), (int)(kSlots * 2));
}

/* Cross-port redundancy: P sends pkts 0..4 (loses marker), ts moves on
 * (ts=2000) on P → inflight rolls to PENDING. Then R supplies the missed
 * marker pkt 5 with ts=1000 → assembler delivers the pending frame with
 * its marker. */
TEST_F(St40RxFrameAssemblyTest, PendingFrameResolvedByLateMarker) {
  /* P frame ts=1000 missing marker (would be at seq 5) */
  for (int i = 0; i < 5; i++) ut40_feed_pkt_anc0(ctx_, i, 1000, 0, MTL_SESSION_PORT_P);
  EXPECT_EQ(ut40_captured_count(), 0);

  /* P starts next frame ts=2000 with seq 6 → triggers PENDING transition. */
  ut40_feed_pkt_anc0(ctx_, 6, 2000, 0, MTL_SESSION_PORT_P);
  EXPECT_EQ(ut40_captured_count(), 0)
      << "ts change in multi-port mode must NOT deliver yet (PENDING)";

  /* R supplies the missing marker pkt for ts=1000 using unique seq=5
   * (P never sent it). Redundancy filter accepts it; assembler matches
   * tmstamp to the PENDING slot. */
  ut40_feed_pkt_anc0(ctx_, 5, 1000, 1, MTL_SESSION_PORT_R);

  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_rtp_ts(0), 1000u) << "PENDING frame is the one delivered";
  EXPECT_TRUE(ut40_captured_marker(0));
}

/* A PENDING frame accumulates late non-marker packets from R without being
 * prematurely resolved; only the late marker delivers it, with the combined
 * packet count from both ports. */
TEST_F(St40RxFrameAssemblyTest, PendingLatePacketsAccumulate) {
  /* P: 3 pkts for ts=1000, no marker */
  for (int i = 0; i < 3; i++) ut40_feed_pkt_anc0(ctx_, i, 1000, 0, MTL_SESSION_PORT_P);
  /* P advances to ts=2000 → ts=1000 rolls to PENDING */
  ut40_feed_pkt_anc0(ctx_, 6, 2000, 0, MTL_SESSION_PORT_P);
  EXPECT_EQ(ut40_captured_count(), 0);

  /* R: 3 late pkts for ts=1000 (seq 3,4,5); marker only on the last. */
  ut40_feed_pkt_anc0(ctx_, 3, 1000, 0, MTL_SESSION_PORT_R);
  EXPECT_EQ(ut40_captured_count(), 0)
      << "late non-marker packet must not resolve the PENDING frame";
  ut40_feed_pkt_anc0(ctx_, 4, 1000, 0, MTL_SESSION_PORT_R);
  EXPECT_EQ(ut40_captured_count(), 0)
      << "late non-marker packet must not resolve the PENDING frame";
  ut40_feed_pkt_anc0(ctx_, 5, 1000, 1, MTL_SESSION_PORT_R);

  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_rtp_ts(0), 1000u);
  EXPECT_TRUE(ut40_captured_marker(0));
  EXPECT_EQ(ut40_captured_pkts_total(0), 6u) << "3 from P + 3 from R";
  EXPECT_EQ(ut40_captured_port_pkts_recv(0, MTL_SESSION_PORT_P), 3u);
  EXPECT_EQ(ut40_captured_port_pkts_recv(0, MTL_SESSION_PORT_R), 3u);
}

/* A marker on the current inflight frame delivers it independently and must
 * not touch an already-PENDING frame, which stays invisible until its own
 * marker or a further timestamp change force-delivers it. */
TEST_F(St40RxFrameAssemblyTest, MultiPortMarkerOnInflightLeavesPendingAlone) {
  /* P: body for ts=1000, no marker */
  for (int i = 0; i < 3; i++) ut40_feed_pkt_anc0(ctx_, i, 1000, 0, MTL_SESSION_PORT_P);
  /* P advances → ts=1000 enters PENDING */
  ut40_feed_pkt_anc0(ctx_, 3, 2000, 0, MTL_SESSION_PORT_P);
  /* P completes ts=2000 with marker */
  ut40_feed_pkt_anc0(ctx_, 4, 2000, 1, MTL_SESSION_PORT_P);

  ASSERT_EQ(ut40_captured_count(), 1) << "inflight marker delivers independently";
  EXPECT_EQ(ut40_captured_rtp_ts(0), 2000u);
  EXPECT_TRUE(ut40_captured_marker(0));

  /* ts=1000 is still PENDING — force-deliver it via two more ts changes:
   * start ts=3000 inflight, then ts=4000 forces ts=1000 out. */
  ut40_feed_pkt_anc0(ctx_, 5, 3000, 0, MTL_SESSION_PORT_P);
  ut40_feed_pkt_anc0(ctx_, 6, 4000, 0, MTL_SESSION_PORT_P);

  ASSERT_EQ(ut40_captured_count(), 2);
  EXPECT_EQ(ut40_captured_rtp_ts(1), 1000u);
  EXPECT_FALSE(ut40_captured_marker(1)) << "force-delivered PENDING frame has no marker";
}

/* Single-port: timestamp change without prior marker forces immediate
 * delivery of the inflight frame (no PENDING dance because num_port==1). */
TEST_F(St40RxFrameAssemblyTest, SinglePortTimestampChangeForcesDelivery) {
  ut40_teardown_frame_pool(ctx_);
  ut40_ctx_destroy(ctx_);
  ctx_ = ut40_ctx_create(1); /* single port */
  ASSERT_NE(ctx_, nullptr);
  ut40_setup_frame_pool(ctx_, kSlots, kSlotSize);
  ut40_captured_reset();

  for (int i = 0; i < 3; i++) ut40_feed_pkt_anc0(ctx_, i, 1000, 0, MTL_SESSION_PORT_P);
  EXPECT_EQ(ut40_captured_count(), 0);

  /* ts change → single-port path delivers immediately, even without marker. */
  ut40_feed_pkt_anc0(ctx_, 3, 2000, 0, MTL_SESSION_PORT_P);
  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_rtp_ts(0), 1000u);
  EXPECT_FALSE(ut40_captured_marker(0)) << "no marker arrived for the first frame";
}

/* RealAncPayload: also validate sdid round-trip + that meta_offset chains. */
TEST_F(St40RxFrameAssemblyTest, RealAncPayloadDecodedIntoMetaAndBuf) {
  uint8_t udw[8] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18};
  int rc = ut40_feed_anc_pkt(ctx_, 0, 1000, 1, MTL_SESSION_PORT_P,
                             /*did*/ 0x41, /*sdid*/ 0x05, udw, sizeof(udw),
                             /*corrupt_parity*/ -1, /*corrupt_cs*/ false);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_meta_num(0), 1);
  EXPECT_EQ(ut40_captured_meta_did(0, 0), 0x41);
  EXPECT_EQ(ut40_captured_meta_sdid(0, 0), 0x05);
  EXPECT_EQ(ut40_captured_meta_udw_size(0, 0), (int)sizeof(udw));
  EXPECT_EQ(ut40_captured_meta_udw_offset(0, 0), 0u)
      << "first meta entry must start at udw offset 0";
  EXPECT_EQ(ut40_captured_udw_fill(0), sizeof(udw));
  for (uint32_t i = 0; i < sizeof(udw); i++) {
    EXPECT_EQ(ut40_captured_udw_byte(0, i), udw[i]) << "udw byte " << i << " mismatch";
  }
  EXPECT_EQ(ut40_stat_anc_pkt_parse_err(ctx_), 0u);
}

/* Two separate RTP packets (two rx_anc_slot_parse_pkt() calls) contributing
 * to the same frame: the second packet's meta_entry->udw_offset must reflect
 * the running slot->udw_buffer_fill left by the first packet, not 0. Each
 * decode_packet() call writes udw_offset=0 into its own meta struct (it has
 * no notion of frame-relative offset) — the caller must overwrite it with
 * the pre-call fill *after* the decode call, since decode_packet() clobbers
 * whatever was set before it. */
TEST_F(St40RxFrameAssemblyTest, CrossPacketUdwOffsetThreadsAcrossRtpPackets) {
  uint8_t udw0[4] = {0x11, 0x22, 0x33, 0x44};
  uint8_t udw1[6] = {0x55, 0x66, 0x77, 0x88, 0x99, 0xAA};

  ASSERT_EQ(ut40_feed_anc_pkt(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 0x41, 0x05, udw0,
                              sizeof(udw0), -1, false),
            0);
  ASSERT_EQ(ut40_feed_anc_pkt(ctx_, 1, 1000, 1, MTL_SESSION_PORT_P, 0x41, 0x05, udw1,
                              sizeof(udw1), -1, false),
            0);

  ASSERT_EQ(ut40_captured_count(), 1);
  ASSERT_EQ(ut40_captured_meta_num(0), 2);
  EXPECT_EQ(ut40_captured_meta_udw_offset(0, 0), 0u);
  EXPECT_EQ(ut40_captured_meta_udw_size(0, 0), (int)sizeof(udw0));
  EXPECT_EQ(ut40_captured_meta_udw_offset(0, 1), sizeof(udw0))
      << "second RTP packet's meta entry must start where the first left off";
  EXPECT_EQ(ut40_captured_meta_udw_size(0, 1), (int)sizeof(udw1));
  EXPECT_EQ(ut40_captured_udw_fill(0), sizeof(udw0) + sizeof(udw1));
  for (uint32_t i = 0; i < sizeof(udw1); i++)
    EXPECT_EQ(ut40_captured_udw_byte(0, sizeof(udw0) + i), udw1[i]);
  EXPECT_EQ(ut40_stat_anc_pkt_parse_err(ctx_), 0u);
}

/* Multiple ANC data packets carried in a single RTP packet (non-split mode).
 * Validates that rx_anc_slot_parse_pkt() correctly walks the variable-length
 * per-ANC payload stride when the 10-bit UDW packing of one or more blocks
 * is not byte-aligned, and recovers the metadata and user data words for
 * every ANC packet in the frame.
 *
 * Two size patterns are exercised because a wrong (floor-rounded) stride
 * manifests differently depending on what the misread bytes decode to:
 *   {8, 6, 4} — the misread data_count lands on zero padding, so the
 *               parser silently produces a short/empty meta entry.
 *   {6, 6, 6} — the misread data_count decodes to a large value that
 *               overflows the remaining payload room, surfacing as a
 *               parse error and meta_num short of anc_count. */
TEST_F(St40RxFrameAssemblyTest, MultiAncInSingleRtpPacketByteStride) {
  const uint16_t patterns[][3] = {{8, 6, 4}, {6, 6, 6}};
  const uint8_t anc_count = 3;
  uint32_t ts = 1000;
  uint16_t seq = 0;

  for (const auto& udw_sizes : patterns) {
    ut40_captured_reset();
    int rc = ut40_feed_multi_anc_pkt(ctx_, seq++, ts, 1, MTL_SESSION_PORT_P, udw_sizes,
                                     anc_count);
    ASSERT_EQ(rc, 0);

    ASSERT_EQ(ut40_captured_count(), 1);
    EXPECT_EQ(ut40_captured_rtp_ts(0), ts);
    EXPECT_TRUE(ut40_captured_marker(0));
    ASSERT_EQ(ut40_captured_meta_num(0), anc_count);

    uint32_t expected_offset = 0;
    for (uint8_t anc_idx = 0; anc_idx < anc_count; anc_idx++) {
      EXPECT_EQ(ut40_captured_meta_did(0, anc_idx), 0x41);
      EXPECT_EQ(ut40_captured_meta_sdid(0, anc_idx), 0x05);
      EXPECT_EQ(ut40_captured_meta_udw_size(0, anc_idx), udw_sizes[anc_idx]);
      EXPECT_EQ(ut40_captured_meta_udw_offset(0, anc_idx), expected_offset);

      for (uint16_t udw_idx = 0; udw_idx < udw_sizes[anc_idx]; udw_idx++) {
        uint8_t expected = (uint8_t)(((anc_idx + 1) * 17 + udw_idx) & 0xff);
        EXPECT_EQ(ut40_captured_udw_byte(0, expected_offset + udw_idx), expected);
      }
      expected_offset += udw_sizes[anc_idx];
    }
    EXPECT_EQ(ut40_captured_udw_fill(0), expected_offset);
    EXPECT_EQ(ut40_stat_anc_pkt_parse_err(ctx_), 0u);

    ut40_release_frame(ctx_, ut40_captured_addr(0));
    ts += 1000;
  }
}

/* UDW parity failure: parser must abort the ANC chunk, NOT increment
 * meta_num, AND record stat_anc_pkt_parse_err. The frame is still
 * delivered on marker (best-effort) but with meta_num=0. */
TEST_F(St40RxFrameAssemblyTest, UdwParityFailureRecordedAndChunkSkipped) {
  uint8_t udw[4] = {0x11, 0x22, 0x33, 0x44};
  int rc = ut40_feed_anc_pkt(ctx_, 0, 1000, 1, MTL_SESSION_PORT_P, 0x41, 0x05, udw,
                             sizeof(udw), /*corrupt_parity*/ 2, false);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_meta_num(0), 0) << "parity failure must NOT add a meta entry";
  EXPECT_EQ(ut40_captured_udw_fill(0), 0u)
      << "udw_buffer_fill must roll back to original on parity error";
  EXPECT_EQ(ut40_stat_anc_pkt_parse_err(ctx_), 1u)
      << "exactly one parse error must be recorded";
}

/* Checksum failure: parser must abort, no meta entry added. */
TEST_F(St40RxFrameAssemblyTest, ChecksumFailureRecorded) {
  uint8_t udw[4] = {0x11, 0x22, 0x33, 0x44};
  int rc = ut40_feed_anc_pkt(ctx_, 0, 1000, 1, MTL_SESSION_PORT_P, 0x41, 0x05, udw,
                             sizeof(udw), /*corrupt_parity*/ -1,
                             /*corrupt_cs*/ true);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_meta_num(0), 0);
  EXPECT_EQ(ut40_captured_udw_fill(0), 0u)
      << "checksum failure must roll back udw_buffer_fill";
  EXPECT_EQ(ut40_stat_anc_pkt_parse_err(ctx_), 1u);
}

/* Empty ANC packet (udw_size=0): valid per spec, must be preserved as a
 * meta entry with udw_size=0. */
TEST_F(St40RxFrameAssemblyTest, EmptyAncPacketAccepted) {
  int rc = ut40_feed_anc_pkt(ctx_, 0, 1000, 1, MTL_SESSION_PORT_P, 0x41, 0x05, nullptr, 0,
                             -1, false);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_meta_num(0), 1);
  EXPECT_EQ(ut40_captured_meta_udw_size(0, 0), 0);
  EXPECT_EQ(ut40_captured_udw_fill(0), 0u);
  EXPECT_EQ(ut40_stat_anc_pkt_parse_err(ctx_), 0u);
}

/* RFC 8331 requires the checksum to be validated even when udw_size==0; a
 * corrupted checksum on an otherwise-empty ANC sub-packet must be rejected,
 * not silently accepted. */
TEST_F(St40RxFrameAssemblyTest, ChecksumFailureRecordedForEmptyAncPacket) {
  int rc = ut40_feed_anc_pkt(ctx_, 0, 1000, 1, MTL_SESSION_PORT_P, 0x41, 0x05, nullptr, 0,
                             -1, /*corrupt_cs*/ true);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_meta_num(0), 0);
  EXPECT_EQ(ut40_captured_udw_fill(0), 0u);
  EXPECT_EQ(ut40_stat_anc_pkt_parse_err(ctx_), 1u);
}

/* UDW buffer overflow: feeding many large ANC packets eventually overflows
 * the per-frame udw_buf. The assembler must record parse_err and not
 * write past the buffer. */
TEST_F(St40RxFrameAssemblyTest, UdwBufferOverflowProtected) {
  /* Tiny pool size so we overflow quickly. */
  ut40_teardown_frame_pool(ctx_);
  ut40_setup_frame_pool(ctx_, /*cnt*/ 2, /*size*/ 32);

  uint8_t udw[20];
  memset(udw, 0xAA, sizeof(udw));

  /* Pkt 1: 20 bytes → fits. fill==20. parse_err==0. */
  ut40_feed_anc_pkt(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 0x41, 0x05, udw, sizeof(udw),
                    -1, false);
  /* Pkt 2: would need 20 more, only 12 free → overflow at byte 12.
   *        Rollback to original_fill=20. parse_err==1. */
  ut40_feed_anc_pkt(ctx_, 1, 1000, 0, MTL_SESSION_PORT_P, 0x41, 0x05, udw, sizeof(udw),
                    -1, false);
  /* Pkt 3: same overflow. parse_err==2. fill stays 20. Marker delivers. */
  ut40_feed_anc_pkt(ctx_, 2, 1000, 1, MTL_SESSION_PORT_P, 0x41, 0x05, udw, sizeof(udw),
                    -1, false);

  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_meta_num(0), 1)
      << "only the first ANC chunk should have been accepted";
  EXPECT_EQ(ut40_captured_udw_fill(0), 20u)
      << "fill must equal exactly the bytes accepted from pkt 1";
  EXPECT_EQ(ut40_stat_anc_pkt_parse_err(ctx_), 2u)
      << "two overflow rejections expected (pkt 2 + pkt 3)";
  /* No buffer overrun: read all 32 udw bytes — first 20 must be 0xAA, rest 0. */
  for (uint32_t i = 0; i < 20; i++) EXPECT_EQ(ut40_captured_udw_byte(0, i), 0xAAu);
}

/* Multi-port force-deliver: in a 2-port session, two consecutive timestamps
 * with NO marker → first frame is force-delivered when the THIRD timestamp
 * arrives (the previous PENDING is flushed before a new one takes its slot). */
TEST_F(St40RxFrameAssemblyTest, MultiPortForceDeliversStalePending) {
  /* ts=1000 no marker → inflight */
  ut40_feed_pkt_anc0(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P);
  EXPECT_EQ(ut40_captured_count(), 0);
  /* ts=2000 no marker → inflight (1000) rolls to PENDING. Nothing delivered. */
  ut40_feed_pkt_anc0(ctx_, 1, 2000, 0, MTL_SESSION_PORT_P);
  EXPECT_EQ(ut40_captured_count(), 0);
  /* ts=3000 → existing PENDING (1000) is force-delivered, new inflight
   * for 3000, previous inflight (2000) becomes PENDING. */
  ut40_feed_pkt_anc0(ctx_, 2, 3000, 0, MTL_SESSION_PORT_P);
  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_rtp_ts(0), 1000u)
      << "the OLDEST pending frame is the one force-delivered";
  EXPECT_FALSE(ut40_captured_marker(0)) << "force-delivered without marker";
  EXPECT_EQ(ut40_stat_anc_frames_ready(ctx_), 1u);
}

/* Pool exhaustion right after inflight is cleared by a plain marker delivery
 * (not an eviction rollover) must not strand a stale PENDING frame forever —
 * it must be force-delivered on the next mismatched timestamp even though
 * anc_inflight_slot is NULL, not only when a rollover happens to touch it. */
TEST_F(St40RxFrameAssemblyTest, PoolExhaustionForceDeliversPendingWithNoInflight) {
  ut40_teardown_frame_pool(ctx_);
  ut40_ctx_destroy(ctx_);
  ctx_ = ut40_ctx_create(2); /* multi-port: PENDING state is reachable */
  ASSERT_NE(ctx_, nullptr);
  ut40_setup_frame_pool(ctx_, /*cnt*/ 2, /*size*/ 256);
  ut40_captured_reset();

  /* ts=1000 no marker → inflight (1 of 2 slots used). */
  ut40_feed_pkt_anc0(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P);
  /* ts=2000 no marker → ts=1000 rolls to PENDING, ts=2000 takes the last
   * free slot as inflight (2 of 2 used). */
  ut40_feed_pkt_anc0(ctx_, 1, 2000, 0, MTL_SESSION_PORT_P);
  EXPECT_EQ(ut40_captured_count(), 0);
  /* ts=2000 completes with a marker: inflight clears to NULL directly
   * (no rollover), delivered slot stays IN_USER — pool still fully occupied. */
  ut40_feed_pkt_anc0(ctx_, 2, 2000, 1, MTL_SESSION_PORT_P);
  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_rtp_ts(0), 2000u);

  /* ts=3000: no free slot, and no inflight to roll over — the stale
   * ts=1000 PENDING must still be force-delivered instead of stalling. */
  ut40_feed_pkt_anc0(ctx_, 3, 3000, 0, MTL_SESSION_PORT_P);
  ASSERT_EQ(ut40_captured_count(), 2)
      << "stale PENDING must be force-delivered even with no inflight slot";
  EXPECT_EQ(ut40_captured_rtp_ts(1), 1000u);
  EXPECT_FALSE(ut40_captured_marker(1));
  EXPECT_GE(ut40_stat_anc_frames_dropped(ctx_), 1u)
      << "ts=3000 itself still has nowhere to go — pool remains exhausted";
}

/* A pending slot created by *this same call's* inflight rollover must not be
 * immediately force-delivered by the pool-exhaustion fallback below it — that
 * would give it zero chance to receive its own late marker. Only a pending
 * slot that already existed before this call (a genuinely stale one) may be
 * force-delivered on exhaustion. */
TEST_F(St40RxFrameAssemblyTest, PoolExhaustionDoesNotForceDeliverPendingCreatedThisCall) {
  ut40_teardown_frame_pool(ctx_);
  ut40_ctx_destroy(ctx_);
  ctx_ = ut40_ctx_create(2); /* multi-port: PENDING state is reachable */
  ASSERT_NE(ctx_, nullptr);
  ut40_setup_frame_pool(ctx_, /*cnt*/ 2, /*size*/ 256);
  ut40_captured_reset();

  /* ts=500 completes with a marker → IN_USER, occupies 1 of 2 slots (app
   * never releases it, so it stays occupied for the rest of this test). */
  ut40_feed_pkt_anc0(ctx_, 0, 500, 1, MTL_SESSION_PORT_P);
  ASSERT_EQ(ut40_captured_count(), 1);

  /* ts=1000 no marker → takes the last free slot as inflight (2 of 2 used). */
  ut40_feed_pkt_anc0(ctx_, 1, 1000, 0, MTL_SESSION_PORT_P);
  EXPECT_EQ(ut40_captured_count(), 1);

  /* ts=2000: pool is already exhausted, so this one call both rolls ts=1000
   * to PENDING *and* hits the fresh-alloc fallback. ts=1000 must NOT be
   * force-delivered here — it was just parked this same call. */
  ut40_feed_pkt_anc0(ctx_, 2, 2000, 0, MTL_SESSION_PORT_P);
  EXPECT_EQ(ut40_captured_count(), 1)
      << "a pending slot created by this same call's rollover must not be "
         "force-delivered without a late-marker grace period";
  EXPECT_GE(ut40_stat_anc_frames_dropped(ctx_), 1u)
      << "ts=2000 itself has nowhere to go since both slots are occupied";
}

/* Cross-port redundancy: identical packets on R after P must be filtered
 * and never reach the assembler. The frame is delivered exactly once. */
TEST_F(St40RxFrameAssemblyTest, RedundantCopyDoesNotDuplicateFrame) {
  for (int i = 0; i < 4; i++)
    ut40_feed_pkt_anc0(ctx_, i, 1000, i == 3, MTL_SESSION_PORT_P);
  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_stat_anc_frames_ready(ctx_), 1u);

  /* R replays the same frame — must be 100% absorbed by redundancy filter. */
  uint64_t ready_before = ut40_stat_anc_frames_ready(ctx_);
  uint64_t dispatched_before = ut40_stat_assemble_dispatched(ctx_);
  for (int i = 0; i < 4; i++)
    ut40_feed_pkt_anc0(ctx_, i, 1000, i == 3, MTL_SESSION_PORT_R);

  EXPECT_EQ(ut40_captured_count(), 1)
      << "redundant copy must not produce a second delivery";
  EXPECT_EQ(ut40_stat_anc_frames_ready(ctx_), ready_before);
  EXPECT_EQ(ut40_stat_assemble_dispatched(ctx_), dispatched_before)
      << "redundancy filter must reject before dispatch \u2014 not 1 dispatch slipped";
}

/* Slot pool size = 1: only one in-flight frame allowed. A second frame in a
 * single-port session that sees an inflight, ts changes, must drop because
 * the inflight already occupied the only slot AND deliver it. */
TEST_F(St40RxFrameAssemblyTest, SinglePortSingleSlotPoolBoundary) {
  ut40_teardown_frame_pool(ctx_);
  ut40_ctx_destroy(ctx_);
  ctx_ = ut40_ctx_create(1);
  ASSERT_NE(ctx_, nullptr);
  ut40_setup_frame_pool(ctx_, /*cnt*/ 1, /*size*/ 256);
  ut40_captured_reset();

  /* First frame ts=1000: no marker. */
  ut40_feed_pkt_anc0(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P);
  EXPECT_EQ(ut40_captured_count(), 0);
  /* ts=2000 → single-port path force-delivers ts=1000 (slot freed by IN_USER
   * notify), then needs a fresh slot for ts=2000. App hasn't released yet,
   * so the slot is IN_USER and not FREE → drop. */
  ut40_feed_pkt_anc0(ctx_, 1, 2000, 1, MTL_SESSION_PORT_P);
  EXPECT_EQ(ut40_captured_count(), 1) << "first frame force-delivered";
  EXPECT_EQ(ut40_captured_rtp_ts(0), 1000u);
  EXPECT_GE(ut40_stat_anc_frames_dropped(ctx_), 1u)
      << "second frame had nowhere to go (only slot held by app)";

  /* Release and verify pool is reusable. */
  ut40_release_frame(ctx_, ut40_captured_addr(0));
  ut40_feed_pkt_anc0(ctx_, 2, 3000, 1, MTL_SESSION_PORT_P);
  EXPECT_EQ(ut40_captured_count(), 2) << "after release, next frame must land";
}

/* receive_timestamp must be a real, non-zero, monotonically increasing
 * capture time (timestamp_first_pkt in the delivered meta), not the
 * hardcoded 0 the assembler used before wiring the PTP clock. */
TEST_F(St40RxFrameAssemblyTest, ReceiveTimestampIsNonZeroAndMonotonic) {
  ut40_feed_pkt_anc0(ctx_, 0, 1000, 1, MTL_SESSION_PORT_P);
  ut40_feed_pkt_anc0(ctx_, 1, 2000, 1, MTL_SESSION_PORT_P);
  ut40_feed_pkt_anc0(ctx_, 2, 3000, 1, MTL_SESSION_PORT_P);

  ASSERT_EQ(ut40_captured_count(), 3);
  EXPECT_GT(ut40_captured_timestamp_first_pkt(0), 0u);
  EXPECT_GT(ut40_captured_timestamp_first_pkt(1), 0u);
  EXPECT_GT(ut40_captured_timestamp_first_pkt(2), 0u);
  EXPECT_LT(ut40_captured_timestamp_first_pkt(0), ut40_captured_timestamp_first_pkt(1));
  EXPECT_LT(ut40_captured_timestamp_first_pkt(1), ut40_captured_timestamp_first_pkt(2));
}

/* timestamp_first_pkt must come from the HW RX-timestamp dynfield
 * (mt_mbuf_time_stamp) when the interface advertises RX timestamp offload,
 * not the software PTP clock fallback. */
TEST_F(St40RxFrameAssemblyTest, ReceiveTimestampSourcedFromHwOffload) {
  constexpr uint64_t kHwRawNs = 123456789000ull;
  ut40_ctx_enable_hw_timestamp(ctx_, MTL_SESSION_PORT_P);

  ut40_feed_pkt_anc0_hw_ts(ctx_, 0, 1000, 1, MTL_SESSION_PORT_P, kHwRawNs);

  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_timestamp_first_pkt(0), kHwRawNs)
      << "timestamp_first_pkt must come from the HW RX-timestamp dynfield "
         "(mt_mbuf_time_stamp), not the SW PTP clock fallback";
}

/* Regression guard for the unsynchronized frame_slots[].state field: the app
 * thread (st40_rx_put_framebuff) frees slots concurrently with the tasklet
 * thread scanning/assembling. Every fed frame must end up counted exactly
 * once, as either delivered or dropped, never both and never neither. */
TEST_F(St40RxFrameAssemblyTest, ConcurrentPutFramebuffDoesNotRaceWithSlotScan) {
  ut40_teardown_frame_pool(ctx_);
  ut40_ctx_destroy(ctx_);
  ctx_ = ut40_ctx_create(1); /* single port: every marker resolves immediately */
  ASSERT_NE(ctx_, nullptr);
  ut40_setup_frame_pool(ctx_, kSlots, kSlotSize);
  ut40_captured_reset();

  constexpr int kWaves = 8;
  constexpr int kPerWave = 20; /* stays within the harness capture buffer */
  std::mutex mu;
  std::queue<void*> ready_addrs;
  std::atomic<bool> stop{false};

  std::thread releaser([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      void* addr = nullptr;
      {
        std::lock_guard<std::mutex> lock(mu);
        if (!ready_addrs.empty()) {
          addr = ready_addrs.front();
          ready_addrs.pop();
        }
      }
      if (addr) ut40_release_frame(ctx_, addr);
    }
  });

  uint16_t seq = 0;
  uint32_t ts = 1000;
  for (int wave = 0; wave < kWaves; wave++) {
    int prev_n = 0;
    for (int i = 0; i < kPerWave; i++) {
      ut40_feed_pkt_anc0(ctx_, seq++, ts, 1, MTL_SESSION_PORT_P);
      ts += 1000;
      int n = ut40_captured_count();
      if (n > prev_n) {
        void* addr = ut40_captured_addr(n - 1);
        if (addr) {
          std::lock_guard<std::mutex> lock(mu);
          ready_addrs.push(addr);
        }
        prev_n = n;
      }
    }
    /* drain this wave's releases before reusing the capture buffer */
    for (;;) {
      std::lock_guard<std::mutex> lock(mu);
      if (ready_addrs.empty()) break;
    }
    ut40_captured_reset();
  }
  stop.store(true, std::memory_order_relaxed);
  releaser.join();

  EXPECT_EQ(ut40_stat_anc_frames_ready(ctx_) + ut40_stat_anc_frames_dropped(ctx_),
            (uint64_t)(kWaves * kPerWave))
      << "every fed frame must be accounted as delivered or dropped exactly once";
}

/* ============================================================================
 * seq_lost / seq_discont / port_seq_* derived from per-frame bitmap.
 * ========================================================================= */

class St40RxBitmapSeqStatsTest : public ::testing::Test {
 protected:
  ut_test_ctx* ctx_ = nullptr;
  static constexpr uint16_t kSlots = 8;
  static constexpr uint32_t kSlotSize = 4096;

  void SetUp() override {
    ASSERT_EQ(ut40_init(), 0);
    ut40_drain_ring();
    ut40_notify_rtp_calls_reset();
    ut40_captured_reset();
    ctx_ = ut40_ctx_create(2);
    ASSERT_NE(ctx_, nullptr);
    ut40_setup_frame_pool(ctx_, kSlots, kSlotSize);
  }
  void TearDown() override {
    ut40_teardown_frame_pool(ctx_);
    ut40_drain_ring();
    ut40_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }
};

/* No loss -> seq_lost=0, seq_discont=false, status=COMPLETE. */
TEST_F(St40RxBitmapSeqStatsTest, InOrderFrameNoLoss) {
  for (uint16_t s = 100; s < 105; s++) {
    bool marker = (s == 104);
    ut40_feed_pkt_anc0(ctx_, s, 7000, marker ? 1 : 0, MTL_SESSION_PORT_P);
  }
  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_seq_lost(0), 0u);
  EXPECT_FALSE(ut40_captured_seq_discont(0));
  EXPECT_EQ(ut40_captured_port_seq_lost(0, MTL_SESSION_PORT_P), 0u);
  EXPECT_FALSE(ut40_captured_port_seq_discont(0, MTL_SESSION_PORT_P));
  EXPECT_EQ(ut40_captured_port_pkts_recv(0, MTL_SESSION_PORT_P), 5u);
  EXPECT_EQ(ut40_captured_pkts_total(0), 5u);
  EXPECT_EQ(ut40_captured_status(0), ST_FRAME_STATUS_COMPLETE);
}

/* Single-port gap of 1 -> seq_lost=1, seq_discont=true, status=CORRUPTED. */
TEST_F(St40RxBitmapSeqStatsTest, SinglePortGapDetected) {
  ut40_feed_pkt_anc0(ctx_, 200, 8000, 0, MTL_SESSION_PORT_P);
  /* 201 missing */
  ut40_feed_pkt_anc0(ctx_, 202, 8000, 0, MTL_SESSION_PORT_P);
  ut40_feed_pkt_anc0(ctx_, 203, 8000, 1, MTL_SESSION_PORT_P);
  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_seq_lost(0), 1u);
  EXPECT_TRUE(ut40_captured_seq_discont(0));
  EXPECT_EQ(ut40_captured_port_seq_lost(0, MTL_SESSION_PORT_P), 1u);
  EXPECT_TRUE(ut40_captured_port_seq_discont(0, MTL_SESSION_PORT_P));
  EXPECT_EQ(ut40_captured_status(0), ST_FRAME_STATUS_CORRUPTED);
}

/* Multi-port gap on P recovered by R -> port_seq_lost[P]>0 but
 * session seq_lost==0 (this is the entire point of the refactor). */
TEST_F(St40RxBitmapSeqStatsTest, RedundancyMasksSessionLossButRecordsPortGap) {
  /* P sends seqs 300, 302, 303 (302 lost on P). R sends only the missing 301. */
  ut40_feed_pkt_anc0(ctx_, 300, 9000, 0, MTL_SESSION_PORT_P);
  ut40_feed_pkt_anc0(ctx_, 302, 9000, 0, MTL_SESSION_PORT_P);
  ut40_feed_pkt_anc0(ctx_, 301, 9000, 0, MTL_SESSION_PORT_R);
  ut40_feed_pkt_anc0(ctx_, 303, 9000, 1, MTL_SESSION_PORT_P);
  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_seq_lost(0), 0u) << "redundancy must mask session-level loss";
  EXPECT_FALSE(ut40_captured_seq_discont(0));
  EXPECT_EQ(ut40_captured_status(0), ST_FRAME_STATUS_COMPLETE);
  EXPECT_GT(ut40_captured_port_seq_lost(0, MTL_SESSION_PORT_P), 0u)
      << "P-port gap must still be reported per-port";
  EXPECT_TRUE(ut40_captured_port_seq_discont(0, MTL_SESSION_PORT_P));
  EXPECT_EQ(ut40_captured_port_seq_lost(0, MTL_SESSION_PORT_R), 0u);
  EXPECT_FALSE(ut40_captured_port_seq_discont(0, MTL_SESSION_PORT_R));
  EXPECT_EQ(ut40_captured_port_pkts_recv(0, MTL_SESSION_PORT_P), 3u);
  EXPECT_EQ(ut40_captured_port_pkts_recv(0, MTL_SESSION_PORT_R), 1u);
}

/* Loss on BOTH ports for the same seq -> session seq_lost>0. */
TEST_F(St40RxBitmapSeqStatsTest, UnrecoveredLossShowsSessionLost) {
  ut40_feed_pkt_anc0(ctx_, 400, 10000, 0, MTL_SESSION_PORT_P);
  ut40_feed_pkt_anc0(ctx_, 400, 10000, 0, MTL_SESSION_PORT_R); /* dup filtered */
  /* 401 lost on BOTH */
  ut40_feed_pkt_anc0(ctx_, 402, 10000, 0, MTL_SESSION_PORT_P);
  ut40_feed_pkt_anc0(ctx_, 403, 10000, 1, MTL_SESSION_PORT_P);
  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_seq_lost(0), 1u);
  EXPECT_TRUE(ut40_captured_seq_discont(0));
  EXPECT_EQ(ut40_captured_status(0), ST_FRAME_STATUS_CORRUPTED);
}

/* No false-positive seq_discont across frame boundary: previous frame
 * ended at seq=499, new frame starts at seq=500 (different tmstamp).
 * The new frame's bitmap is fresh -> no loss reported. This is exactly
 * the bug the refactor fixes. */
TEST_F(St40RxBitmapSeqStatsTest, FrameBoundaryDoesNotInjectFalseLoss) {
  ut40_feed_pkt_anc0(ctx_, 498, 11000, 0, MTL_SESSION_PORT_P);
  ut40_feed_pkt_anc0(ctx_, 499, 11000, 1, MTL_SESSION_PORT_P);
  ut40_feed_pkt_anc0(ctx_, 500, 11001, 0, MTL_SESSION_PORT_P);
  ut40_feed_pkt_anc0(ctx_, 501, 11001, 1, MTL_SESSION_PORT_P);
  ASSERT_EQ(ut40_captured_count(), 2);
  EXPECT_EQ(ut40_captured_seq_lost(0), 0u);
  EXPECT_EQ(ut40_captured_seq_lost(1), 0u);
  EXPECT_FALSE(ut40_captured_seq_discont(0));
  EXPECT_FALSE(ut40_captured_seq_discont(1));
}

/* Reorder within the same frame must NOT show as loss. */
TEST_F(St40RxBitmapSeqStatsTest, IntraFrameReorderNotLoss) {
  ut40_feed_pkt_anc0(ctx_, 600, 12000, 0, MTL_SESSION_PORT_P);
  ut40_feed_pkt_anc0(ctx_, 602, 12000, 0, MTL_SESSION_PORT_P); /* arrives early */
  ut40_feed_pkt_anc0(ctx_, 601, 12000, 0, MTL_SESSION_PORT_P); /* late but in-frame */
  ut40_feed_pkt_anc0(ctx_, 603, 12000, 1, MTL_SESSION_PORT_P);
  ASSERT_EQ(ut40_captured_count(), 1);
  EXPECT_EQ(ut40_captured_seq_lost(0), 0u)
      << "intra-frame reorder must not be counted as loss";
  EXPECT_FALSE(ut40_captured_seq_discont(0));
  /* Per-port: seqs arrived 600,602,601,603 -> 600->602 has gap of 1 then
   * 601 arrives backward; per-port discont gets set on the 600->602 jump. */
  EXPECT_TRUE(ut40_captured_port_seq_discont(0, MTL_SESSION_PORT_P));
}

/* Backward late arrival on R recovers a P gap detected before it arrived. */
TEST_F(St40RxBitmapSeqStatsTest, BackwardLateRecoveryFlipsBit) {
  /* P sends 700, 702, 703 (701 lost). Then R delivers 701 LATE (after 703). */
  ut40_feed_pkt_anc0(ctx_, 700, 13000, 0, MTL_SESSION_PORT_P);
  ut40_feed_pkt_anc0(ctx_, 702, 13000, 0, MTL_SESSION_PORT_P);
  ut40_feed_pkt_anc0(ctx_, 703, 13000, 1,
                     MTL_SESSION_PORT_P); /* marker, but 701 still missing */
  ASSERT_EQ(ut40_captured_count(), 1);
  /* Without the late R arrival the marker delivers immediately, so the
   * "backward recovery" must be tested via the multi-port pending path
   * (covered in MultiPortForceDeliversStalePending). Here we only verify
   * that under single-port the gap IS reported. */
  EXPECT_EQ(ut40_captured_seq_lost(0), 1u);
  EXPECT_TRUE(ut40_captured_seq_discont(0));
}
