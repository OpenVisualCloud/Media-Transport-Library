/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "session/st20_tx/rl_vrx_model.h"
#include "session/st20_tx_harness.h"
#include "session/st_video_transmitter_harness.h"
#include "st2110/st_tx_video_rl.h"

/* Fault matrix for RL sessions on one scheduler: a cold first burst (F1), a long
 * co-located tasklet (F2), whole-thread stalls (F3), random short stalls (F4), every
 * cost scaled (F5) and a full NIC ring by call (F6) or for a while (F7), each at every
 * phase around packet 0's target, judged by the VRX of
 * the frame. Each case runs twice: on the library's transmitter tasklet with the plan's
 * absorb_ns, and with the hold: one visit a session a round, no absorb_ns, which holds
 * packet 0 on every counted stall. A reclaiming port's PMD
 * holds the done batches of the frame before it keeps, and from the visit after the
 * pre-arm on those of the pads drained since; it frees one a burst while its free
 * threshold lets it. Packet 0 is never early and no case goes below VRX 0 where the hold
 * does not. Where it does, the frame may go deeper only by the information limit: the
 * absorbed gap, at most absorb_ns, plus a tail burst sent with packet 0. Every session
 * below VRX 0 is flagged in stat_rl.vrx_frames, on either transmitter. */
namespace {

constexpr int kPortP = 0;
constexpr uint64_t kTargetTsc = 100000000;
constexpr long double kBurstBytes = 2048;
constexpr int kRoundCap = 1000000;
constexpr uint64_t kOthersNs = 12000; /* the other tasklets of a scheduler round */
/* the first pad burst on a reclaiming port, hardware p99 (no rs batch left to free) */
constexpr uint64_t kFirstNs = 750;
/* a first pad burst that still frees a cold rs batch, or runs cold: hardware 2.0-2.8 us,
 * up to 7.2 us */
constexpr uint64_t kColdNs[] = {2000, 3000, 5000};
constexpr uint16_t kRsThresh = kUtRlRsThresh;

struct Fmt {
  const char* name;
  uint32_t width, height;
  enum st_fps fps = ST_FPS_P59_94;
  enum st20_fmt fmt = ST20_FMT_YUV_422_10BIT;
  uint32_t pf_burst = 0;   /* the rl_burst_size of an ice PF, 0 for a VF */
  bool frame_pkt0 = false; /* packet 0 carries its frame, as the frame builders' */
};

struct Stall {
  uint64_t at, ns;
};

/* a frame's worst VRX over its sessions, and the sessions below 0 or flagged */
struct Verdict {
  int worst = 1 << 30;
  int negative = 0, missed = 0;
};

/* the plan of a format */
struct Plan {
  long double p = 0, credit = 0, absorb = 0, trs = 0, bucket = 0, tpr0 = 0;
  long double pad_interval = 0, refill = 0;
  int64_t budget = 0;
  uint32_t warm_pkts = 0;
  uint64_t window = 0;
  uint16_t pad_size = 0;
};

struct Case {
  int sessions;
  uint64_t first_ns;           /* the first pad burst's cost */
  uint64_t entry_ns;           /* the first visit, past the warm-up open */
  long double k;               /* every cost scaled */
  std::vector<Stall> stalls;   /* whole-thread stalls, sorted, absolute mock time */
  std::vector<Stall> tasklets; /* a long other tasklet in the first round from at */
  uint32_t refuse_call = 0, refuse_calls = 0; /* see ut_trs_set_burst_refuse() */
  uint64_t refuse_from = 0, refuse_to = 0;    /* see ut_trs_set_burst_refuse_window() */
};

class St20TxRlVrxFaults : public ::testing::TestWithParam<Fmt> {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
    PlanOf(GetParam(), &plan_);
  }

  static void PlanOf(const Fmt& f, Plan* plan) {
    const float trained[1] = {164};
    auto session = [&f] {
      ut_txv_ctx* txv = ut_txv_create();
      ut_txv_set_rl_format(txv, f.width, f.height, false, f.fps);
      if (f.fmt != ST20_FMT_YUV_422_10BIT)
        ut_txv_set_rl_layout(txv, ST20_PACKING_GPM, f.fmt, 512);
      if (f.pf_burst) ut_txv_set_ice_pf(txv, kPortP, f.pf_burst);
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
    plan->p = ut_txv_rl_drain(txv, kPortP);
    plan->credit = ut_txv_rl_credit(txv, kPortP);
    plan->absorb = ut_txv_rl_absorb(txv, kPortP);
    plan->budget = ut_txv_rl_vrx_budget(txv, kPortP);
    plan->refill = ut_txv_rl_refill(txv, kPortP);
    plan->pad_size = ut_txv_rl_pad_size(txv, kPortP);
    plan->trs = ut_txv_pacing_trs(txv);
    plan->warm_pkts = ut_txv_pacing_warm_pkts(txv);
    plan->pad_interval = ut_txv_pad_interval(txv);
    const uint32_t vrx = ut_txv_pacing_vrx(txv);
    struct st_fps_timing fps_tm;
    ASSERT_EQ(0, st_get_fps_timing(f.fps, &fps_tm));
    plan->tpr0 = kTargetTsc + vrx * plan->trs -
                 ut_rl_snap_loss(fps_tm.mul, fps_tm.den, ut_txv_pacing_tr_offset(txv),
                                 vrx, plan->trs);
    ut_txv_destroy(txv);
    plan->bucket = (f.pf_burst ? f.pf_burst : kBurstBytes) * 1e9L / programmed;
    plan->window = (uint64_t)(plan->warm_pkts * plan->trs);
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

  /* the sessions of one frame, on the library's transmitter tasklet or the hold's */
  Verdict Frame(const Case& c, bool absorb, bool real_handler, bool* tail = nullptr) {
    const uint64_t open = kTargetTsc - plan_.window;
    uint64_t now = open + c.entry_ns;
    std::vector<ut_trs_ctx*> ctx(c.sessions);
    std::vector<uint64_t> last(c.sessions, now);
    /* the pre-arm's return, 0 before it, UINT64_MAX once its done pad batches are set */
    std::vector<uint64_t> pre_armed(c.sessions, 0);
    std::vector<uint32_t> calls(c.sessions, 0);
    int frame = 0; /* stands in for packet 0's st_frame_trans */
    std::vector<uint64_t> at, ns;
    for (const Stall& s : c.stalls) {
      at.push_back(s.at);
      ns.push_back(s.ns);
    }
    for (int i = 0; i < c.sessions; i++) {
      const Plan& q = plan_;
      ctx[i] = ut_trs_create();
      ut_trs_set_trs(ctx[i], q.trs);
      ut_trs_set_warm_pkts_cap(ctx[i], q.warm_pkts);
      ut_trs_set_drain(ctx[i], kPortP, q.p);
      ut_trs_set_credit(ctx[i], kPortP, q.credit);
      ut_trs_set_absorb(ctx[i], kPortP, absorb ? q.absorb : 0);
      ut_trs_set_vrx_detect(ctx[i], kPortP, q.budget, q.refill);
      ut_trs_set_shaper_bucket(ctx[i], kPortP, q.bucket);
      ut_trs_set_mock_tsc_script(ctx[i], &now, 1);
      std::vector<uint64_t> d(64), g(64);
      for (int j = 0; j < 64; j++) {
        d[j] = llroundl(c.k * (j % 2 ? 200 : 1000));
        g[j] = llroundl(c.k * (j % 2 ? 250 : 300));
      }
      d[0] = llroundl(c.k * c.first_ns);
      ut_trs_set_burst_cost(ctx[i], d.data(), g.data(), (int)d.size());
      ut_trs_set_cpu_stalls(ctx[i], at.data(), ns.data(), (int)at.size());
      /* the waiting visits left the batches of the frame before the PMD keeps */
      ut_trs_set_pmd_done_batches(ctx[i], kRsThresh, ut_trs_rl_kept_batches(),
                                  kUtRlColdBatchFreeNs);
      if (GetParam().frame_pkt0) ut_trs_set_first_pkt_frame(ctx[i], &frame, 1262);
      if (c.refuse_calls) ut_trs_set_burst_refuse(ctx[i], c.refuse_call, c.refuse_calls);
      if (c.refuse_to) ut_trs_set_burst_refuse_window(ctx[i], c.refuse_from, c.refuse_to);
      ut_trs_enqueue_first_pkt(ctx[i], kTargetTsc);
    }
    /* the visit after the pre-arm finds done the batches of its pads the shaper drained
     * since, one a rs_thresh pads */
    auto before = [&](int i) {
      if (ut_trs_real_send_count(ctx[i])) return false;
      if (pre_armed[i] && pre_armed[i] != UINT64_MAX) {
        const uint64_t done = (uint64_t)((now - pre_armed[i]) / (kRsThresh * plan_.p));
        const uint64_t queued = ut_trs_pad_send_count(ctx[i]) / kRsThresh;
        ut_trs_add_pmd_done_batches(ctx[i], (uint32_t)std::min(done, queued),
                                    kUtRlHotBatchFreeNs);
        pre_armed[i] = UINT64_MAX;
      }
      calls[i] = ut_trs_burst_call_count(ctx[i]);
      return true;
    };
    auto after = [&](int i) {
      if (!pre_armed[i] && ut_trs_burst_call_count(ctx[i]) > calls[i])
        pre_armed[i] = ut_trs_last_tsc(ctx[i]);
    };
    Visits visits{before, after};
    ut_trs_sch* sch = nullptr;
    if (real_handler) {
      sch = ut_trs_sch_create(ctx.data(), c.sessions, &now);
      ut_trs_sch_set_visit(sch, Visits::Before, Visits::After, &visits);
    }
    size_t next_stall = 0, next_tasklet = 0;
    for (int round = 0; round < kRoundCap; round++) {
      bool done = true;
      for (int i = 0; i < c.sessions; i++)
        if (!ut_trs_real_send_count(ctx[i])) done = false;
      if (done) break;
      if (sch) {
        ut_trs_sch_call(sch);
      } else {
        /* the hold's transmitter: each session once a round, in order */
        for (int i = 0; i < c.sessions; i++) {
          if (!before(i)) continue;
          if (now > last[i]) ut_trs_advance_mock_tsc(ctx[i], now - last[i]);
          ut_trs_call_rl_tasklet(ctx[i]);
          last[i] = ut_trs_last_tsc(ctx[i]);
          after(i);
          now = std::max(now, last[i]);
        }
      }
      /* the other tasklets, a long one when its phase came, and the thread stalls in
       * them */
      uint64_t end = now + llroundl(c.k * kOthersNs);
      if (next_tasklet < c.tasklets.size() && end >= c.tasklets[next_tasklet].at)
        end += c.tasklets[next_tasklet++].ns;
      while (next_stall < c.stalls.size() && c.stalls[next_stall].at <= end) {
        if (c.stalls[next_stall].at > now) end += c.stalls[next_stall].ns;
        next_stall++;
      }
      now = end;
    }
    ut_trs_sch_destroy(sch);
    Verdict v;
    if (tail) *tail = false;
    for (int i = 0; i < c.sessions; i++) {
      if (tail && ut_trs_mixed_calls(ctx[i])) *tail = true;
      const Plan& q = plan_;
      const long double launch = ut_trs_modeled_launch_tsc(ctx[i], kPortP);
      EXPECT_GE(launch, kTargetTsc);
      const int vrx = ut_rl_vrx(launch, q.tpr0, q.trs, q.p, q.pad_interval, kSlots);
      const bool flagged = ut_trs_get_rl_stat(ctx[i]).vrx_frames > 0;
      v.worst = std::min(v.worst, vrx);
      v.negative += vrx < 0;
      v.missed += vrx < 0 && !flagged;
      ut_trs_destroy(ctx[i]);
    }
    return v;
  }

  /* the case with and without absorb_ns; absolute: VRX >= 0 whatever the hold's;
   * not_below_main: no session below both VRX 0 and the hold's worst */
  void Check(const char* cls, long double param, const Case& c, long double phase,
             bool absolute = false, bool not_below_main = false) {
    bool tail = false;
    const Verdict v = Frame(c, true, true, &tail), h = Frame(c, false, false);
    const int vrx = v.worst, held = h.worst;
    /* a launch later by info drains at most ceil(info / p) fewer pkts at any slot */
    const long double info =
        plan_.absorb + (tail ? ST_TX_VIDEO_RL_MAX_PADS_PER_BURST * plan_.p : 0);
    const int limit = (int)ceill(info / plan_.p);
    const std::string what =
        std::string(GetParam().name) + " " + cls + " " + std::to_string((double)param) +
        " x" + std::to_string(c.sessions) + " at " + std::to_string((double)phase) +
        " entry " + std::to_string(c.entry_ns);
    if (absolute) {
      EXPECT_GE(vrx, 0) << what << ": held " << held;
    }
    if (not_below_main) {
      EXPECT_TRUE(vrx >= 0 || vrx >= held)
          << what << ": VRX " << vrx << ", held " << held;
    }
    EXPECT_FALSE(held >= 0 && vrx < 0) << what << ": VRX " << vrx << ", held " << held;
    EXPECT_FALSE(held < 0 && vrx < held - limit)
        << what << ": VRX " << vrx << ", held " << held << ", limit " << limit;
    EXPECT_EQ(0, v.missed) << what << ": " << v.missed << " of " << v.negative
                           << " sessions below VRX 0 not flagged";
    EXPECT_EQ(0, h.missed) << what << " held: " << h.missed << " of " << h.negative
                           << " sessions below VRX 0 not flagged";
  }

  Case Base(int sessions) const {
    return {sessions, kFirstNs, 0, 1, {}, {}, 0, 0};
  }

  /* F1 cold cache: the first pad burst slow, the first visit at every phase of the window
   */
  void F1() {
    for (int n : {1, 8})
      for (uint64_t cold : {2000, 3000, 5000, 10000, 20000}) {
        if (n > 1 && cold > kColdNs[2]) continue;
        for (uint64_t e = 0; e + plan_.window / 2 <= plan_.window; e += 5000) {
          Case c = Base(n);
          c.first_ns = cold;
          c.entry_ns = e;
          Check("F1", cold / 1000.0L, c, -(long double)(plan_.window - e) / 1000, n == 1);
        }
      }
  }

  /* F2 long co-located tasklet at every phase relative to the target */
  void F2() {
    for (int n : {1, 8})
      for (uint64_t len : {20000, 50000, 100000, 200000, 383000})
        for (int64_t t = -(int64_t)plan_.window; t <= 50000; t += 5000) {
          Case c = Base(n);
          c.tasklets.push_back({(uint64_t)((int64_t)kTargetTsc + t), len});
          Check("F2", len / 1000.0L, c, t / 1000.0L);
          c.first_ns = kColdNs[1];
          Check("F2c", len / 1000.0L, c, t / 1000.0L);
        }
  }

  /* F6 a full NIC ring refuses calls bursts from burst call at, at every call of the
   * frame */
  void F6() {
    for (int n : {1, 8})
      for (uint32_t calls : {1, 4, 32})
        for (uint32_t at = 1; at <= 12; at++) {
          Case c = Base(n);
          c.refuse_call = at;
          c.refuse_calls = calls;
          Check("F6", calls, c, at);
        }
  }

  /* F8 the first frame of a stream: the first frames of the sessions sharing the
   * scheduler hold it past the warm-up open, so the first visit finds fewer than
   * trim_pads pads to the target, enough coarse cuts for any cut of a pad, and a long
   * tasklet sits at the target. Packet 0 goes behind those pads, so it keeps VRX >= 0
   * whatever runs at the target. */
  void F8() {
    const long double coarse =
        ceill((long double)plan_.pad_size / ST_TX_VIDEO_RL_TRIM_COARSE);
    const int64_t trim_pads = st_tx_video_rl_trim_pads(plan_.pad_size);
    /* the gaps from the visit to the target where that many pads, and fewer than
     * trim_pads, fit */
    const int64_t lo = (int64_t)ceill(coarse * plan_.p - plan_.credit);
    const int64_t hi = (int64_t)((trim_pads - 1) * plan_.p - plan_.credit);
    for (int n : {1, 8})
      for (int64_t gap = lo + 1000; gap < hi; gap += 5000)
        for (uint64_t len : {50000, 100000})
          for (int64_t t : {-10000, -5000, 0}) {
            Case c = Base(n);
            /* the first visit dequeues packet 0, the one a round later pre-arms */
            c.entry_ns = plan_.window - gap - kOthersNs;
            c.tasklets.push_back({(uint64_t)((int64_t)kTargetTsc + t), len});
            Check("F8", len / 1000.0L, c, t / 1000.0L, true);
          }
  }

  /* F9 a thread stall in the pre-arm call of 8 sessions delays its later visits to an
   * entry as late as F8's, and another comes at every instant of what they then send.
   * Late by the call alone, the round left them in time: packet 0 holds, no tail, so no
   * stall in a tail call takes it below VRX 0 where the hold is not. A format whose pad
   * is within absorb_ns sends every late entry not behind refused pads as a tail. */
  void F9() {
    if (plan_.p <= plan_.absorb) return;
    const int64_t trim_pads = st_tx_video_rl_trim_pads(plan_.pad_size);
    /* fewer than trim_pads pads to the target, and enough coarse cuts */
    const uint64_t gap = (uint64_t)((trim_pads - 2) * plan_.p - plan_.credit);
    /* 15 us into the round after the dequeue at the warm-up open: in the pre-arm call
     * for any entry up to 11 us past the open */
    const uint64_t in_call = kTargetTsc - plan_.window + kOthersNs + 15000;
    for (uint64_t at = 0; at < 5000; at += 250) {
      Case c = Base(8);
      c.stalls.push_back({in_call, kTargetTsc - gap - in_call});
      c.stalls.push_back({kTargetTsc - gap + at, 50000});
      Check("F9", 50, c, at / 1000.0L);
      bool tail = false;
      Frame(c, true, true, &tail);
      EXPECT_FALSE(tail) << GetParam().name << " F9 at " << at;
    }
  }

  static constexpr int kSlots = 512; /* the drain slots judged, from packet 0 */
  Plan plan_;
};

TEST_P(St20TxRlVrxFaults, F1ColdFirstBurst) {
  F1();
}

TEST_P(St20TxRlVrxFaults, F2LongTasklet) {
  F2();
}

/* F3 whole-thread stall at every instant, inside a burst before its doorbell included */
TEST_P(St20TxRlVrxFaults, F3ThreadStall) {
  for (int n : {1, 8})
    for (uint64_t len : {5000, 20000, 50000, 100000, 500000, 1000000, 4000000}) {
      const int64_t step = len <= 50000 ? 1000 : 5000;
      for (int64_t t = -(int64_t)plan_.window - (int64_t)std::min<uint64_t>(len, 600000);
           t <= 50000; t += step) {
        Case c = Base(n);
        c.stalls.push_back({(uint64_t)((int64_t)kTargetTsc + t), len});
        Check("F3", len / 1000.0L, c, t / 1000.0L);
        c.first_ns = kColdNs[1];
        Check("F3c", len / 1000.0L, c, t / 1000.0L);
      }
    }
}

/* F4 seeded short stalls, 5-50 us at random instants of the frame */
TEST_P(St20TxRlVrxFaults, F4RandomShortStalls) {
  for (int n : {1, 8})
    for (uint64_t mean_gap : {200000, 50000, 20000}) {
      std::mt19937 rng(2110 + mean_gap + n);
      std::exponential_distribution<double> gap(1.0 / (double)mean_gap);
      std::uniform_int_distribution<uint64_t> len(5000, 50000);
      for (int seed = 0; seed < 200; seed++) {
        Case c = Base(n);
        long double t = kTargetTsc - plan_.window - 50000;
        while ((t += gap(rng)) < kTargetTsc + 50000)
          c.stalls.push_back({(uint64_t)t, len(rng)});
        Check("F4", mean_gap / 1000.0L, c, seed);
      }
    }
}

/* F5 every cost scaled by k, the first visit at every phase */
TEST_P(St20TxRlVrxFaults, F5ScaledCosts) {
  for (int n : {1, 8})
    for (long double k : {1.0L, 2.0L, 4.0L, 8.0L, 16.0L})
      for (uint64_t e = 0; e <= 100000; e += 10000) {
        Case c = Base(n);
        c.k = k;
        c.entry_ns = e;
        Check("F5", k, c, -(long double)(plan_.window - e) / 1000);
      }
}

TEST_P(St20TxRlVrxFaults, F6RefusedBursts) {
  F6();
}

TEST_P(St20TxRlVrxFaults, F8StreamStart) {
  F8();
}

TEST_P(St20TxRlVrxFaults, F9StallInThePreArmCall) {
  F9();
}

/* F7 a full NIC ring refuses every burst for a while from each instant of the window, the
 * scheduler round at three phases of the first visit: a session whose pads the ring
 * refused, which count as queued, is late by its retry, so no transmitter work may move
 * that retry behind main's */
TEST_P(St20TxRlVrxFaults, F7RefusedRingWindow) {
  for (uint64_t dur : {5000, 10000, 20000})
    for (uint64_t entry : {0, 5000, 9000})
      for (int64_t t = -(int64_t)plan_.window; t <= 10000; t += 2000) {
        Case c = Base(8);
        c.entry_ns = entry;
        c.refuse_from = (uint64_t)((int64_t)kTargetTsc + t);
        c.refuse_to = c.refuse_from + dur;
        Check("F7", dur / 1000.0L, c, t / 1000.0L, false, true);
      }
}

std::string FmtName(const ::testing::TestParamInfo<Fmt>& info) {
  return info.param.name;
}

INSTANTIATE_TEST_SUITE_P(Formats, St20TxRlVrxFaults,
                         ::testing::Values(Fmt{"720p50", 1280, 720, ST_FPS_P50},
                                           Fmt{"720p59", 1280, 720},
                                           Fmt{"1080p59", 1920, 1080},
                                           Fmt{"2160p59", 3840, 2160},
                                           Fmt{"2160p59_444_12bit", 3840, 2160,
                                               ST_FPS_P59_94, ST20_FMT_YUV_444_12BIT}),
                         FmtName);

/* an ice PF bucket and packet 0 carrying its frame */
class St20TxRlVrxFaultRows : public St20TxRlVrxFaults {};

TEST_P(St20TxRlVrxFaultRows, F1ColdFirstBurst) {
  F1();
}

TEST_P(St20TxRlVrxFaultRows, F2LongTasklet) {
  F2();
}

TEST_P(St20TxRlVrxFaultRows, F6RefusedBursts) {
  F6();
}

INSTANTIATE_TEST_SUITE_P(Rows, St20TxRlVrxFaultRows,
                         ::testing::Values(Fmt{"1080p59_pf", 1920, 1080, ST_FPS_P59_94,
                                               ST20_FMT_YUV_422_10BIT, 4096},
                                           Fmt{"1080p59_frame_pkt0", 1920, 1080,
                                               ST_FPS_P59_94, ST20_FMT_YUV_422_10BIT, 0,
                                               true}),
                         FmtName);

}  // namespace
