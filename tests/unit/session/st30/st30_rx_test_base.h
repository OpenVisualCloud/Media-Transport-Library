/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * Shared gtest fixture for the per-feature ST 2110-30 RX session tests
 * under tests/unit/session/st30/.
 *
 * Each per-feature .cpp derives a thin subclass:
 *
 *   class St30RxStatsTest : public St30RxBaseTest {};
 *
 * so that --gtest_filter='St30RxStatsTest.*' selects only that feature's
 * tests, while all setup/teardown and stat-accessor wrappers live here in
 * one place.
 */

#pragma once

#include <gtest/gtest.h>

#include "session/st30_harness.h"

class St30RxBaseTest : public ::testing::Test {
 protected:
  ut30_test_ctx* ctx_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(ut30_init(), 0) << "EAL init failed";
    ctx_ = ut30_ctx_create(2); /* 2 ports = redundant */
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut30_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }

  int feed(uint16_t seq, uint32_t ts, enum mtl_session_port port) {
    return ut30_feed_pkt(ctx_, seq, ts, port);
  }

  void feed_burst(uint16_t seq_start, int count, uint32_t ts,
                  enum mtl_session_port port) {
    ut30_feed_burst(ctx_, seq_start, count, ts, port);
  }

  uint64_t unrecovered() {
    return ut30_stat_unrecovered(ctx_);
  }
  uint64_t redundant() {
    return ut30_stat_redundant(ctx_);
  }
  uint64_t received() {
    return ut30_stat_received(ctx_);
  }
  uint64_t ooo() {
    return ut30_stat_lost_pkts(ctx_);
  }
  int session_seq() {
    return ut30_session_seq_id(ctx_);
  }
  int frames_done() {
    return ut30_frames_received(ctx_);
  }
  int ppf() {
    return ut30_pkts_per_frame(ctx_);
  }

  uint64_t port_pkts(enum mtl_session_port p) {
    return ut30_stat_port_pkts(ctx_, p);
  }
  uint64_t port_bytes(enum mtl_session_port p) {
    return ut30_stat_port_bytes(ctx_, p);
  }
  uint64_t port_ooo(enum mtl_session_port p) {
    return ut30_stat_port_lost(ctx_, p);
  }
  uint64_t wrong_pt() {
    return ut30_stat_wrong_pt(ctx_);
  }
  uint64_t wrong_ssrc() {
    return ut30_stat_wrong_ssrc(ctx_);
  }
  uint64_t len_mismatch() {
    return ut30_stat_len_mismatch(ctx_);
  }
  uint64_t port_reordered(enum mtl_session_port p) {
    return ut30_stat_port_reordered(ctx_, p);
  }
  uint64_t port_duplicates(enum mtl_session_port p) {
    return ut30_stat_port_duplicates(ctx_, p);
  }
};
