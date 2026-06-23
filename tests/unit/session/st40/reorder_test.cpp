/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Per-port reorder and same-port duplicate accounting:
 * the per-port reordered_packets and duplicates_same_port counters,
 * their interaction with cross-port redundancy, and behaviour at the
 * 16-bit seq wrap boundary.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St40RxReorderTest.*'
 */

#include <gtest/gtest.h>

#include "session/st40/st40_rx_test_base.h"

class St40RxReorderTest : public St40RxBaseTest {};

/* Backward seq on the same port (never seen on the other port) is a real
 * intra-port reorder and must bump reordered_packets, NOT lost_packets.
 * The bitmap path must accept the late arrival, cancelling any pending
 * unrecovered count from the forward gap. */
TEST_F(St40RxReorderTest, ReorderedPacketsCounted) {
  constexpr uint32_t ts = 1000;
  /* feed 0, 2, 1, 3 on port P only. Order 0-2 creates a forward gap (lost=1).
   * When seq 1 then arrives, the old "late" packet is a same-port reorder. */
  feed(0, ts, false, MTL_SESSION_PORT_P);
  feed(2, ts, false, MTL_SESSION_PORT_P);
  feed(1, ts, false, MTL_SESSION_PORT_P); /* backward — reorder */
  feed(3, ts, true, MTL_SESSION_PORT_P);

  EXPECT_GE(port_reordered(MTL_SESSION_PORT_P), 1u)
      << "Backward seq on same port must be counted as reordered";
  EXPECT_EQ(port_duplicates(MTL_SESSION_PORT_P), 0u) << "Reorder is not a duplicate";
  EXPECT_GE(port_ooo(MTL_SESSION_PORT_P), 1u);
  EXPECT_EQ(unrecovered(), 0u)
      << "intra-frame backward arrival should cancel the pending unrecovered count";
}

/* Exact same seq seen twice on the *same* port is a same-port duplicate
 * (e.g. switch/cable loop). It must NOT be confused with the cross-port
 * redundant copy tracked by stat_pkts_redundant. */
TEST_F(St40RxReorderTest, DuplicateSamePortCounted) {
  constexpr uint32_t ts = 1000;
  feed(0, ts, false, MTL_SESSION_PORT_P);
  feed(1, ts, false, MTL_SESSION_PORT_P);
  feed(1, ts, false, MTL_SESSION_PORT_P); /* true same-port dup */
  feed(2, ts, true, MTL_SESSION_PORT_P);

  EXPECT_EQ(port_duplicates(MTL_SESSION_PORT_P), 1u);
  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_P), 0u);
}

/* Backward arrival must NOT inflate lost_packets (lost is forward-gap only). */
TEST_F(St40RxReorderTest, ReorderDoesNotInflateLostPackets) {
  constexpr uint32_t ts = 1000;
  feed(0, ts, false, MTL_SESSION_PORT_P);
  feed(1, ts, false, MTL_SESSION_PORT_P);
  feed(2, ts, false, MTL_SESSION_PORT_P);
  uint64_t lost_before = port_ooo(MTL_SESSION_PORT_P);

  /* Pure reorder: 1 arrives again-as-backward (after 2). No forward gap. */
  feed(1, ts, false, MTL_SESSION_PORT_P);

  EXPECT_EQ(port_ooo(MTL_SESSION_PORT_P), lost_before)
      << "A backward arrival must not bump lost_packets";
  EXPECT_GE(port_reordered(MTL_SESSION_PORT_P), 1u);
}

/* Multiple same-port duplicates must each be counted. */
TEST_F(St40RxReorderTest, DuplicateSamePortMultiple) {
  constexpr uint32_t ts = 1000;
  feed(0, ts, false, MTL_SESSION_PORT_P);
  feed(1, ts, false, MTL_SESSION_PORT_P);
  feed(1, ts, false, MTL_SESSION_PORT_P); /* dup #1 */
  feed(1, ts, false, MTL_SESSION_PORT_P); /* dup #2 */
  feed(1, ts, true, MTL_SESSION_PORT_P);  /* dup #3 */

  EXPECT_EQ(port_duplicates(MTL_SESSION_PORT_P), 3u)
      << "Every same-port re-arrival of the same seq must be counted";
}

/* Cross-port "normal" redundancy must NOT bump reordered_packets / duplicates_same_port
 * on either port. The counter is strictly for same-port repetitions. */
TEST_F(St40RxReorderTest, CrossPortRedundantIsNotSamePortDuplicate) {
  constexpr uint32_t ts = 1000;
  feed_burst(0, 4, ts, true, MTL_SESSION_PORT_P);
  feed_burst(0, 4, ts, true, MTL_SESSION_PORT_R);

  EXPECT_EQ(port_duplicates(MTL_SESSION_PORT_P), 0u);
  EXPECT_EQ(port_duplicates(MTL_SESSION_PORT_R), 0u);
  EXPECT_GE(redundant(), 1u) << "Cross-port redundancy must still count as redundant";
}

/* Reorder at the seq-wrap boundary: a small-value seq arriving after a
 * near-max seq must NOT be counted as reorder — mt_seq16_greater treats
 * the small value as the *forward* next wrap. */
TEST_F(St40RxReorderTest, SeqWrapNotCountedAsReorder) {
  constexpr uint32_t ts = 1000;
  feed(65534, ts, false, MTL_SESSION_PORT_P);
  feed(65535, ts, false, MTL_SESSION_PORT_P);
  uint64_t reord_before = port_reordered(MTL_SESSION_PORT_P);

  feed(0, ts, false, MTL_SESSION_PORT_P); /* wraps forward, not backward */
  feed(1, ts, true, MTL_SESSION_PORT_P);

  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_P), reord_before)
      << "Wrap-around must be seen as forward, not reorder";
}

/* Same-port duplicate at seq-wrap boundary must still be counted. */
TEST_F(St40RxReorderTest, DuplicateSamePortAtSeqWrap) {
  constexpr uint32_t ts = 1000;
  feed(65534, ts, false, MTL_SESSION_PORT_P);
  feed(65535, ts, false, MTL_SESSION_PORT_P);
  feed(65535, ts, false, MTL_SESSION_PORT_P); /* dup right at boundary */

  EXPECT_EQ(port_duplicates(MTL_SESSION_PORT_P), 1u);
}
