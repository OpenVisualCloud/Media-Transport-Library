/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * Shared gtest fixture for the per-feature ST 2110-40 RX session tests
 * under tests/unit/session/st40/.
 *
 * Each per-feature .cpp derives a thin subclass:
 *
 *   class St40RxMarkerTest : public St40RxBaseTest {};
 *
 * so that --gtest_filter='St40RxMarkerTest.*' selects only that feature's
 * tests, while all setup/teardown and stat-accessor wrappers live here in
 * one place.
 */

#pragma once

#include <gtest/gtest.h>

#include "session/st40_harness.h"

class St40RxBaseTest : public ::testing::Test {
 protected:
  ut_test_ctx* ctx_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(ut40_init(), 0) << "EAL init failed";
    ut40_drain_ring();
    ctx_ = ut40_ctx_create(2); /* 2 ports = redundant */
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut40_drain_ring();
    ut40_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }

  /* convenience wrappers */
  int feed(uint16_t seq, uint32_t ts, bool marker, enum mtl_session_port port) {
    return ut40_feed_pkt(ctx_, seq, ts, marker ? 1 : 0, port);
  }

  void feed_burst(uint16_t seq_start, int count, uint32_t ts, bool last_marker,
                  enum mtl_session_port port) {
    ut40_feed_burst(ctx_, seq_start, count, ts, last_marker ? 1 : 0, port);
  }

  uint64_t unrecovered() {
    return ut40_stat_unrecovered(ctx_);
  }
  uint64_t redundant() {
    return ut40_stat_redundant(ctx_);
  }
  uint64_t received() {
    return ut40_stat_received(ctx_);
  }
  uint64_t ooo() {
    return ut40_stat_lost_pkts(ctx_);
  }
  int session_seq() {
    return ut40_session_seq_id(ctx_);
  }

  uint64_t port_pkts(enum mtl_session_port p) {
    return ut40_stat_port_pkts(ctx_, p);
  }
  uint64_t port_bytes(enum mtl_session_port p) {
    return ut40_stat_port_bytes(ctx_, p);
  }
  uint64_t port_ooo(enum mtl_session_port p) {
    return ut40_stat_port_lost(ctx_, p);
  }
  uint64_t port_frames(enum mtl_session_port p) {
    return ut40_stat_port_frames(ctx_, p);
  }
  uint64_t port_reordered(enum mtl_session_port p) {
    return ut40_stat_port_reordered(ctx_, p);
  }
  uint64_t port_duplicates(enum mtl_session_port p) {
    return ut40_stat_port_duplicates(ctx_, p);
  }
  uint64_t field_bit_mismatch() {
    return ut40_stat_field_bit_mismatch(ctx_);
  }
  uint64_t wrong_pt() {
    return ut40_stat_wrong_pt(ctx_);
  }
  uint64_t wrong_ssrc() {
    return ut40_stat_wrong_ssrc(ctx_);
  }
  uint64_t wrong_interlace() {
    return ut40_stat_wrong_interlace(ctx_);
  }
  uint64_t interlace_first() {
    return ut40_stat_interlace_first(ctx_);
  }
  uint64_t interlace_second() {
    return ut40_stat_interlace_second(ctx_);
  }
  uint64_t enqueue_fail() {
    return ut40_stat_enqueue_fail(ctx_);
  }
  int frames_received() {
    return ut40_frames_received(ctx_);
  }
};
