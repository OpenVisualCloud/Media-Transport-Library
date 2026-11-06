/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

/*
 * Each video transmitter pacing path puts the frame's Sender Report on the session
 * queue immediately before packet 0 and after the previous frame's tail (TR-10-1
 * section 8.8.2, VSF TP-10-1 13.3b). An armed RL pre-arm queues the SR's shaper time
 * fewer pads so packet 0 keeps its launch; the unarmed warm-up, a whole trs a pad,
 * is left as is and packet 0 goes the SR's shaper time later.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='IpmxSrTrs*'
 */

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "ipmx/ipmx_sr_harness.h"

namespace {

constexpr uint8_t kSip[4] = {192, 168, 1, 10};
constexpr uint32_t kSsrc = 0x12345678;
constexpr uint16_t kMediaPort = 20000;
constexpr int kPortP = 0;
constexpr uint32_t kRtpTs = 0xa1b2c3d4;
constexpr uint64_t kEarly = 1000000;
constexpr uint64_t kTarget = 1050000;
constexpr uint64_t kLate = 1060000;
constexpr long double kTrs = 1000.0L;
constexpr int kFineTrim = 8;

/* a wire entry as a short code: P<idx>, S for an SR, D for a pad */
std::string Code(const struct ut_sr_wire& w) {
  if (w.kind == UT_SR_WIRE_SR) return "S";
  if (w.kind == UT_SR_WIRE_PAD) return "D";
  return "P" + std::to_string(w.pkt_idx);
}

class IpmxSrTrsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_sr_init(), 0);
    ctx_ = ut_sr_create(kSip, kSsrc, kMediaPort);
    ASSERT_NE(ctx_, nullptr);
    ASSERT_EQ(ut_sr_attach(ctx_), 0);
    ut_sr_trs_setup(ctx_, 1, kTrs, 0);
  }
  void TearDown() override {
    ut_sr_destroy(ctx_);
  }
  /* the non-pad wire entries */
  std::vector<std::string> Wire() {
    std::vector<std::string> codes;
    for (int i = 0; i < ut_sr_wire_count(ctx_); i++) {
      std::string c = Code(ut_sr_wire_at(ctx_, i));
      if (c != "D") codes.push_back(c);
    }
    return codes;
  }
  uint64_t Late() {
    struct ut_sr_counts c;
    ut_sr_counts(ctx_, kPortP, &c);
    return c.late;
  }
  ut_sr_ctx* ctx_ = nullptr;
};

using Codes = std::vector<std::string>;

TEST_F(IpmxSrTrsTest, RlBoundaryBulkSendsTailThenSrRightBeforePacket0) {
  ut_sr_trs_setup(ctx_, 4, kTrs, 0);
  for (uint32_t idx : {8u, 9u})
    ASSERT_EQ(ut_sr_trs_enqueue(ctx_, idx, kRtpTs - 1501, 0), 0);
  for (uint32_t idx : {0u, 1u})
    ASSERT_EQ(ut_sr_trs_enqueue(ctx_, idx, kRtpTs, kTarget), 0);
  ut_sr_set_now(ctx_, kEarly);
  ut_sr_call_rl(ctx_);
  EXPECT_EQ(Wire(), (Codes{"P8", "P9"}));

  ut_sr_set_now(ctx_, kTarget);
  ut_sr_call_rl(ctx_);
  EXPECT_EQ(Wire(), (Codes{"P8", "P9", "S", "P0", "P1"}));
  EXPECT_EQ(Late(), 0u);
}

TEST_F(IpmxSrTrsTest, RlOverdueBoundaryBulkStillOrdersAndCountsLate) {
  ut_sr_trs_setup(ctx_, 4, kTrs, 0);
  for (uint32_t idx : {8u, 9u})
    ASSERT_EQ(ut_sr_trs_enqueue(ctx_, idx, kRtpTs - 1501, 0), 0);
  for (uint32_t idx : {0u, 1u})
    ASSERT_EQ(ut_sr_trs_enqueue(ctx_, idx, kRtpTs, kTarget), 0);
  ut_sr_set_now(ctx_, kLate);
  ut_sr_call_rl(ctx_);
  EXPECT_EQ(Wire(), (Codes{"P8", "P9", "S", "P0", "P1"}));
  EXPECT_EQ(Late(), 1u);
}

/* the pad bytes an armed RL pre-arm queues ahead of packet 0 */
uint32_t PreArmPadBytes(ut_sr_ctx* ctx, bool* sr_before_pkt0) {
  uint32_t bytes = 0;
  ut_sr_trs_setup(ctx, 1, kTrs, 100);
  if (ut_sr_trs_arm(ctx, kTrs, 0) < 0) return 0;
  if (ut_sr_trs_enqueue(ctx, 0, kRtpTs, kTarget) < 0) return 0;
  ut_sr_set_now(ctx, kTarget - 50000);
  ut_sr_call_rl(ctx); /* dequeue and hold packet 0 */
  ut_sr_call_rl(ctx); /* pre-arm, then packet 0 behind the pads */
  *sr_before_pkt0 = false;
  for (int i = 0; i < ut_sr_wire_count(ctx); i++) {
    struct ut_sr_wire w = ut_sr_wire_at(ctx, i);
    if (w.kind == UT_SR_WIRE_PAD) bytes += w.len;
    if (w.kind == UT_SR_WIRE_SR)
      *sr_before_pkt0 = i + 1 < ut_sr_wire_count(ctx) &&
                        ut_sr_wire_at(ctx, i + 1).kind == UT_SR_WIRE_PKT;
  }
  return bytes;
}

TEST_F(IpmxSrTrsTest, RlPreArmQueuesSrBytesFewerPadBytes) {
  ut_sr_ctx* plain = ut_sr_create(kSip, kSsrc, kMediaPort); /* no SR */
  ASSERT_NE(plain, nullptr);
  bool sr_first = false, unused = false;
  uint32_t with_sr = PreArmPadBytes(ctx_, &sr_first);
  uint32_t without_sr = PreArmPadBytes(plain, &unused);
  ut_sr_destroy(plain);

  ASSERT_GT(without_sr, 0u);
  EXPECT_TRUE(sr_first);
  uint8_t sr[UT_SR_PKT_MAX];
  int sr_len = ut_sr_last_sr(ctx_, sr);
  EXPECT_NEAR((int)without_sr - (int)with_sr, sr_len, kFineTrim);
}

/* the pads an unarmed RL warm-up queues ahead of packet 0 */
int WarmUpPads(ut_sr_ctx* ctx, bool* sr_before_pkt0) {
  int pads = 0;
  ut_sr_trs_setup(ctx, 1, kTrs, 100);
  if (ut_sr_trs_enqueue(ctx, 0, kRtpTs, kTarget) < 0) return -1;
  ut_sr_set_now(ctx, kTarget - 50000);
  ut_sr_call_rl(ctx); /* dequeue and hold packet 0 */
  ut_sr_call_rl(ctx); /* the warm-up pads */
  ut_sr_set_now(ctx, kTarget);
  ut_sr_call_rl(ctx); /* packet 0 at its target */
  *sr_before_pkt0 = false;
  for (int i = 0; i < ut_sr_wire_count(ctx); i++) {
    struct ut_sr_wire w = ut_sr_wire_at(ctx, i);
    if (w.kind == UT_SR_WIRE_PAD) pads++;
    if (w.kind == UT_SR_WIRE_SR)
      *sr_before_pkt0 = i + 1 < ut_sr_wire_count(ctx) &&
                        ut_sr_wire_at(ctx, i + 1).kind == UT_SR_WIRE_PKT;
  }
  return pads;
}

TEST_F(IpmxSrTrsTest, RlUnarmedWarmUpKeepsItsPadsAndSendsSrBeforePacket0) {
  ut_sr_ctx* plain = ut_sr_create(kSip, kSsrc, kMediaPort); /* no SR */
  ASSERT_NE(plain, nullptr);
  bool sr_first = false, unused = false;
  int with_sr = WarmUpPads(ctx_, &sr_first);
  int without_sr = WarmUpPads(plain, &unused);
  ut_sr_destroy(plain);

  EXPECT_EQ(without_sr, 50);
  EXPECT_EQ(with_sr, without_sr);
  EXPECT_TRUE(sr_first);
}

TEST_F(IpmxSrTrsTest, TscHeldPacket0SendsSrWithIt) {
  ASSERT_EQ(ut_sr_trs_enqueue(ctx_, 0, kRtpTs, kTarget), 0);
  ut_sr_set_now(ctx_, kEarly);
  ut_sr_call_tsc(ctx_);
  EXPECT_EQ(Wire(), Codes{});
  ut_sr_set_now(ctx_, kTarget);
  ut_sr_call_tsc(ctx_);
  EXPECT_EQ(Wire(), (Codes{"S", "P0"}));
  EXPECT_EQ(Late(), 0u);
}

TEST_F(IpmxSrTrsTest, TscOverduePacket0CountsLate) {
  ASSERT_EQ(ut_sr_trs_enqueue(ctx_, 0, kRtpTs, kTarget), 0);
  ut_sr_set_now(ctx_, kLate);
  ut_sr_call_tsc(ctx_);
  EXPECT_EQ(Wire(), (Codes{"S", "P0"}));
  EXPECT_EQ(Late(), 1u);
}

TEST_F(IpmxSrTrsTest, TscLaterPacketsCarryNoSr) {
  for (uint32_t idx : {0u, 1u})
    ASSERT_EQ(ut_sr_trs_enqueue(ctx_, idx, kRtpTs, kTarget), 0);
  ut_sr_set_now(ctx_, kLate);
  ut_sr_call_tsc(ctx_);
  ut_sr_call_tsc(ctx_);
  EXPECT_EQ(Wire(), (Codes{"S", "P0", "P1"}));
}

TEST_F(IpmxSrTrsTest, PtpHeldPacket0SendsSrWithIt) {
  ASSERT_EQ(ut_sr_trs_enqueue(ctx_, 0, kRtpTs, kTarget), 0);
  ut_sr_set_now(ctx_, kEarly);
  ut_sr_call_ptp(ctx_);
  EXPECT_EQ(Wire(), Codes{});
  ut_sr_set_now(ctx_, kTarget);
  ut_sr_call_ptp(ctx_);
  EXPECT_EQ(Wire(), (Codes{"S", "P0"}));
  EXPECT_EQ(Late(), 0u);
}

TEST_F(IpmxSrTrsTest, PtpOverduePacket0CountsLate) {
  ASSERT_EQ(ut_sr_trs_enqueue(ctx_, 0, kRtpTs, kTarget), 0);
  ut_sr_set_now(ctx_, kLate);
  ut_sr_call_ptp(ctx_);
  EXPECT_EQ(Wire(), (Codes{"S", "P0"}));
  EXPECT_EQ(Late(), 1u);
}

TEST_F(IpmxSrTrsTest, LaunchTimeSrTakesPacket0LaunchTime) {
  ASSERT_EQ(ut_sr_trs_enqueue(ctx_, 0, kRtpTs, kTarget), 0);
  ut_sr_set_now(ctx_, kEarly);
  ASSERT_GE(ut_sr_call_launch_time(ctx_), 0);
  ASSERT_EQ(Wire(), (Codes{"S", "P0"}));
  struct ut_sr_wire sr = ut_sr_wire_at(ctx_, 0);
  struct ut_sr_wire pkt0 = ut_sr_wire_at(ctx_, 1);
  EXPECT_TRUE(sr.launch);
  EXPECT_TRUE(pkt0.launch);
  EXPECT_EQ(sr.launch_time, kTarget);
  EXPECT_EQ(pkt0.launch_time, kTarget);
  EXPECT_EQ(Late(), 0u);
}

TEST_F(IpmxSrTrsTest, LaunchTimeOverduePacket0CountsLate) {
  ASSERT_EQ(ut_sr_trs_enqueue(ctx_, 0, kRtpTs, kTarget), 0);
  ut_sr_set_now(ctx_, kLate);
  ASSERT_GE(ut_sr_call_launch_time(ctx_), 0);
  EXPECT_EQ(Wire(), (Codes{"S", "P0"}));
  EXPECT_EQ(Late(), 1u);
}

TEST(IpmxSrTrsOffTest, SessionWithoutSrSendsOnlyMedia) {
  ASSERT_EQ(ut_sr_init(), 0);
  ut_sr_ctx* ctx = ut_sr_create(kSip, kSsrc, kMediaPort);
  ASSERT_NE(ctx, nullptr);
  ut_sr_trs_setup(ctx, 1, kTrs, 0);
  ASSERT_EQ(ut_sr_trs_enqueue(ctx, 0, kRtpTs, kTarget), 0);
  ut_sr_set_now(ctx, kLate);
  ut_sr_call_rl(ctx);
  ASSERT_EQ(ut_sr_wire_count(ctx), 1);
  EXPECT_EQ(ut_sr_wire_at(ctx, 0).kind, UT_SR_WIRE_PKT);
  ut_sr_destroy(ctx);
}

}  // namespace
