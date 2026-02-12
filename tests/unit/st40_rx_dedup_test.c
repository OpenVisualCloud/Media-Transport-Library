/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024-2025 Intel Corporation
 *
 * Unit test for st_rx_dedup_check — the ST 2022-7 dedup logic used by
 * ST30 / ST40 / ST41 RX sessions.
 *
 * Tests focus on the Class A redundancy scenario: primary port (P) advances
 * ahead of redundant port (R) because R's packets are delayed by up to 10 ms.
 * The merge-sort tasklet handles within-burst reordering, but here we isolate
 * the per-packet dedup decision — exactly what happens when the burst boundary
 * falls between P's advance and R's gap-fill.
 *
 * Build — via meson (from the project root):
 *   meson setup build && ninja -C build
 *
 * Run:
 *   ./build/tests/unit/st40_rx_dedup_test    (exit 0 = pass, non-zero = fail)
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

/*
 * Block the real mt_log.h so we don't pull in DPDK logger / USDT runtime.
 * Provide a trivial warn() that just prints to stderr.
 */
#define _MT_LIB_LOG_HEAD_H_
#define warn(fmt, ...) fprintf(stderr, "WARN: " fmt, ##__VA_ARGS__)

/* Now include the real dedup header.
 * With DPDK + MTL include paths provided by the build system, every other
 * include in st_rx_dedup.h resolves normally:
 *   <rte_mbuf.h>  → DPDK
 *   "mtl_api.h"   → include/
 *   "st_api.h"    → include/
 *   "st_pkt.h"    → lib/src/st2110/
 *   "mtl_sch_api.h" → include/
 *
 * st_pkt.h uses DPDK network struct types (rte_ether_hdr, rte_ipv4_hdr,
 * rte_udp_hdr) which are NOT transitively included by rte_mbuf.h,
 * and ST20/ST22 RTP header types from st20_api.h.
 * Pull them in explicitly.
 */
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>

#include "st20_api.h"
#include "st2110/st_rx_dedup.h"

/* ── Test helpers ────────────────────────────────────────────────────── */

static int tests_run = 0;
static int tests_failed = 0;

#define ASSERT_MSG(cond, fmt, ...)                                                     \
  do {                                                                                 \
    if (!(cond)) {                                                                     \
      fprintf(stderr, "  FAIL [%s:%d]: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__); \
      tests_failed++;                                                                  \
      return;                                                                          \
    }                                                                                  \
  } while (0)

#define RUN_TEST(fn)                 \
  do {                               \
    tests_run++;                     \
    printf("  %-60s ", #fn);         \
    fn();                            \
    if (tests_failed == prev_failed) \
      printf("PASS\n");              \
    else                             \
      printf("FAIL\n");              \
    prev_failed = tests_failed;      \
  } while (0)

/* ────────────────────────────────────────────────────────────────────── *
 * Test 1: Baseline — single-port progressive delivery, no drops.
 * ────────────────────────────────────────────────────────────────────── */
static void test_single_port_progressive(void) {
  struct st_rx_dedup d;
  st_rx_dedup_init(&d, ST_RX_DEDUP_MODE_TIMESTAMP_AND_SEQ, 1, 0);

  /* Deliver 5 packets, seq 100–104, same timestamp 1000 */
  for (uint16_t seq = 100; seq < 105; seq++) {
    struct st_rx_dedup_result r = st_rx_dedup_check(&d, seq, 1000, MTL_SESSION_PORT_P);
    ASSERT_MSG(!r.drop, "seq %u should not be dropped", seq);
    ASSERT_MSG(!r.threshold_override, "should not threshold at seq %u", seq);
  }

  ASSERT_MSG(d.session_seq_id == 104, "session_seq_id=%d want 104", d.session_seq_id);
  ASSERT_MSG(d.tmstamp == 1000, "tmstamp=%ld want 1000", (long)d.tmstamp);
}

/* ────────────────────────────────────────────────────────────────────── *
 * Test 2: Same-burst merge — P and R interleaved in correct seq order.
 *         This is the happy path that merge-sort tasklet provides.
 * ────────────────────────────────────────────────────────────────────── */
static void test_same_burst_merge_happy_path(void) {
  struct st_rx_dedup d;
  st_rx_dedup_init(&d, ST_RX_DEDUP_MODE_TIMESTAMP_AND_SEQ, 2, 0);

  /* Frame 1: 6 packets, seq 10–15, ts=1000.
   * Merge-sort delivers them interleaved: P=10, R=11, P=12, R=13, P=14, R=15.
   * All should be accepted. */
  struct {
    uint16_t seq;
    enum mtl_session_port port;
  } burst[] = {
      {10, MTL_SESSION_PORT_P}, {11, MTL_SESSION_PORT_R}, {12, MTL_SESSION_PORT_P},
      {13, MTL_SESSION_PORT_R}, {14, MTL_SESSION_PORT_P}, {15, MTL_SESSION_PORT_R},
  };

  for (int i = 0; i < 6; i++) {
    struct st_rx_dedup_result r =
        st_rx_dedup_check(&d, burst[i].seq, 1000, burst[i].port);
    ASSERT_MSG(!r.drop, "pkt %d (seq %u port %d) dropped", i, burst[i].seq,
               burst[i].port);
  }
  ASSERT_MSG(d.session_seq_id == 15, "session_seq_id=%d want 15", d.session_seq_id);
}

/* ────────────────────────────────────────────────────────────────────── *
 * Test 3: Same-burst merge with duplicates — both ports deliver the same
 *         seq. Merge-sort feeds P first, then R. R's copy is dropped.
 * ────────────────────────────────────────────────────────────────────── */
static void test_same_burst_dedup_drops_duplicate(void) {
  struct st_rx_dedup d;
  st_rx_dedup_init(&d, ST_RX_DEDUP_MODE_TIMESTAMP_AND_SEQ, 2, 0);

  /* seq 10 from P — accepted */
  struct st_rx_dedup_result r1 = st_rx_dedup_check(&d, 10, 1000, MTL_SESSION_PORT_P);
  ASSERT_MSG(!r1.drop, "P seq 10 should be accepted");

  /* seq 10 from R — duplicate, dropped */
  struct st_rx_dedup_result r2 = st_rx_dedup_check(&d, 10, 1000, MTL_SESSION_PORT_R);
  ASSERT_MSG(r2.drop, "R seq 10 (dup) should be dropped");

  /* seq 11 from R — new, accepted */
  struct st_rx_dedup_result r3 = st_rx_dedup_check(&d, 11, 1000, MTL_SESSION_PORT_R);
  ASSERT_MSG(!r3.drop, "R seq 11 should be accepted");
}

/* ────────────────────────────────────────────────────────────────────── *
 * Test 4: Same-burst gap-fill — P has a hole, R fills it.
 *         Merge-sort delivers: R=10, P=11(skip 10), R=11(dup), P=12.
 * ────────────────────────────────────────────────────────────────────── */
static void test_same_burst_gap_fill(void) {
  struct st_rx_dedup d;
  st_rx_dedup_init(&d, ST_RX_DEDUP_MODE_TIMESTAMP_AND_SEQ, 2, 0);

  struct st_rx_dedup_result r;

  /* R delivers seq 10 — accepted (R fills the gap P missed) */
  r = st_rx_dedup_check(&d, 10, 1000, MTL_SESSION_PORT_R);
  ASSERT_MSG(!r.drop, "R seq 10 accepted (gap-fill)");

  /* P delivers seq 11 (P missed its own seq 10) — accepted */
  r = st_rx_dedup_check(&d, 11, 1000, MTL_SESSION_PORT_P);
  ASSERT_MSG(!r.drop, "P seq 11 accepted");

  /* R delivers seq 11 (dup) — dropped */
  r = st_rx_dedup_check(&d, 11, 1000, MTL_SESSION_PORT_R);
  ASSERT_MSG(r.drop, "R seq 11 dup dropped");

  /* P delivers seq 12 — accepted */
  r = st_rx_dedup_check(&d, 12, 1000, MTL_SESSION_PORT_P);
  ASSERT_MSG(!r.drop, "P seq 12 accepted");

  ASSERT_MSG(d.session_seq_id == 12, "session_seq_id=%d want 12", d.session_seq_id);
}

/* ────────────────────────────────────────────────────────────────────── *
 * Test 5: CLASS A — within same burst, merge-sort lets R fill the gap.
 *
 * This is the scenario the current merge-sort tasklet DOES handle:
 * both ports' packets arrive in the same burst and are fed in seq order.
 *   Merged burst: R=10, P=10(dup), R=11, P=11(dup), R=12, P=13, P=14
 *   P skipped seq 12, but R delivered it in-order before P advanced past it.
 *   All unique seqs 10-14 should be accepted.
 * ────────────────────────────────────────────────────────────────────── */
static void test_class_a_within_burst_gap_fill(void) {
  struct st_rx_dedup d;
  st_rx_dedup_init(&d, ST_RX_DEDUP_MODE_TIMESTAMP_AND_SEQ, 2, 0);

  struct {
    uint16_t seq;
    enum mtl_session_port port;
    bool expect_drop;
  } merged[] = {
      {10, MTL_SESSION_PORT_R, false}, /* R fills first */
      {10, MTL_SESSION_PORT_P, true},  /* P dup */
      {11, MTL_SESSION_PORT_R, false}, /* R fills */
      {11, MTL_SESSION_PORT_P, true},  /* P dup */
      {12, MTL_SESSION_PORT_R, false}, /* R fills — P never had this! */
      {13, MTL_SESSION_PORT_P, false}, /* P continues */
      {14, MTL_SESSION_PORT_P, false}, /* P continues */
  };

  for (int i = 0; i < 7; i++) {
    struct st_rx_dedup_result r =
        st_rx_dedup_check(&d, merged[i].seq, 1000, merged[i].port);
    ASSERT_MSG(r.drop == merged[i].expect_drop,
               "pkt %d (seq %u port %d): drop=%d want %d", i, merged[i].seq,
               merged[i].port, r.drop, merged[i].expect_drop);
  }

  ASSERT_MSG(d.session_seq_id == 14, "session_seq_id=%d want 14", d.session_seq_id);
}

/* ────────────────────────────────────────────────────────────────────── *
 * Test 6: CLASS A — cross-burst, R late but within same timestamp.
 *
 * With the received-sequence bitmap, R's gap-fill seq 12 is recognised
 * as never-received (bit not set) and accepted even though session_seq_id
 * already advanced past it.
 *
 *   Tasklet call 1: P delivers seq 10, 11, 13, 14 (ts=1000)
 *     → session_seq_id = 14, bitmap has bits 10,11,13,14 set, 12 clear
 *   Tasklet call 2: R delivers seq 10, 11, 12, 13, 14 (ts=1000)
 *     → seq 10,11,13,14 have bitmap bits set → dropped (true dups)
 *     → seq 12 has bitmap bit CLEAR → accepted (gap-fill!)
 * ────────────────────────────────────────────────────────────────────── */
static void test_class_a_cross_burst_same_ts_r_late(void) {
  struct st_rx_dedup d;
  st_rx_dedup_init(&d, ST_RX_DEDUP_MODE_TIMESTAMP_AND_SEQ, 2, 0);

  struct st_rx_dedup_result r;

  /* Tasklet call 1: P delivers seq 10,11,13,14 (gap at 12), ts=1000 */
  uint16_t p_seqs[] = {10, 11, 13, 14};
  for (int i = 0; i < 4; i++) {
    r = st_rx_dedup_check(&d, p_seqs[i], 1000, MTL_SESSION_PORT_P);
    ASSERT_MSG(!r.drop, "P seq %u accepted", p_seqs[i]);
  }

  /* Tasklet call 2: R delivers seq 10-14 (all of them), ts=1000 (same ts) */
  int gap_fill_accepted = 0;
  int dups_dropped = 0;
  for (uint16_t seq = 10; seq <= 14; seq++) {
    r = st_rx_dedup_check(&d, seq, 1000, MTL_SESSION_PORT_R);
    if (!r.drop && seq == 12) gap_fill_accepted = 1;
    if (r.drop) dups_dropped++;
  }

  printf("\n    [Class A same-ts cross-burst] gap-fill seq 12 accepted: %s\n",
         gap_fill_accepted ? "YES" : "NO");
  ASSERT_MSG(gap_fill_accepted, "bitmap should let gap-fill seq 12 through");
  ASSERT_MSG(dups_dropped == 4,
             "4 true duplicates (10,11,13,14) should be dropped, got %d", dups_dropped);
}

/* ────────────────────────────────────────────────────────────────────── *
 * Test 7: CLASS A — cross-burst delayed R with new-timestamp advance.
 *
 * With the received-sequence bitmap, gap-fill works even across
 * timestamp boundaries and 5-10 ms path differential delay.
 *
 *   Tasklet call 1 (P burst): seq 10, 11, 13, 14 (ts=1000)
 *     → session_seq_id = 14, bitmap: bits 10,11,13,14 set, 12 clear
 *
 *   Tasklet call 2 (P new frame): seq 15 (ts=2000)
 *     → session_seq_id = 15, bitmap: bits 10-15 except 12
 *
 *   Tasklet call 3 (R finally arrives — 5-10 ms late):
 *     → R delivers seq 10, 11, 12, 13, 14 (ts=1000)
 *     → seq 10,11,13,14: bitmap bits set → dropped (true dups)
 *     → seq 12: bitmap bit CLEAR → accepted (gap-fill!)
 * ────────────────────────────────────────────────────────────────────── */
static void test_class_a_cross_burst_r_late(void) {
  struct st_rx_dedup d;
  st_rx_dedup_init(&d, ST_RX_DEDUP_MODE_TIMESTAMP_AND_SEQ, 2, 0);

  struct st_rx_dedup_result r;

  /* ── Tasklet call 1: P delivers frame 1, but with seq 12 missing ── */
  uint16_t p_frame1_seqs[] = {10, 11, 13, 14};
  for (int i = 0; i < 4; i++) {
    r = st_rx_dedup_check(&d, p_frame1_seqs[i], 1000, MTL_SESSION_PORT_P);
    ASSERT_MSG(!r.drop, "P frame1 seq %u should be accepted", p_frame1_seqs[i]);
  }
  ASSERT_MSG(d.session_seq_id == 14, "after P frame1: session_seq_id=%d want 14",
             d.session_seq_id);

  /* ── Tasklet call 2: P delivers start of frame 2 (new timestamp) ── */
  r = st_rx_dedup_check(&d, 15, 2000, MTL_SESSION_PORT_P);
  ASSERT_MSG(!r.drop, "P frame2 seq 15 accepted");
  ASSERT_MSG(d.session_seq_id == 15, "after P frame2 start: session_seq_id=%d want 15",
             d.session_seq_id);
  ASSERT_MSG(d.tmstamp == 2000, "tmstamp=%ld want 2000", (long)d.tmstamp);

  /* ── Tasklet call 3: R finally delivers frame 1 (5-10 ms late) ── */
  int r_accepted = 0;
  int r_dropped = 0;
  int seq12_accepted = 0;
  for (uint16_t seq = 10; seq <= 14; seq++) {
    r = st_rx_dedup_check(&d, seq, 1000, MTL_SESSION_PORT_R);
    if (r.drop)
      r_dropped++;
    else {
      r_accepted++;
      if (seq == 12) seq12_accepted = 1;
    }
  }

  printf(
      "\n    [Class A cross-burst] R delivered 5 pkts: %d accepted, %d "
      "dropped\n",
      r_accepted, r_dropped);
  ASSERT_MSG(seq12_accepted,
             "bitmap should let gap-fill seq 12 through even across ts boundary");
  ASSERT_MSG(r_dropped == 4, "4 true duplicates (10,11,13,14) should be dropped, got %d",
             r_dropped);
  ASSERT_MSG(r_accepted == 1, "only seq 12 (gap-fill) should be accepted, got %d",
             r_accepted);
}

/* ────────────────────────────────────────────────────────────────────── *
 * Test 8: Threshold override — after enough consecutive drops from ALL
 *         ports the dedup force-accepts to avoid deadlock on stream reset.
 * ────────────────────────────────────────────────────────────────────── */
static void test_threshold_override_fires(void) {
  struct st_rx_dedup d;
  st_rx_dedup_init(&d, ST_RX_DEDUP_MODE_TIMESTAMP_AND_SEQ, 2, 0);

  struct st_rx_dedup_result r;

  /* Establish session state: P delivers seq 60..100, ts=5000.
   * This fills the bitmap with bits 60..100 so re-delivering them
   * later produces true duplicates (bitmap bit set → redundant). */
  for (uint16_t seq = 60; seq <= 100; seq++) {
    r = st_rx_dedup_check(&d, seq, 5000, MTL_SESSION_PORT_P);
    ASSERT_MSG(!r.drop, "initial P seq %u accepted", seq);
  }

  /*
   * Now re-deliver the SAME seqs from alternating ports with old ts.
   * Bitmap has all of 60..100 marked → all are true duplicates →
   * redundant_error_cnt climbs for both ports until threshold fires.
   */
  int override_fired_at = -1;
  for (int i = 1; i <= 50; i++) {
    enum mtl_session_port port = (i % 2 == 0) ? MTL_SESSION_PORT_P : MTL_SESSION_PORT_R;
    uint16_t seq = (uint16_t)(60 + (i % 41)); /* cycle through 60..100 */
    r = st_rx_dedup_check(&d, seq, 1000, port);
    if (r.threshold_override) {
      override_fired_at = i;
      break;
    }
  }

  ASSERT_MSG(override_fired_at > 0, "threshold override should eventually fire");
  printf("\n    [Threshold] override fired after %d redundant packets\n",
         override_fired_at);
}

/* ────────────────────────────────────────────────────────────────────── *
 * Test 9: ST30 timestamp-only mode — packets with same ts are dropped.
 *
 * ST30 audio uses ST_RX_DEDUP_MODE_TIMESTAMP: only the RTP timestamp
 * is checked.  Multiple packets with the same timestamp (but different
 * seq) are dropped because the timestamp isn't "strictly advancing".
 * ────────────────────────────────────────────────────────────────────── */
static void test_st30_timestamp_mode(void) {
  struct st_rx_dedup d;
  st_rx_dedup_init(&d, ST_RX_DEDUP_MODE_TIMESTAMP, 2, 0);

  struct st_rx_dedup_result r;

  /* First packet: ts=1000, accepted (initialises tmstamp) */
  r = st_rx_dedup_check(&d, 0, 1000, MTL_SESSION_PORT_P);
  ASSERT_MSG(!r.drop, "ST30 first pkt accepted");

  /* Second packet: same ts=1000, dropped (ts not advancing) */
  r = st_rx_dedup_check(&d, 1, 1000, MTL_SESSION_PORT_R);
  ASSERT_MSG(r.drop, "ST30 same ts from R should be dropped");

  /* Third packet: new ts=2000, accepted */
  r = st_rx_dedup_check(&d, 2, 2000, MTL_SESSION_PORT_P);
  ASSERT_MSG(!r.drop, "ST30 new ts=2000 accepted");

  /* R delivers same ts=2000, dropped */
  r = st_rx_dedup_check(&d, 3, 2000, MTL_SESSION_PORT_R);
  ASSERT_MSG(r.drop, "ST30 dup ts=2000 from R dropped");
}

/* ────────────────────────────────────────────────────────────────────── *
 * Test 10: Seq16 wrap-around — verify dedup handles 0xFFFF → 0x0000.
 * ────────────────────────────────────────────────────────────────────── */
static void test_seq16_wraparound(void) {
  struct st_rx_dedup d;
  st_rx_dedup_init(&d, ST_RX_DEDUP_MODE_TIMESTAMP_AND_SEQ, 1, 0);

  struct st_rx_dedup_result r;

  /* Deliver seq 0xFFFE, 0xFFFF, 0x0000, 0x0001 — all accepted */
  uint16_t seqs[] = {0xFFFE, 0xFFFF, 0x0000, 0x0001};
  for (int i = 0; i < 4; i++) {
    r = st_rx_dedup_check(&d, seqs[i], 1000, MTL_SESSION_PORT_P);
    ASSERT_MSG(!r.drop, "seq 0x%04x should be accepted", seqs[i]);
  }
  ASSERT_MSG(d.session_seq_id == 1, "session_seq_id=%d want 1", d.session_seq_id);
}

/* ────────────────────────────────────────────────────────────────────── *
 * Test 11: Bitmap window overflow — seq more than 64 behind is too old.
 *
 * If R's packets are SO late that they've fallen off the 64-bit bitmap
 * window entirely, they are treated as stale and dropped.
 * This prevents accepting ancient packets that could corrupt the stream.
 * ────────────────────────────────────────────────────────────────────── */
static void test_bitmap_window_overflow(void) {
  struct st_rx_dedup d;
  st_rx_dedup_init(&d, ST_RX_DEDUP_MODE_TIMESTAMP_AND_SEQ, 2, 0);

  struct st_rx_dedup_result r;

  /* P delivers seq 10 (ts=1000) — establishes bitmap_base near 10 */
  r = st_rx_dedup_check(&d, 10, 1000, MTL_SESSION_PORT_P);
  ASSERT_MSG(!r.drop, "P seq 10 accepted");

  /* P jumps ahead by 80 — delivers seq 90 (ts=2000).
   * Bitmap slides: base moves to ~27, so seq 10 is off the window. */
  r = st_rx_dedup_check(&d, 90, 2000, MTL_SESSION_PORT_P);
  ASSERT_MSG(!r.drop, "P seq 90 accepted");

  /* R delivers seq 10 (ts=1000) — 80 behind, off bitmap → dropped as stale */
  r = st_rx_dedup_check(&d, 10, 1000, MTL_SESSION_PORT_R);
  ASSERT_MSG(r.drop, "seq 10 fell off bitmap window, should be dropped");

  /* R delivers seq 50 (ts=1000) — within bitmap window, never received → gap-fill */
  r = st_rx_dedup_check(&d, 50, 1000, MTL_SESSION_PORT_R);
  ASSERT_MSG(!r.drop, "seq 50 within bitmap window, never received, gap-fill accepted");
}

/* ────────────────────────────────────────────────────────────────────── */
int main(void) {
  int prev_failed = 0;

  printf("st_rx_dedup unit tests\n");
  printf("======================\n");

  RUN_TEST(test_single_port_progressive);
  RUN_TEST(test_same_burst_merge_happy_path);
  RUN_TEST(test_same_burst_dedup_drops_duplicate);
  RUN_TEST(test_same_burst_gap_fill);
  RUN_TEST(test_class_a_within_burst_gap_fill);
  RUN_TEST(test_class_a_cross_burst_same_ts_r_late);
  RUN_TEST(test_class_a_cross_burst_r_late);
  RUN_TEST(test_threshold_override_fires);
  RUN_TEST(test_st30_timestamp_mode);
  RUN_TEST(test_seq16_wraparound);
  RUN_TEST(test_bitmap_window_overflow);

  printf("\n%d tests run, %d passed, %d failed\n", tests_run, tests_run - tests_failed,
         tests_failed);
  return tests_failed ? 1 : 0;
}
