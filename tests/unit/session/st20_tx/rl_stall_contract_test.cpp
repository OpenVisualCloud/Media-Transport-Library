/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "session/st20_tx/rl_vrx_model.h"
#include "session/st20_tx_harness.h"
#include "session/st_video_transmitter_harness.h"
#include "st2110/st_tx_video_rl.h"

/* The stall contract of RL sessions on one scheduler, judged on the wire model alone. A
 * stall of g lands at an event of the frame, wherever the transmitter makes that call:
 * S1 in the first pad call ahead of packet 0 before its doorbell, S2 after it, S3 in the
 * second pad call, S4 in the call that queues packet 0, S5 between the visits so that
 * the warm-up window is first reached r before the target, S6 in a late entry's first
 * pad call (recorded only). The first call at or after the target comes d late, a
 * measured release delay. Packet 0 never leaves early and every session below VRX 0 is
 * flagged. A stall the plan's absorb_ns covers in S1-S3 is kept: VRX >= 0. Past it each
 * packet 0 is kept with VRX >= 0 or held, queued by the first release scan. With
 * MTL_UT_RL_CONTRACT_CSV set, each cell appends one row to that file. */
namespace {

constexpr int kPortP = 0;
constexpr uint64_t kTargetTsc = 100000000;
constexpr long double kBurstBytes = 2048;
constexpr int kRoundCap = 100000;
constexpr int kSlots = 512;
constexpr uint32_t kScheduleNs = 200000;
constexpr uint64_t kOthersNs = 12000; /* the other tasklets of a scheduler round */
/* the first pad burst on a reclaiming port, hardware p99: the gap a healthy one shows */
constexpr uint64_t kFirstNs = 750;
/* the most a burst call costs in this harness: its cost, its return and a cold free */
constexpr uint64_t kReleaseNs = 1000 + 300 + kUtRlColdBatchFreeNs;
constexpr uint64_t kStallNs[] = {1000,  2500,  4000,  6000,  8000,  10000,
                                 15000, 20000, 30000, 50000, 100000};
/* round14 DATA.md: idle, then a held packet 0's p50, p90 and max past its target */
constexpr uint64_t kHoldNs[] = {0, 3300, 10000, 28000};

struct Fmt {
  const char* name;
  uint32_t width, height;
  enum st_fps fps = ST_FPS_P59_94;
  int sessions = 8; /* as many as fit one scheduler, 12 1080p59 */
  enum st20_fmt fmt = ST20_FMT_YUV_422_10BIT;
  bool interlaced = false;
};

struct Scenario {
  const char* name;
  uint32_t pad_call; /* the event: that pad call, 0 the call of packet 0 */
  bool stall;        /* false: S5, only the late window */
  bool after_doorbell;
  long double entry_pads; /* the first visit in the window: that many trim_pads of pads
                           * before the target, 0 for the matrix's at the open */
  bool asserted;
};

constexpr Scenario kScenarios[] = {
    {"S1", 1, true, false, 0, true},         {"S2", 1, true, true, 0, true},
    {"S3", 2, true, false, 0, true},         {"S4", 0, true, false, 0, true},
    {"S5_0.5", 0, false, false, 0.5L, true}, {"S5_1", 0, false, false, 1, true},
    {"S5_2", 0, false, false, 2, true},      {"S6", 1, true, false, 0.5L, false},
};

struct Plan {
  long double p = 0, credit = 0, absorb = 0, trs = 0, bucket = 0, tpr0 = 0;
  long double pkt = 0; /* the NORMAL pkt's drain, p the pre-arm pad's */
  long double pad_interval = 0, refill = 0;
  int64_t budget = 0, trim_pads = 0;
  uint32_t warm_pkts = 0;
  uint64_t window = 0;
  uint16_t normal = 0, pad_size = 0;
};

/* packet 0 of one session */
struct Pkt0 {
  long double late = 0;   /* launch - target */
  long double queued = 0; /* the doorbell of its call - target: held when >= 0 */
  int vrx = 0;
  bool nic = false, flagged = false; /* nic: behind pads still draining */
};

struct Cell {
  std::vector<Pkt0> pkt0;
  uint64_t g = 0;
  uint64_t stall_end = 0; /* 0: the event never came */
  uint32_t stalled_calls = 0;
  bool pkt0_stalled = false; /* session 0's packet 0 went in the stalled call */
};

class St20TxRlStallContract : public ::testing::TestWithParam<Fmt> {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
    PlanOf(GetParam());
  }

  void PlanOf(const Fmt& f) {
    const float trained[1] = {164};
    auto session = [&f] {
      ut_txv_ctx* txv = ut_txv_create();
      ut_txv_set_rl_format(txv, f.width, f.height, f.interlaced, f.fps);
      if (f.fmt != ST20_FMT_YUV_422_10BIT)
        ut_txv_set_rl_layout(txv, ST20_PACKING_GPM, f.fmt, 512);
      return txv;
    };
    ut_txv_ctx* txv = session();
    ASSERT_EQ(0, ut_txv_init_rl_pacing(txv, 1, 0, false, trained, false));
    const uint64_t programmed = ut_txv_rl_queue_bps(txv, kPortP) * 15 / 16;
    ut_txv_destroy(txv);
    txv = session();
    ut_txv_set_rl_queue_bps(txv, kPortP, programmed);
    ASSERT_EQ(0, ut_txv_init_rl_pacing(txv, 1, 0, false, trained, false));
    ASSERT_TRUE(ut_txv_rl_armed(txv, kPortP)) << f.name;
    plan_.p = ut_txv_rl_drain(txv, kPortP);
    plan_.credit = ut_txv_rl_credit(txv, kPortP);
    plan_.absorb = ut_txv_rl_absorb(txv, kPortP);
    plan_.budget = ut_txv_rl_vrx_budget(txv, kPortP);
    plan_.refill = ut_txv_rl_refill(txv, kPortP);
    plan_.normal = ut_txv_normal_size(txv);
    plan_.pad_size = ut_txv_rl_pad_size(txv, kPortP);
    plan_.pkt = st_tx_video_rl_pad_ns(plan_.p, plan_.pad_size, plan_.normal);
    plan_.trim_pads = st_tx_video_rl_trim_pads(plan_.pad_size);
    plan_.trs = ut_txv_pacing_trs(txv);
    plan_.warm_pkts = ut_txv_pacing_warm_pkts(txv);
    plan_.pad_interval = ut_txv_pad_interval(txv);
    const uint32_t vrx = ut_txv_pacing_vrx(txv);
    struct st_fps_timing fps_tm;
    ASSERT_EQ(0, st_get_fps_timing(f.fps, &fps_tm));
    plan_.tpr0 = kTargetTsc + vrx * plan_.trs -
                 ut_rl_snap_loss(fps_tm.mul, fps_tm.den, ut_txv_pacing_tr_offset(txv),
                                 vrx, plan_.trs);
    ut_txv_destroy(txv);
    plan_.bucket = kBurstBytes * 1e9L / programmed;
    plan_.window = (uint64_t)(plan_.warm_pkts * plan_.trs);
  }

  struct Visits {
    std::function<bool(int)> before;
    std::function<void(int)> after;
    static bool Before(int i, void* arg) {
      return static_cast<Visits*>(arg)->before(i);
    }
    static bool After(int i, void* arg) {
      static_cast<Visits*>(arg)->after(i);
      return true;
    }
  };

  /* one frame of n sessions, the stall in session 0 */
  Cell Run(const Scenario& sc, uint64_t g, uint64_t d, int n) {
    const uint64_t r = (uint64_t)(sc.entry_pads * plan_.trim_pads * plan_.p);
    /* the visit that dequeues packet 0, a round before the first in the window */
    uint64_t now = kTargetTsc - plan_.window - (r ? kOthersNs : 0);
    std::vector<ut_trs_ctx*> ctx(n);
    std::vector<uint64_t> pre_armed(n, 0);
    for (int i = 0; i < n; i++) {
      ctx[i] = ut_trs_create();
      ut_trs_set_trs(ctx[i], plan_.trs);
      ut_trs_set_warm_pkts_cap(ctx[i], plan_.warm_pkts);
      ut_trs_set_drain(ctx[i], kPortP, plan_.p);
      ut_trs_set_credit(ctx[i], kPortP, plan_.credit);
      EXPECT_EQ(0, ut_trs_set_pad_sizes(ctx[i], plan_.normal, plan_.pad_size));
      ut_trs_set_absorb(ctx[i], kPortP, plan_.absorb);
      ut_trs_set_vrx_detect(ctx[i], kPortP, plan_.budget, plan_.refill);
      ut_trs_set_shaper_bucket(ctx[i], kPortP, plan_.bucket);
      ut_trs_set_mock_tsc_script(ctx[i], &now, 1);
      std::vector<uint64_t> dn(64), gn(64);
      for (int j = 0; j < 64; j++) {
        dn[j] = j % 2 ? 200 : 1000;
        gn[j] = j % 2 ? 250 : 300;
      }
      dn[0] = kFirstNs;
      ut_trs_set_burst_cost(ctx[i], dn.data(), gn.data(), (int)dn.size());
      ut_trs_set_pmd_done_batches(ctx[i], kUtRlRsThresh, ut_trs_rl_kept_batches(),
                                  kUtRlColdBatchFreeNs);
      ut_trs_enqueue_first_pkt(ctx[i], kTargetTsc);
    }
    if (sc.stall) ut_trs_set_frame_burst_stall(ctx[0], sc.pad_call, g, sc.after_doorbell);
    /* the visit after the pre-arm finds done the batches of its pads the shaper drained
     * since, one a rs_thresh pads */
    auto before = [&](int i) {
      if (ut_trs_real_send_count(ctx[i])) return false;
      if (pre_armed[i] && pre_armed[i] != UINT64_MAX) {
        const uint64_t done =
            (uint64_t)((now - pre_armed[i]) / (kUtRlRsThresh * plan_.p));
        const uint64_t queued = ut_trs_pad_send_count(ctx[i]) / kUtRlRsThresh;
        ut_trs_add_pmd_done_batches(ctx[i], (uint32_t)std::min(done, queued),
                                    kUtRlHotBatchFreeNs);
        pre_armed[i] = UINT64_MAX;
      }
      return true;
    };
    auto after = [&](int i) {
      if (!pre_armed[i] && ut_trs_pad_send_count(ctx[i]))
        pre_armed[i] = ut_trs_last_tsc(ctx[i]);
    };
    Visits visits{before, after};
    ut_trs_sch* sch = ut_trs_sch_create(ctx.data(), n, &now);
    ut_trs_sch_set_visit(sch, Visits::Before, Visits::After, &visits);
    ut_trs_sch_set_schedule_ns(sch, kScheduleNs);
    Cell c;
    c.g = g;
    for (int round = 0; round < kRoundCap; round++) {
      bool done = true;
      for (int i = 0; i < n; i++)
        if (!ut_trs_real_send_count(ctx[i])) done = false;
      if (done) break;
      const uint32_t calls = ut_trs_burst_call_count(ctx[0]);
      const bool stalled = ut_trs_burst_stall_end_tsc(ctx[0]);
      ut_trs_sch_call(sch);
      if (!sc.stall && round == 1)
        c.stalled_calls = ut_trs_burst_call_count(ctx[0]) - calls;
      if (!stalled && ut_trs_burst_stall_end_tsc(ctx[0])) {
        c.stall_end = ut_trs_burst_stall_end_tsc(ctx[0]);
        c.stalled_calls = ut_trs_burst_call_count(ctx[0]) - calls;
      }
      uint64_t next = now + kOthersNs;
      if (r && round == 0) {
        /* S5's stall between the visits */
        next = kTargetTsc - r;
        if (!sc.stall) {
          c.g = next - now;
          c.stall_end = next;
        }
      } else if (next >= kTargetTsc) {
        next = std::max(now, kTargetTsc) + d;
      }
      now = next;
    }
    ut_trs_sch_destroy(sch);
    for (int i = 0; i < n; i++) {
      Pkt0 q;
      EXPECT_TRUE(ut_trs_real_send_count(ctx[i])) << "session " << i << " never sent";
      const long double launch = ut_trs_modeled_launch_tsc(ctx[i], kPortP);
      const long double queued = ut_trs_last_real_send_tsc(ctx[i]);
      q.late = launch - kTargetTsc;
      q.queued = queued - kTargetTsc;
      q.vrx =
          ut_rl_vrx(launch, plan_.tpr0, plan_.trs, plan_.pkt, plan_.pad_interval, kSlots);
      q.nic = launch > queued;
      q.flagged = ut_trs_get_rl_stat(ctx[i]).vrx_frames > 0;
      if (!i && sc.stall && c.stall_end)
        c.pkt0_stalled = queued + g >= c.stall_end && queued <= c.stall_end;
      c.pkt0.push_back(q);
      ut_trs_destroy(ctx[i]);
    }
    return c;
  }

  /* Within the limit a stall in S1-S3 is kept: packet 0 behind its pads, VRX >= 0. Past
   * it packet 0 is kept, queued before its target, only with VRX >= 0, unless the stall
   * was in its own call, or held: queued by the first release scan, the later of
   * target + d and the stall's end, plus a burst call for each session. */
  void Judge(const Scenario& sc, uint64_t d, int n, const Cell& c) {
    if (!sc.asserted) return;
    const std::string what = std::string(GetParam().name) + " " + sc.name + " g " +
                             std::to_string(c.g) + " d " + std::to_string(d) + " x" +
                             std::to_string(n);
    /* the gap the plan sees: the stall and a healthy first burst */
    const bool within = sc.stall && c.g + kFirstNs <= plan_.absorb;
    const long double scan =
        std::max<long double>(kTargetTsc + d, c.stall_end) - kTargetTsc + n * kReleaseNs;
    int vrx = 1 << 30;
    for (int i = 0; i < n; i++) {
      const Pkt0& q = c.pkt0[i];
      const std::string who = what + " session " + std::to_string(i);
      vrx = std::min(vrx, q.vrx);
      EXPECT_GE(q.late, 0) << who << ": early";
      EXPECT_TRUE(q.vrx >= 0 || q.flagged) << who << ": VRX " << q.vrx << " not flagged";
      if (within || !c.stall_end) continue;
      if (q.queued >= 0) {
        EXPECT_LE(q.queued, scan) << who << ": held past the release scan";
      } else if (i || !c.pkt0_stalled) {
        EXPECT_GE(q.vrx, 0) << who << ": kept past its slot, late " << q.late;
      }
    }
    if (within && sc.pad_call && c.stall_end) {
      EXPECT_TRUE(c.pkt0[0].nic && c.pkt0[0].queued < 0)
          << what << ": a stall within absorb_ns held";
      EXPECT_GE(vrx, 0) << what << ": kept below VRX 0";
    }
  }

  void Row(FILE* csv, const Scenario& sc, uint64_t d, int n, const Cell& c) {
    int vrx = 1 << 30, neg = 0, missed = 0, flagged = 0, early = 0;
    long double late_max = 0;
    for (const Pkt0& q : c.pkt0) {
      vrx = std::min(vrx, q.vrx);
      neg += q.vrx < 0;
      missed += q.vrx < 0 && !q.flagged;
      flagged += q.flagged;
      early += q.late < 0;
      late_max = std::max(late_max, q.late);
    }
    const Pkt0& q = c.pkt0[0];
    (void)fprintf(csv,
                  "%s,%s,%" PRIu64 ",%" PRIu64
                  ",%d,%d,%.0Lf,%.0Lf,%s,%d,%d,%.0Lf,%d,%d,%d,%d,%u,%d,%d\n",
                  GetParam().name, sc.name, c.g, d, n, c.stall_end != 0, q.late, q.queued,
                  q.nic ? "nic" : "cpu", q.vrx, q.flagged, late_max, vrx, neg, flagged,
                  missed, c.stalled_calls, early, c.pkt0_stalled);
  }

  void Contract() {
    const char* path = getenv("MTL_UT_RL_CONTRACT_CSV");
    FILE* csv = path ? fopen(path, "a") : nullptr;
    if (csv && !ftell(csv))
      (void)fprintf(
          csv,
          "fmt,scenario,g_ns,d_ns,sessions,stalled,late_ns,queued_ns,sent_by,vrx,flagged,"
          "late_max_ns,vrx_min,negative,flagged_n,missed,stalled_calls,early,"
          "pkt0_stalled\n");
    std::vector<int> counts{1};
    if (GetParam().sessions > 1) counts.push_back(GetParam().sessions);
    for (const Scenario& sc : kScenarios)
      for (uint64_t g : kStallNs) {
        /* S5's stall is the gap to its late window */
        if (!sc.stall && g != kStallNs[0]) continue;
        for (uint64_t d : kHoldNs)
          for (int n : counts) {
            const Cell c = Run(sc, g, d, n);
            Judge(sc, d, n, c);
            if (csv) Row(csv, sc, d, n, c);
          }
      }
    if (csv) (void)fclose(csv);
  }

  Plan plan_;
};

TEST_P(St20TxRlStallContract, Holds) {
  Contract();
}

std::string FmtName(const ::testing::TestParamInfo<Fmt>& info) {
  return info.param.name;
}

INSTANTIATE_TEST_SUITE_P(Formats, St20TxRlStallContract,
                         ::testing::Values(Fmt{"720p50", 1280, 720, ST_FPS_P50},
                                           Fmt{"720p59", 1280, 720},
                                           Fmt{"1080p29", 1920, 1080, ST_FPS_P29_97},
                                           Fmt{"1080i59", 1920, 1080, ST_FPS_P59_94, 8,
                                               ST20_FMT_YUV_422_10BIT, true},
                                           Fmt{"1080p59", 1920, 1080},
                                           Fmt{"2160p59", 3840, 2160, ST_FPS_P59_94, 3},
                                           Fmt{"2160p59_444_12bit", 3840, 2160,
                                               ST_FPS_P59_94, 1, ST20_FMT_YUV_444_12BIT},
                                           Fmt{"4320p59", 7680, 4320, ST_FPS_P59_94, 1}),
                         FmtName);

}  // namespace
