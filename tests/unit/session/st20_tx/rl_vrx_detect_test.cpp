/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The VRX detector of the RL transmitter: a frame a pkt of which may reach the receiver
 * past its ST 2110-21 drain slot is counted in stat_rl.vrx_frames, at packet 0 or
 * mid-frame, and the stats dump says so in an error line. The fault matrix
 * (rl_vrx_faults_test.cpp) checks that no session below VRX 0 goes unflagged.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxRlVrxDetect*'
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "session/st20_tx/rl_vrx_model.h"
#include "session/st20_tx_harness.h"
#include "session/st_video_transmitter_harness.h"
#include "session/stderr_capture.h"
#include "st2110/st_tx_video_rl.h"

namespace {

struct Fmt {
  const char* name;
  uint32_t width, height;
};

constexpr int kPortP = 0;
constexpr int kSlots = 512;

class St20TxRlVrxDetectBudget : public ::testing::TestWithParam<Fmt> {};

/* A packet 0 that leaves the shaper the budget and the wire path past its target keeps
 * every drain slot at the worst 90 kHz snap, of a frame whose pads round up too; a trs
 * later it does not. */
TEST_P(St20TxRlVrxDetectBudget, KeepsVrxAndIsTight) {
  ASSERT_EQ(0, ut_txv_init());
  const Fmt& f = GetParam();
  const float trained[1] = {164};
  ut_txv_ctx* txv = ut_txv_create();
  ASSERT_NE(nullptr, txv);
  ut_txv_set_rl_format(txv, f.width, f.height, false, ST_FPS_P59_94);
  ASSERT_EQ(0, ut_txv_init_rl_pacing(txv, 1, 0, false, trained, false));
  ASSERT_TRUE(ut_txv_rl_armed(txv, kPortP));
  const long double trs = ut_txv_pacing_trs(txv), p = ut_txv_rl_drain(txv, kPortP);
  const long double pi = ut_txv_pad_interval(txv);
  const uint32_t vrx = ut_txv_pacing_vrx(txv);
  const long double tpr0 =
      vrx * trs - ut_rl_snap_loss_5994(ut_txv_pacing_tr_offset(txv), vrx, trs);
  const int64_t budget = ut_txv_rl_vrx_budget(txv, kPortP);
  ut_txv_destroy(txv);

  ASSERT_GT(budget, 0) << f.name;
  EXPECT_GE(ut_rl_vrx(budget + ST_TX_VIDEO_RL_WIRE_NS, tpr0, trs, p, pi, kSlots, 1), 0)
      << f.name;
  EXPECT_LT(ut_rl_vrx(budget + ST_TX_VIDEO_RL_WIRE_NS + trs, tpr0, trs, p, pi, kSlots, 1),
            0)
      << f.name;
}

INSTANTIATE_TEST_SUITE_P(Formats, St20TxRlVrxDetectBudget,
                         ::testing::Values(Fmt{"720p59", 1280, 720},
                                           Fmt{"1080p59", 1920, 1080},
                                           Fmt{"2160p59", 3840, 2160}),
                         [](const ::testing::TestParamInfo<Fmt>& info) {
                           return std::string(info.param.name);
                         });

/* 1080p59.94 epochs take two phases on the 90 kHz grid and the plan's budget the worse
 * snap. A frame of the other phase has its target farther from its drain slots by the
 * snaps' difference, the room: a pkt between the two budgets late keeps VRX >= 0. */
class St20TxRlVrxDetectSnap : public ::testing::Test {
 protected:
  void SetUp() override {
    SetUpFormat(1920, 1080);
  }
  void SetUpFormat(uint32_t width, uint32_t height) {
    ASSERT_EQ(0, ut_txv_init());
    const float trained[1] = {164};
    ctx_ = ut_txv_create();
    ASSERT_NE(nullptr, ctx_);
    ut_txv_set_rl_format(ctx_, width, height, false, ST_FPS_P59_94);
    ASSERT_EQ(0, ut_txv_init_rl_pacing(ctx_, 1, 0, false, trained, false));
    trs_ = ut_txv_pacing_trs(ctx_);
    vrx_ = ut_txv_pacing_vrx(ctx_);
    budget_ = ut_txv_rl_vrx_budget(ctx_, kPortP);
    ASSERT_GT(budget_, 0);
    const bool first_worse = Snap(kEpoch) > Snap(kEpoch + 1);
    worst_ = first_worse ? kEpoch : kEpoch + 1;
    other_ = first_worse ? kEpoch + 1 : kEpoch;
    room_ = Snap(worst_) - Snap(other_);
    ASSERT_GT(room_, 2000);
    between_ = budget_ + (int64_t)(room_ / 2);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  long double Snap(uint64_t epoch) const {
    return ut_rl_snap(60000, 1001, ut_txv_pacing_tr_offset(ctx_), vrx_, trs_, epoch);
  }
  uint64_t Target(uint64_t epoch) const {
    const uint64_t start = (uint64_t)(st_epoch_to_tai(epoch, 60000, 1001) +
                                      ut_txv_pacing_tr_offset(ctx_) - vrx_ * trs_);
    return st_tai_round_to_media_clk_ns(start, kUtRlRate90k);
  }
  /* the fewest packets the frame of epoch buffers, its packet 0 late_ns past its target
   */
  int Vrx(uint64_t epoch, long double late_ns, int extra_pads = 0) const {
    return ut_rl_vrx(late_ns, vrx_ * trs_ - Snap(epoch), trs_,
                     ut_txv_rl_drain(ctx_, kPortP), ut_txv_pad_interval(ctx_), kSlots,
                     extra_pads);
  }
  /* tv_sync_pacing() starts the frame of epoch late_ns past its target */
  uint64_t StartLate(uint64_t epoch, int64_t late_ns) {
    ut_txv_set_cur_epochs(ctx_, epoch - 1);
    ut_txv_set_mock_ptp_time(ctx_, Target(epoch) + late_ns);
    EXPECT_EQ(0, ut_txv_sync_pacing(ctx_, 0));
    return ut_txv_rl_start_late(ctx_);
  }
  /* the frame of epoch is built on time and its pkt late_pkt leaves late_ns late */
  int SentLate(uint64_t epoch, uint32_t late_pkt, int64_t late_ns) {
    ut_txv_set_cur_epochs(ctx_, epoch - 1);
    ut_txv_set_mock_tsc_time(ctx_, 1000000000);
    ut_txv_set_mock_ptp_time(ctx_, Target(epoch) - 100000);
    return ut_txv_run_frame_rl_late(ctx_, late_pkt, late_ns);
  }
  /* the frame of epoch starts start_late_ns past its target, so tv_sync_pacing() re-times
   * it from now, and its pkt late_pkt leaves late_ns past the re-timed target */
  int SentLateAfterALateStart(uint64_t epoch, int64_t start_late_ns, uint32_t late_pkt,
                              int64_t late_ns) {
    ut_txv_set_cur_epochs(ctx_, epoch - 1);
    ut_txv_set_mock_tsc_time(ctx_, 1000000000);
    ut_txv_set_mock_ptp_time(ctx_, Target(epoch) + start_late_ns);
    return ut_txv_run_frame_rl_late(ctx_, late_pkt, late_ns);
  }
  /* the same on an ST 2022-7 frame, pkt 1 late_p_ns late on P and late_r_ns on R */
  int SentLateOnPorts(uint64_t epoch, int64_t start_late_ns, int64_t late_p_ns,
                      int64_t late_r_ns) {
    const uint64_t late[2] = {(uint64_t)late_p_ns, (uint64_t)late_r_ns};
    ut_txv_set_cur_epochs(ctx_, epoch - 1);
    ut_txv_set_mock_tsc_time(ctx_, 1000000000);
    ut_txv_set_mock_ptp_time(ctx_, Target(epoch) + start_late_ns);
    return ut_txv_run_frame_rl_late_ports(ctx_, 1, late);
  }
  static constexpr uint64_t kEpoch = 100000000000;
  ut_txv_ctx* ctx_ = nullptr;
  long double trs_ = 0, room_ = 0;
  uint32_t vrx_ = 0;
  int64_t budget_ = 0, between_ = 0;
  uint64_t worst_ = 0, other_ = 0;
};

TEST_F(St20TxRlVrxDetectSnap, AStartWithinItsOwnSnapsBudgetIsNotCounted) {
  EXPECT_GE(Vrx(other_, between_), 0);
  EXPECT_EQ(0u, StartLate(other_, between_));
}

TEST_F(St20TxRlVrxDetectSnap, TheSameStartOnTheWorseSnapIsCounted) {
  EXPECT_EQ(1u, StartLate(worst_, between_));
}

TEST_F(St20TxRlVrxDetectSnap, AStartIsCountedOnlyPastItsOwnSnapsBudget) {
  EXPECT_EQ(0u, StartLate(other_, budget_ + (int64_t)room_));
  EXPECT_EQ(1u, StartLate(other_, budget_ + (int64_t)room_ + 2));
}

/* A frame started past its budget is counted in K, the app's lateness, and in neither
 * port's frames MTL sent late, however late its pkts then leave. */
TEST_F(St20TxRlVrxDetectSnap, AFrameStartedPastItsBudgetIsInKNotInN) {
  const int64_t start = budget_ + (int64_t)room_ + 1000;
  EXPECT_EQ(0, SentLateAfterALateStart(other_, start, 0, 4 * budget_));
  EXPECT_EQ(0, SentLateAfterALateStart(other_, start, 1, 4 * budget_));
  ut_txv_set_redundant_budget(ctx_, 2 * budget_);
  EXPECT_EQ(0, SentLateOnPorts(other_, start, 4 * budget_, 4 * budget_));
  EXPECT_EQ(3u, ut_txv_rl_start_late(ctx_));
}

/* a start seconds late, past what a 32-bit room holds, is in K alone too */
TEST_F(St20TxRlVrxDetectSnap, AStartSecondsLateIsInKNotInN) {
  EXPECT_EQ(0, SentLateAfterALateStart(other_, 3000000000LL, 0, 4 * budget_));
  EXPECT_EQ(1u, ut_txv_rl_start_late(ctx_));
}

/* An exact user start is not snapped, so its drain slot is vrx trs after it: the snap
 * loss the budget takes off is its room. */
TEST_F(St20TxRlVrxDetectSnap, AnExactUserStartHasTheSnapLossAsRoom) {
  const int64_t loss =
      (int64_t)ut_rl_snap_loss_5994(ut_txv_pacing_tr_offset(ctx_), vrx_, trs_);
  ASSERT_EQ(0u, StartLate(other_, 0));
  ut_txv_set_exact_user_pacing(ctx_, true);
  const uint64_t start = Target(other_ + 1);
  ut_txv_set_mock_ptp_time(ctx_, start + budget_ + loss - 2);
  ASSERT_EQ(0, ut_txv_sync_pacing(ctx_, start));
  const uint64_t counted = ut_txv_rl_start_late(ctx_);
  EXPECT_EQ(0u, counted);
  ut_txv_set_mock_ptp_time(ctx_, start + budget_ + loss + 2);
  ASSERT_EQ(0, ut_txv_sync_pacing(ctx_, start));
  EXPECT_EQ(counted + 1, ut_txv_rl_start_late(ctx_));
}

TEST_F(St20TxRlVrxDetectSnap, Packet0WithinItsOwnSnapsBudgetIsNotFlagged) {
  EXPECT_EQ(0, SentLate(other_, 0, between_));
}

TEST_F(St20TxRlVrxDetectSnap, AMidFramePktWithinItsOwnSnapsBudgetIsNotFlagged) {
  EXPECT_EQ(0, SentLate(other_, 1, between_));
}

TEST_F(St20TxRlVrxDetectSnap, TheSamePktOnTheWorseSnapIsFlagged) {
  EXPECT_EQ(1, SentLate(worst_, 0, between_));
  EXPECT_EQ(1, SentLate(worst_, 1, between_));
}

TEST_F(St20TxRlVrxDetectSnap, APktIsFlaggedOnlyPastItsOwnSnapsBudget) {
  for (uint32_t pkt : {0u, 1u}) {
    EXPECT_EQ(0, SentLate(other_, pkt, budget_ + (int64_t)room_)) << pkt;
    EXPECT_EQ(1, SentLate(other_, pkt, budget_ + (int64_t)room_ + 2)) << pkt;
  }
}

/* A frame re-timed from a start within its budget has only the rest of the budget left:
 * the lateness of the start and of the pkt add up. */
TEST_F(St20TxRlVrxDetectSnap, ALateStartLeavesItsPktsTheRestOfTheBudget) {
  const int64_t start = budget_ / 2;
  for (uint32_t pkt : {0u, 1u}) {
    EXPECT_EQ(1, SentLateAfterALateStart(worst_, start, pkt, budget_ - start + 1000))
        << pkt;
    EXPECT_EQ(0, SentLateAfterALateStart(other_, start, pkt,
                                         budget_ - start + (int64_t)room_ - 2))
        << pkt;
    EXPECT_EQ(1, SentLateAfterALateStart(other_, start, pkt,
                                         budget_ - start + (int64_t)room_ + 2))
        << pkt;
  }
  EXPECT_EQ(0u, ut_txv_rl_start_late(ctx_));
}

/* An all-RL frame's builder stamps packet 0 only: a mid-frame pkt is judged against
 * packet 0's target and room, whatever its mbuf's last use left in it. */
class St20TxRlVrxDetectStale : public St20TxRlVrxDetectSnap,
                               public ::testing::WithParamInterface<uint64_t> {};

TEST_P(St20TxRlVrxDetectStale, AMidFramePktIsJudgedFromPacket0) {
  ut_txv_set_stale_stamps(ctx_, GetParam());
  EXPECT_EQ(0, SentLate(other_, 1, budget_ + (int64_t)room_));
  EXPECT_EQ(1, SentLate(other_, 1, budget_ + (int64_t)room_ + 2));
}

INSTANTIATE_TEST_SUITE_P(Stamps, St20TxRlVrxDetectStale,
                         ::testing::Values(0, UT_TXV_NO_STAMP));

/* The budget is the drain slot less the worst snap, the sawtooth and the wire path: a
 * packet 0 that left the CPU past it is flagged, and a start past it is in K, at its
 * phase's room on either 59.94 phase. */
class St20TxRlVrxDetectWire : public St20TxRlVrxDetectSnap,
                              public ::testing::WithParamInterface<Fmt> {
 protected:
  void SetUp() override {
    SetUpFormat(GetParam().width, GetParam().height);
    const long double pi = ut_txv_pad_interval(ctx_);
    const long double sawtooth =
        (pi / 2 + ut_txv_bulk(ctx_)) / (pi + 1) * trs_ + ut_txv_rl_drain(ctx_, kPortP);
    slot_less_wire_ =
        (int64_t)(vrx_ * trs_ - Snap(worst_) - sawtooth - ST_TX_VIDEO_RL_WIRE_NS);
    ASSERT_GT(slot_less_wire_, 0);
  }
  int64_t slot_less_wire_ = 0;
};

TEST_P(St20TxRlVrxDetectWire, Packet0IsFlaggedPastTheSlotLessTheWirePath) {
  const int64_t room = (int64_t)room_;
  EXPECT_EQ(0, SentLate(worst_, 0, slot_less_wire_ - 2));
  EXPECT_EQ(1, SentLate(worst_, 0, slot_less_wire_ + 2));
  EXPECT_EQ(0, SentLate(other_, 0, slot_less_wire_ + room - 2));
  EXPECT_EQ(1, SentLate(other_, 0, slot_less_wire_ + room + 2));
}

TEST_P(St20TxRlVrxDetectWire, AStartIsCountedPastTheSlotLessTheWirePath) {
  EXPECT_EQ(0u, StartLate(other_, slot_less_wire_ + (int64_t)room_ - 2));
  EXPECT_EQ(1u, StartLate(other_, slot_less_wire_ + (int64_t)room_ + 2));
}

/* ST 2022-7, R's budget twice P's: a start late within P's budget leaves P the rest of
 * the slot less the wire path, and R the rest of its own budget. */
TEST_P(St20TxRlVrxDetectWire, ALateStartLeavesEachPortTheRestOfItsBudget) {
  const int64_t budget = slot_less_wire_, room = (int64_t)room_, start = budget / 2;
  ut_txv_set_redundant_budget(ctx_, 2 * budget);
  const int64_t p_left = budget + room - start, r_left = 2 * budget + room - start;
  EXPECT_EQ(0, SentLateOnPorts(other_, start, p_left - 2, 0));
  EXPECT_EQ(1, SentLateOnPorts(other_, start, p_left + 2, 0)) << "P";
  EXPECT_EQ(0, SentLateOnPorts(other_, start, 0, r_left - 2));
  EXPECT_EQ(1, SentLateOnPorts(other_, start, 0, r_left + 2)) << "R";
  EXPECT_EQ(0u, ut_txv_rl_start_late(ctx_));
}

TEST_P(St20TxRlVrxDetectWire, StatDumpPrintsTheBudget) {
  char budget[32];
  (void)snprintf(budget, sizeof(budget), "over the %.1fus budget",
                 (double)slot_less_wire_ / 1000);
  int locked = 0;
  ut_txv_set_rl_vrx(ctx_, 1, 1, 47500);
  ut_txv_run_sessions_stat(ctx_, &locked);
  const std::string line = ut_txv_stat_error_log_line();
  EXPECT_NE(std::string::npos, line.find(budget)) << line;
}

INSTANTIATE_TEST_SUITE_P(Formats, St20TxRlVrxDetectWire,
                         ::testing::Values(Fmt{"720p59", 1280, 720},
                                           Fmt{"1080p59", 1920, 1080}),
                         [](const ::testing::TestParamInfo<Fmt>& info) {
                           return std::string(info.param.name);
                         });

/* a start or packet 0 late enough that a pkt of a frame whose pads round up misses its
 * drain slot, on either snap phase, is counted */
TEST_F(St20TxRlVrxDetectSnap, EveryLaunchPastADrainSlotIsFlagged) {
  int negative = 0;
  for (uint64_t epoch : {worst_, other_})
    for (long double late = 0; late < (vrx_ + 1) * trs_; late += trs_ / 8) {
      if (Vrx(epoch, late, 1) >= 0) continue;
      negative++;
      const uint64_t counted = ut_txv_rl_start_late(ctx_);
      EXPECT_EQ(counted + 1, StartLate(epoch, (int64_t)late))
          << epoch << " late " << (double)late;
      EXPECT_EQ(1, SentLate(epoch, 0, (int64_t)late))
          << epoch << " late " << (double)late;
    }
  EXPECT_GT(negative, 0);
}

/* each phase's budget and the wire path keep every drain slot at its own snap, of a frame
 * whose pads round up too; a trs later they do not */
TEST_F(St20TxRlVrxDetectSnap, EachPhasesBudgetKeepsVrx) {
  for (uint64_t epoch : {worst_, other_}) {
    const long double budget =
        budget_ + ST_TX_VIDEO_RL_WIRE_NS + Snap(worst_) - Snap(epoch);
    EXPECT_GE(Vrx(epoch, budget, 1), 0) << epoch;
    EXPECT_LT(Vrx(epoch, budget + trs_, 1), 0) << epoch;
  }
}

/* A user start_vrx whose slot the snap, the sawtooth and the wire path use up leaves no
 * budget: it would flag every frame, so the port judges none. One whose budget leaves no
 * stall limit past check 1's aims keeps no stall and may be flagged on every frame. Each
 * port warns once at create, for the first cause or the second. start_vrx 3 at
 * 1080p59.94 has no budget by the wire path alone; at 1080p29.97 BPM, 2.5 us. */
TEST(St20TxRlVrxDetectBudgetBelowZero, ASmallStartVrxWarnsOncePerPort) {
  const struct {
    enum st_fps fps;
    uint16_t start_vrx;
    bool budget;
  } cases[] = {
      {ST_FPS_P59_94, 1, false}, {ST_FPS_P59_94, 3, false}, {ST_FPS_P29_97, 3, true}};
  ASSERT_EQ(0, ut_txv_init());
  const float trained[2] = {164, 164};
  for (const auto& c : cases) {
    ut_txv_ctx* txv = ut_txv_create();
    ASSERT_NE(nullptr, txv);
    ut_txv_set_rl_format(txv, 1920, 1080, false, c.fps);
    if (c.budget)
      ut_txv_set_rl_layout(txv, ST20_PACKING_BPM, ST20_FMT_YUV_422_10BIT, 512);
    ut_txv_set_start_vrx(txv, c.start_vrx);
    int ret = -1;
    const std::string log = ut_session::capture_stderr(
        [&] { ret = ut_txv_init_rl_pacing(txv, 2, 0, false, trained, false); });
    ASSERT_EQ(0, ret);
    ASSERT_EQ(c.start_vrx, ut_txv_pacing_vrx(txv));
    for (int port = 0; port < 2; port++) {
      const std::string what = "fps " + std::to_string(c.fps) + " start_vrx " +
                               std::to_string(c.start_vrx) + " port " +
                               std::to_string(port);
      const std::string prefix = "port " + std::to_string(port) + " vrx " +
                                 std::to_string(c.start_vrx) + " leaves no ";
      const std::string warn = prefix + (c.budget ? "stall limit, frames may be flagged"
                                                  : "VRX budget, VRX not judged");
      const size_t at = log.find(warn);
      ASSERT_TRUE(ut_txv_rl_armed(txv, port)) << what;
      EXPECT_EQ(c.budget, ut_txv_rl_vrx_budget(txv, port) > 0) << what;
      EXPECT_LE((double)ut_txv_rl_absorb(txv, port), 0.0) << what;
      EXPECT_NE(std::string::npos, at) << what << ": " << warn << " in:\n" << log;
      EXPECT_EQ(std::string::npos, log.find(prefix, at + 1)) << what << ": twice";
    }
    ut_txv_destroy(txv);
  }
}

/* 1080p59.94 GPM, a zero-copy pkt on 2 descriptors */
constexpr long double kTrs = 16683333.333L * 1080 / 1125 / 4320;
constexpr uint32_t kVrx = 5;
constexpr uint16_t kChainedSegs = 2;
constexpr uint64_t kCallNs = 500;
constexpr uint64_t kSteadyNs = 1000000;
/* a stream without pads: the drain slot vrx trs past the target, less the sawtooth bound
 * tv_rl_plan_init() takes, (pad_interval / 2 + bulk) / (pad_interval + 1) trs + a pad,
 * here half a trs and one */
constexpr int64_t kBudgetNs = (int64_t)(kVrx * kTrs - 1.5L * kTrs);

class St20TxRlVrxDetectStream : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
  }

  /* the stream after the tasklet lost its CPU for stall_ns: how late it left the wire
   * past its stamps, and whether the frame was flagged */
  long double Run(uint16_t nic_desc, uint64_t stall_ns, ut_trs_rl_stat* st) {
    ut_trs_ctx* ctx = ut_trs_create();
    EXPECT_NE(nullptr, ctx);
    EXPECT_EQ(0, ut_trs_setup_stream(ctx, kTrs, nic_desc, kChainedSegs));
    ut_trs_set_vrx_detect(ctx, kPortP, kBudgetNs, 0);
    /* pkt 1 is stamped at the first call, when it leaves an idle queue */
    ut_trs_set_stream_target(ctx, ut_trs_last_tsc(ctx) + kCallNs - (uint64_t)kTrs);
    ut_trs_stream(ctx, kSteadyNs, kCallNs);
    ut_trs_stall_stream(ctx, stall_ns);
    ut_trs_stream(ctx, kSteadyNs, kCallNs);
    *st = ut_trs_get_rl_stat(ctx);
    const long double late = ut_trs_stream_max_late_vs_target_ns(ctx);
    ut_trs_destroy(ctx);
    return late;
  }
};

/* A stall that leaves the queue dry delays the pkts after it. Each one past its drain
 * slot, vrx trs after its stamp, is in a flagged frame, and a flag means a pkt
 * left at least the budget late: shallow as right after packet 0, and deep. */
TEST_F(St20TxRlVrxDetectStream, EveryUnderflowPastTheDrainSlotIsFlagged) {
  int negative = 0;
  for (uint16_t nic_desc : {32, 512}) {
    const long double covered = nic_desc / kChainedSegs * kTrs;
    for (long double stall = 0; stall < covered + 3 * (kVrx + 1) * kTrs;
         stall += kTrs / 4) {
      ut_trs_rl_stat st;
      const long double late = Run(nic_desc, (uint64_t)stall, &st);
      const bool flagged = st.vrx_frames > 0;
      const std::string what =
          std::to_string(nic_desc) + " desc, stall " + std::to_string((double)stall);
      if (late > kVrx * kTrs) {
        negative++;
        EXPECT_TRUE(flagged) << what << ": late " << (double)late;
        EXPECT_EQ(0u, st.vrx_pkt0) << what;
      }
      if (flagged) {
        EXPECT_GE(late, kBudgetNs) << what;
      }
    }
  }
  EXPECT_GT(negative, 0);
}

/* The builder adds one bulk a scheduler round: a round past bulk trs drains the ring and
 * then the queue, and the pkts leave as they are built, later every round. */
TEST_F(St20TxRlVrxDetectStream, EveryBuilderDeficitPastTheDrainSlotIsFlagged) {
  constexpr uint32_t kBulk = 4;
  constexpr uint16_t kNicDesc = 32;
  int negative = 0;
  for (long double round = 0.9L * kBulk * kTrs; round < 1.2L * kBulk * kTrs;
       round += 0.01L * kBulk * kTrs) {
    ut_trs_ctx* ctx = ut_trs_create();
    ASSERT_NE(nullptr, ctx);
    ASSERT_EQ(0, ut_trs_setup_stream(ctx, kTrs, kNicDesc, kChainedSegs));
    ut_trs_set_vrx_detect(ctx, kPortP, kBudgetNs, 0);
    ut_trs_set_stream_target(ctx,
                             ut_trs_last_tsc(ctx) + (uint64_t)round - (uint64_t)kTrs);
    ut_trs_set_stream_build(ctx, kBulk);
    ut_trs_stream(ctx, 2 * kSteadyNs, (uint64_t)round);
    const ut_trs_rl_stat st = ut_trs_get_rl_stat(ctx);
    const long double late = ut_trs_stream_max_late_vs_target_ns(ctx);
    ut_trs_destroy(ctx);
    const std::string what = "round " + std::to_string((double)round);
    if (late > kVrx * kTrs) {
      negative++;
      EXPECT_EQ(1u, st.vrx_frames) << what << ": late " << (double)late;
    }
    if (st.vrx_frames) {
      EXPECT_GE(late, kBudgetNs) << what;
    }
  }
  EXPECT_GT(negative, 0);
}

TEST_F(St20TxRlVrxDetectStream, StallTheQueueCoversIsNotFlagged) {
  constexpr uint16_t kNicDesc = 512;
  ut_trs_rl_stat st;

  Run(kNicDesc, (uint64_t)((kNicDesc / kChainedSegs - 2) * kTrs), &st);
  EXPECT_EQ(0u, st.vrx_frames);
}

/* A frame whose room is far below 0: pkts queued well ahead of their targets are judged
 * and recorded by how far they are past target + room, never by a wrapped lateness. */
TEST_F(St20TxRlVrxDetectStream, AFrameBelowItsRoomRecordsItsLatenessPastTheRoom) {
  constexpr int32_t kRoomNs = -(int32_t)kBudgetNs - 1000000;
  ut_trs_ctx* ctx = ut_trs_create();
  ASSERT_NE(nullptr, ctx);
  ASSERT_EQ(0, ut_trs_setup_stream(ctx, kTrs, 512, kChainedSegs));
  ut_trs_set_vrx_detect(ctx, kPortP, kBudgetNs, 0);
  ut_trs_set_vrx_room(ctx, kPortP, kRoomNs);
  ut_trs_set_stream_target(ctx, ut_trs_last_tsc(ctx) + kSteadyNs);
  ut_trs_stream(ctx, kSteadyNs / 2, kCallNs);
  const ut_trs_rl_stat st = ut_trs_get_rl_stat(ctx);
  ut_trs_destroy(ctx);

  EXPECT_EQ(1u, st.vrx_frames);
  EXPECT_GT(st.vrx_late_max_ns, (uint64_t)kBudgetNs);
  EXPECT_LE(st.vrx_late_max_ns, (uint64_t)-kRoomNs);
}

class St20TxRlVrxDetectPkt0 : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
    ctx_ = ut_trs_create();
    ASSERT_NE(nullptr, ctx_);
    ut_trs_set_trs(ctx_, kTrs);
    ut_trs_set_vrx_detect(ctx_, kPortP, kBudgetNs, 0);
  }
  void TearDown() override {
    ut_trs_destroy(ctx_);
  }
  /* packet 0 reaches the transmitter late ns past its target */
  ut_trs_rl_stat DequeueLate(uint64_t late) {
    const uint64_t now = kTarget + late;
    ut_trs_set_mock_tsc_script(ctx_, &now, 1);
    ut_trs_enqueue_first_pkt(ctx_, kTarget);
    ut_trs_call_rl_tasklet(ctx_);
    EXPECT_EQ(1u, ut_trs_real_send_count(ctx_));
    return ut_trs_get_rl_stat(ctx_);
  }
  static constexpr uint64_t kTarget = 100000000;
  ut_trs_ctx* ctx_ = nullptr;
};

TEST_F(St20TxRlVrxDetectPkt0, DequeuedPastTheBudgetIsFlagged) {
  const ut_trs_rl_stat st = DequeueLate(kBudgetNs + 1000);

  EXPECT_EQ(1u, st.vrx_frames);
  EXPECT_EQ(1u, st.vrx_pkt0);
  EXPECT_GT(st.vrx_late_max_ns, (uint64_t)kBudgetNs);
}

/* The frame before's last pkt, refused at the frame boundary, is retried late after
 * packet 0 is dequeued: it counts the frame before, and a late packet 0 still counts its
 * own frame. */
TEST_F(St20TxRlVrxDetectPkt0, ALateRetriedTailOfTheFrameBeforeHidesNoFlag) {
  constexpr uint64_t kPrev = kTarget - 100000;
  ut_trs_set_hang_detect_thresh_ns(ctx_, UINT64_MAX);
  uint64_t now = kPrev;
  ut_trs_set_mock_tsc_script(ctx_, &now, 1);
  ut_trs_enqueue_first_pkt(ctx_, kPrev);
  ut_trs_call_rl_tasklet(ctx_);
  ASSERT_EQ(1u, ut_trs_real_send_count(ctx_));
  now = kTarget - 1000;
  ut_trs_set_mock_tsc_script(ctx_, &now, 1);
  ut_trs_enqueue_frame_boundary(ctx_, kTarget);
  ut_trs_set_burst_force_fail(ctx_, true);
  ut_trs_call_rl_tasklet(ctx_);
  ASSERT_EQ(1u, ut_trs_inflight_num2(ctx_));
  ut_trs_set_burst_force_fail(ctx_, false);
  now = kTarget + 3 * kBudgetNs;
  ut_trs_set_mock_tsc_script(ctx_, &now, 1);
  for (int call = 0; call < 4 && ut_trs_real_send_count(ctx_) < 3; call++)
    ut_trs_call_rl_tasklet(ctx_);
  ASSERT_EQ(3u, ut_trs_real_send_count(ctx_));
  const ut_trs_rl_stat st = ut_trs_get_rl_stat(ctx_);
  EXPECT_EQ(2u, st.vrx_frames);
  EXPECT_EQ(1u, st.vrx_pkt0);
}

TEST_F(St20TxRlVrxDetectPkt0, DequeuedWithinTheBudgetIsNotFlagged) {
  EXPECT_EQ(0u, DequeueLate(kBudgetNs - 1000).vrx_frames);
}

/* the scheduler first visits the warm-up window past the target, a late entry */
TEST_F(St20TxRlVrxDetectPkt0, WindowEnteredPastTheBudgetIsALateEntry) {
  const uint64_t now = kTarget - 1000000;
  ut_trs_set_warm_pkts_cap(ctx_, 64);
  ut_trs_set_drain(ctx_, kPortP, kTrs);
  ut_trs_set_credit(ctx_, kPortP, 2 * kTrs);
  ut_trs_set_mock_tsc_script(ctx_, &now, 1);
  ut_trs_enqueue_first_pkt(ctx_, kTarget);
  ut_trs_call_rl_tasklet(ctx_);
  ASSERT_EQ(0u, ut_trs_real_send_count(ctx_));

  ut_trs_advance_mock_tsc(ctx_, kTarget + kBudgetNs + 1000 - ut_trs_last_tsc(ctx_));
  ut_trs_call_rl_tasklet(ctx_);
  ASSERT_EQ(1u, ut_trs_real_send_count(ctx_));
  const ut_trs_rl_stat st = ut_trs_get_rl_stat(ctx_);
  EXPECT_EQ(1u, st.vrx_frames);
  EXPECT_EQ(1u, st.vrx_pkt0);
}

class St20TxRlVrxDetectSession : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_txv_init());
    ctx_ = ut_txv_create();
    ASSERT_NE(nullptr, ctx_);
    ut_txv_set_frame_time(ctx_, kFrameNs);
    ut_txv_set_max_onward_epochs(ctx_, 3);
    ut_txv_set_cur_epochs(ctx_, kTai / kFrameNs - 1);
    ut_txv_set_tr_offset(ctx_, kFrameNs / 1000);
    ut_txv_set_vrx(ctx_, 2);
    ut_txv_set_trs(ctx_, kFrameNs / 10000);
    ut_txv_set_mock_tsc_time(ctx_, kFrameNs / 2);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  static constexpr uint64_t kFrameNs = 1000000;
  static constexpr uint64_t kTai = 10 * kFrameNs;
  static constexpr int64_t kStartBudgetNs = kFrameNs / 100;
  ut_txv_ctx* ctx_ = nullptr;
};

/* the builder starts a frame whose packet 0 target already passed: it leaves late */
TEST_F(St20TxRlVrxDetectSession, FrameStartedPastItsTargetIsCounted) {
  ut_txv_set_port_pacing(ctx_, UT_TXV_PACING_RL, kFrameNs / 100);
  ut_txv_set_mock_ptp_time(ctx_, kTai + kFrameNs / 4);

  ASSERT_EQ(0, ut_txv_sync_pacing(ctx_, 0));
  EXPECT_EQ(1u, ut_txv_rl_start_late(ctx_));
}

/* a just-in-time start within the VRX budget is compliant: not counted, no err */
TEST_F(St20TxRlVrxDetectSession, FrameStartedWithinTheBudgetIsNotCounted) {
  for (uint64_t late_ns : {1000u, 5000u}) {
    SCOPED_TRACE(late_ns);
    if (late_ns != 1000u) {
      TearDown();
      SetUp();
    }
    ut_txv_set_port_pacing(ctx_, UT_TXV_PACING_RL, kStartBudgetNs);
    ut_txv_set_mock_ptp_time(ctx_, kTai + late_ns);
    ASSERT_EQ(0, ut_txv_sync_pacing(ctx_, 0));
    EXPECT_EQ(0u, ut_txv_rl_start_late(ctx_));
    int locked = 0;
    ut_txv_run_sessions_stat(ctx_, &locked);
    EXPECT_EQ(0, ut_txv_stat_err_lines()) << ut_txv_stat_error_log_line();
  }
}

TEST_F(St20TxRlVrxDetectSession, FrameStartedPastTheBudgetErrs) {
  ut_txv_set_port_pacing(ctx_, UT_TXV_PACING_RL, kStartBudgetNs);
  ut_txv_set_mock_ptp_time(ctx_, kTai + kStartBudgetNs + 20000);
  ASSERT_EQ(0, ut_txv_sync_pacing(ctx_, 0));
  EXPECT_EQ(1u, ut_txv_rl_start_late(ctx_));
  int locked = 0;
  ut_txv_run_sessions_stat(ctx_, &locked);
  EXPECT_EQ(1, ut_txv_stat_err_lines());
}

TEST_F(St20TxRlVrxDetectSession, FrameStartedOnTimeIsNotCounted) {
  ut_txv_set_mock_ptp_time(ctx_, kTai);

  ASSERT_EQ(0, ut_txv_sync_pacing(ctx_, 0));
  EXPECT_EQ(0u, ut_txv_rl_start_late(ctx_));
}

/* st22 sets its trs per frame from the codestream, larger for a small one, so a budget
 * from the trs at init would flag its frames. Like main it judges none and has no plan:
 * a frame started past the budget of a wide st20 session, whose vrx is as large, is not
 * counted. */
TEST(St20TxRlVrxDetectSt22, AWideSt22SessionJudgesNoFrame) {
  ASSERT_EQ(0, ut_txv_init());
  constexpr uint64_t kFrameNs = 1000000, kTai = 10 * kFrameNs;
  const float trained[1] = {164};
  int64_t st20_budget = 0;

  for (bool st22 : {false, true}) {
    SCOPED_TRACE(st22 ? "st22" : "st20");
    ut_txv_ctx* ctx = ut_txv_create();
    ASSERT_NE(nullptr, ctx);
    ut_txv_set_st22_rtp_level(ctx, st22);
    ASSERT_EQ(0, ut_txv_init_rl_pacing(ctx, 1, 0, false, trained, true));
    if (!st22) st20_budget = ut_txv_rl_vrx_budget(ctx, kPortP);
    ASSERT_GT(st20_budget, 0);
    EXPECT_EQ(st22 ? 0 : st20_budget, ut_txv_rl_vrx_budget(ctx, kPortP));
    EXPECT_FALSE(ut_txv_rl_armed(ctx, kPortP));
    EXPECT_EQ(0.0, (double)ut_txv_rl_absorb(ctx, kPortP));
    ut_txv_set_frame_time(ctx, kFrameNs);
    ut_txv_set_max_onward_epochs(ctx, 3);
    ut_txv_set_cur_epochs(ctx, kTai / kFrameNs - 1);
    ut_txv_set_tr_offset(ctx, kFrameNs / 1000);
    ut_txv_set_vrx(ctx, 2);
    ut_txv_set_trs(ctx, kFrameNs / 10000);
    ut_txv_set_mock_tsc_time(ctx, kFrameNs / 2);
    ut_txv_set_mock_ptp_time(ctx, kTai + st20_budget + 20000);
    ASSERT_EQ(0, ut_txv_sync_pacing(ctx, 0));
    EXPECT_EQ(st22 ? 0u : 1u, ut_txv_rl_start_late(ctx));
    ut_txv_destroy(ctx);
  }
}

/* one error line a dump period that names the session, where and how late */
TEST_F(St20TxRlVrxDetectSession, StatDumpReportsFlaggedFramesOnce) {
  int locked = 0;
  ut_txv_set_rl_vrx(ctx_, 3, 1, 47500);

  ut_txv_run_sessions_stat(ctx_, &locked);
  const std::string line = ut_txv_stat_error_log_line();
  EXPECT_NE(std::string::npos, line.find("TX_VIDEO_SESSION(")) << line;
  EXPECT_NE(std::string::npos, line.find("3 frames")) << line;
  EXPECT_NE(std::string::npos, line.find("packet 0 1 mid-frame 2")) << line;
  EXPECT_NE(std::string::npos, line.find("47.5us")) << line;

  ut_txv_run_sessions_stat(ctx_, &locked);
  EXPECT_STREQ("", ut_txv_stat_error_log_line());
}

}  // namespace
