/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Lip-synced ancillary sessions all send at one instant. One call of the TX ancillary
 * tasklet builds packets for at most 4 of them, and the next call starts at the session
 * after the last one it built, so the other tasklets of the scheduler run between the
 * parts of the burst and no session waits more than its turn.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St40TxTaskletBound*'
 */

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "mtl_sch_api.h"
#include "session/st40_tx_harness.h"

namespace {
constexpr int kSessions = 6;
constexpr uint64_t kPktTimeNs = 1000 * 1000 / 2; /* the harness frame: 1 ms, 2 packets */
using Built = std::array<uint64_t, kSessions>;
}  // namespace

class St40TxTaskletBoundTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txa_init(), 0);
    for (int i = 0; i < kSessions; i++) {
      ctx_[i] = ut_txa_create();
      ASSERT_NE(ctx_[i], nullptr);
      ASSERT_EQ(ut_txa_prepare_frame_tasklet(ctx_[i], ST10_TIMESTAMP_FMT_TAI, 0, 2), 0);
      if (i) {
        ASSERT_EQ(ut_txa_mgr_add(ctx_[0], i, ctx_[i]), 0);
      }
    }
  }

  /* ctx_[0] first: its manager ring holds the mbufs of every session pool */
  void TearDown() override {
    for (ut_txa_ctx* ctx : ctx_) ut_txa_destroy(ctx);
  }

  int step() {
    return ut_txa_step_frame_tasklet(ctx_[0]);
  }

  Built built() const {
    Built b{};
    for (int i = 0; i < kSessions; i++) b[i] = ut_txa_stat_port_build(ctx_[i]);
    return b;
  }

  std::array<ut_txa_ctx*, kSessions> ctx_{};
};

TEST_F(St40TxTaskletBoundTest, OneCallBuildsForFourSessionsAndTheNextResumes) {
  /* a session due within one loop pass returns HAS_PENDING with no build */
  ut_txa_set_sch_schedule_ns(ctx_[0], 2 * 1000 * 1000);
  step(); /* each session takes its frame and waits for the same epoch */
  ASSERT_EQ(built(), (Built{0, 0, 0, 0, 0, 0}));
  uint64_t epoch_tsc = ut_txa_tsc_time_cursor(ctx_[0]);
  ut_txa_set_mock_tsc_time(ctx_[0], epoch_tsc);

  EXPECT_EQ(step(), MTL_TASKLET_HAS_PENDING);
  EXPECT_EQ(built(), (Built{1, 1, 1, 1, 0, 0}));

  /* the second packet of sessions 0..3 is due now too, but sessions 4 and 5 go first */
  ut_txa_set_mock_tsc_time(ctx_[0], epoch_tsc + kPktTimeNs);
  EXPECT_EQ(step(), MTL_TASKLET_HAS_PENDING);
  EXPECT_EQ(built(), (Built{2, 2, 1, 1, 1, 1}));
}
