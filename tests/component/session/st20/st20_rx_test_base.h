/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * Shared gtest fixture for the per-feature ST 2110-20 RX session tests
 * under tests/unit/session/st20/.
 *
 * Each per-feature .cpp derives a thin subclass:
 *
 *   class St20RxSlotTest : public St20RxBaseTest {};
 *
 * so that --gtest_filter='St20RxSlotTest.*' selects only that feature's
 * tests, while all setup/teardown and stat-accessor wrappers live here in
 * one place.
 */

#pragma once

#include <gtest/gtest.h>

#include "session/st20_harness.h"

class St20RxBaseTest : public ::testing::Test {
 protected:
  ut20_test_ctx* ctx_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(ut20_init(), 0);
    ctx_ = ut20_ctx_create(2);
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    if (ctx_) ut20_ctx_destroy(ctx_);
  }

  /* convenience wrappers */
  uint64_t received() {
    return ut20_stat_received(ctx_);
  }
  uint64_t redundant() {
    return ut20_stat_redundant(ctx_);
  }
  uint64_t ooo() {
    return ut20_stat_lost_pkts(ctx_);
  }
  uint64_t port_reordered(enum mtl_session_port p) {
    return ut20_stat_port_reordered(ctx_, p);
  }
  uint64_t port_lost(enum mtl_session_port p) {
    return ut20_stat_port_lost(ctx_, p);
  }
  uint64_t port_pkts(enum mtl_session_port p) {
    return ut20_stat_port_packets(ctx_, p);
  }
  uint64_t no_slot() {
    return ut20_stat_no_slot(ctx_);
  }
  uint64_t idx_oo_bitmap() {
    return ut20_stat_idx_oo_bitmap(ctx_);
  }
  uint64_t frames_incomplete() {
    return ut20_stat_frames_incomplete(ctx_);
  }
  int frames_received() {
    return ut20_frames_received(ctx_);
  }
  uint64_t wrong_pt() {
    return ut20_stat_wrong_pt(ctx_);
  }
  uint64_t wrong_ssrc() {
    return ut20_stat_wrong_ssrc(ctx_);
  }
  uint64_t wrong_interlace() {
    return ut20_stat_wrong_interlace(ctx_);
  }
  uint64_t offset_dropped() {
    return ut20_stat_offset_dropped(ctx_);
  }

  void feed(int pkt_idx, uint32_t ts, enum mtl_session_port port) {
    ut20_feed_frame_pkt(ctx_, pkt_idx, ts, port);
  }

  void feed_seq(int pkt_idx, uint32_t seq, uint32_t ts, enum mtl_session_port port) {
    ut20_feed_frame_pkt_seq(ctx_, pkt_idx, seq, ts, port);
  }

  void feed_full(uint32_t ts, enum mtl_session_port port) {
    ut20_feed_full_frame(ctx_, ts, port);
  }
};
