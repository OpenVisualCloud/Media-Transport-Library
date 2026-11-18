/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "session/st20_tx/rl_vrx_model.h"
#include "session/st20_tx_harness.h"
#include "session/st_video_transmitter_harness.h"
#include "st2110/st_tx_video_rl.h"

/* A first pad burst that still returns late on a reclaiming port (a cold cache, a
 * preemption inside it, a ring that does not reclaim): within the plan's absorb_ns the
 * pre-arm goes on from its plan, so packet 0 does not wait for a scheduler round. */
namespace {

constexpr int kPortP = 0;
constexpr uint64_t kTargetTsc = 100000000;
constexpr long double kBurstBytes = 2048;
constexpr int kRoundCap = 100000;
constexpr uint16_t kPadSize = 1262;          /* the harness's NORMAL pad */
constexpr long double kSpecNs = 10000;       /* the 0..+10 us launch budget */
constexpr long double kGridLateMaxNs = 4000; /* ST_TX_VIDEO_RL_GRID_LATE_MAX_NS */

struct Fmt {
  const char* name;
  uint32_t width, height;
  enum st_fps fps = ST_FPS_P59_94;
};

class St20TxRlAbsorb : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
  }

  void Plan(const Fmt& f, bool user_ts, uint16_t start_vrx = 0) {
    const float trained[1] = {164};
    ut_txv_ctx* txv = ut_txv_create();
    ASSERT_NE(nullptr, txv);
    ut_txv_set_rl_format(txv, f.width, f.height, false, f.fps);
    ut_txv_set_user_timestamp(txv, user_ts);
    ut_txv_set_start_vrx(txv, start_vrx);
    ASSERT_EQ(0, ut_txv_init_rl_pacing(txv, 1, 0, false, trained, false));
    ASSERT_TRUE(ut_txv_rl_armed(txv, kPortP)) << f.name;
    bps_ = ut_txv_rl_queue_bps(txv, kPortP);
    p_ = ut_txv_rl_drain(txv, kPortP);
    credit_ = ut_txv_rl_credit(txv, kPortP);
    absorb_ = ut_txv_rl_absorb(txv, kPortP);
    trs_ = ut_txv_pacing_trs(txv);
    warm_pkts_ = ut_txv_pacing_warm_pkts(txv);
    vrx_ = ut_txv_pacing_vrx(txv);
    tr_offset_ = ut_txv_pacing_tr_offset(txv);
    pad_interval_ = ut_txv_pad_interval(txv);
    ut_txv_destroy(txv);
    refill_ = kBurstBytes * 1e9L / bps_;
    aim_ = credit_ - refill_;
    fine_ = p_ * ST_TX_VIDEO_RL_TRIM_FINE / kPadSize;
  }

  /* packet 0's launch past its target: the first pad burst costs first_ns before its
   * doorbell, the other tasklets of a scheduler round others_ns */
  long double Late(uint64_t first_ns, uint64_t others_ns, uint64_t last_g_ns = 250,
                   bool held = false, int* visits = nullptr) {
    uint64_t now = kTargetTsc - (uint64_t)(warm_pkts_ * trs_);
    ut_trs_ctx* ctx = ut_trs_create();
    ut_trs_set_trs(ctx, trs_);
    ut_trs_set_warm_pkts_cap(ctx, warm_pkts_);
    ut_trs_set_drain(ctx, kPortP, p_);
    ut_trs_set_credit(ctx, kPortP, credit_);
    ut_trs_set_absorb(ctx, kPortP, held ? 0 : absorb_);
    ut_trs_set_shaper_bucket(ctx, kPortP, refill_);
    ut_trs_set_mock_tsc_script(ctx, &now, 1);
    const uint64_t d[2] = {first_ns, 200}, g[2] = {300, last_g_ns};
    ut_trs_set_burst_cost(ctx, d, g, 2);
    ut_trs_enqueue_first_pkt(ctx, kTargetTsc);
    int round = 0;
    for (; round < kRoundCap && !ut_trs_real_send_count(ctx); round++) {
      ut_trs_call_rl_tasklet(ctx);
      ut_trs_advance_mock_tsc(ctx, others_ns);
    }
    if (visits) *visits = round;
    mixed_ = ut_trs_mixed_calls(ctx);
    const long double late = ut_trs_modeled_launch_tsc(ctx, kPortP) - kTargetTsc;
    ut_trs_destroy(ctx);
    return late;
  }

  long double p_ = 0, credit_ = 0, absorb_ = 0, trs_ = 0, refill_ = 0, aim_ = 0,
              fine_ = 0;
  long double tr_offset_ = 0;
  float pad_interval_ = 0;
  uint64_t bps_ = 0;
  uint32_t warm_pkts_ = 0, vrx_ = 0;
  uint32_t mixed_ = 0; /* the last Late()'s calls of pads and packet 0 */
};

/* Packet 0 launched absorb_ns late, past the timeslot aim and a fine trim, still has
 * every packet of the frame in by its drain slot at the worse 90 kHz snap phase, within
 * the launch budget, a frame whose whole pads round up one included. A user vrx of 2 or
 * 3 leaves some leads short of the budget. */
TEST_F(St20TxRlAbsorb, AbsorbedLaunchKeepsVrx) {
  int short_of_spec = 0;
  for (const Fmt& f :
       {Fmt{"720p59", 1280, 720}, Fmt{"1080p59", 1920, 1080}, Fmt{"2160p59", 3840, 2160}})
    for (uint16_t start_vrx : {0, 2, 3}) {
      long double snapped = 0;
      for (bool user_ts : {false, true}) {
        ASSERT_NO_FATAL_FAILURE(Plan(f, user_ts, start_vrx));
        const std::string what = std::string(f.name) +
                                 (user_ts ? " user ts" : " snapped") + " vrx " +
                                 std::to_string(vrx_);
        EXPECT_LE(absorb_, kSpecNs) << what;
        if (absorb_ < kSpecNs) short_of_spec++;
        if (absorb_ <= 0) continue;
        const long double snap =
            user_ts ? 0 : ut_rl_snap_loss_5994(tr_offset_, vrx_, trs_);
        const long double tpr0 = kTargetTsc + vrx_ * trs_ - snap;
        const long double launch = kTargetTsc + aim_ + fine_ + absorb_;
        const int pkts = (int)(4 * pad_interval_);
        EXPECT_GE(ut_rl_vrx(launch, tpr0, trs_, p_, pad_interval_, pkts, 1), 0) << what;
        if (user_ts) {
          EXPECT_GE(absorb_, snapped) << what;
        } else {
          snapped = absorb_;
        }
      }
    }
  EXPECT_GT(short_of_spec, 0);
}

/* On an idle scheduler, where a held packet 0 would leave within the launch budget, a
 * stalled pad burst lands it within that budget too, the timeslot grid's included. */
TEST_F(St20TxRlAbsorb, StalledBurstKeepsPacket0WithinTheLaunchBudget) {
  for (const Fmt& f : {Fmt{"1080p25", 1920, 1080, ST_FPS_P25}, Fmt{"720p59", 1280, 720},
                       Fmt{"1080p59", 1920, 1080}, Fmt{"2160p59", 3840, 2160}}) {
    ASSERT_NO_FATAL_FAILURE(Plan(f, true));
    const long double timeslot = st_tx_video_rl_timeslot_tokens(bps_) * p_ / kPadSize;
    int checked = 0;
    for (uint64_t first = ut_trs_rl_stall_ns() + 500; first <= kSpecNs; first += 500) {
      if (Late(first, 250, 250, true) + timeslot > kGridLateMaxNs) continue;
      const long double late = Late(first, 250);
      const std::string what =
          std::string(f.name) + " first burst " + std::to_string(first);
      EXPECT_GE(late, 0) << what;
      EXPECT_LE(late + timeslot, kGridLateMaxNs) << what;
      checked++;
    }
    EXPECT_GT(checked, 0) << f.name;
  }
}

TEST_F(St20TxRlAbsorb, StallWithinAbsorbDoesNotWaitForTheRound) {
  ASSERT_NO_FATAL_FAILURE(Plan(Fmt{"1080p59", 1920, 1080}, false));
  const uint64_t stall = ut_trs_rl_stall_ns();
  ASSERT_GT(absorb_, stall);
  for (long double k : {0.25L, 0.5L, 0.9L}) {
    const uint64_t first = stall + llroundl(k * (absorb_ - stall));
    const long double late = Late(first, 12000);
    const std::string what = "first burst " + std::to_string(first);
    EXPECT_GE(late, 0) << what;
    EXPECT_LE(late, first + aim_ + fine_) << what;
    /* the round moves the window entry, so the cut, by up to a fine trim */
    for (uint64_t others : {26000, 60000})
      EXPECT_NEAR(double(late), double(Late(first, others)), double(fine_))
          << what << " others " << others;
  }
}

/* The tasklet carried past the target after the pre-arm's last doorbell (here the work
 * after its last pad burst) sends packet 0 in that visit, as the release scan does, not a
 * scheduler round later. */
TEST_F(St20TxRlAbsorb, VisitPastTheTargetSendsPacket0) {
  ASSERT_NO_FATAL_FAILURE(Plan(Fmt{"1080p59", 1920, 1080}, false));
  const uint64_t first = ut_trs_rl_stall_ns() + 100;
  ASSERT_LE(first, absorb_);
  const uint64_t past = (uint64_t)(warm_pkts_ * trs_);
  int visits = 0, held_visits = 0;
  const long double late = Late(first, 60000, past, false, &visits);
  ASSERT_EQ(0u, mixed_);
  const long double held = Late(first, 60000, past, true, &held_visits);
  EXPECT_EQ(held_visits, visits);
  EXPECT_GE(late, 0);
  EXPECT_LE(late, held + fine_);
}

}  // namespace
