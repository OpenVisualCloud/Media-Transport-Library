/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "session/st20_tx/rl_vrx_model.h"
#include "session/st20_tx_harness.h"
#include "session/st_video_transmitter_harness.h"
#include "st2110/st_tx_video_rl.h"

/* The first pad burst of a pre-arm into an idle shaper is the anchor: the fewest pads
 * that keep the shaper draining through a timer tick in the burst behind it. An anchor
 * that returns late tells nothing of when the shaper started, so the pre-arm starts over
 * once its pads have drained, and packet 0 is held only when no pre-arm fits any more. */
namespace {

constexpr int kPortP = 0;
constexpr uint64_t kTargetTsc = 100000000;
constexpr long double kBurstBytes = 2048;
/* ST_TX_VIDEO_RL_TICK_NS + ST_TX_VIDEO_RL_STALL_NS: a tick and a healthy burst */
constexpr long double kCoverNs = 12000;
constexpr uint64_t kOthersNs = 12000; /* the other tasklets of a scheduler round */
constexpr int kRoundCap = 100000;
constexpr uint16_t kPadSize = 1262; /* the harness's NORMAL pad */
constexpr int kSlots = 512;
/* a pad burst's cost before and after its doorbell, the anchor's first */
constexpr uint64_t kBurstD[2] = {750, 200}, kBurstG[2] = {300, 250};

struct Fmt {
  const char* name;
  uint32_t width, height;
  enum st_fps fps = ST_FPS_P59_94;
  bool interlaced = false;
  enum st20_fmt fmt = ST20_FMT_YUV_422_10BIT;
  enum st20_packing packing = ST20_PACKING_MAX; /* MAX: the harness's own layout */
};

const Fmt kFormats[] = {
    {"720p50", 1280, 720, ST_FPS_P50},
    {"720p59", 1280, 720},
    {"1080p29", 1920, 1080, ST_FPS_P29_97},
    {"1080p59", 1920, 1080},
    {"2160p59", 3840, 2160},
    {"4320p59", 7680, 4320},
};

/* packet 0 of one frame */
struct Pkt0 {
  long double late = 0;   /* launch - target */
  long double queued = 0; /* the return of its call - target: held when >= 0 */
  uint32_t pads = 0, stalls = 0;
  int vrx = 0;
  bool flagged = false;
  bool leak = false; /* pads left to retry, or a pad reference not given back */
};

class St20TxRlAnchor : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
  }

  void Plan(const Fmt& f) {
    const float trained[1] = {164};
    ut_txv_ctx* txv = ut_txv_create();
    ASSERT_NE(nullptr, txv);
    ut_txv_set_rl_format(txv, f.width, f.height, f.interlaced, f.fps);
    if (f.fmt != ST20_FMT_YUV_422_10BIT || f.packing != ST20_PACKING_MAX)
      ut_txv_set_rl_layout(
          txv, f.packing == ST20_PACKING_MAX ? ST20_PACKING_GPM : f.packing, f.fmt, 512);
    ASSERT_EQ(0, ut_txv_init_rl_pacing(txv, 1, 0, false, trained, false));
    ASSERT_TRUE(ut_txv_rl_armed(txv, kPortP)) << f.name;
    p_ = ut_txv_rl_drain(txv, kPortP);
    plan_pad_ = ut_txv_rl_pad_size(txv, kPortP);
    bps_ = ut_txv_rl_queue_bps(txv, kPortP);
    credit_ = ut_txv_rl_credit(txv, kPortP);
    absorb_ = ut_txv_rl_absorb(txv, kPortP);
    plan_anchor_ = ut_txv_rl_anchor_pads(txv, kPortP);
    budget_ = ut_txv_rl_vrx_budget(txv, kPortP);
    refill_ = kBurstBytes * 1e9L / ut_txv_rl_queue_bps(txv, kPortP);
    trs_ = ut_txv_pacing_trs(txv);
    warm_pkts_ = ut_txv_pacing_warm_pkts(txv);
    pad_interval_ = ut_txv_pad_interval(txv);
    const uint32_t vrx = ut_txv_pacing_vrx(txv);
    struct st_fps_timing fps_tm;
    ASSERT_EQ(0, st_get_fps_timing(f.fps, &fps_tm));
    tpr0_ =
        kTargetTsc + vrx * trs_ -
        ut_rl_snap_loss(fps_tm.mul, fps_tm.den, ut_txv_pacing_tr_offset(txv), vrx, trs_);
    ut_txv_destroy(txv);
    aim_ = credit_ - refill_;
    fine_ = p_ * ST_TX_VIDEO_RL_TRIM_FINE / kPadSize;
    window_ = (uint64_t)(warm_pkts_ * trs_);
    trim_pads_ = st_tx_video_rl_trim_pads(kPadSize);
    anchor_ = (uint32_t)std::min<long double>(ceill((credit_ + kCoverNs) / p_),
                                              ST_TX_VIDEO_RL_MAX_PADS_PER_BURST);
  }

  ut_trs_ctx* Session(const uint64_t* now, uint64_t target_tsc) {
    ut_trs_ctx* ctx = ut_trs_create();
    ut_trs_set_trs(ctx, trs_);
    ut_trs_set_warm_pkts_cap(ctx, warm_pkts_);
    ut_trs_set_drain(ctx, kPortP, p_);
    ut_trs_set_credit(ctx, kPortP, credit_);
    ut_trs_set_absorb(ctx, kPortP, absorb_);
    ut_trs_set_vrx_detect(ctx, kPortP, budget_, refill_);
    ut_trs_set_shaper_bucket(ctx, kPortP, refill_);
    ut_trs_set_mock_tsc_script(ctx, now, 1);
    ut_trs_set_burst_cost(ctx, kBurstD, kBurstG, 2);
    ut_trs_enqueue_first_pkt(ctx, target_tsc);
    return ctx;
  }

  Pkt0 Result(ut_trs_ctx* ctx) const {
    Pkt0 r;
    const long double launch = ut_trs_modeled_launch_tsc(ctx, kPortP);
    r.late = launch - kTargetTsc;
    r.queued = (long double)ut_trs_last_real_send_tsc(ctx) - kTargetTsc;
    r.pads = ut_trs_pad_send_count(ctx);
    r.stalls = (uint32_t)ut_trs_stat_recalculate_warmup(ctx);
    r.vrx = ut_rl_vrx(launch, tpr0_, trs_, p_, pad_interval_, kSlots);
    r.flagged = ut_trs_get_rl_stat(ctx).vrx_frames > 0;
    r.leak = ut_trs_pad_inflight_num(ctx) || ut_trs_pad_refcnt(ctx) != 1 ||
             ut_trs_trim_pad_refcnt(ctx, 0) != 1 || ut_trs_trim_pad_refcnt(ctx, 1) != 1;
    return r;
  }

  /* One frame whose first visit in the window comes entry_ns past its open, the nth
   * call carrying pads ahead of packet 0 stalling stall_ns, 0 for none; the other
   * tasklets of a round take others_ns. nic(ctx, visit) sets the NIC up ahead of each
   * visit, the pre-arm's being visit 1. */
  Pkt0 Frame(uint32_t nth, uint64_t stall_ns, bool after_doorbell,
             uint64_t others_ns = kOthersNs, uint64_t entry_ns = 0,
             const std::function<void(ut_trs_ctx*, int)>& nic = nullptr) {
    uint64_t now = kTargetTsc - window_ + entry_ns - others_ns;
    ut_trs_ctx* ctx = Session(&now, kTargetTsc);
    if (stall_ns) ut_trs_set_frame_burst_stall(ctx, nth, stall_ns, after_doorbell);
    for (int round = 0; round < kRoundCap && !ut_trs_real_send_count(ctx); round++) {
      if (nic) nic(ctx, round);
      ut_trs_call_rl_tasklet(ctx);
      ut_trs_advance_mock_tsc(ctx, others_ns);
    }
    const Pkt0 r = Result(ctx);
    ut_trs_destroy(ctx);
    return r;
  }

  long double p_ = 0, credit_ = 0, absorb_ = 0, trs_ = 0, refill_ = 0, aim_ = 0;
  long double fine_ = 0, tpr0_ = 0, pad_interval_ = 0;
  int64_t budget_ = 0, trim_pads_ = 0;
  uint64_t window_ = 0, bps_ = 0;
  uint16_t plan_pad_ = 0; /* the plan's NORMAL pad, kPadSize the harness's */
  uint32_t warm_pkts_ = 0, anchor_ = 0;
  uint32_t plan_anchor_ = 0; /* what tv_rl_plan_init() sized, anchor_ what it should */
};

/* The anchor keeps the shaper draining for its bucket, a tick and a healthy burst, and is
 * no larger: one pad fewer would not. At 4320p a full burst is still short of that. */
TEST_F(St20TxRlAnchor, FirstBurstIsThePadsOfTheBucketAndATick) {
  for (const Fmt& f : kFormats) {
    ASSERT_NO_FATAL_FAILURE(Plan(f));
    uint64_t now = kTargetTsc - window_;
    ut_trs_ctx* ctx = Session(&now, kTargetTsc);
    ut_trs_set_target_tsc(ctx, kTargetTsc);
    ut_trs_set_burst_refuse(ctx, 2, 1000);
    ASSERT_TRUE(ut_trs_pre_arm(ctx)) << f.name;
    const uint32_t first = ut_trs_pad_send_count(ctx);
    ut_trs_destroy(ctx);
    EXPECT_EQ(anchor_, first) << f.name;
    EXPECT_EQ(anchor_, plan_anchor_) << f.name;
    EXPECT_LT((first - 1) * p_, credit_ + kCoverNs) << f.name;
    if (first < ST_TX_VIDEO_RL_MAX_PADS_PER_BURST) {
      EXPECT_GE(first * p_, credit_ + kCoverNs) << f.name;
    }
  }
}

/* A tick in the burst behind the anchor, before or after its doorbell, finds the shaper
 * still draining the anchor: no stall is counted and packet 0 leaves as on a frame with
 * none, within the fine trim its later sizing read moves the cut. At 4320p the anchor is
 * a full burst, 7.4 us of a 30 us window: a 10 us tick behind it is a counted stall that
 * leaves too few pads to the target, and packet 0 is held for the visit past it. */
TEST_F(St20TxRlAnchor, ATickInTheBurstBehindTheAnchorChangesNothing) {
  for (const Fmt& f : kFormats) {
    ASSERT_NO_FATAL_FAILURE(Plan(f));
    const bool full = anchor_ == ST_TX_VIDEO_RL_MAX_PADS_PER_BURST;
    const Pkt0 plain = Frame(0, 0, false);
    ASSERT_LT(plain.queued, 0) << f.name;
    for (bool after : {false, true})
      for (uint64_t tick : {2500, 5000, 10000}) {
        const std::string what = std::string(f.name) + " tick " + std::to_string(tick) +
                                 (after ? " after" : " before") + " the doorbell";
        const Pkt0 r = Frame(2, tick, after);
        if (full && tick > anchor_ * p_) {
          EXPECT_GT(r.stalls, 0u) << what;
          EXPECT_GE(r.queued, 0) << what;
          EXPECT_LT(r.queued, kOthersNs + kBurstD[0] + kBurstG[0]) << what;
          continue;
        }
        EXPECT_LT(r.queued, 0) << what;
        EXPECT_EQ(plain.pads, r.pads) << what;
        EXPECT_EQ(0u, r.stalls) << what;
        EXPECT_NEAR((double)plain.late, (double)r.late, (double)fine_) << what;
      }
  }
}

/* An anchor stalled 2.5 to 30 us past absorb_ns, before or after its doorbell, with
 * rounds of 2, 12 or 26 us: the pre-arm starts over behind it, and packet 0 goes behind
 * the new pads, never early, late by no more than a frame on its plan may be (the aim, a
 * fine trim and an uncounted gap), whatever the stall. The frame keeps VRX >= 0 and is
 * not flagged. */
TEST_F(St20TxRlAnchor, StalledAnchorStartsOverAndPacket0LeavesOnThePlan) {
  for (const Fmt& f : kFormats) {
    ASSERT_NO_FATAL_FAILURE(Plan(f));
    /* a full first burst leaves no room to start over in a 30 us window */
    if (anchor_ == ST_TX_VIDEO_RL_MAX_PADS_PER_BURST) continue;
    for (bool after : {false, true})
      /* the last one is just past absorb_ns, where that is a counted stall */
      for (uint64_t stall : {(uint64_t)2500, (uint64_t)6000, (uint64_t)10000,
                             (uint64_t)30000, (uint64_t)absorb_ - kBurstD[0] + 150})
        for (uint64_t others : {2000, 12000, 26000}) {
          if (stall + kBurstD[0] <= absorb_ || stall > 30000 ||
              stall + kBurstD[0] <= ut_trs_rl_stall_ns() + 20)
            continue;
          const std::string what = std::string(f.name) + " stall " +
                                   std::to_string(stall) +
                                   (after ? " after" : " before") +
                                   " the doorbell, rounds of " + std::to_string(others);
          const Pkt0 r = Frame(1, stall, after, others);
          EXPECT_EQ(1u, r.stalls) << what;
          EXPECT_LT(r.queued, 0) << what << ": held";
          EXPECT_GE(r.late, 0) << what;
          EXPECT_LE(r.late, aim_ + fine_ + ut_trs_rl_stall_ns()) << what;
          EXPECT_GE(r.vrx, 0) << what;
          EXPECT_FALSE(r.flagged) << what;
          if (HasFailure()) return;
        }
  }
}

/* The anchor stalls 6 us, and so does the anchor of the pre-arm that starts over: it
 * starts over again, and packet 0 leaves on the plan of the third. */
TEST_F(St20TxRlAnchor, ASecondStalledAnchorStartsOverAgain) {
  for (const Fmt& f : kFormats) {
    ASSERT_NO_FATAL_FAILURE(Plan(f));
    if (anchor_ == ST_TX_VIDEO_RL_MAX_PADS_PER_BURST) continue;
    const Pkt0 r = Frame(0, 0, false, kOthersNs, 0, [](ut_trs_ctx* ctx, int visit) {
      uint64_t d[64], g[64];
      for (int j = 0; j < 64; j++) {
        d[j] = kBurstD[j % 2] + (j < 2 ? 6000 : 0);
        g[j] = kBurstG[j % 2];
      }
      if (!visit) ut_trs_set_burst_cost(ctx, d, g, 64);
    });
    EXPECT_EQ(2u, r.stalls) << f.name;
    EXPECT_LT(r.queued, 0) << f.name;
    EXPECT_GE(r.late, 0) << f.name;
    EXPECT_LE(r.late, aim_ + fine_ + ut_trs_rl_stall_ns()) << f.name;
    EXPECT_FALSE(r.flagged) << f.name;
  }
}

/* At 4320p the anchor is a full burst and the window 30 us. Stalled 6 us at the window's
 * open, the pre-arm still starts over and packet 0 leaves on its plan; stalled 12 us
 * into it, no pre-arm fits behind, and packet 0 is held for the visit past its target. */
TEST_F(St20TxRlAnchor, AFullBurstAnchorStartsOverWhereItsWindowAllows) {
  ASSERT_NO_FATAL_FAILURE(Plan(Fmt{"4320p59", 7680, 4320}));
  ASSERT_EQ((uint32_t)ST_TX_VIDEO_RL_MAX_PADS_PER_BURST, anchor_);
  ASSERT_GT(6000 + kBurstD[0], absorb_);
  const Pkt0 fits = Frame(1, 6000, false, 2000);
  EXPECT_EQ(1u, fits.stalls);
  EXPECT_LT(fits.queued, 0);
  EXPECT_GE(fits.late, 0);
  EXPECT_LE(fits.late, aim_ + fine_ + ut_trs_rl_stall_ns());
  const Pkt0 held = Frame(1, 6000, false, kOthersNs, 12000);
  EXPECT_EQ(1u, held.stalls);
  EXPECT_GE(held.queued, 0);
  EXPECT_LT(held.queued, kOthersNs + kBurstD[0] + kBurstG[0]);
}

/* While a port waits to start over, its visits free the done batch that is due, as its
 * visits before the window do. The anchor of the pre-arm that starts over then carries
 * no batch free ahead of its doorbell: one of 2 us would make it a counted stall. */
TEST_F(St20TxRlAnchor, WaitingToStartOverFreesTheDueBatch) {
  ASSERT_NO_FATAL_FAILURE(Plan(Fmt{"1080p59", 1920, 1080}));
  const Pkt0 r = Frame(1, 6000, false, 2000, 0, [](ut_trs_ctx* ctx, int visit) {
    if (visit == 2)
      ut_trs_set_pmd_done_batches(ctx, kUtRlRsThresh, ut_trs_rl_kept_batches() + 1,
                                  ut_trs_rl_stall_ns());
  });
  EXPECT_EQ(1u, r.stalls);
  EXPECT_LT(r.queued, 0);
  EXPECT_LE(r.late, aim_ + fine_ + ut_trs_rl_stall_ns());
}

/* A stall within absorb_ns, what the TX share of the launch budget leaves a pad burst,
 * keeps the plan: packet 0 goes behind its pads in that visit, late by no more than the
 * gap, and waits for no later visit, so a scheduler then away past the target changes
 * nothing. The frame keeps VRX >= 0 and is not flagged, at the limit too. */
TEST_F(St20TxRlAnchor, StallWithinAbsorbKeepsThePlanInThatVisit) {
  int kept = 0;
  for (const Fmt& f : kFormats) {
    ASSERT_NO_FATAL_FAILURE(Plan(f));
    /* the harness's tsc reads put the gap a few ns past the stall and the cost */
    const uint64_t limit = (uint64_t)std::max<long double>(0, absorb_ - kBurstD[0] - 20);
    for (bool after : {false, true})
      for (uint64_t stall :
           {(uint64_t)1500, (uint64_t)2000, (uint64_t)2500, (uint64_t)3000, limit}) {
        const long double gap = stall + kBurstD[0];
        if (gap + 20 > absorb_ || gap <= ut_trs_rl_stall_ns() + 20) continue;
        const std::string what = std::string(f.name) + " stall " + std::to_string(stall) +
                                 (after ? " after" : " before") + " the doorbell";
        const Pkt0 r = Frame(1, stall, after, window_);
        EXPECT_EQ(1u, r.stalls) << what;
        EXPECT_LT(r.queued, 0) << what << ": waited for a later visit";
        EXPECT_GE(r.late, 0) << what;
        EXPECT_LE(r.late, aim_ + fine_ + gap + 20) << what;
        EXPECT_GE(r.vrx, 0) << what;
        EXPECT_FALSE(r.flagged) << what;
        kept++;
      }
  }
  EXPECT_GT(kept, 0);
}

/* absorb_ns is the smaller of the TX share of the launch budget and what check 1's budget
 * leaves past the plan's aim, its own aim and a fine trim. A stall kept at it is not
 * flagged. A counted stall past it, just past the limit or the 2 us stall test, whichever
 * is higher, is not kept: with the scheduler away the rest of the window, packet 0 then
 * leaves at the visit past its target. */
TEST_F(St20TxRlAnchor, AStallAtTheLimitIsKeptUnflaggedAndPastItIsNotKept) {
  const Fmt formats[] = {
      {"720p50", 1280, 720, ST_FPS_P50},
      {"720p59", 1280, 720},
      {"1080p25", 1920, 1080, ST_FPS_P25},
      {"1080p29", 1920, 1080, ST_FPS_P29_97},
      {"1080i59", 1920, 1080, ST_FPS_P59_94, true},
      {"1080p59", 1920, 1080},
      {"1080p60", 1920, 1080, ST_FPS_P60},
      {"1080p60_gpm", 1920, 1080, ST_FPS_P60, false, ST20_FMT_YUV_422_10BIT,
       ST20_PACKING_GPM},
      {"1080p60_bpm", 1920, 1080, ST_FPS_P60, false, ST20_FMT_YUV_422_10BIT,
       ST20_PACKING_BPM},
      {"2160p59", 3840, 2160},
      {"2160p59_444_12bit", 3840, 2160, ST_FPS_P59_94, false, ST20_FMT_YUV_444_12BIT},
      {"4320p59", 7680, 4320},
  };
  const long double counted = ut_trs_rl_stall_ns() + 20;
  for (const Fmt& f : formats) {
    ASSERT_NO_FATAL_FAILURE(Plan(f));
    const long double fine = p_ * ST_TX_VIDEO_RL_TRIM_FINE / plan_pad_;
    const long double timeslot = st_tx_video_rl_timeslot_tokens(bps_) * p_ / plan_pad_;
    const long double limit =
        std::min(4000 - aim_ - timeslot - fine, budget_ - 2 * aim_ - fine);
    EXPECT_NEAR((double)limit, (double)absorb_, 1.0) << f.name;
    for (bool after : {false, true}) {
      const std::string what = std::string(f.name) + (after ? " after" : " before");
      /* the harness's tsc reads put the gap up to 20 ns past the stall and the cost */
      const uint64_t stall = (uint64_t)std::max<long double>(0, limit - kBurstD[0] - 20);
      const Pkt0 at = Frame(1, stall, after, window_);
      EXPECT_EQ(limit > counted ? 1u : 0u, at.stalls) << what;
      EXPECT_LT(at.queued, 0) << what << ": not kept";
      EXPECT_GE(at.vrx, 0) << what;
      EXPECT_FALSE(at.flagged) << what;
      const Pkt0 past = Frame(1, (uint64_t)(std::max(limit, counted) - kBurstD[0] + 50),
                              after, window_);
      EXPECT_EQ(1u, past.stalls) << what;
      EXPECT_GE(past.queued, 0) << what << ": kept past the limit";
    }
  }
}

/* With the scheduler back at once, a stall just past the limit starts the pre-arm over
 * and packet 0 leaves on its plan. */
TEST_F(St20TxRlAnchor, AStallPastTheLimitStartsOverOnThePlan) {
  ASSERT_NO_FATAL_FAILURE(Plan(Fmt{"1080p60", 1920, 1080, ST_FPS_P60}));
  ASSERT_GT(absorb_, ut_trs_rl_stall_ns() + 20);
  const Pkt0 r = Frame(1, (uint64_t)(absorb_ - kBurstD[0] + 50), false, 2000);
  EXPECT_EQ(1u, r.stalls);
  EXPECT_LT(r.queued, 0);
  EXPECT_GT(r.pads, anchor_);
  EXPECT_GE(r.late, 0);
  EXPECT_LE(r.late, aim_ + fine_ + ut_trs_rl_stall_ns());
  EXPECT_FALSE(r.flagged);
}

/* At 1080p60 GPM and BPM the limit is under the 2 us stall test. A first pad burst whose
 * gap is between the two is no counted stall: the plan goes on, packet 0 is NIC-timed
 * behind its pads in that visit, and check 1 flags the frame. A gap under the limit is
 * kept the same way and not flagged. */
TEST_F(St20TxRlAnchor, AGapBetweenTheLimitAndTheStallTestIsKeptAndFlagged) {
  for (enum st20_packing packing : {ST20_PACKING_GPM, ST20_PACKING_BPM}) {
    ASSERT_NO_FATAL_FAILURE(Plan(
        Fmt{"1080p60", 1920, 1080, ST_FPS_P60, false, ST20_FMT_YUV_422_10BIT, packing}));
    const long double counted = ut_trs_rl_stall_ns();
    ASSERT_LT(absorb_ + 40, counted) << packing;
    for (bool between : {false, true}) {
      const std::string what =
          std::to_string(packing) + (between ? " between" : " under the limit");
      const long double gap = between ? (absorb_ + counted) / 2 : absorb_ - 20;
      const Pkt0 r = Frame(1, (uint64_t)(gap - kBurstD[0]), false, window_);
      EXPECT_EQ(0u, r.stalls) << what;
      EXPECT_LT(r.queued, 0) << what << ": not kept";
      EXPECT_GT(r.late, r.queued) << what << ": not NIC-timed";
      EXPECT_EQ(between, r.flagged) << what;
    }
  }
}

/* The first visit comes so late that the anchor is the one pad ahead of the trim pads:
 * once it stalled, no pre-arm fits behind it, and packet 0 is held for the first visit
 * past its target, not for the pads to drain. */
TEST_F(St20TxRlAnchor, StalledAnchorWithNoRoomLeftHoldsPacket0) {
  for (const Fmt& f : kFormats) {
    ASSERT_NO_FATAL_FAILURE(Plan(f));
    /* the pre-arm visit finds trim_pads + 1 pads to the target */
    const uint64_t gap = (uint64_t)((trim_pads_ + 0.5L) * p_ - credit_);
    ASSERT_GT(window_, gap) << f.name;
    const Pkt0 r = Frame(1, 6000, false, kOthersNs, window_ - gap);
    EXPECT_EQ(1u, r.stalls) << f.name;
    EXPECT_EQ(1u, r.pads) << f.name;
    EXPECT_GE(r.queued, 0) << f.name;
    EXPECT_LT(r.queued, kOthersNs + kBurstD[0] + kBurstG[0]) << f.name;
    EXPECT_GE(r.late, 0) << f.name;
  }
}

/* The NIC refuses every burst for 20 us behind the anchor, and the next visit comes 60 us
 * later, when the anchor has drained and the bucket refilled. The refused pads, counted
 * as queued, would then launch packet 0 late by that wait: they are dropped and the
 * pre-arm starts over, so packet 0 leaves on its plan. At 4320p, a 30 us window whose
 * full-burst anchor drains in 7.4 us, the refusal is 10 us and the visit 12 us later. */
TEST_F(St20TxRlAnchor, PadsRefusedBehindADrainedAnchorStartOver) {
  for (const Fmt& f : kFormats) {
    ASSERT_NO_FATAL_FAILURE(Plan(f));
    const bool full = anchor_ == ST_TX_VIDEO_RL_MAX_PADS_PER_BURST;
    const uint64_t others = full ? kOthersNs : 60000;
    ASSERT_LT(anchor_ * p_ + credit_, others) << f.name;
    const uint64_t open = kTargetTsc - window_;
    const uint64_t until = open + (full ? 10000 : 20000);
    const Pkt0 r = Frame(0, 0, false, others, 0, [open, until](ut_trs_ctx* ctx, int) {
      ut_trs_set_burst_refuse_window(ctx, open + kBurstD[0] + 50, until);
    });
    EXPECT_EQ(1u, r.stalls) << f.name;
    EXPECT_LT(r.queued, 0) << f.name;
    EXPECT_GE(r.late, 0) << f.name;
    EXPECT_LE(r.late, aim_ + fine_ + ut_trs_rl_stall_ns()) << f.name;
    EXPECT_GE(r.vrx, 0) << f.name;
    EXPECT_FALSE(r.flagged) << f.name;
    EXPECT_FALSE(r.leak) << f.name;
  }
}

/* A refusal of 3 us with the next visit 2 us later, the anchor still draining: the
 * refused pads retry behind it as the plan counted them, and nothing starts over. The
 * cut pads among them retry as NORMAL ones, so packet 0 is up to a pad later. */
TEST_F(St20TxRlAnchor, PadsRefusedBehindAnAnchorStillDrainingRetry) {
  for (const Fmt& f : kFormats) {
    ASSERT_NO_FATAL_FAILURE(Plan(f));
    const Pkt0 plain = Frame(0, 0, false, 2000);
    const uint64_t open = kTargetTsc - window_;
    const Pkt0 r = Frame(0, 0, false, 2000, 0, [open](ut_trs_ctx* ctx, int) {
      ut_trs_set_burst_refuse_window(ctx, open + kBurstD[0] + 50, open + 3000);
    });
    EXPECT_EQ(plain.pads, r.pads) << f.name;
    EXPECT_EQ(0u, r.stalls) << f.name;
    EXPECT_LT(r.queued, 0) << f.name;
    EXPECT_GE(r.late, 0) << f.name;
    EXPECT_LE(r.late, aim_ + fine_ + ut_trs_rl_stall_ns() + p_) << f.name;
    EXPECT_FALSE(r.leak) << f.name;
  }
}

/* The NIC takes 2 pads of an anchor that stalls 6 us, past absorb_ns: the pre-arm that
 * starts over plans from an idle shaper, so the refused rest is dropped, not retried
 * behind the new pads, where it would launch packet 0 late by its drain. */
TEST_F(St20TxRlAnchor, StalledAnchorPartlyRefusedDropsItsRest) {
  for (const Fmt& f : kFormats) {
    ASSERT_NO_FATAL_FAILURE(Plan(f));
    ASSERT_GT(anchor_, 2u) << f.name;
    const Pkt0 r = Frame(1, 6000, false, kOthersNs, 0, [](ut_trs_ctx* ctx, int visit) {
      ut_trs_set_burst_accept_limit(ctx, visit == 1 ? 2 : 0);
    });
    EXPECT_EQ(1u, r.stalls) << f.name;
    EXPECT_LT(r.queued, 0) << f.name;
    EXPECT_GE(r.late, 0) << f.name;
    EXPECT_LE(r.late, aim_ + fine_ + ut_trs_rl_stall_ns()) << f.name;
    EXPECT_FALSE(r.flagged) << f.name;
    EXPECT_FALSE(r.leak) << f.name;
  }
}

/* The last session's anchor stalls, and the scheduler is then away past its target: the
 * release scan ahead of the first session's visit sends its packet 0, as a held one, not
 * its own visit behind the other session's. */
TEST_F(St20TxRlAnchor, StartingOverPastTheTargetLeavesInTheReleaseScan) {
  ASSERT_NO_FATAL_FAILURE(Plan(Fmt{"1080p59", 1920, 1080}));
  struct Seen {
    ut_trs_ctx* last;
    const uint64_t* now;
    int64_t sent =
        -1; /* its packets 0 sent at the first session's visit past the target */
  };
  auto before = [](int i, void* arg) {
    Seen* seen = static_cast<Seen*>(arg);
    if (!i && seen->sent < 0 && *seen->now > kTargetTsc)
      seen->sent = ut_trs_real_send_count(seen->last);
    return true;
  };
  uint64_t now = kTargetTsc - window_ - kOthersNs;
  std::vector<ut_trs_ctx*> ctx = {Session(&now, kTargetTsc + 900000),
                                  Session(&now, kTargetTsc)};
  ut_trs_set_frame_burst_stall(ctx[1], 1, 6000, false);
  Seen seen = {ctx[1], &now};
  ut_trs_sch* sch = ut_trs_sch_create(ctx.data(), (int)ctx.size(), &now);
  ASSERT_NE(nullptr, sch);
  ut_trs_sch_set_visit(sch, before, nullptr, &seen);
  ut_trs_sch_call(sch);
  now = kTargetTsc - window_;
  ut_trs_sch_call(sch);
  ASSERT_EQ(1u, ut_trs_stat_recalculate_warmup(ctx[1]));
  ASSERT_EQ(0u, ut_trs_real_send_count(ctx[1]));
  now = kTargetTsc + 50000;
  ut_trs_sch_call(sch);
  EXPECT_EQ(1, seen.sent);
  /* late by the stall it counted, as a held one: no troffset mismatch on top */
  EXPECT_EQ(0u, ut_trs_stat_troffset_mismatch(ctx[1]));
  EXPECT_GE(ut_trs_modeled_launch_tsc(ctx[1], kPortP), kTargetTsc);
  ut_trs_sch_destroy(sch);
  for (ut_trs_ctx* c : ctx) ut_trs_destroy(c);
}

}  // namespace
