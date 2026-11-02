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

#include <cstring>
#include <string>

#include "session/st20_tx/rl_vrx_model.h"
#include "session/st20_tx_harness.h"
#include "session/st_video_transmitter_harness.h"

namespace {

struct Fmt {
  const char* name;
  uint32_t width, height;
};

constexpr int kPortP = 0;
constexpr int kSlots = 512;

class St20TxRlVrxDetectBudget : public ::testing::TestWithParam<Fmt> {};

/* A packet 0 that leaves the budget past its target keeps every drain slot at the worst
 * 90 kHz snap; 2 trs later it does not, the most the sawtooth bound takes. */
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
  EXPECT_GE(ut_rl_vrx(budget, tpr0, trs, p, pi, kSlots), 0) << f.name;
  EXPECT_LT(ut_rl_vrx(budget + 2 * trs, tpr0, trs, p, pi, kSlots), 0) << f.name;
}

INSTANTIATE_TEST_SUITE_P(Formats, St20TxRlVrxDetectBudget,
                         ::testing::Values(Fmt{"720p59", 1280, 720},
                                           Fmt{"1080p59", 1920, 1080},
                                           Fmt{"2160p59", 3840, 2160}),
                         [](const ::testing::TestParamInfo<Fmt>& info) {
                           return std::string(info.param.name);
                         });

/* 1080p59.94 GPM, a zero-copy pkt on 2 descriptors */
constexpr long double kTrs = 16683333.333L * 1080 / 1125 / 4320;
constexpr uint32_t kVrx = 5;
constexpr uint16_t kChainedSegs = 2;
constexpr uint64_t kCallNs = 500;
constexpr uint64_t kSteadyNs = 1000000;
/* a stream without pads: the drain slot (vrx + 1) trs past the target, less the
 * sawtooth bound tv_rl_plan_init() takes, (pad_interval / 2 + bulk) / (pad_interval + 1)
 * trs + a pad, here half a trs and one */
constexpr int64_t kBudgetNs = (int64_t)((kVrx + 1) * kTrs - 1.5L * kTrs);

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
 * slot, (vrx + 1) trs after its stamp, is in a flagged frame, and a flag means a pkt
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
      if (late > (kVrx + 1) * kTrs) {
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
    if (late > (kVrx + 1) * kTrs) {
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
