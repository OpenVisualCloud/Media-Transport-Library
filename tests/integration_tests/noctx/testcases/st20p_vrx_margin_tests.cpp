/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

/* ST20p RL pacing on the wire under load, judged by the library's RX timing parser and
 * packet 0 on NIC RX timestamps (runRlMix(), expectRlWire()), and the equal VRX margin
 * of every ST 2022-7 session. See README.md, "RL wire oracle" and "Equal VRX margin
 * oracle".
 */

#include <dirent.h>

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
#include <limits>
#include <map>
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
  size_t underflows = 0;
};

/* Mean of vrx_min less its lowest 1 %, and its percentiles. */
LegVrx summarize(std::vector<int32_t> vrx) {
  LegVrx leg;
  leg.frames = vrx.size();
  if (vrx.empty()) return leg;
  leg.underflows = std::count_if(vrx.begin(), vrx.end(), [](int32_t v) { return v < 0; });
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
                          const std::vector<int>& tx_sch, double frame_time_ns) {
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

/* One RL load on the strict topology, MTL's PTP time on the RX PHC (PhcFollowingClock):
 * sessions video sessions of one format, all default pacing. The first upfront are
 * created before mtl_start(), the rest one per create_gap_ms into the streaming
 * schedulers once the windows open, with stop_at_negative until a received frame is
 * negative. rx (empty: every one) lists the up-front video sessions received and judged
 * (RlWireCheck). Session i is named rl_tx_<i>. */
struct RlMix {
  const char* name;
  int sessions;
  uint width, height;
  enum st_fps fps;
  /* empty: every video session */
  std::vector<int> rx;
  bool redundant = false;
  /* One scheduler's quota holds every video session, so the round alone decides. */
  bool one_quota = false;
  int upfront = INT32_MAX;
  int create_gap_ms = 0;
  bool stop_at_negative = false;
  /* Negative frames are expected, and each must be flagged. */
  bool loud = false;
  /* With loud: the load first keeps up, and no received session errs in a period its
   * scheduler kept up before a frame is negative. */
  bool keeps_up_first = false;
  bool pkt0_never_early = true;
  /* An unjudgeable window fails tagged RL_INVALID, which gtest.sh retries once. */
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
  /* Per rx, over the window: RX packets lost and frames without an RX buffer, TX
   * get_next_frame calls with no frame. */
  std::vector<uint64_t> lost_pkts, user_busy;
  /* Per rx: its session's index in run.tx and the RlLogTap names. */
  std::vector<int> rx_session;
};

/* The dump period runRlMix() sets, which the RL lines are counted in. */
constexpr int kDumpPeriodS = 1;

St20pHandler* createRlVideo(NoCtxTest* test, const RlMix& mix, int i, bool rx) {
  auto bundle = test->createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false,
      [&](St20pHandler* handler) -> FrameTestStrategy* {
        if (!rx) return nullptr;
        /* runRlMix() closes the window */
        return new RlWireCheck(handler, kPtpMinusPhcNs, INT32_MAX / 2);
      },
      [&](St20pHandler* handler) {
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
      });
  return bundle.handler;
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
  /* The TX ports' links carry every created session, both legs on a shared PF. */
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

  const int64_t u = (int64_t)ptp.maxLagNs();
  int32_t worst_vrx = INT32_MAX;
  uint64_t negative = 0, from = UINT64_MAX, to = 0;
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
    for (int port = 0; port < (mix.redundant ? 2 : 1); port++) {
      const std::vector<int32_t>& vrx = r->vrx_min[port];
      const int32_t vmin =
          vrx.empty() ? INT32_MIN : *std::min_element(vrx.begin(), vrx.end());
      const std::string leg = leg_name + " port " + std::to_string(port);
      (void)fprintf(stderr,
                    "NoCtx rl: %s: frames %zu of %" PRIu64 " (sw %" PRIu64
                    ", off band %" PRIu64 ", other epoch %" PRIu64 ", partial %" PRIu64
                    "), vrx_min min %d, negative %" PRIu64 ", pkt0 EBU-late %" PRIu64
                    ", pkt0 margin min %" PRId64 " ns, early max %" PRId64
                    " ns (u %" PRId64 " ns)\n",
                    leg.c_str(), vrx.size(), window_frames, r->sw_rx_frames[port],
                    r->off_band_frames[port], r->epoch_off_frames[port],
                    r->partial_frames[port], vmin, r->negative_frames[port],
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
      if (!mix.loud) {
        EXPECT_GE(vrx.size() + r->off_band_frames[port], window_frames)
            << leg << ": " << vrx.size() << " NIC-timed frames of " << window_frames;
        EXPECT_EQ(r->negative_frames[port], 0u)
            << leg << ": " << r->negative_frames[port] << " negative frames (vrx_min "
            << vmin << ", " << r->pkt0_late_frames[port] << " with packet 0 past "
            << "TR_offset + trs, " << r->epoch_off_frames[port] << " on another epoch)";
      }
      negative += r->negative_frames[port];
      worst_vrx = std::min(worst_vrx, vmin);
    }
  }

  /* A line printed in the dump that ends the period [dumps[d - 2], dumps[d - 1]), d =
   * dump_of(its time), tells what happened in that period. */
  const auto dump_of = [&](uint64_t t) {
    return (size_t)(std::upper_bound(tap.dumps.begin(), tap.dumps.end(), t) -
                    tap.dumps.begin());
  };
  const auto in_window = [&](const RlLogTap::Line& l) {
    const size_t d = dump_of(l.ptp_ns);
    return d >= 2 && tap.dumps[d - 2] >= from && tap.dumps[d - 2] <= to;
  };
  /* Scheduler sch printed a negative lead among the lines of dump_of() d. The round part
   * is a 2 s loop average, not tied to the period, so it excuses nothing. */
  const auto behind = [&](int sch, size_t d) {
    return std::any_of(tap.round.begin(), tap.round.end(), [&](const RlLogTap::Line& l) {
      return l.lead && l.sch == sch && dump_of(l.ptp_ns) == d;
    });
  };
  uint64_t first_negative = UINT64_MAX;
  for (const RlWireCheck* r : run.rx)
    for (uint64_t t : r->negative_ptp_ns) first_negative = std::min(first_negative, t);
  const auto before_negative = [&](const RlLogTap::Line& l) {
    const size_t d = dump_of(l.ptp_ns);
    return in_window(l) && tap.dumps[d - 1] <= first_negative;
  };
  const size_t dumps = std::count_if(tap.dumps.begin(), tap.dumps.end(),
                                     [&](uint64_t t) { return t >= from && t <= to; });
  const uint64_t window_s = to > from ? (to - from) / NS_PER_S : 0;
  EXPECT_GE(dumps + 2, window_s / kDumpPeriodS)
      << dumps << " stats dumps parsed in a " << window_s << " s window: the RL lines "
      << "cannot be counted";
  const size_t detector =
      std::count_if(tap.detector.begin(), tap.detector.end(), in_window);
  const size_t round = std::count_if(tap.round.begin(), tap.round.end(), in_window);
  if (!mix.loud) {
    EXPECT_TRUE(detector == 0 && round == 0)
        << detector << " \"frames may break VRX\" and " << round
        << " \"frames fall behind\" lines in the window, on a load every received frame "
           "met";
  } else {
    EXPECT_GT(negative, 0u) << "no negative frame: the load is too light to show that "
                               "an overloaded scheduler is loud";
    std::set<int> schs;
    for (size_t i = 0; i < run.rx.size(); i++) {
      const auto at = tap.session_of.find(run.rx_session[i]);
      if (at == tap.session_of.end()) {
        ADD_FAILURE() << "rx " << i << ": no stats line names rl_tx_"
                      << run.rx_session[i];
        continue;
      }
      const int sch = at->second.first, idx = at->second.second;
      schs.insert(sch);
      const auto* handler = static_cast<St20pHandler*>(run.rx[i]->parent);
      const uint64_t frame_ns =
          (uint64_t)(NS_PER_S / st_frame_rate(handler->sessionsOpsTx.fps));
      const auto mine = [&](const RlLogTap::Line& l) {
        return l.sch == sch && l.idx == idx;
      };
      /* its detector line printed in the dump that ends the period of dump_of() d */
      const auto flagged = [&](size_t d) {
        return std::any_of(tap.detector.begin(), tap.detector.end(), [&](const auto& l) {
          return mine(l) && dump_of(l.ptp_ns) == d + 1;
        });
      };
      uint64_t silent = 0;
      for (uint64_t t : run.rx[i]->negative_ptp_ns) {
        const size_t d = dump_of(t);
        /* a pkt late after the next dump began is counted in the dump after it */
        if (!flagged(d) &&
            !(d < tap.dumps.size() && tap.dumps[d] - t < frame_ns && flagged(d + 1)))
          silent++;
      }
      EXPECT_EQ(silent, 0u) << "rx " << i << " (tx sch " << sch << " session " << idx
                            << "): " << silent << " of "
                            << run.rx[i]->negative_ptp_ns.size()
                            << " negative frames without its \"frames may break VRX\" "
                               "line in their stats period";
      if (mix.keeps_up_first) {
        const size_t early = std::count_if(
            tap.detector.begin(), tap.detector.end(), [&](const RlLogTap::Line& l) {
              return mine(l) && before_negative(l) && !behind(sch, dump_of(l.ptp_ns));
            });
        EXPECT_EQ(early, 0u)
            << "rx " << i << ": " << early
            << " \"frames may break VRX\" lines while every frame met and "
               "its scheduler kept up";
      }
    }
    for (int sch : schs) {
      const auto on_sch = [&](const RlLogTap::Line& l) { return l.sch == sch; };
      EXPECT_TRUE(std::any_of(tap.round.begin(), tap.round.end(),
                              [&](const auto& l) { return on_sch(l) && in_window(l); }))
          << "tx sch " << sch << " sent negative frames and never printed its cause, "
          << "\"frames fall behind\", in the window";
      if (!mix.keeps_up_first) continue;
      size_t kept_up = 0;
      for (size_t d = 1; d < tap.dumps.size(); d++)
        if (tap.dumps[d - 1] >= from && tap.dumps[d] <= first_negative &&
            !behind(sch, d + 1))
          kept_up++;
      EXPECT_GT(kept_up, 0u) << "no stats period in the window before the first negative "
                                "frame: the load never kept up";
    }
  }

  std::map<int, int> per_sch;
  for (const RlCreated& c : run.tx) per_sch[c.sch]++;
  std::string sessions = "{";
  for (const auto& s : per_sch)
    sessions += (sessions.size() > 1 ? "," : "") + std::string("\"") +
                std::to_string(s.first) + "\":" + std::to_string(s.second);
  sessions += "}";
  (void)printf(
      "RL_TREND {\"case\":\"%s\",\"worst_vrx_min\":%d,\"sessions_per_sch\":%s}\n",
      mix.name, worst_vrx, sessions.c_str());
  (void)fflush(stdout);
}

/* Runs the mix for kSt20pVrxWarmupS from the first frame, then the staged creates, and
 * kSt20pVrxWindowS more, and judges it (expectRlWire()); placed()
 * sees the TX schedulers before any RX is created. */
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
        std::max<uint16_t>(ctx->para.tx_queues_cnt[p], mix.sessions + 8);
  test->initStrictPacingContext(PhcFollowingClock::now);
  if (::testing::Test::IsSkipped() || ::testing::Test::HasFatalFailure()) return run;

  const int upfront = std::min(mix.upfront, mix.sessions);
  const auto is_rx = [&](int i) {
    return i < upfront &&
           (mix.rx.empty() || std::find(mix.rx.begin(), mix.rx.end(), i) != mix.rx.end());
  };
  for (int i = 0; i < upfront; i++) {
    St20pHandler* h = createRlVideo(test, mix, i, is_rx(i));
    if (!h->sessionsHandleTx) return run;
    run.tx.push_back({h, st20p_tx_get_sch_idx(h->sessionsHandleTx)});
  }
  if (placed) placed(run.tx);
  if (::testing::Test::HasFatalFailure()) return run;
  for (int i = 0; i < upfront; i++) {
    if (!is_rx(i)) continue;
    St20pHandler* h = run.tx[i].handler;
    h->createSessionRx();
    if (!h->sessionsHandleRx) return run;
    run.rx.push_back(static_cast<RlWireCheck*>(h->frameTestStrategy));
    run.rx_sch.push_back(run.tx[i].sch);
    run.rx_session.push_back(i);
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
  const auto any_negative = [&]() {
    return std::any_of(run.rx.begin(), run.rx.end(), [](const RlWireCheck* r) {
      return r->any_negative.load(std::memory_order_acquire);
    });
  };
  for (int i = upfront; i < mix.sessions && !::testing::Test::HasFailure() &&
                        !(mix.stop_at_negative && any_negative());
       i++) {
    const auto at = std::chrono::steady_clock::now();
    St20pHandler* h = createRlVideo(test, mix, i, false);
    if (!h->sessionsHandleTx) break;
    h->startSessionTx();
    run.tx.push_back({h, st20p_tx_get_sch_idx(h->sessionsHandleTx)});
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
 *            1. one TX scheduler holds >= 3 sessions and >= 2 are in use
 *            after stop, expectRlWire() on both legs of every session, its invalid
 *            windows failing as any other check: the compliant load, so every frame
 *            of the window NIC timed with vrx_min >= 0, packet 0 never early and no
 *            "frames may break VRX" or "frames fall behind" line in the window; then
 *            expectEqualVrxMargin():
 *            2. per session and port, <= 1 % of the frames + kRtpWrapOffBandFrames
 *               with rtp_offset off the K shift by >= T/2
 *            3. per port, each session's vrx_min mean less its lowest 1 % >= the
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
    int most = 0;
    std::string packing;
    for (const auto& sch : sessions_per_sch) {
      most = std::max(most, sch.second);
      packing += " sch " + std::to_string(sch.first) + ": " + std::to_string(sch.second);
    }
    ASSERT_TRUE(sessions_per_sch.size() >= 2 && most >= 3)
        << "TX sessions per scheduler:" << packing
        << "; the check needs one scheduler with >= 3 sessions and >= 2 schedulers";
  });
  if (IsSkipped() || HasFatalFailure() || run.rx.size() != kSessions) return;
  expectEqualVrxMargin({run.rx.begin(), run.rx.end()}, run.rx_sch,
                       NS_PER_S / st_frame_rate(ST_FPS_P59_94));
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
 *            received, then one a second into the streaming scheduler until a received
 *            frame is negative, at most as many as the TX link carries
 *            (linkSessions1080p()); then kSt20pVrxWindowS more.
 * Expect:    expectRlWire() with negative frames expected, packet 0 not judged early:
 *            1. >= 1 negative frame received
 *            2. each in a stats period with its session's "frames may break VRX" line,
 *               and the scheduler printed "frames fall behind"
 *            3. >= 1 stats period of the window ends before the first negative frame
 *               without a negative lead in the scheduler's "frames fall behind", and
 *               none of those holds a received session's "frames may break VRX". A
 *               period that fell behind did not keep up: there the detector may flag
 *               ahead of the wire (the VRX margin case judges both lines on a
 *               compliant load)
 */
TEST_F(NoCtxTest, st20p_rl_staged_overload_is_loud) {
  const int n = linkSessions1080p(ctx);
  if (!n) return;
  RlMix mix{"st20p_rl_staged_overload_is_loud", n, 1920, 1080, ST_FPS_P59_94};
  mix.rx = {0, 1};
  mix.one_quota = true;
  mix.upfront = 2;
  mix.create_gap_ms = 1000;
  mix.stop_at_negative = true;
  mix.loud = true;
  mix.keeps_up_first = true;
  /* Each create commits the VF's RL hierarchy, which can release packet 0 of the queues
   * already streaming up to ~0.7 us early: a VF effect, not judged here. */
  mix.pkt0_never_early = false;
  runRlMix(this, ctx, mix);
}
