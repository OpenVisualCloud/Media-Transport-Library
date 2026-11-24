/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <vector>

#include "session/st20_tx_harness.h"
#include "session/st_video_transmitter_harness.h"

/* An idle E830/E835 shaper sends about two pads more at once than its 2 KB bucket when
 * they are 1274 B or shorter, and starts pads of 1292 B or more on the plan; an E810 one
 * is on the plan at every size. 1080p25 at the rates and pad intervals those NICs
 * trained. */
namespace {

constexpr int kPortP = 0;
constexpr uint64_t kTargetTsc = 10000000;
constexpr long double kBurstBytes = 2048;
constexpr uint16_t kE830EarlyPad = 1274;
constexpr long double kE830EarlyPads = 2;
constexpr long double kRoundingNs = 1e-3L;
/* RTE_ETHER_CRC_LEN: a shaper charges each frame its pkt and FCS bytes */
constexpr long double kFcs = 4;

struct Fmt {
  const char* name;
  enum st20_fmt fmt;
  uint32_t width;
  enum st20_packing packing;
  uint64_t bps;       /* the queue rate after the E830 retrain */
  float pad_interval; /* trained there */
  bool one_size;      /* every pkt NORMAL */
};

/* 1262 and 1214 B, one size: packet 0 left before its RTP timestamp on E830/E835 */
const Fmt kShort[] = {
    {"422-10", ST20_FMT_YUV_422_10BIT, 1920, ST20_PACKING_GPM_SL, 128059879, 126.2f,
     true},
    {"422-12", ST20_FMT_YUV_422_12BIT, 1920, ST20_PACKING_GPM_SL, 153664404, 289.2f,
     true},
    {"rgb-10", ST20_FMT_RGB_10BIT, 1920, ST20_PACKING_GPM_SL, 192090059, 177.4f, true},
};
/* 1292 and 1322 B, and BPM's 1322 B with EXTRA pkts: on the plan there */
const Fmt kLong[] = {
    {"422-10 1968", ST20_FMT_YUV_422_10BIT, 1968, ST20_PACKING_GPM_SL, 130856571, 213.7f,
     true},
    {"rgb-10 2016", ST20_FMT_RGB_10BIT, 2016, ST20_PACKING_GPM_SL, 200479547, 264.7f,
     true},
    {"422-10 bpm", ST20_FMT_YUV_422_10BIT, 1920, ST20_PACKING_BPM, 127611279, 414.3f,
     false},
};

std::vector<Fmt> AllFormats() {
  std::vector<Fmt> all(std::begin(kShort), std::end(kShort));
  all.insert(all.end(), std::begin(kLong), std::end(kLong));
  return all;
}

struct Plan {
  long double trs = 0, drain = 0, credit = 0, refill = 0;
  uint32_t warm_pkts = 0;
  uint16_t normal = 0, pad_size = 0;
  int64_t trim_pads = 0;
};

class St20TxRlPadSizeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
  }

  /* the plan tv_init_pacing() makes for f on an iavf VF */
  static void MakePlan(const Fmt& f, Plan* p) {
    ut_txv_ctx* txv = ut_txv_create();
    ASSERT_NE(nullptr, txv);
    ut_txv_set_rl_format(txv, f.width, 1080, false, ST_FPS_P25);
    ut_txv_set_rl_layout(txv, f.packing, f.fmt, 512);
    ut_txv_set_rl_queue_bps(txv, kPortP, f.bps);
    ASSERT_EQ(0, ut_txv_init_rl_pacing(txv, 1, 0, false, &f.pad_interval, false));
    ASSERT_TRUE(ut_txv_rl_armed(txv, kPortP));
    p->trs = ut_txv_pacing_trs(txv);
    p->drain = ut_txv_rl_drain(txv, kPortP);
    p->credit = ut_txv_rl_credit(txv, kPortP);
    p->refill = kBurstBytes * 1e9L / f.bps;
    p->warm_pkts = ut_txv_pacing_warm_pkts(txv);
    p->normal = ut_txv_normal_size(txv);
    p->pad_size = ut_txv_rl_pad_size(txv, kPortP);
    p->trim_pads = ut_txv_rl_trim_pads(txv, kPortP);
    ut_txv_destroy(txv);
  }

  /* Pre-arms from every window entry whose first burst is an anchor of uncut pads, on a
   * fluid shaper whose bucket is B/bps, an aim less than the plan's, and whose idle start
   * on a pad of at most early_len bytes sends early_pads more. Packet 0 must launch an
   * aim past its target, within a fine trim. */
  static void ExpectOnPlan(const Fmt& f, const Plan& p, uint16_t early_len,
                           long double early_pads) {
    const long double aim = p.credit - p.refill;
    const uint64_t edge = (uint64_t)(p.trim_pads * p.drain - p.credit);
    ut_trs_ctx* ctx = ut_trs_create();
    ASSERT_NE(nullptr, ctx);
    ut_trs_set_trs(ctx, p.trs);
    ut_trs_set_warm_pkts_cap(ctx, p.warm_pkts);
    ut_trs_set_drain(ctx, kPortP, p.drain);
    ut_trs_set_credit(ctx, kPortP, p.credit);
    ASSERT_EQ(0, ut_trs_set_pad_sizes(ctx, p.normal, p.pad_size));
    ut_trs_set_shaper_bucket(ctx, kPortP, p.refill);
    ut_trs_set_shaper_small_pads(ctx, kPortP, early_len, early_pads);
    const long double fine = ut_trs_fine_trim_ns(ctx, kPortP);
    long double earliest = HUGE_VALL, latest = -HUGE_VALL;
    uint32_t held = 0;

    for (uint64_t gap = edge + 1; gap <= p.warm_pkts * p.trs; gap += 997) {
      const uint64_t now[] = {kTargetTsc - gap};
      ut_trs_clear_train(ctx);
      ut_trs_set_target_tsc(ctx, kTargetTsc);
      ut_trs_set_mock_tsc_script(ctx, now, 1);
      if (!ut_trs_pre_arm(ctx)) {
        held++;
        continue;
      }
      const long double late = ut_trs_modeled_launch_tsc(ctx, kPortP) - kTargetTsc;
      earliest = std::min(earliest, late);
      latest = std::max(latest, late);
    }
    ut_trs_destroy(ctx);
    EXPECT_EQ(0u, held) << f.name;
    EXPECT_GE(earliest, 0) << f.name << ": packet 0 early";
    EXPECT_GE(earliest - aim, -kRoundingNs) << f.name;
    EXPECT_LT(latest - aim, fine) << f.name;
  }
};

}  // namespace

/* a stream of 1292 B pkts or more keeps its NORMAL pad and its plan */
TEST_F(St20TxRlPadSizeTest, PadsOf1292BytesOrMoreKeepTheirPlan) {
  for (const Fmt& f : kLong) {
    SCOPED_TRACE(f.name);
    Plan p;
    ASSERT_NO_FATAL_FAILURE(MakePlan(f, &p));
    EXPECT_EQ(p.normal, p.pad_size);
    EXPECT_EQ(std::min<int64_t>(p.normal / 64 + 64 / 8, 32), p.trim_pads);
    if (f.one_size) {
      EXPECT_NEAR((double)p.drain,
                  (double)(p.trs * f.pad_interval / (f.pad_interval + 1)),
                  1e-9 * (double)p.drain);
    }
  }
}

/* on an E830 shaper, and on an E810 one, which starts every pad size on the model */
TEST_F(St20TxRlPadSizeTest, Packet0LaunchesOnThePlan) {
  for (bool e830 : {true, false}) {
    SCOPED_TRACE(e830 ? "E830" : "E810");
    for (const Fmt& f : AllFormats()) {
      Plan p;
      ASSERT_NO_FATAL_FAILURE(MakePlan(f, &p));
      ASSERT_NO_FATAL_FAILURE(
          ExpectOnPlan(f, p, e830 ? kE830EarlyPad : 0, e830 ? kE830EarlyPads : 0));
    }
  }
}

namespace {

/* an armed port whose 1262 B NORMAL pkts pre-arm with 1292 B pads of 1 us */
ut_trs_ctx* LiftedPort() {
  ut_trs_ctx* ctx = ut_trs_create();
  if (!ctx) return nullptr;
  ut_trs_set_trs(ctx, 1000);
  ut_trs_set_drain(ctx, kPortP, 1000);
  ut_trs_set_hang_detect_thresh_ns(ctx, UINT64_MAX);
  if (ut_trs_set_pad_sizes(ctx, 1262, 1292) < 0) {
    ut_trs_destroy(ctx);
    return nullptr;
  }
  return ctx;
}

}  // namespace

/* refused pre-arm pads, cut or not, hold their references on the uncut pre-arm pad and
 * retry as it, a whole pad each: later, never early; the retry and a cleanup give them
 * back */
TEST_F(St20TxRlPadSizeTest, RefusedPadsRetryAsThePreArmPad) {
  constexpr uint64_t kTarget = 40000;
  const uint64_t now[] = {kTarget - 29625};
  for (bool cleanup : {false, true}) {
    SCOPED_TRACE(cleanup ? "cleanup" : "retry");
    ut_trs_ctx* ctx = LiftedPort();
    ASSERT_NE(nullptr, ctx);
    ut_trs_set_warm_pkts_cap(ctx, 30);
    ut_trs_set_target_tsc(ctx, kTarget);
    ut_trs_set_mock_tsc_script(ctx, now, 1);
    ut_trs_set_burst_force_fail(ctx, true);

    ut_trs_pre_arm(ctx);

    EXPECT_EQ(30u, ut_trs_pad_inflight_num(ctx));
    EXPECT_EQ(31u, ut_trs_pre_arm_pad_refcnt(ctx));
    EXPECT_EQ(1u, ut_trs_pad_refcnt(ctx));
    EXPECT_EQ(1u, ut_trs_trim_pad_refcnt(ctx, 0));
    EXPECT_EQ(1u, ut_trs_trim_pad_refcnt(ctx, 1));

    if (cleanup) {
      ut_trs_cleanup_state(ctx);
    } else {
      ut_trs_set_burst_force_fail(ctx, false);
      ut_trs_call_rl_tasklet(ctx);
      EXPECT_EQ(30u, ut_trs_train_pads(ctx, kPortP));
      EXPECT_NEAR(30 * 1000.0,
                  (double)(ut_trs_modeled_launch_tsc(ctx, kPortP) -
                           (long double)ut_trs_last_pad_send_tsc(ctx)),
                  1e-6);
    }

    EXPECT_EQ(0u, ut_trs_pad_inflight_num(ctx));
    EXPECT_EQ(1u, ut_trs_pre_arm_pad_refcnt(ctx));
    EXPECT_EQ(1u, ut_trs_pad_refcnt(ctx));
    ut_trs_destroy(ctx);
  }
}

/* a refused in-frame NORMAL pad keeps its reference and retries as itself, not as the
 * longer pre-arm pad the sawtooth does not count; the retry and a cleanup give it back */
TEST_F(St20TxRlPadSizeTest, RefusedInFramePadRetriesAsNormal) {
  for (bool cleanup : {false, true}) {
    SCOPED_TRACE(cleanup ? "cleanup" : "retry");
    ut_trs_ctx* ctx = LiftedPort();
    ASSERT_NE(nullptr, ctx);
    ut_trs_set_pad_interval(ctx, 4);
    ut_trs_enqueue_mid_pkt(ctx);
    /* the pkt goes, its pad and the visit's retry of it are refused */
    ut_trs_set_burst_refuse(ctx, 2, 2);

    ut_trs_call_rl_tasklet(ctx);

    EXPECT_EQ(1u, ut_trs_real_send_count(ctx));
    EXPECT_EQ(1u, ut_trs_pad_inflight_num(ctx));
    EXPECT_EQ(2u, ut_trs_pad_refcnt(ctx));
    EXPECT_EQ(1u, ut_trs_pre_arm_pad_refcnt(ctx));

    if (cleanup) {
      ut_trs_cleanup_state(ctx);
    } else {
      ut_trs_call_rl_tasklet(ctx);
      EXPECT_EQ(1u, ut_trs_pad_send_count(ctx));
      EXPECT_EQ(1262u, ut_trs_last_pad_len(ctx));
    }

    EXPECT_EQ(0u, ut_trs_pad_inflight_num(ctx));
    EXPECT_EQ(1u, ut_trs_pad_refcnt(ctx));
    EXPECT_EQ(1u, ut_trs_pre_arm_pad_refcnt(ctx));
    ut_trs_destroy(ctx);
  }
}

/* the pad of a frame's last pkt, refused in the call that dequeues the next packet 0, is
 * dropped ahead of its pre-arm from the NORMAL pad it holds */
TEST_F(St20TxRlPadSizeTest, InFramePadRefusedAheadOfPacket0IsDroppedAsNormal) {
  const uint64_t now[] = {1000};
  ut_trs_ctx* ctx = LiftedPort();
  ASSERT_NE(nullptr, ctx);
  ut_trs_set_pad_interval(ctx, 4);
  ut_trs_set_mock_tsc_script(ctx, now, 1);
  ut_trs_enqueue_frame_boundary(ctx, 100000);
  /* the last pkt goes, its pad is refused */
  ut_trs_set_burst_refuse(ctx, 2, 1);

  ut_trs_call_rl_tasklet(ctx);

  ASSERT_EQ(1u, ut_trs_pad_inflight_num(ctx));
  EXPECT_EQ(2u, ut_trs_pad_refcnt(ctx));
  EXPECT_EQ(1u, ut_trs_pre_arm_pad_refcnt(ctx));

  ut_trs_call_rl_tasklet(ctx);

  EXPECT_EQ(0u, ut_trs_pad_inflight_num(ctx));
  EXPECT_EQ(0u, ut_trs_pad_send_count(ctx));
  EXPECT_EQ(1u, ut_trs_pad_refcnt(ctx));
  EXPECT_EQ(1u, ut_trs_pre_arm_pad_refcnt(ctx));
  ut_trs_destroy(ctx);
}

/* the in-frame pad is the NORMAL one, which the sawtooth counts */
TEST_F(St20TxRlPadSizeTest, InFramePadIsNormal) {
  ut_trs_ctx* ctx = LiftedPort();
  ASSERT_NE(nullptr, ctx);
  ut_trs_set_pad_interval(ctx, 4);
  ut_trs_enqueue_mid_pkt(ctx);

  ut_trs_call_rl_tasklet(ctx);

  EXPECT_EQ(1u, ut_trs_real_send_count(ctx));
  EXPECT_EQ(1u, ut_trs_pad_send_count(ctx));
  EXPECT_EQ(1262u, ut_trs_last_pad_len(ctx));
  ut_trs_destroy(ctx);
}

/* a port without a plan warms up with NORMAL pads and retries the refused ones as them,
 * though it has a pre-arm pad */
TEST_F(St20TxRlPadSizeTest, UnplannedPortRetriesRefusedPadsAsNormalOnes) {
  const uint64_t warm_up[] = {8000};
  ut_trs_ctx* ctx = ut_trs_create();
  ASSERT_NE(nullptr, ctx);
  ut_trs_set_trs(ctx, 1000);
  ut_trs_set_warm_pkts_cap(ctx, 2);
  ut_trs_set_hang_detect_thresh_ns(ctx, UINT64_MAX);
  ASSERT_EQ(0, ut_trs_set_pad_sizes(ctx, 1262, 1292));
  ut_trs_set_target_tsc(ctx, 10000);
  ut_trs_set_mock_tsc_script(ctx, warm_up, 1);
  ut_trs_set_burst_force_fail(ctx, true);

  ut_trs_warm_up(ctx);

  ASSERT_EQ(2u, ut_trs_pad_inflight_num(ctx));
  EXPECT_EQ(3u, ut_trs_pad_refcnt(ctx));
  EXPECT_EQ(1u, ut_trs_pre_arm_pad_refcnt(ctx));

  ut_trs_set_burst_force_fail(ctx, false);
  ut_trs_call_rl_tasklet(ctx);
  ut_trs_call_rl_tasklet(ctx);

  EXPECT_EQ(0u, ut_trs_pad_inflight_num(ctx));
  EXPECT_EQ(2u, ut_trs_pad_send_count(ctx));
  EXPECT_EQ(1262u, ut_trs_last_pad_len(ctx));
  EXPECT_EQ(1u, ut_trs_pad_refcnt(ctx));
  EXPECT_EQ(1u, ut_trs_pre_arm_pad_refcnt(ctx));
  ut_trs_destroy(ctx);
}

/* a reclaim batch is due each rs_thresh / 2 NORMAL pkts, not pre-arm pads */
TEST_F(St20TxRlPadSizeTest, ReclaimBatchesAreNormalPkts) {
  constexpr uint16_t kRsThresh = 32;
  const uint64_t now[] = {10000000};
  ut_trs_ctx* ctx = LiftedPort();
  ASSERT_NE(nullptr, ctx);
  ut_trs_set_pmd_done_batches(ctx, kRsThresh, 0, 0);
  ut_trs_set_mock_tsc_script(ctx, now, 1);

  ASSERT_EQ(1u, ut_trs_call_burst_pad(ctx));

  const uint64_t batch =
      (uint64_t)(kRsThresh / 2 * (1000.0L * (1262 + kFcs) / (1292 + kFcs)));
  const uint64_t behind = ut_trs_deep_nb_tx_desc() / kRsThresh * batch;
  EXPECT_EQ(ut_trs_get_last_burst_succ_tsc(ctx) - behind + batch,
            ut_trs_reclaim_tsc(ctx));
  ut_trs_destroy(ctx);
}
