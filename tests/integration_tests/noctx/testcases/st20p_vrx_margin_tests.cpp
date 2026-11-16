/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

/* ST20p RL pacing on the wire under load, judged by the library's RX timing parser and
 * packet 0 on NIC RX timestamps (runRlMix(), expectRlWire()), and the equal VRX margin
 * of every ST 2022-7 session. See README.md, "RL wire oracle" and "Equal VRX margin
 * oracle".
 */

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/constants.hpp"
#include "core/test_fixture.hpp"
#include "handlers/st20p_handler.hpp"
#include "strategies/st20p_strategies.hpp"

namespace {
/* Indexes into --port_list, interleaved as in CI: TX on PF A, RX on PF B. */
constexpr int kTxPortP = 0;
constexpr int kRxPortP = 1;
constexpr int kTxPortR = 2;
constexpr int kRxPortR = 3;
constexpr int kSessions = 8;
constexpr uint16_t kUdpPortBase = 20000;
constexpr double kVrxMarginMaxDiffPkts = 1.0;
/* The RTP wrap every ~13.25 h is not handled: ~1 s of frames then falls off band. */
constexpr uint64_t kRtpWrapOffBandFrames = 61;
/* MTL's PTP time - the RX PHC: 60 1080p59.94 frames exactly, so the TX epochs keep
 * their phase on the PHC, and far above the delivery latency of a software RX time. */
constexpr int64_t kPtpMinusPhcNs = 1001 * static_cast<int64_t>(NS_PER_MS);
static_assert(kPtpMinusPhcNs * 60 % (1001 * static_cast<int64_t>(NS_PER_MS)) == 0,
              "a whole number of frames");

struct LegVrx {
  size_t frames = 0;
  double mean = 0;
  int32_t p1 = 0, p5 = 0, p50 = 0;
};

/* Mean of vrx_min less its lowest 1 %, and its percentiles. */
LegVrx summarize(std::vector<int32_t> vrx) {
  LegVrx leg;
  leg.frames = vrx.size();
  if (vrx.empty()) return leg;
  std::sort(vrx.begin(), vrx.end());
  const size_t trim = vrx.size() / 100;
  for (size_t k = trim; k < vrx.size(); k++) leg.mean += vrx[k];
  leg.mean /= static_cast<double>(vrx.size() - trim);
  leg.p1 = vrx[vrx.size() / 100];
  leg.p5 = vrx[vrx.size() / 20];
  leg.p50 = vrx[vrx.size() / 2];
  return leg;
}

/* The checks expectRlWire() does not make; every session and port is logged first. */
void expectEqualVrxMargin(const std::vector<St20pVrxRecorder*>& rx,
                          const std::vector<int>& tx_sch) {
  if (std::all_of(rx.begin(), rx.end(),
                  [](const auto* r) { return r->delivery_ns.empty(); })) {
    ADD_FAILURE() << "no frames in the measurement window";
    return;
  }

  LegVrx legs[MTL_SESSION_PORT_MAX][kSessions];
  for (int port = 0; port < MTL_SESSION_PORT_MAX; port++) {
    for (size_t i = 0; i < rx.size(); i++) {
      const LegVrx& leg = legs[port][i] = summarize(rx[i]->vrx_min[port]);
      (void)fprintf(
          stderr,
          "NoCtx vrx: session %zu port %d tx sch %d: frames %zu, off band %" PRIu64
          ", missing max %u pkts, vrx_min trimmed mean %.2f p1 %d p5 %d p50 %d\n",
          i, port, tx_sch[i], leg.frames, rx[i]->off_band_frames[port],
          rx[i]->max_missing_pkts[port], leg.mean, leg.p1, leg.p5, leg.p50);
    }
  }

  for (int port = 0; port < MTL_SESSION_PORT_MAX; port++) {
    double best = -std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < rx.size(); i++) {
      const LegVrx& leg = legs[port][i];
      EXPECT_LE(rx[i]->off_band_frames[port], leg.frames / 100 + kRtpWrapOffBandFrames)
          << "session " << i << " port " << port << ": " << rx[i]->off_band_frames[port]
          << " frames with rtp_offset more than half a frame from the K shift, so not "
             "timed against their own epoch";
      if (leg.frames) best = std::max(best, leg.mean);
    }
    for (size_t i = 0; i < rx.size(); i++) {
      if (!legs[port][i].frames) continue;
      EXPECT_GE(legs[port][i].mean, best - kVrxMarginMaxDiffPkts)
          << "session " << i << " port " << port << " on tx sch " << tx_sch[i]
          << ": trimmed mean vrx_min " << legs[port][i].mean << " vs best " << best;
    }
  }
}

/* MTL's log lines that tell an RL frame or scheduler at risk, counted with the MTL PTP
 * time they were printed at; every line still goes to stderr. One tap per process. */
class RlLogTap {
 public:
  struct Line {
    uint64_t ptp_ns;
    int sch, idx;
    bool lead; /* "frames fall behind" with its " lead -" part */
  };
  RlLogTap() {
    std::lock_guard<std::mutex> lock(tapMutex());
    tap = this;
    mtl_set_log_printer(print);
  }
  ~RlLogTap() {
    mtl_set_log_printer(nullptr);
    std::lock_guard<std::mutex> lock(tapMutex());
    tap = nullptr;
  }
  std::mutex mutex;
  std::vector<uint64_t> dumps;
  /* "frames may break VRX >= 0", per session; "frames fall behind", per scheduler. */
  std::vector<Line> detector, round;
  /* runRlMix() session i ("rl_tx_<i>") -> its scheduler and index there. */
  std::map<int, std::pair<int, int>> session_of;

 private:
  static RlLogTap* tap;
  static std::mutex& tapMutex() {
    static std::mutex m;
    return m;
  }
  static void print(enum mtl_log_level /*level*/, const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    (void)fputs(line, stderr);
    std::lock_guard<std::mutex> lock(tapMutex());
    if (tap) tap->parse(line);
  }
  void parse(const char* line) {
    const uint64_t now = PhcFollowingClock::now(nullptr);
    const char* p;
    int sch = -1, idx = -1, tx = -1;
    std::lock_guard<std::mutex> lock(mutex);
    if (strstr(line, "M T    D E V   S T A T E")) dumps.push_back(now);
    p = strstr(line, "TX_VIDEO_SESSION(");
    if (p && sscanf(p, "TX_VIDEO_SESSION(%d,%d:rl_tx_%d)", &sch, &idx, &tx) == 3)
      session_of[tx] = {sch, idx};
    if (p && strstr(p, "frames may break VRX") &&
        sscanf(p, "TX_VIDEO_SESSION(%d,%d)", &sch, &idx) == 2)
      detector.push_back({now, sch, idx, false});
    if (p && strstr(p, "frames fall behind") &&
        sscanf(p, "TX_VIDEO_SESSION(%d)", &sch) == 1)
      round.push_back({now, sch, -1, strstr(p, " lead -") != nullptr});
  }
};
RlLogTap* RlLogTap::tap = nullptr;

/* The sysfs device of port's PF: port itself, or a VF's physfn. */
std::string pfPath(const char* port) {
  const std::string dev = std::string("/sys/bus/pci/devices/") + port;
  char pf[PATH_MAX];
  return realpath((dev + "/physfn").c_str(), pf) ? pf : dev;
}

/* The link speed of port's PF in bit/s, 0 if unknown. */
uint64_t linkBps(const char* port) {
  const std::string net = pfPath(port) + "/net";
  DIR* dir = opendir(net.c_str());
  if (!dir) return 0;
  uint64_t mbps = 0;
  for (dirent* e; !mbps && (e = readdir(dir));) {
    if (e->d_name[0] == '.') continue;
    FILE* f = fopen((net + "/" + e->d_name + "/speed").c_str(), "r");
    if (!f) continue;
    if (fscanf(f, "%" SCNu64, &mbps) != 1) mbps = 0;
    (void)fclose(f);
  }
  closedir(dir);
  return mbps * 1000 * 1000;
}

uint64_t sessionBps(uint width, uint height, enum st_fps fps) {
  uint64_t bps = 0;
  EXPECT_EQ(
      st20_get_bandwidth_bps(width, height, ST20_FMT_YUV_422_10BIT, fps, false, &bps), 0);
  return bps;
}

/* The chance B1 allows of failing a cell whose loaded and control frames are alike. */
constexpr double kWorseAlpha = 1e-3;
/* B1 judges the low tail, up to the control's 5 % quantile of the value judged. Above
 * it, a loaded scheduler later by a fraction of a slot, still well in VRX, would be
 * significant over ~10 k frames. */
constexpr double kWorseTail = 0.05;
/* A tail test shows the loaded frames worse only with at least this share of them at or
 * below its threshold, and twice the control's share. */
constexpr double kWorseMinShare = 0.001;

double logChoose(double n, double k) {
  return std::lgamma(n + 1) - std::lgamma(k + 1) - std::lgamma(n - k + 1);
}

/* One-sided Fisher exact test: the chance, if both groups were alike, that a or more of
 * the a + c frames at or below a threshold are among the n_a loaded frames, c being
 * among the n_c control ones. */
double fisherWorseP(uint64_t a, uint64_t n_a, uint64_t c, uint64_t n_c) {
  const double n = (double)(n_a + n_c), k = (double)(a + c);
  const double all = logChoose(n, (double)n_a);
  double p = 0;
  for (uint64_t x = a; x <= std::min(a + c, n_a); x++)
    p += std::exp(logChoose(k, (double)x) + logChoose(n - k, (double)(n_a - x)) - all);
  return std::min(p, 1.0);
}

/* The control frames with the value judged <= t, the loaded ones with it <= t - 1, and
 * their fisherWorseP(). */
struct TailTest {
  double t;
  uint64_t a, n_a, c, n_c;
  double p;
};

/* A TailTest at each distinct value among the control's lowest value and its 1 % and
 * kWorseTail quantiles, in slots. A loaded frame counts one slot lower than a control
 * one, so a loaded group later by less than a slot never counts, nor, for d, a +-1 of
 * the parser's truncation; packet 0's margin is not truncated. Few thresholds keep the
 * Bonferroni alpha high enough for a sparse tail. */
std::vector<TailTest> tailTests(std::vector<double> loaded, std::vector<double> control) {
  std::vector<TailTest> tests;
  if (loaded.empty() || control.empty()) return tests;
  std::sort(loaded.begin(), loaded.end());
  std::sort(control.begin(), control.end());
  std::vector<double> at = {control.front(), control[control.size() / 100],
                            control[(size_t)(kWorseTail * (double)control.size())]};
  at.erase(std::unique(at.begin(), at.end()), at.end());
  for (double t : at) {
    const uint64_t c =
        std::upper_bound(control.begin(), control.end(), t) - control.begin();
    const uint64_t a =
        std::upper_bound(loaded.begin(), loaded.end(), t - 1) - loaded.begin();
    tests.push_back({t, a, loaded.size(), c, control.size(),
                     fisherWorseP(a, loaded.size(), c, control.size())});
  }
  return tests;
}

/* The verdict on tailTests(): worse when a test has p < alpha, kWorseMinShare of the
 * loaded frames and twice the control's share. t is the worse test with the smallest p,
 * else the test with the smallest p. */
struct Worse {
  bool tested = false, worse = false;
  TailTest t{};
  /* what: the value judged */
  std::string str(const char* what) const {
    if (!tested) return "no test";
    char s[224];
    (void)snprintf(s, sizeof(s),
                   "p %.3g at %s <= %.2f (control %" PRIu64 " of %" PRIu64
                   ", loaded %" PRIu64 " of %" PRIu64 " at %s <= %.2f)%s",
                   t.p, what, t.t, t.c, t.n_c, t.a, t.n_a, what, t.t - 1,
                   worse ? ", worse" : "");
    return s;
  }
};
Worse judgeTail(const std::vector<TailTest>& tests, double alpha) {
  Worse w;
  for (const TailTest& t : tests) {
    const bool worse = t.p < alpha && (double)t.a >= kWorseMinShare * (double)t.n_a &&
                       (double)t.a * (double)t.n_c >= 2.0 * (double)t.c * (double)t.n_a;
    if (!w.tested || (worse && !w.worse) || (worse == w.worse && t.p < w.t.p)) {
      w.t = t;
      w.worse = worse;
    }
    w.tested = true;
  }
  return w;
}

/* What B1 judges per frame, in Trs: d, and packet 0's margin (TR_offset - fpt) / Trs,
 * which a whole frame late moves while it keeps its d. */
constexpr int kMeasures = 2;
constexpr const char* kMeasure[kMeasures] = {"d", "pkt0 margin"};
constexpr const char* kMeasureKey[kMeasures] = {"d", "pkt0"};
const std::vector<double>& measureOf(const RlWireCheck* r, int m, int port) {
  return m ? r->pkt0_margin_trs[port] : r->vrx_rel[port];
}

/* min, p1, p5 and p50 of values, as JSON. */
std::string quantiles(std::vector<double> values) {
  if (values.empty()) return "{\"frames\":0}";
  std::sort(values.begin(), values.end());
  const auto q = [&](size_t per) { return values[values.size() * per / 100]; };
  char s[128];
  (void)snprintf(s, sizeof(s),
                 "{\"frames\":%zu,\"min\":%.2f,\"p1\":%.2f,\"p5\":%.2f,\"p50\":%.2f}",
                 values.size(), q(0), q(1), q(5), q(50));
  return s;
}

/* The frames of one port's loaded or control group, by vrx_min. */
struct VrxGroup {
  std::vector<int32_t> vrx;
  int sessions = 0;
  int32_t min = 0, p1 = 0, p50 = 0;
  size_t below_1 = 0, below_0 = 0;
  void summarize() {
    if (vrx.empty()) return;
    std::sort(vrx.begin(), vrx.end());
    min = vrx.front();
    p1 = vrx[vrx.size() / 100];
    p50 = vrx[vrx.size() / 2];
    below_1 = std::lower_bound(vrx.begin(), vrx.end(), 1) - vrx.begin();
    below_0 = std::lower_bound(vrx.begin(), vrx.end(), 0) - vrx.begin();
  }
  /* " value:frames" for each value seen up to upto */
  std::string histogram(int32_t upto) const {
    std::string h;
    for (auto it = vrx.begin(); it != vrx.end() && *it <= upto;) {
      const auto end = std::upper_bound(it, vrx.end(), *it);
      h += " " + std::to_string(*it) + ":" + std::to_string(end - it);
      it = end;
    }
    return h;
  }
  std::string json() const {
    char s[160];
    (void)snprintf(s, sizeof(s),
                   "{\"frames\":%zu,\"min\":%d,\"p1\":%d,\"p50\":%d,\"below_1\":%zu,"
                   "\"below_0\":%zu}",
                   vrx.size(), min, p1, p50, below_1, below_0);
    return s;
  }
};

/* One RL load on the strict topology, MTL's PTP time on the RX PHC (PhcFollowingClock):
 * sessions + control video sessions of one format, all default pacing. The first
 * upfront are created before mtl_start(), the rest of the sessions one per
 * create_gap_ms into the streaming schedulers once the windows open, with loud until
 * the loaded scheduler prints "frames fall behind" with a lead. rx (empty: every one)
 * lists the up-front video sessions received and judged (RlWireCheck); the control ones,
 * created last, are received too. Session i is named rl_tx_<i>. */
struct RlMix {
  const char* name;
  int sessions;
  uint width, height;
  enum st_fps fps;
  /* empty: every up-front video session */
  std::vector<int> rx = {};
  bool redundant = false;
  /* One scheduler's quota holds the sessions, so the round alone decides, and no
   * control one. */
  bool one_quota = false;
  /* Sessions upfront to sessions - 1 hold their place on the loaded scheduler while
   * these are created, so they go to another one. */
  int control = 0;
  int upfront = INT32_MAX;
  int create_gap_ms = 0;
  /* The load overloads its scheduler, and the library must say so (expectLoud()). */
  bool loud = false;
  bool pkt0_never_early = true;
  /* An unjudgeable window fails tagged RL_INVALID, unlike a pacing failure. */
  bool tag_invalid = true;
};

struct RlCreated {
  St20pHandler* handler;
  int sch;
};

struct RlRun {
  std::vector<RlWireCheck*> rx;
  std::vector<int> rx_sch;
  std::vector<RlCreated> tx;
  /* The TX scheduler with the most sessions; received sessions elsewhere are the
   * control. */
  int loaded_sch = -1;
  /* Per rx, over the window: RX packets lost and frames without an RX buffer, TX
   * get_next_frame calls with no frame. */
  std::vector<uint64_t> lost_pkts, user_busy;
  /* Per rx: its session's i, the RlLogTap name rl_tx_<i>. */
  std::vector<int> rx_session;
};

/* The dump period runRlMix() sets, which the RL lines are counted in. */
constexpr int kDumpPeriodS = 1;

/* The dumps by t: a frame at t is in the period [dumps[d - 1], dumps[d]), and a line
 * printed at t in the dump that ends the period [dumps[d - 2], dumps[d - 1]). */
size_t dumpOf(const std::vector<uint64_t>& dumps, uint64_t t) {
  return std::upper_bound(dumps.begin(), dumps.end(), t) - dumps.begin();
}

void configureRlVideo(St20pHandler* handler, const RlMix& mix, int i) {
  handler->fillSt20Ops(kUdpPortBase + 2 * i, 3, ST20_FMT_YUV_422_10BIT, mix.width,
                       mix.height, 112, mix.fps);
  static std::deque<std::string> names;
  names.push_back("rl_tx_" + std::to_string(i));
  handler->sessionsOpsTx.name = names.back().c_str();
  handler->sessionsOpsTx.input_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_BLOCK_GET;
  handler->sessionsOpsRx.output_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  handler->sessionsOpsRx.flags |=
      ST20P_RX_FLAG_BLOCK_GET | ST20P_RX_FLAG_TIMING_PARSER_META;
  if (mix.redundant)
    handler->setSessionPorts(kTxPortP, kRxPortP, kTxPortR, kRxPortR);
  else
    handler->setSessionPorts(kTxPortP, kRxPortP);
}

St20pHandler* createRlVideo(NoCtxTest* test, const RlMix& mix, int i, bool rx) {
  auto bundle = test->createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false,
      [&](St20pHandler* handler) -> FrameTestStrategy* {
        if (!rx) return nullptr;
        /* runRlMix() closes the window */
        return new RlWireCheck(handler, kPtpMinusPhcNs, INT32_MAX / 2);
      },
      [&](St20pHandler* handler) { configureRlVideo(handler, mix, i); });
  return bundle.handler;
}

/* The overload cell, per stats period and received session on the loaded scheduler. A
 * period is library-worse when its frames' d or packet 0 margin are worse than the
 * control's of the same period (judgeTail(), Bonferroni over every test made here), or
 * when a frame has packet 0 past TR_offset or is on another epoch. Only a "frames fall
 * behind" line with its lead part counts for the loaded scheduler: the round part is a 2
 * s loop average, not tied to the period. Returns the library-worse periods. */
size_t expectLoud(const RlRun& run, const RlLogTap& tap, int legs, uint64_t from,
                  uint64_t to,
                  const std::function<void(bool, const std::string&)>& expectRlValid) {
  const std::vector<uint64_t>& dumps = tap.dumps;
  const auto judged = [&](size_t d) {
    return d >= 1 && d < dumps.size() && dumps[d - 1] >= from && dumps[d - 1] <= to;
  };
  /* A line printed in the dump that ends period d. */
  const auto of_period = [&](const RlLogTap::Line& l, size_t d) {
    return dumpOf(dumps, l.ptp_ns) == d + 1;
  };
  const auto in_window = [&](const RlLogTap::Line& l) {
    return judged(dumpOf(dumps, l.ptp_ns) - 1);
  };
  std::vector<std::pair<int, int>> name(run.rx.size(), {-1, -1});
  for (size_t i = 0; i < run.rx.size(); i++) {
    const auto at = tap.session_of.find(run.rx_session[i]);
    if (at == tap.session_of.end()) {
      ADD_FAILURE() << "rx " << i << ": no stats line names rl_tx_" << run.rx_session[i];
      continue;
    }
    name[i] = at->second;
  }
  const auto flagged = [&](size_t i, size_t d) {
    return std::any_of(tap.detector.begin(), tap.detector.end(), [&](const auto& l) {
      return l.sch == name[i].first && l.idx == name[i].second && of_period(l, d);
    });
  };
  const auto behind = [&](int sch, size_t d) {
    return std::any_of(tap.round.begin(), tap.round.end(), [&](const auto& l) {
      return l.lead && l.sch == sch && of_period(l, d);
    });
  };
  struct Frames {
    std::vector<double> value[kMeasures];
    std::vector<int32_t> raw;
  };
  using ByPeriod = std::map<size_t, Frames>;
  std::vector<size_t> loaded, control;
  for (size_t i = 0; i < run.rx.size(); i++)
    (run.rx_sch[i] == run.loaded_sch ? loaded : control).push_back(i);
  std::vector<ByPeriod> frames[MTL_SESSION_PORT_MAX];
  ByPeriod ctls[MTL_SESSION_PORT_MAX];
  for (int port = 0; port < legs; port++) {
    frames[port].resize(run.rx.size());
    for (size_t i = 0; i < run.rx.size(); i++) {
      const RlWireCheck* r = run.rx[i];
      for (size_t k = 0; k < r->vrx_min[port].size(); k++) {
        Frames& f = frames[port][i][dumpOf(dumps, r->frame_ptp_ns[port][k])];
        for (int m = 0; m < kMeasures; m++)
          f.value[m].push_back(measureOf(r, m, port)[k]);
        f.raw.push_back(r->vrx_min[port][k]);
      }
    }
    for (size_t i : control) {
      for (const auto& p : frames[port][i]) {
        Frames& c = ctls[port][p.first];
        for (int m = 0; m < kMeasures; m++)
          c.value[m].insert(c.value[m].end(), p.second.value[m].begin(),
                            p.second.value[m].end());
      }
    }
  }
  const auto lowest = [](const std::vector<double>& v) {
    return *std::min_element(v.begin(), v.end());
  };

  struct Period {
    size_t d, rx;
    int port;
    size_t frames, control_frames;
    std::vector<TailTest> tests[kMeasures];
    Worse w[kMeasures];
  };
  std::vector<Period> periods;
  size_t tests = 0, with_frames = 0, skipped = 0;
  for (int port = 0; port < legs; port++) {
    for (size_t i : loaded) {
      for (const auto& p : frames[port][i]) {
        if (!judged(p.first)) continue;
        with_frames++;
        const auto c = ctls[port].find(p.first);
        if (c == ctls[port].end()) {
          skipped++;
          continue;
        }
        Period period{};
        period.d = p.first;
        period.rx = i;
        period.port = port;
        period.frames = p.second.raw.size();
        period.control_frames = c->second.value[0].size();
        for (int m = 0; m < kMeasures; m++) {
          period.tests[m] = tailTests(p.second.value[m], c->second.value[m]);
          tests += period.tests[m].size();
        }
        periods.push_back(std::move(period));
      }
    }
  }
  const double alpha = kWorseAlpha / (double)std::max<size_t>(tests, 1);

  std::map<size_t, std::set<size_t>> worse; /* per loaded rx */
  for (Period& p : periods) {
    const double at_s = (double)(dumps[p.d - 1] - from) / NS_PER_S;
    const char* check_1 = flagged(p.rx, p.d) ? "yes" : "no";
    const char* check_2 = behind(run.loaded_sch, p.d) ? "yes" : "no";
    for (int m = 0; m < kMeasures; m++) {
      p.w[m] = judgeTail(p.tests[m], alpha);
      if (p.w[m].worse) worse[p.rx].insert(p.d);
      (void)fprintf(stderr,
                    "NoCtx rl period %s %zu (+%.0f s): rx %zu port %d: frames %zu, "
                    "control %zu, %s (alpha %.3g), check 1 %s, check 2 lead %s\n",
                    kMeasureKey[m], p.d, at_s, p.rx, p.port, p.frames, p.control_frames,
                    p.w[m].str(kMeasure[m]).c_str(), alpha, check_1, check_2);
    }
  }
  (void)fprintf(
      stderr,
      "NoCtx rl overload: %zu of %zu periods of received loaded sessions have no "
      "control frames, not judged\n",
      skipped, with_frames);
  EXPECT_LE(skipped * 10, with_frames)
      << skipped << " of " << with_frames << " periods of received loaded sessions have "
      << "no control frames to be judged against";
  size_t worse_periods = 0;
  size_t first = SIZE_MAX;
  for (size_t i : loaded) {
    const auto* handler = static_cast<St20pHandler*>(run.rx[i]->parent);
    const uint64_t frame_ns =
        (uint64_t)(NS_PER_S / st_frame_rate(handler->sessionsOpsTx.fps));
    const auto loud = [&](size_t d) {
      return flagged(i, d) && behind(run.loaded_sch, d);
    };
    std::set<size_t> periods_worse = worse[i];
    size_t silent = 0, late = 0, late_silent = 0;
    for (size_t d : worse[i])
      if (!loud(d)) silent++;
    for (uint64_t t : run.rx[i]->late_ptp_ns) {
      const size_t d = dumpOf(dumps, t);
      if (!judged(d)) continue;
      late++;
      periods_worse.insert(d);
      /* a pkt late after the next dump began is counted in the dump after it */
      if (!loud(d) && !(d < dumps.size() && dumps[d] - t < frame_ns && loud(d + 1)))
        late_silent++;
    }
    if (!periods_worse.empty()) first = std::min(first, *periods_worse.begin());
    worse_periods += periods_worse.size();
    (void)fprintf(stderr,
                  "NoCtx rl overload: rx %zu (tx sch %d session %d): %zu library-worse "
                  "periods, %zu of them by d or packet 0's margin (%zu silent), %zu "
                  "frames with packet 0 late or on another epoch (%zu silent)\n",
                  i, name[i].first, name[i].second, periods_worse.size(), worse[i].size(),
                  silent, late, late_silent);
    EXPECT_TRUE(silent == 0 && late_silent == 0)
        << "rx " << i << " (tx sch " << name[i].first << " session " << name[i].second
        << "): " << silent << " library-worse periods and " << late_silent
        << " frames with packet 0 late or on another epoch without its \"frames may "
        << "break VRX\" line and the scheduler's \"frames fall behind\" line with a lead "
        << "in their stats period";
  }
  expectRlValid(worse_periods > 0,
                "no library-worse period: the load is too light to show that an "
                "overloaded scheduler is loud");
  /* The wire shows session i's frames of period d were fine: none with d or packet 0's
   * margin more than a Trs below the control's lowest of the period, none with
   * vrx_min < 0. */
  const auto wire_fine = [&](size_t i, size_t d) {
    for (int port = 0; port < legs; port++) {
      const auto m = frames[port][i].find(d);
      const auto c = ctls[port].find(d);
      if (m == frames[port][i].end() || c == ctls[port].end()) return false;
      const Frames& f = m->second;
      for (int k = 0; k < kMeasures; k++)
        if (lowest(f.value[k]) < lowest(c->second.value[k]) - 1) return false;
      if (*std::min_element(f.raw.begin(), f.raw.end()) < 0) return false;
    }
    return true;
  };
  if (worse_periods) {
    size_t quiet = 0, early = 0;
    for (size_t d = 1; d < first; d++) {
      if (!judged(d)) continue;
      const bool round = behind(run.loaded_sch, d);
      bool detector = false;
      for (size_t i : loaded) {
        if (!flagged(i, d)) continue;
        detector = true;
        if (!round && wire_fine(i, d)) early++;
      }
      if (!round && !detector) quiet++;
    }
    EXPECT_GT(quiet, 0u) << "no stats period of the window before the first "
                         << "library-worse one without a received session's "
                         << "\"frames may break VRX\" line or tx sch " << run.loaded_sch
                         << "'s \"frames fall behind\" line with a lead";
    EXPECT_EQ(early, 0u)
        << early << " \"frames may break VRX\" lines of received "
        << "sessions before the first library-worse period on frames the "
        << "wire shows fine, in periods where tx sch " << run.loaded_sch
        << " printed no \"frames fall behind\" line with a lead";
  }
  /* Pooled over a session's periods with neither line, sparse silent frames add up. */
  struct Pool {
    size_t rx;
    int port;
    size_t periods;
    Frames loaded, control;
    std::vector<TailTest> tests[kMeasures];
  };
  const auto append = [](Frames& pooled, const Frames& period) {
    for (int m = 0; m < kMeasures; m++)
      pooled.value[m].insert(pooled.value[m].end(), period.value[m].begin(),
                             period.value[m].end());
  };
  std::vector<Pool> pools;
  size_t pool_tests = 0;
  for (int port = 0; port < legs; port++) {
    for (size_t i : loaded) {
      Pool pool{};
      pool.rx = i;
      pool.port = port;
      for (const auto& p : frames[port][i]) {
        const auto c = ctls[port].find(p.first);
        if (!judged(p.first) || c == ctls[port].end() || flagged(i, p.first) ||
            behind(run.loaded_sch, p.first))
          continue;
        pool.periods++;
        append(pool.loaded, p.second);
        append(pool.control, c->second);
      }
      for (int m = 0; m < kMeasures; m++) {
        pool.tests[m] = tailTests(pool.loaded.value[m], pool.control.value[m]);
        pool_tests += pool.tests[m].size();
      }
      pools.push_back(std::move(pool));
    }
  }
  const double pool_alpha = kWorseAlpha / (double)std::max<size_t>(pool_tests, 1);
  for (const Pool& pool : pools) {
    for (int m = 0; m < kMeasures; m++) {
      const Worse w = judgeTail(pool.tests[m], pool_alpha);
      (void)fprintf(stderr,
                    "NoCtx rl silent pool %s: rx %zu port %d: %zu periods with neither "
                    "line, frames %zu, control %zu, %s (alpha %.3g over %zu tests)\n",
                    kMeasureKey[m], pool.rx, pool.port, pool.periods,
                    pool.loaded.value[m].size(), pool.control.value[m].size(),
                    w.str(kMeasure[m]).c_str(), pool_alpha, pool_tests);
      EXPECT_FALSE(w.worse) << "rx " << pool.rx << " port " << pool.port << ": its "
                            << kMeasure[m] << " in the " << pool.periods
                            << " stats periods with neither line is worse than the "
                            << "control's: " << w.str(kMeasure[m]);
    }
  }
  for (size_t i : control) {
    const size_t detector =
        std::count_if(tap.detector.begin(), tap.detector.end(), [&](const auto& l) {
          return l.sch == name[i].first && l.idx == name[i].second && in_window(l);
        });
    const size_t round = std::count_if(
        tap.round.begin(), tap.round.end(),
        [&](const auto& l) { return l.sch == name[i].first && in_window(l); });
    EXPECT_TRUE(detector == 0 && round == 0)
        << "control rx " << i << " (tx sch " << name[i].first << "): " << detector
        << " \"frames may break VRX\" and " << round << " \"frames fall behind\" lines "
        << "in the window";
  }
  return worse_periods;
}

/* The verdict on every received leg, after logging it, and one RL_TREND line. */
void expectRlWire(const RlMix& mix, RlRun& run, RlLogTap& tap, PhcFollowingClock& ptp,
                  st_tests_context* ctx) {
  std::lock_guard<std::mutex> lock(tap.mutex);
  const auto expectRlValid = [&mix](bool ok, const std::string& why) {
    EXPECT_TRUE(ok) << (mix.tag_invalid ? "RL_INVALID: " : "invalid window: ") << why;
  };
  const char* isolation = getenv("MTL_CPU_ISOLATION");
  expectRlValid(isolation && !strcmp(isolation, "exclusive"),
                std::string("MTL_CPU_ISOLATION=") + (isolation ? isolation : "<unset>") +
                    ", needs an exclusive partition (tests/tools/isolate/isolate.sh)");
  const std::string follower = ptp.healthError();
  expectRlValid(follower.empty(), follower);
  /* The TX ports' links carry every streaming session, both legs on a shared PF. */
  uint64_t tx_bps = 0;
  for (const RlCreated& c : run.tx) {
    const auto& ops = c.handler->sessionsOpsTx;
    tx_bps += sessionBps(ops.width, ops.height, ops.fps);
  }
  const uint64_t link_p = linkBps(ctx->para.port[kTxPortP]);
  const bool shared_pf = mix.redundant && pfPath(ctx->para.port[kTxPortP]) ==
                                              pfPath(ctx->para.port[kTxPortR]);
  const uint64_t need_p = tx_bps * (shared_pf ? 2 : 1);
  expectRlValid(link_p >= need_p, "the TX link runs at " + std::to_string(link_p) +
                                      " bit/s, the load needs " + std::to_string(need_p));

  const int legs = mix.redundant ? 2 : 1;
  const int64_t u = (int64_t)ptp.maxLagNs();
  int32_t worst_vrx = INT32_MAX;
  uint64_t from = UINT64_MAX, to = 0;
  for (size_t i = 0; i < run.rx.size(); i++) {
    RlWireCheck* r = run.rx[i];
    auto* handler = static_cast<St20pHandler*>(r->parent);
    const St20pFrameRate rate = st20pFrameRate(handler->sessionsOpsTx.fps);
    from = std::min(from, r->window_from_ptp_ns);
    to = std::max(to, r->window_to_ptp_ns);
    /* the frames from the first to the last of the window */
    const uint64_t window_frames =
        r->window_to_ptp_ns > r->window_from_ptp_ns
            ? (uint64_t)llround(
                  (long double)(r->window_to_ptp_ns - r->window_from_ptp_ns) * rate.mul /
                  (NS_PER_S * rate.den)) +
                  1
            : 1;
    const std::string leg_name = "rx " + std::to_string(i) + " (" +
                                 std::to_string(handler->sessionsOpsTx.height) +
                                 "p) on tx sch " + std::to_string(run.rx_sch[i]);
    expectRlValid(run.lost_pkts[i] == 0, leg_name + ": " +
                                             std::to_string(run.lost_pkts[i]) +
                                             " packets lost, or frames the RX app was "
                                             "too slow for");
    expectRlValid(run.user_busy[i] == 0, leg_name + ": the TX app was late " +
                                             std::to_string(run.user_busy[i]) + " times");
    for (int port = 0; port < legs; port++) {
      const std::vector<int32_t>& vrx = r->vrx_min[port];
      const int32_t vmin =
          vrx.empty() ? INT32_MIN : *std::min_element(vrx.begin(), vrx.end());
      const auto below = [&vrx](int32_t v) {
        return (size_t)std::count_if(vrx.begin(), vrx.end(),
                                     [v](int32_t x) { return x < v; });
      };
      const std::string leg = leg_name + " port " + std::to_string(port);
      (void)fprintf(stderr,
                    "NoCtx rl: %s: frames %zu of %" PRIu64 " (sw %" PRIu64
                    ", off band %" PRIu64 ", other epoch %" PRIu64 ", partial %" PRIu64
                    "), vrx_min min %d, below 1 %zu, below 0 %zu, pkt0 late %" PRIu64
                    ", pkt0 margin min %" PRId64 " ns, early max %" PRId64
                    " ns (u %" PRId64 " ns)\n",
                    leg.c_str(), vrx.size(), window_frames, r->sw_rx_frames[port],
                    r->off_band_frames[port], r->epoch_off_frames[port],
                    r->partial_frames[port], vmin, below(1), below(0),
                    r->pkt0_late_frames[port], r->pkt0_margin_min_ns[port],
                    r->pkt0_early_max_ns[port], u);
      expectRlValid(r->sw_rx_frames[port] == 0,
                    leg + ": frames timed with software RX times");
      if (mix.redundant)
        expectRlValid(r->partial_frames[port] <= vrx.size() / 100,
                      leg + ": the leg finished more than " +
                          std::to_string(kSt20pVrxMaxLegLagPkts) +
                          " packets behind the other in " +
                          std::to_string(r->partial_frames[port]) + " frames");
      if (mix.pkt0_never_early) {
        EXPECT_LE(r->pkt0_early_max_ns[port], u)
            << leg << ": packet 0 up to " << r->pkt0_early_max_ns[port]
            << " ns before its launch target, more than the PTP follower's " << u
            << " ns";
      }
      if (!mix.loud || run.rx_sch[i] != run.loaded_sch) {
        EXPECT_GE(vrx.size() + r->off_band_frames[port], window_frames)
            << leg << ": " << vrx.size() << " NIC-timed frames of " << window_frames;
      }
      if (!mix.loud) {
        EXPECT_EQ(r->pkt0_late_frames[port], 0u)
            << leg << ": " << r->pkt0_late_frames[port] << " frames with packet 0 past "
            << "TR_offset";
        EXPECT_EQ(r->epoch_off_frames[port], 0u)
            << leg << ": " << r->epoch_off_frames[port] << " frames on another epoch";
      }
      worst_vrx = std::min(worst_vrx, vmin);
    }
  }

  /* B1 on d, vrx_min less packet 0's unrounded vrx, so a constant delay of a session's or
   * a frame's packets cancels, and on packet 0's margin, which a whole frame late moves:
   * the loaded scheduler not worse than the control in the low tail of either, with a
   * loaded frame counted a slot lower. Each fails only if it still does with every
   * loaded session left out in turn, so a defect of one loaded session alone is left to
   * the equal margin check. C1: the median frame stretched by <= 2 Trs. The pad rule
   * alone leaves up to about one pad of stretch by its phase in the frame, a platform
   * delay of a few frames' last packets moves their max but not the median, and a
   * shaper 0.05 % slower than trained adds 2 Trs. */
  double trs_ns = 0;
  for (const RlWireCheck* r : run.rx)
    if (trs_ns <= 0) trs_ns = r->trs_ns;
  expectRlValid(trs_ns > 0, "no received session read its pacing params in the window");
  VrxGroup groups[MTL_SESSION_PORT_MAX][2];
  std::vector<double> values[kMeasures][MTL_SESSION_PORT_MAX][2];
  std::vector<TailTest> b1_tests[kMeasures][MTL_SESSION_PORT_MAX];
  double stretch_median[MTL_SESSION_PORT_MAX] = {},
         stretch_max[MTL_SESSION_PORT_MAX] = {};
  size_t b1_count = 0;
  for (int port = 0; port < legs; port++) {
    std::vector<double> stretch;
    for (size_t i = 0; i < run.rx.size(); i++) {
      const int k = run.rx_sch[i] == run.loaded_sch ? 0 : 1;
      const RlWireCheck* r = run.rx[i];
      groups[port][k].vrx.insert(groups[port][k].vrx.end(), r->vrx_min[port].begin(),
                                 r->vrx_min[port].end());
      groups[port][k].sessions++;
      for (int m = 0; m < kMeasures; m++)
        values[m][port][k].insert(values[m][port][k].end(), measureOf(r, m, port).begin(),
                                  measureOf(r, m, port).end());
      stretch.insert(stretch.end(), r->stretch_ns[port].begin(),
                     r->stretch_ns[port].end());
    }
    VrxGroup floors[2];
    for (int k = 0; k < 2; k++) {
      groups[port][k].summarize();
      for (double x : values[0][port][k]) floors[k].vrx.push_back((int32_t)std::floor(x));
      floors[k].summarize();
    }
    for (int m = 0; m < kMeasures; m++) {
      b1_tests[m][port] = tailTests(values[m][port][0], values[m][port][1]);
      b1_count += b1_tests[m][port].size();
    }
    for (int k = 0; k < 2; k++) {
      const char* group = k ? "control" : "loaded";
      const VrxGroup& g = groups[port][k];
      (void)fprintf(
          stderr,
          "NoCtx rl group: port %d %s (%d sessions): frames %zu, vrx_min min %d "
          "p1 %d p50 %d, below 1 %zu, below 0 %zu, frames per vrx_min to the "
          "control's median:%s\n",
          port, group, g.sessions, g.vrx.size(), g.min, g.p1, g.p50, g.below_1, g.below_0,
          g.histogram(mix.loud ? INT32_MIN : groups[port][1].p50).c_str());
      (void)fprintf(stderr,
                    "NoCtx rl group d: port %d %s: %s, frames per floor(d) to the "
                    "control's median:%s\n",
                    port, group, quantiles(values[0][port][k]).c_str(),
                    floors[k].histogram(mix.loud ? INT32_MIN : floors[1].p50).c_str());
      (void)fprintf(stderr, "NoCtx rl group pkt0: port %d %s: margin in Trs %s\n", port,
                    group, quantiles(values[1][port][k]).c_str());
    }
    if (stretch.empty()) continue;
    std::sort(stretch.begin(), stretch.end());
    stretch_median[port] = stretch[stretch.size() / 2];
    stretch_max[port] = stretch.back();
    (void)fprintf(stderr,
                  "NoCtx rl stretch: port %d: frames %zu, min %.0f median %.0f p99 %.0f "
                  "max %.0f ns (Trs %.0f ns)\n",
                  port, stretch.size(), stretch.front(), stretch_median[port],
                  stretch[stretch.size() * 99 / 100], stretch.back(), trs_ns);
    if (!mix.loud) {
      EXPECT_LE(stretch_median[port], 2 * trs_ns)
          << "port " << port << ": the median frame's packets took "
          << stretch_median[port]
          << " ns longer than (packets - 1) * Trs, over 2 Trs: the shaper is slower "
             "than trained";
    }
  }
  const double b1_alpha = kWorseAlpha / (double)std::max<size_t>(b1_count, 1);
  /* per value and port: the full test, and the best with one loaded session out */
  Worse b1[kMeasures][MTL_SESSION_PORT_MAX], b1_best[kMeasures][MTL_SESSION_PORT_MAX];
  for (int m = 0; m < kMeasures; m++) {
    for (int port = 0; port < legs; port++) {
      b1[m][port] = judgeTail(b1_tests[m][port], b1_alpha);
      Worse& best = b1_best[m][port];
      Worse worst;
      int best_out = -1, worst_out = -1;
      for (size_t out = 0; out < run.rx.size(); out++) {
        if (run.rx_sch[out] != run.loaded_sch) continue;
        std::vector<double> rest;
        for (size_t i = 0; i < run.rx.size(); i++) {
          if (run.rx_sch[i] != run.loaded_sch || i == out) continue;
          const std::vector<double>& v = measureOf(run.rx[i], m, port);
          rest.insert(rest.end(), v.begin(), v.end());
        }
        const Worse w = judgeTail(tailTests(rest, values[m][port][1]), b1_alpha);
        /* best: not worse, then largest p; worst: worse, then smallest p */
        if (best_out < 0 || (!w.worse && best.worse) ||
            (w.worse == best.worse && w.t.p > best.t.p)) {
          best = w;
          best_out = (int)out;
        }
        if (worst_out < 0 || (w.worse && !worst.worse) ||
            (w.worse == worst.worse && w.t.p < worst.t.p)) {
          worst = w;
          worst_out = (int)out;
        }
      }
      (void)fprintf(
          stderr,
          "NoCtx rl B1 %s: port %d: loaded tx sch %d against the control: %s; one "
          "loaded session out in turn, best without rx %d: %s, worst without "
          "rx %d: %s; alpha %.3g over %zu tests\n",
          kMeasureKey[m], port, run.loaded_sch, b1[m][port].str(kMeasure[m]).c_str(),
          best_out, best.str(kMeasure[m]).c_str(), worst_out,
          worst.str(kMeasure[m]).c_str(), b1_alpha, b1_count);
      if (!mix.loud) {
        EXPECT_FALSE(b1[m][port].worse && best.worse)
            << "port " << port << ": the " << kMeasure[m] << " of loaded tx sch "
            << run.loaded_sch << " is worse than the control's, "
            << b1[m][port].str(kMeasure[m]) << ", and with any one loaded session out, "
            << "at best without rx " << best_out << ", " << best.str(kMeasure[m]);
      }
    }
  }

  const size_t dumps = std::count_if(tap.dumps.begin(), tap.dumps.end(),
                                     [&](uint64_t t) { return t >= from && t <= to; });
  const uint64_t window_s = to > from ? (to - from) / NS_PER_S : 0;
  EXPECT_GE(dumps + 2, window_s / kDumpPeriodS)
      << dumps << " stats dumps parsed in a " << window_s << " s window: the RL lines "
      << "cannot be counted";
  size_t worse_periods = 0;
  if (!mix.loud) {
    /* A line printed in the dump that ends a period that began in the window. */
    const auto in_window = [&](const RlLogTap::Line& l) {
      const size_t d = dumpOf(tap.dumps, l.ptp_ns);
      return d >= 2 && tap.dumps[d - 2] >= from && tap.dumps[d - 2] <= to;
    };
    const size_t detector =
        std::count_if(tap.detector.begin(), tap.detector.end(), in_window);
    const size_t round = std::count_if(tap.round.begin(), tap.round.end(), in_window);
    EXPECT_TRUE(detector == 0 && round == 0)
        << detector << " \"frames may break VRX\" and " << round
        << " \"frames fall behind\" lines in the window, on a load every received frame "
           "met";
  } else {
    worse_periods = expectLoud(run, tap, legs, from, to, expectRlValid);
  }

  std::map<int, int> per_sch;
  for (const RlCreated& c : run.tx) per_sch[c.sch]++;
  std::string sessions = "{";
  for (const auto& s : per_sch)
    sessions += (sessions.size() > 1 ? "," : "") + std::string("\"") +
                std::to_string(s.first) + "\":" + std::to_string(s.second);
  sessions += "}";
  std::string ports = "[";
  for (int port = 0; port < legs; port++) {
    ports += (port ? ",{\"port\":" : "{\"port\":") + std::to_string(port) +
             ",\"loaded\":" + groups[port][0].json() +
             ",\"control\":" + groups[port][1].json();
    for (int m = 0; m < kMeasures; m++) {
      const Worse& w = b1[m][port];
      const Worse& best = b1_best[m][port];
      char s[160];
      (void)snprintf(s, sizeof(s),
                     ",\"b1_p\":%.3g,\"b1_t\":%.2f,\"b1_worse\":%s,\"loo_best_p\":%.3g,"
                     "\"loo_worse\":%s}",
                     w.tested ? w.t.p : 1.0, w.t.t, w.worse ? "true" : "false",
                     best.tested ? best.t.p : 1.0, best.worse ? "true" : "false");
      ports += ",\"" + std::string(kMeasureKey[m]) +
               "\":{\"loaded\":" + quantiles(values[m][port][0]) +
               ",\"control\":" + quantiles(values[m][port][1]) + s;
    }
    char s[96];
    (void)snprintf(s, sizeof(s), ",\"stretch_median_ns\":%.0f,\"stretch_max_ns\":%.0f}",
                   stretch_median[port], stretch_max[port]);
    ports += s;
  }
  ports += "]";
  char host[256] = "";
  (void)gethostname(host, sizeof(host) - 1);
  (void)printf(
      "RL_TREND {\"case\":\"%s\",\"host\":\"%s\",\"worst_vrx_min\":%d,"
      "\"sessions_per_sch\":%s,\"loaded_sch\":%d,\"ports\":%s,\"worse_periods\":%zu}\n",
      mix.name, host, worst_vrx, sessions.c_str(), run.loaded_sch, ports.c_str(),
      worse_periods);
  (void)fflush(stdout);
}

/* Runs the mix for kSt20pVrxWarmupS from the first frame, then the staged creates, and
 * kSt20pVrxWindowS more, and judges it (expectRlWire()); placed() sees the TX
 * schedulers of the up-front and control sessions before any RX is created. */
RlRun runRlMix(
    NoCtxTest* test, st_tests_context* ctx, const RlMix& mix,
    const std::function<void(const std::vector<RlCreated>&)>& placed = nullptr) {
  RlRun run;
  const int ports = mix.redundant ? 4 : 2;
  if ((int)ctx->para.num_ports < ports) {
    ADD_FAILURE() << "needs " << ports << " ports";
    return run;
  }
  std::vector<std::string> unsupported = {
      strictPacingTopologyError(ctx->para.port[kTxPortP], ctx->para.port[kRxPortP])};
  if (mix.redundant) {
    unsupported.push_back(
        strictPacingTopologyError(ctx->para.port[kTxPortR], ctx->para.port[kRxPortR]));
    unsupported.push_back(
        sharedPhcError(ctx->para.port[kRxPortP], ctx->para.port[kRxPortR]));
  }
  for (const std::string& why : unsupported) {
    NoCtxTest::requireStrictTopology(why);
    if (::testing::Test::IsSkipped() || ::testing::Test::HasFatalFailure()) return run;
  }
  PhcFollowingClock ptp;
  const std::string ptp_error = ptp.open(ctx->para.port[kRxPortP], kPtpMinusPhcNs);
  if (!ptp_error.empty()) {
    ADD_FAILURE() << ptp_error;
    return run;
  }
  /* Following through mtl_init() keeps the first read's lag to 10 ms of drift. */
  ptp.follow();
  RlLogTap tap;

  if (mix.one_quota)
    ctx->para.data_quota_mbs_per_sch =
        (uint64_t)mix.sessions *
        (sessionBps(mix.width, mix.height, mix.fps) / 1000000 + 1) *
        (mix.redundant ? 2 : 1);
  ctx->para.dump_period_s = kDumpPeriodS;
  for (int p = 0; p < ports; p++)
    ctx->para.tx_queues_cnt[p] =
        std::max<uint16_t>(ctx->para.tx_queues_cnt[p], mix.sessions + mix.control + 8);
  test->initStrictPacingContext(PhcFollowingClock::now);
  if (::testing::Test::IsSkipped() || ::testing::Test::HasFatalFailure()) return run;

  const int upfront = std::min(mix.upfront, mix.sessions);
  const auto is_rx = [&](int i) {
    return i >= mix.sessions ||
           (i < upfront && (mix.rx.empty() ||
                            std::find(mix.rx.begin(), mix.rx.end(), i) != mix.rx.end()));
  };
  std::vector<int> created;   /* the session i of each run.tx */
  std::map<int, int> per_sch; /* the places held too */
  for (int i = 0; i < upfront; i++) {
    St20pHandler* h = createRlVideo(test, mix, i, is_rx(i));
    if (!h->sessionsHandleTx) return run;
    run.tx.push_back({h, st20p_tx_get_sch_idx(h->sessionsHandleTx)});
    created.push_back(i);
    per_sch[run.tx.back().sch]++;
  }
  if (mix.control) {
    std::vector<std::unique_ptr<St20pHandler>> places;
    for (int i = upfront; i < mix.sessions; i++) {
      places.push_back(std::make_unique<St20pHandler>(ctx));
      configureRlVideo(places.back().get(), mix, i);
      places.back()->createSessionTx();
      if (!places.back()->sessionsHandleTx) return run;
      per_sch[st20p_tx_get_sch_idx(places.back()->sessionsHandleTx)]++;
    }
    for (int i = mix.sessions; i < mix.sessions + mix.control; i++) {
      St20pHandler* h = createRlVideo(test, mix, i, true);
      if (!h->sessionsHandleTx) return run;
      run.tx.push_back({h, st20p_tx_get_sch_idx(h->sessionsHandleTx)});
      created.push_back(i);
      per_sch[run.tx.back().sch]++;
    }
  }
  if (placed) placed(run.tx);
  if (::testing::Test::HasFatalFailure()) return run;
  run.loaded_sch =
      std::max_element(per_sch.begin(), per_sch.end(), [](const auto& a, const auto& b) {
        return a.second < b.second;
      })->first;
  for (size_t k = 0; k < run.tx.size(); k++) {
    if (!is_rx(created[k])) continue;
    St20pHandler* h = run.tx[k].handler;
    h->createSessionRx();
    if (!h->sessionsHandleRx) return run;
    run.rx.push_back(static_cast<RlWireCheck*>(h->frameTestStrategy));
    run.rx_sch.push_back(run.tx[k].sch);
    run.rx_session.push_back(created[k]);
  }
  const size_t control_rx = std::count_if(run.rx_sch.begin(), run.rx_sch.end(),
                                          [&](int sch) { return sch != run.loaded_sch; });
  if (control_rx < 2) {
    ADD_FAILURE() << control_rx << " received sessions off the loaded tx sch "
                  << run.loaded_sch << ": the control needs >= 2";
    return run;
  }

  EXPECT_EQ(mtl_start(ctx->handle), 0);
  for (const RlCreated& c : run.tx) {
    if (c.handler->sessionsHandleRx) c.handler->startSessionRx();
    c.handler->startSessionTx();
  }
  const auto window_stats = [&](int sign) {
    run.lost_pkts.resize(run.rx.size());
    run.user_busy.resize(run.rx.size());
    for (size_t i = 0; i < run.rx.size(); i++) {
      auto* h = static_cast<St20pHandler*>(run.rx[i]->parent);
      st20_tx_user_stats tx_stats = {};
      st20_rx_user_stats rx_stats = {};
      EXPECT_EQ(st20p_tx_get_session_stats(h->sessionsHandleTx, &tx_stats), 0);
      EXPECT_EQ(st20p_rx_get_session_stats(h->sessionsHandleRx, &rx_stats), 0);
      run.lost_pkts[i] +=
          sign * (rx_stats.common.stat_lost_packets + rx_stats.stat_slot_get_frame_fail);
      run.user_busy[i] += sign * tx_stats.stat_user_busy;
    }
  };
  test->sleepUntilFailure(kSt20pVrxWarmupS);
  window_stats(-1);
  const uint64_t staged_from = PhcFollowingClock::now(nullptr);
  /* The loaded scheduler printed "frames fall behind" with a lead for a period that
   * began staged. */
  const auto falls_behind = [&]() {
    std::lock_guard<std::mutex> lock(tap.mutex);
    return std::any_of(tap.round.begin(), tap.round.end(), [&](const RlLogTap::Line& l) {
      const size_t d = dumpOf(tap.dumps, l.ptp_ns);
      return l.lead && l.sch == run.loaded_sch && d >= 2 &&
             tap.dumps[d - 2] >= staged_from;
    });
  };
  for (int i = upfront; i < mix.sessions && !::testing::Test::HasFailure() &&
                        !(mix.loud && falls_behind());
       i++) {
    const auto at = std::chrono::steady_clock::now();
    St20pHandler* h = createRlVideo(test, mix, i, false);
    if (!h->sessionsHandleTx) break;
    h->startSessionTx();
    run.tx.push_back({h, st20p_tx_get_sch_idx(h->sessionsHandleTx)});
    EXPECT_EQ(run.tx.back().sch, run.loaded_sch)
        << "staged session " << i << " is not on the loaded tx sch";
    std::this_thread::sleep_until(at + std::chrono::milliseconds(mix.create_gap_ms));
  }
  test->sleepUntilFailure(kSt20pVrxWindowS);
  for (RlWireCheck* r : run.rx) r->closed.store(true, std::memory_order_release);
  window_stats(1);
  std::vector<std::thread> stops;
  for (const RlCreated& c : run.tx)
    stops.emplace_back([h = c.handler] { h->stopSession(); });
  for (auto& t : stops) t.join();
  ptp.stop();
  expectRlWire(mix, run, tap, ptp, ctx);
  return run;
}
}  // namespace

/* st20p_redundant_1080p59_s8_equal_vrx_margin
 * Config:    runRlMix(): 4 ports, 8 sessions 1080p59.94 YUV 4:2:2 10-bit BPM, RFC 4175
 *            frames (no conversion), 3 buffers, UDP kUdpPortBase + 2i; TX kTxPortP +
 *            kTxPortR -> RX kRxPortP + kRxPortR (ST 2022-7); default RL pacing and
 *            scheduler quota; every TX session created before any RX one.
 * Plan:      default pacing, kSt20pVrxWarmupS + kSt20pVrxWindowS = 50 s.
 * Expect:    after TX creation:
 *            1. one TX scheduler holds >= 3 sessions, >= 2 are in use, and each other
 *               one at most half as many; the one with the most is loaded, the others'
 *               >= 2 sessions are the control
 *            after stop, expectRlWire() on both legs of every session, its invalid
 *            windows failing as any other check: every frame of the window NIC timed,
 *            packet 0 never early, never past TR_offset nor on another epoch, no
 *            "frames may break VRX" or "frames fall behind" line in the window, and per
 *            port:
 *            2. B1: the loaded sessions' d (vrx_min less packet 0's unrounded vrx) and
 *               packet 0 margin (TR_offset - fpt) / Trs, a slot lower, not worse than
 *               the control's lowest 5 %: judgeTail() at alpha 1e-3 / the tests of
 *               both values and ports. Each fails only if it does with every loaded
 *               session left out in turn, so a defect of one loaded session alone is
 *               left to check 5. vrx_min < 1 is only printed: how late a packet is
 *               mid-frame depends on the host's NIC path, which both groups share
 *            3. C1: the median frame's packets took <= (packets - 1) * Trs + 2 Trs,
 *               so the shaper is not slower than trained
 *            then expectEqualVrxMargin():
 *            4. per session and port, <= 1 % of the frames + kRtpWrapOffBandFrames
 *               with rtp_offset off the K shift by >= T/2
 *            5. per port, each session's vrx_min mean less its lowest 1 % >= the
 *               best session's - 1 packet (kVrxMarginMaxDiffPkts)
 * Skip/Fail: strict topology of (kTxPortP, kRxPortP) and (kTxPortR, kRxPortR), or RX
 *            ports on different PHCs: SKIP, FAIL with NOCTX_REQUIRE_STRICT=1.
 */
TEST_F(NoCtxTest, st20p_redundant_1080p59_s8_equal_vrx_margin) {
  RlMix mix{"st20p_redundant_1080p59_s8_equal_vrx_margin", kSessions, 1920, 1080,
            ST_FPS_P59_94};
  mix.redundant = true;
  mix.tag_invalid = false;
  RlRun run = runRlMix(this, ctx, mix, [](const std::vector<RlCreated>& tx) {
    std::map<int, int> sessions_per_sch;
    for (const RlCreated& c : tx) sessions_per_sch[c.sch]++;
    int most = 0, second = 0;
    std::string packing;
    for (const auto& sch : sessions_per_sch) {
      second = std::max(second, std::min(most, sch.second));
      most = std::max(most, sch.second);
      packing += " sch " + std::to_string(sch.first) + ": " + std::to_string(sch.second);
    }
    ASSERT_TRUE(sessions_per_sch.size() >= 2 && most >= 3 && 2 * second <= most)
        << "TX sessions per scheduler:" << packing
        << "; the check needs one scheduler with >= 3 sessions, >= 2 schedulers, and "
           "each other one with at most half as many";
  });
  if (IsSkipped() || HasFatalFailure() || run.rx.size() != kSessions) return;
  expectEqualVrxMargin({run.rx.begin(), run.rx.end()}, run.rx_sch);
}

/* The 1080p59.94 sessions the TX link carries, capped at the TX video sessions one MTL
 * scheduler holds (ST_SCH_MAX_TX_VIDEO_SESSIONS); RL_INVALID on a link speed it cannot
 * read or none. */
static int linkSessions1080p(st_tests_context* ctx) {
  constexpr uint64_t kMaxTxVideoPerSch = 60;
  const uint64_t link = linkBps(ctx->para.port[kTxPortP]);
  EXPECT_GT(link, 0u) << "RL_INVALID: no link speed for " << ctx->para.port[kTxPortP];
  return (int)std::min<uint64_t>(kMaxTxVideoPerSch,
                                 link / sessionBps(1920, 1080, ST_FPS_P59_94));
}

/* st20p_rl_staged_overload_is_loud
 * Config:    runRlMix(): 1080p59.94 on one quota, sessions 0 and 1 up front and
 *            received on the loaded scheduler; sessions 2 to n - 3 hold their place
 *            there while 2 control sessions, received, are created on another one,
 *            which stays light. Then sessions 2 on, one a second, into the loaded
 *            scheduler until it prints "frames fall behind", at most n in all, as many
 *            as the TX link carries (linkSessions1080p()); then kSt20pVrxWindowS more.
 *            MTL_FLAG_TX_NO_CHAIN: the builder copies every payload, so the core
 *            overloads near half the link. Chained, one core carries the whole link,
 *            and there RX loses packets before a received frame is negative.
 * Expect:    expectRlWire() with an overload expected, packet 0 not judged early, and
 *            per stats period and received session on the loaded scheduler,
 *            library-worse when its frames' d or packet 0 margin are worse than the
 *            control's of the period (B1's tail test), or one has packet 0 past
 *            TR_offset or is on another epoch; "frames fall behind" counts only with
 *            its lead part:
 *            1. >= 1 library-worse period
 *            2. each with its session's "frames may break VRX" line and the loaded
 *               scheduler's "frames fall behind" line
 *            3. >= 1 stats period of the window before the first library-worse one
 *               with neither line, and before it no "frames may break VRX" line on
 *               frames the wire shows fine in a period without "frames fall behind"
 *            4. per received session, its frames pooled over the periods with neither
 *               line not worse than the control's of those periods (B1's tail test on
 *               d and on packet 0 margin)
 *            5. no line of the control sessions or their scheduler in the window, and
 *               every frame of the window of the control legs
 *            6. <= 10 % of the received sessions' periods without control frames
 */
TEST_F(NoCtxTest, st20p_rl_staged_overload_is_loud) {
  constexpr int kControl = 2;
  const int n = linkSessions1080p(ctx);
  if (!n) return;
  if (n < kControl + 3) {
    ADD_FAILURE() << "RL_INVALID: the TX link carries " << n << " sessions, the cell "
                  << "needs " << kControl << " control and >= 3 staged ones";
    return;
  }
  ctx->para.flags |= MTL_FLAG_TX_NO_CHAIN;
  RlMix mix{"st20p_rl_staged_overload_is_loud", n - kControl, 1920, 1080, ST_FPS_P59_94};
  mix.rx = {0, 1};
  mix.one_quota = true;
  mix.control = kControl;
  mix.upfront = 2;
  mix.create_gap_ms = 1000;
  mix.loud = true;
  /* Each create commits the VF's RL hierarchy, which can release packet 0 of the queues
   * already streaming up to ~0.7 us early: a VF effect, not judged here. */
  mix.pkt0_never_early = false;
  runRlMix(this, ctx, mix, [&mix](const std::vector<RlCreated>& tx) {
    std::set<int> loaded, control;
    std::string packing;
    for (size_t k = 0; k < tx.size(); k++) {
      (k < (size_t)mix.upfront ? loaded : control).insert(tx[k].sch);
      packing += " " + std::to_string(tx[k].sch);
    }
    ASSERT_TRUE(loaded.size() == 1 && control.size() == 1 &&
                *loaded.begin() != *control.begin())
        << "TX scheduler of the up-front and control sessions:" << packing
        << "; the check needs the control on its own scheduler";
  });
}
