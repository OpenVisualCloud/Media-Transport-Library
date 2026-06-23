/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Per-port reorder and same-port duplicate accounting:
 * the per-port reordered_packets and duplicates_same_port counters,
 * and their interaction with cross-port redundancy.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St30RxReorderTest.*'
 */

#include <gtest/gtest.h>

#include "session/st30/st30_rx_test_base.h"

class St30RxReorderTest : public St30RxBaseTest {};

/* Backward arrival on same port: must bump reordered_packets, not lost. */
TEST_F(St30RxReorderTest, ReorderedPacketsCounted) {
  /* feed strictly increasing ts (required by ST30) but re-send seq 1 */
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(2, 1001, MTL_SESSION_PORT_P);
  feed(1, 1002, MTL_SESSION_PORT_P); /* backward seq, fresh ts */
  feed(3, 1003, MTL_SESSION_PORT_P);

  EXPECT_GE(port_reordered(MTL_SESSION_PORT_P), 1u);
  EXPECT_EQ(port_duplicates(MTL_SESSION_PORT_P), 0u);
}

/* Same seq re-sent on same port must bump duplicates_same_port. */
TEST_F(St30RxReorderTest, DuplicateSamePortCounted) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(1, 1001, MTL_SESSION_PORT_P);
  feed(1, 1002, MTL_SESSION_PORT_P); /* same seq, fresh ts */
  feed(2, 1003, MTL_SESSION_PORT_P);

  EXPECT_EQ(port_duplicates(MTL_SESSION_PORT_P), 1u);
  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_P), 0u);
}

/* Reorder must NOT inflate lost_packets. */
TEST_F(St30RxReorderTest, ReorderDoesNotInflateLost) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(1, 1001, MTL_SESSION_PORT_P);
  feed(2, 1002, MTL_SESSION_PORT_P);
  uint64_t lost_before = port_ooo(MTL_SESSION_PORT_P);

  feed(1, 1003, MTL_SESSION_PORT_P); /* pure reorder */

  EXPECT_EQ(port_ooo(MTL_SESSION_PORT_P), lost_before);
}

/* Multiple same-port duplicates must each be counted. */
TEST_F(St30RxReorderTest, DuplicateSamePortMultiple) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(1, 1001, MTL_SESSION_PORT_P);
  feed(1, 1002, MTL_SESSION_PORT_P);
  feed(1, 1003, MTL_SESSION_PORT_P);
  feed(1, 1004, MTL_SESSION_PORT_P);

  EXPECT_EQ(port_duplicates(MTL_SESSION_PORT_P), 3u);
}

/* Cross-port normal redundancy must NOT count as same-port duplicate. */
TEST_F(St30RxReorderTest, CrossPortRedundantIsNotSamePortDuplicate) {
  feed_burst(0, 4, 1000, MTL_SESSION_PORT_P);
  feed_burst(0, 4, 1000, MTL_SESSION_PORT_R);

  EXPECT_EQ(port_duplicates(MTL_SESSION_PORT_P), 0u);
  EXPECT_EQ(port_duplicates(MTL_SESSION_PORT_R), 0u);
  EXPECT_GE(redundant(), 1u);
}
