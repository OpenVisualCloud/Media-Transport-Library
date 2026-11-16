/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "session/st20_tx_harness.h"

/* The stats dump's round err against a loop simulation with injected costs. Each loop
 * visits every session once: the visit costs visit_ns, and its builder adds up to a bulk
 * of pkts, pkt_ns each, while it runs less than ring_count pkts ahead of the wire, which
 * takes one a trs from tr_offset. A load fits when no pkt meets the wire unbuilt and the
 * loop averages within the tightest bulk * frame_time / pkts. */
namespace {

constexpr unsigned int kRingCount = 512;
constexpr int kMaxSessions = 60;           /* ST_SCH_MAX_TX_VIDEO_SESSIONS */
constexpr int kFrames = 3;                 /* the first one warms up */
constexpr uint64_t kWindowNs = 2000000000; /* the span of the scheduler's loop avg */

struct Cost {
  long double visit_ns, pkt_ns;
};

/* the costs fitted on the reference host */
constexpr Cost k720Cost = {140, 233};
constexpr Cost k1080Cost = {90, 198};
constexpr Cost k2160Cost = {110, 151};

enum Fmt { k720, k1080, k2160, k1080p50, k1080p60, kFmts };

/* Work of other tasklets: a stall each frame mid-way, or at its epoch where the video
 * builders are in their frame gap, work_ns every period_ns, and a share of every loop's
 * time, as an RX session polled on the scheduler. */
struct Other {
  long double stall_ns, period_ns, work_ns, share;
  bool at_epoch;
};

/* The pkt cost of n sessions of 512 pkts in flight each, whatever the format: the build
 * measured on the reference host, plus the transmitter
 * and PMD share, 68 to 102 ns there, taken to grow with the same L2 cliff. */
long double FootprintPktNs(int n) {
  const long double at[][2] = {{1, 36},   {5, 54},   {8, 80},
                               {18, 135}, {36, 139}, {60, 145}};
  long double build = at[5][1];
  for (int i = 1; i < 6; i++) {
    if (n > at[i][0]) continue;
    build = at[i - 1][1] +
            (at[i][1] - at[i - 1][1]) * (n - at[i - 1][0]) / (at[i][0] - at[i - 1][0]);
    break;
  }
  return build + 68 + 34 * (build - at[0][1]) / (at[5][1] - at[0][1]);
}

struct Loops {
  long double avg_ns;
  bool fits;
  bool late; /* a pkt met the wire unbuilt */
  /* each session's shortest lead at a visit inside a frame, warm-up frame included */
  std::vector<int64_t> lead_ns;
};

class St20TxRoundOracle : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_txv_init());
    const uint32_t dims[kFmts][2] = {
        {1280, 720}, {1920, 1080}, {3840, 2160}, {1920, 1080}, {1920, 1080}};
    const enum st_fps fps[kFmts] = {ST_FPS_P59_94, ST_FPS_P59_94, ST_FPS_P59_94,
                                    ST_FPS_P50, ST_FPS_P60};
    const float trained[1] = {164};
    for (int f = 0; f < kFmts; f++) {
      ctx_[f] = ut_txv_create();
      ASSERT_NE(nullptr, ctx_[f]);
      ut_txv_set_rl_format(ctx_[f], dims[f][0], dims[f][1], false, fps[f]);
      ASSERT_EQ(0, ut_txv_init_rl_pacing(ctx_[f], 1, 0, false, trained, false));
    }
  }
  void TearDown() override {
    for (ut_txv_ctx* ctx : ctx_) ut_txv_destroy(ctx);
  }

  long double Budget(const std::vector<Fmt>& load) const {
    long double budget = std::numeric_limits<long double>::max();
    for (Fmt f : load)
      budget = std::min(budget, ut_txv_bulk(ctx_[f]) * ut_txv_pacing_frame_time(ctx_[f]) /
                                    ut_txv_total_pkts(ctx_[f]));
    return budget;
  }

  struct Sim {
    long double frame_time, trs, tr_offset, visit_ns, pkt_ns;
    long pkts, bulk, built;
    long double phase;
  };

  /* session k of format f, paced by its user k * stagger_ns_ later */
  Sim MakeSim(Fmt f, const Cost* costs, long double pkt_ns, size_t k) const {
    const long double frame_time = ut_txv_pacing_frame_time(ctx_[f]);
    return {frame_time,
            ut_txv_pacing_trs(ctx_[f]),
            ut_txv_pacing_tr_offset(ctx_[f]),
            costs[f].visit_ns,
            pkt_ns != 0 ? pkt_ns : costs[f].pkt_ns,
            ut_txv_total_pkts(ctx_[f]),
            (long)ut_txv_bulk(ctx_[f]),
            ring_,
            fmodl(k * stagger_ns_, frame_time)};
  }

  /* the pkts due at the wire by t, one a trs from each frame's tr_offset */
  static long Due(const Sim& s, long double t) {
    t -= s.phase;
    long frame = (long)floorl(t / s.frame_time);
    long double in = t - frame * s.frame_time - s.tr_offset;
    return frame * s.pkts + (in < 0 ? 0 : std::min(s.pkts, (long)floorl(in / s.trs) + 1));
  }

  /* the time the next pkt s builds is due at the wire, less now, inside a frame */
  static bool Lead(const Sim& s, long double now, int64_t* lead_ns) {
    if (s.built % s.pkts == 0) return false;
    *lead_ns = (int64_t)floorl(s.phase + (s.built / s.pkts) * s.frame_time + s.tr_offset +
                               (s.built % s.pkts) * s.trs - now);
    return true;
  }

  /* the loops of load streaming, each format at costs[] */
  Loops Run(const std::vector<Fmt>& load, const Cost* costs, const Other& other = {},
            bool footprint = false) const {
    std::vector<Sim> sims;
    for (Fmt f : load)
      sims.push_back(MakeSim(f, costs, footprint ? FootprintPktNs((int)load.size()) : 0,
                             sims.size()));
    long double frame_time = sims[0].frame_time, end = frames_ * frame_time;
    if (window_ns_ != 0) {
      for (const Sim& s : sims) frame_time = std::max(frame_time, s.frame_time);
      end = frame_time + window_ns_;
    }
    for (Sim& s : sims) s.built += Due(s, start_ns_);
    end += start_ns_;
    Loops l = {0, true, false, std::vector<int64_t>(sims.size(), INT64_MAX)};
    long double now = start_ns_, measured_from = -1,
                stall_at = other.at_epoch ? ceill(now / frame_time) * frame_time
                                          : now + frame_time / 2,
                work_at = now;
    uint64_t loops = 0;

    while (now < end) {
      const long double start = now;
      if (other.stall_ns != 0 && now >= stall_at) {
        now += other.stall_ns;
        stall_at += frame_time;
      }
      if (other.work_ns != 0 && now >= work_at) {
        now += other.work_ns;
        work_at += other.period_ns;
      }
      for (size_t i = 0; i < sims.size(); i++) {
        Sim& s = sims[i];
        const long due_now = Due(s, now);
        if (s.built < due_now) l.late = true;
        int64_t lead;
        if (Lead(s, now, &lead)) l.lead_ns[i] = std::min(l.lead_ns[i], lead);
        const long add = std::clamp((long)ring_ - (s.built - due_now), 0L, s.bulk);
        s.built += add;
        now += s.visit_ns + add * s.pkt_ns;
      }
      now += (now - start) * other.share / (1 - other.share);
      if (start < start_ns_ + frame_time) continue;
      if (measured_from < 0) measured_from = start;
      loops++;
    }
    l.avg_ns = (now - measured_from) / loops;
    l.fits = !l.late && l.avg_ns <= Budget(load);
    return l;
  }

  /* the err lines of the stats dump of load streaming at l */
  int StatErrs(const std::vector<Fmt>& load, const Loops& l) {
    int errs = 0, locked = 0;
    bool seen[kFmts] = {};
    int64_t lead[kFmts];
    std::fill(lead, lead + kFmts, INT64_MAX);
    for (size_t i = 0; i < load.size(); i++) {
      seen[load[i]] = true;
      lead[load[i]] = std::min(lead[load[i]], l.lead_ns[i]);
    }
    for (int f = 0; f < kFmts; f++) {
      if (!seen[f]) continue;
      ut_txv_set_sch_avg_loop_ns(ctx_[f], (uint64_t)llroundl(l.avg_ns));
      ut_txv_set_lead_ns(ctx_[f], lead[f]);
      ut_txv_run_sessions_stat(ctx_[f], &locked);
      errs += ut_txv_stat_err_lines();
    }
    return errs;
  }

  struct Sweep {
    int limit = 0; /* the most sessions of the order that fit */
    bool fell_behind = false;
    int silent = 0, false_alarms = 0; /* simulated loads the stats dump misjudges */
  };

  /* the loads order[0..n), n from first on, up to the first that falls behind */
  Sweep Grow(const std::vector<Fmt>& order, const Cost* costs, const Other& other,
             bool footprint, size_t first = 1) {
    Sweep w;
    w.limit = (int)first - 1;
    for (size_t n = first; n <= order.size(); n++) {
      const std::vector<Fmt> load(order.begin(), order.begin() + (std::ptrdiff_t)n);
      const Loops l = Run(load, costs, other, footprint);
      const bool errs = StatErrs(load, l);
      w.silent += !l.fits && !errs;
      w.false_alarms += l.fits && errs;
      if (!l.fits) {
        w.fell_behind = true;
        break;
      }
      w.limit = (int)n;
    }
    return w;
  }

  static std::vector<Fmt> Repeat(std::vector<Fmt> pattern, int n) {
    std::vector<Fmt> order;
    for (int i = 0; i < n; i++) order.push_back(pattern[i % pattern.size()]);
    return order;
  }

  void SetRing(unsigned int ring) {
    ring_ = ring;
  }

  ut_txv_ctx* ctx_[kFmts] = {};
  const Cost costs_[kFmts] = {k720Cost, k1080Cost, k2160Cost, k1080Cost, k1080Cost};
  unsigned int ring_ = kRingCount;
  int frames_ = kFrames;
  long double window_ns_ = 0; /* simulated past the longest frame, 0 for frames_ */
  long double start_ns_ = 0;  /* the time the simulation starts at */
  long double stagger_ns_ = 0;
};

struct Mix {
  const char* name;
  std::vector<Fmt> head; /* created first */
  std::vector<Fmt> tail; /* then repeated */
  Other other;
  bool footprint;       /* FootprintPktNs() for every format */
  unsigned int ring;    /* ring_count, 0 for kRingCount */
  bool long_window;     /* a whole loop avg span, from 4 below the limit of frames_ */
  long double start_ns; /* of the simulation, so frame rates start at their phase there */
  long double
      stagger_ns; /* session k's frames k * stagger_ns late, as a user paces them */
};

/* tail repeated, each load simulated over a whole loop avg span from start_ns */
Mix Window(const char* name, std::vector<Fmt> tail, long double start_ns = 0,
           Other other = {}, long double stagger_ns = 0) {
  Mix m = {name, {}, tail, other};
  m.long_window = true;
  m.start_ns = start_ns;
  m.stagger_ns = stagger_ns;
  return m;
}

/* tail repeated, session k's frames k * stagger_ns late, as its user paces them */
Mix Stagger(const char* name, std::vector<Fmt> tail, long double stagger_ns) {
  Mix m = {name, {}, tail};
  m.stagger_ns = stagger_ns;
  return m;
}

class St20TxRoundLoads : public St20TxRoundOracle,
                         public ::testing::WithParamInterface<Mix> {
 protected:
  Sweep GrowMix() {
    const Mix& m = GetParam();
    if (m.ring) SetRing(m.ring);
    start_ns_ = m.start_ns;
    stagger_ns_ = m.stagger_ns;
    std::vector<Fmt> order = m.head;
    for (Fmt f : Repeat(m.tail, kMaxSessions - (int)m.head.size())) order.push_back(f);
    Sweep w = Grow(order, costs_, m.other, m.footprint);
    if (m.long_window) {
      const Sweep frames = w;
      window_ns_ = kWindowNs;
      w = Grow(order, costs_, m.other, m.footprint, std::max(1, w.limit - 4));
      w.silent += frames.silent;
      w.false_alarms += frames.false_alarms;
    }
    return w;
  }
};

/* every load the simulation shows falling behind errs in the stats dump, and no load
 * that keeps up does */
TEST_P(St20TxRoundLoads, ALoadErrsIffItFallsBehind) {
  const Sweep w = GrowMix();
  EXPECT_TRUE(w.fell_behind) << GetParam().name << ": limit " << w.limit;
  EXPECT_EQ(0, w.silent) << GetParam().name << ": limit " << w.limit;
  EXPECT_EQ(0, w.false_alarms) << GetParam().name << ": limit " << w.limit;
}

INSTANTIATE_TEST_SUITE_P(
    Loads, St20TxRoundLoads,
    ::testing::Values(
        Mix{"all_720p", {}, {k720}}, Mix{"all_1080p", {}, {k1080}},
        Mix{"all_2160p", {}, {k2160}}, Mix{"720p_1080p_alternating", {}, {k720, k1080}},
        Mix{"1080p_then_720p", {k1080, k1080, k1080, k1080, k1080, k1080}, {k720}},
        Mix{"720p_then_2160p", {k720, k720, k720, k720}, {k2160}},
        Mix{"1080p_then_2160p", {k1080, k1080, k1080}, {k2160}},
        /* the ancillary and audio work at the window open */
        Mix{"720p_beside_a_242us_stall", {}, {k720}, {242000, 0, 0}},
        Mix{"2160p_beside_a_242us_stall", {}, {k2160}, {242000, 0, 0}},
        Mix{"2160p_beside_a_480us_stall", {}, {k2160}, {480000, 0, 0}},
        /* a hardware mix: 32 ancillary and an audio session at the epoch */
        Mix{"uhd_hd_mix_beside_anc_audio_at_the_epoch",
            {k2160, k2160, k1080, k1080, k1080, k720, k720},
            {k720},
            {240000, 1000000, 233, 0, true}},
        /* 16 audio pkts a ms, at the 720p pkt cost */
        Mix{"1080p_beside_audio", {}, {k1080}, {0, 62500, 233}},
        Mix{"720p_at_the_l2_cliff", {}, {k720}, {}, true},
        Mix{"2160p_then_720p_at_the_l2_cliff", {k2160, k2160}, {k720}, {}, true},
        /* work in every loop: RX polled on the scheduler; a deep ring */
        Mix{"1080p_beside_rx_every_loop", {}, {k1080}, {0, 0, 0, 0.2L}},
        Mix{"720p_beside_rx_every_loop", {}, {k720}, {0, 0, 0, 0.4L}},
        Mix{"1080p_ring_4096", {}, {k1080}, {}, false, 4096},
        /* whole 2 s windows of frame rates whose gaps meet, and never meet */
        Window("window_1080p59_1080p50", {k1080, k1080p50}),
        Window("window_720p59_1080p60_at_8s", {k720, k1080p60}, 8.3e9L),
        /* user-paced, session k's frames k * 8 ms late */
        Stagger("stagger_2160p59_8ms", {k2160}, 8e6L)),
    [](const ::testing::TestParamInfo<Mix>& info) {
      return std::string(info.param.name);
    });

}  // namespace
