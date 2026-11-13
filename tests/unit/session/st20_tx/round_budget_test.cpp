/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <string>

#include "session/st20_tx_harness.h"

/* A TX video session keeps up with its frames while its builder, one bulk a scheduler
 * visit, stays ahead of the wire: the round averages at most bulk * frame_time / pkts,
 * and the lead the builder measures at its visits, the next pkt's due time less now,
 * never falls below 0. Past either the stats dump errs. Only a session that sent a frame
 * or measured a lead in the period has a budget; a sleeping scheduler's round is not
 * judged, its leads are.
 */
namespace {

struct Fmt {
  const char* name;
  uint32_t width, height;
  enum st_fps fps;
};

class St20TxRoundBudget : public ::testing::TestWithParam<Fmt> {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_txv_init());
    ctx_ = ut_txv_create();
    ASSERT_NE(nullptr, ctx_);
    const float trained[1] = {164};
    ut_txv_set_rl_format(ctx_, GetParam().width, GetParam().height, false,
                         GetParam().fps);
    ASSERT_EQ(0, ut_txv_init_rl_pacing(ctx_, 1, 0, false, trained, false));
    pkts_ = ut_txv_total_pkts(ctx_);
    bulk_ = ut_txv_bulk(ctx_);
    trs_ = ut_txv_pacing_trs(ctx_);
    budget_ = bulk_ * ut_txv_pacing_frame_time(ctx_) / pkts_;
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }

  static void SendAFrame(ut_txv_ctx* ctx) {
    ut_txv_set_stat_port_frames(ctx, ut_txv_stat_snapshot_port_frames(ctx) + 1);
  }

  int StatErrs(uint64_t round_ns, int64_t lead_ns) {
    int locked_lines = 0;
    ut_txv_set_sch_avg_loop_ns(ctx_, round_ns);
    ut_txv_set_lead_ns(ctx_, lead_ns);
    SendAFrame(ctx_);
    ut_txv_run_sessions_stat(ctx_, &locked_lines);
    return ut_txv_stat_err_lines();
  }

  ut_txv_ctx* ctx_ = nullptr;
  long double pkts_ = 0, bulk_ = 0, trs_ = 0, budget_ = 0;
};

/* the line names only what fired */
TEST_P(St20TxRoundBudget, StatDumpErrsOnARoundPastTheBudget) {
  ASSERT_EQ(1, StatErrs((uint64_t)ceill(budget_) + 1, INT64_MAX)) << GetParam().name;
  EXPECT_NE(nullptr, std::strstr(ut_txv_stat_error_log_line(), "round"))
      << ut_txv_stat_error_log_line();
  EXPECT_EQ(nullptr, std::strstr(ut_txv_stat_error_log_line(), "lead"))
      << ut_txv_stat_error_log_line();
}

/* a pkt met the wire unbuilt, which the frame-average round hides */
TEST_P(St20TxRoundBudget, StatDumpErrsOnALeadBelowZero) {
  const uint64_t round = (uint64_t)floorl(bulk_ * trs_);
  ASSERT_EQ(1, StatErrs(round, -1)) << GetParam().name;
  EXPECT_NE(nullptr, std::strstr(ut_txv_stat_error_log_line(), "fall behind"))
      << ut_txv_stat_error_log_line();
  int locked_lines = 0;
  ut_txv_run_sessions_stat(ctx_, &locked_lines);
  EXPECT_EQ(0, ut_txv_stat_err_lines()) << "the dump takes the lead once";
  EXPECT_EQ(INT64_MAX, ut_txv_lead_ns(ctx_));
}

TEST_P(St20TxRoundBudget, StatDumpIsQuietWithinTheBudgetAndALeadOfZero) {
  EXPECT_EQ(0, StatErrs((uint64_t)floorl(budget_), 0)) << GetParam().name;
}

/* a session freed or migrated before the dump takes its lead along: it errs then */
TEST_P(St20TxRoundBudget, ASessionFreedBehindErrs) {
  ut_txv_set_lead_ns(ctx_, -1);
  EXPECT_EQ(1, ut_txv_run_detach(ctx_)) << GetParam().name;
  EXPECT_NE(nullptr, std::strstr(ut_txv_stat_error_log_line(), "fall behind"))
      << ut_txv_stat_error_log_line();
}

TEST_P(St20TxRoundBudget, ASessionFreedOnTimeIsQuiet) {
  ut_txv_set_lead_ns(ctx_, 0);
  EXPECT_EQ(0, ut_txv_run_detach(ctx_)) << GetParam().name;
}

TEST_P(St20TxRoundBudget, ASessionMigratedBehindErrsOnTheSchedulerItLeaves) {
  ut_txv_set_lead_ns(ctx_, -1);
  EXPECT_EQ(1, ut_txv_run_migrate(ctx_)) << GetParam().name;
  EXPECT_NE(nullptr, std::strstr(ut_txv_stat_error_log_line(), "TX_VIDEO_SESSION(0)"))
      << ut_txv_stat_error_log_line();
  EXPECT_EQ(INT64_MAX, ut_txv_lead_ns(ctx_));
}

TEST_P(St20TxRoundBudget, ASessionMigratedOnTimeIsQuiet) {
  ut_txv_set_lead_ns(ctx_, 0);
  EXPECT_EQ(0, ut_txv_run_migrate(ctx_)) << GetParam().name;
}

/* a sleeping scheduler's loop avg holds its sleeps, a pkt met the wire unbuilt all the
 * same */
TEST_P(St20TxRoundBudget, SleepingSchedulerErrsOnALeadBelowZero) {
  ut_txv_set_sch_allow_sleep(ctx_, true);
  EXPECT_EQ(1, StatErrs(0, -1)) << GetParam().name;
}

TEST_P(St20TxRoundBudget, SleepingSchedulerJudgesNoRound) {
  ut_txv_set_sch_allow_sleep(ctx_, true);
  EXPECT_EQ(0, StatErrs((uint64_t)ceill(budget_) + 1, INT64_MAX)) << GetParam().name;
}

/* a session the app sends nothing to sets no budget */
TEST_P(St20TxRoundBudget, IdleSessionSetsNoBudget) {
  int locked_lines = 0;
  ut_txv_set_sch_avg_loop_ns(ctx_, (uint64_t)ceill(budget_) + 1);
  ut_txv_set_lead_ns(ctx_, INT64_MAX);
  ut_txv_run_sessions_stat(ctx_, &locked_lines);
  EXPECT_EQ(0, ut_txv_stat_err_lines()) << GetParam().name;
}

/* a session measured a lead, so it streamed: its budget counts without a frame ended */
TEST_P(St20TxRoundBudget, SessionThatMeasuredALeadSetsItsBudget) {
  int locked_lines = 0;
  ut_txv_set_sch_avg_loop_ns(ctx_, (uint64_t)ceill(budget_) + 1);
  ut_txv_set_lead_ns(ctx_, 0);
  ut_txv_run_sessions_stat(ctx_, &locked_lines);
  EXPECT_EQ(1, ut_txv_stat_err_lines()) << GetParam().name;
}

/* An idle 2160p session beside a 1080p one at a round within the 1080p budget only: the
 * 1080p one keeps up. Past the 1080p budget it falls behind. */
TEST(St20TxRoundBudgetBeside, AnIdleSessionDoesNotJudgeABusyOne) {
  ASSERT_EQ(0, ut_txv_init());
  const float trained[1] = {164};
  ut_txv_ctx* busy = ut_txv_create();
  ut_txv_ctx* idle = ut_txv_create();
  ASSERT_NE(nullptr, busy);
  ASSERT_NE(nullptr, idle);
  ut_txv_set_rl_format(busy, 1920, 1080, false, ST_FPS_P59_94);
  ASSERT_EQ(0, ut_txv_init_rl_pacing(busy, 1, 0, false, trained, false));
  ut_txv_set_rl_format(idle, 3840, 2160, false, ST_FPS_P59_94);
  ASSERT_EQ(0, ut_txv_init_rl_pacing(idle, 1, 0, false, trained, false));
  auto budget = [](ut_txv_ctx* ctx) {
    return ut_txv_bulk(ctx) * ut_txv_pacing_frame_time(ctx) / ut_txv_total_pkts(ctx);
  };
  ASSERT_LT(budget(idle), budget(busy));
  ut_txv_set_mgr_neighbour(busy, idle);
  ut_txv_set_lead_ns(idle, INT64_MAX);
  int locked_lines = 0;
  for (uint64_t round :
       {(uint64_t)floorl(budget(busy)), (uint64_t)ceill(budget(busy)) + 1}) {
    ut_txv_set_sch_avg_loop_ns(busy, round);
    ut_txv_set_lead_ns(busy, 0);
    ut_txv_set_stat_port_frames(busy, ut_txv_stat_snapshot_port_frames(busy) + 1);
    ut_txv_run_sessions_stat(busy, &locked_lines);
    EXPECT_EQ(round > budget(busy) ? 1 : 0, ut_txv_stat_err_lines()) << "at " << round;
  }
  ut_txv_set_mgr_neighbour(busy, nullptr);
  ut_txv_destroy(idle);
  ut_txv_destroy(busy);
}

/* the lead of the next pkt, measured at a visit inside a frame only */
TEST(St20TxLead, TheFrameBuilderMeasuresTheLeadOfItsNextPkt) {
  ASSERT_EQ(0, ut_txv_init());
  ut_txv_ctx* ctx = ut_txv_create();
  ASSERT_NE(nullptr, ctx);
  ut_txv_set_mock_tsc_time(ctx, 1000000);
  EXPECT_EQ(INT64_MAX, ut_txv_run_frame_visit(ctx, 0, -500)) << "at the frame's start";
  EXPECT_EQ(700, ut_txv_run_frame_visit(ctx, 1, 700));
  EXPECT_EQ(-300, ut_txv_run_frame_visit(ctx, 1, -300));
  EXPECT_EQ(-300, ut_txv_run_frame_visit(ctx, 1, 200)) << "the shortest";
  ut_txv_destroy(ctx);
}

/* Once a slice-level frame waited for the app's lines, its pkts are due before they are
 * built, so it measures no lead until its end; a slice frame that never waited does. */
TEST(St20TxLead, ASliceFrameThatWaitedForItsLinesMeasuresNoLead) {
  ASSERT_EQ(0, ut_txv_init());
  ut_txv_ctx* ctx = ut_txv_create();
  ASSERT_NE(nullptr, ctx);
  ut_txv_set_mock_tsc_time(ctx, 1000000);
  ut_txv_set_visit_slice_lines(ctx, 1);
  EXPECT_EQ(-300, ut_txv_run_frame_visit(ctx, 1, -300)) << "lines ready, behind";
  ut_txv_set_lead_ns(ctx, INT64_MAX);
  ut_txv_set_visit_slice_lines(ctx, 0);
  EXPECT_EQ(INT64_MAX, ut_txv_run_frame_visit(ctx, 1, -300)) << "lines not ready";
  ut_txv_set_visit_slice_lines(ctx, 1);
  EXPECT_EQ(INT64_MAX, ut_txv_run_frame_visit(ctx, 1, -200000)) << "after the wait";
  EXPECT_EQ(-300, ut_txv_run_frame_visit(ctx, 1, -300)) << "the next frame, behind";
  int locked_lines = 0;
  ut_txv_run_sessions_stat(ctx, &locked_lines);
  EXPECT_EQ(1, ut_txv_stat_err_lines()) << "a slice session behind on an overload";
  ut_txv_destroy(ctx);
}

constexpr Fmt kFormats[] = {
    {"720p59", 1280, 720, ST_FPS_P59_94},   {"1080p59", 1920, 1080, ST_FPS_P59_94},
    {"2160p59", 3840, 2160, ST_FPS_P59_94}, {"1080p50", 1920, 1080, ST_FPS_P50},
    {"1080p25", 1920, 1080, ST_FPS_P25},
};
std::string FmtName(const ::testing::TestParamInfo<Fmt>& info) {
  return info.param.name;
}
INSTANTIATE_TEST_SUITE_P(Formats, St20TxRoundBudget, ::testing::ValuesIn(kFormats),
                         FmtName);

}  // namespace
