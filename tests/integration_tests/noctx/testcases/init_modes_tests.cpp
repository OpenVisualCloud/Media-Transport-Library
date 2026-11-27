/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <arpa/inet.h>
#include <openssl/sha.h>
#ifndef WINDOWSENV
#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/test_fixture.hpp"
#include "st20/st20_common.h"
#include "test_harness.hpp"

/* Self-contained execution-mode integration tests.
 *
 * Each case configures mtl_init_params directly in its own clean process instead
 * of relying on global KahawaiTest CLI flags or duplicate CI runner profiles.
 */

namespace {

using ScopedSt20Tx = mtl_test::ScopedHandle<st20_tx_handle, st20_tx_free>;
using ScopedSt20Rx = mtl_test::ScopedHandle<st20_rx_handle, st20_rx_free>;
using ScopedSt20pTx = mtl_test::ScopedHandle<st20p_tx_handle, st20p_tx_free>;
using ScopedSt20pRx = mtl_test::ScopedHandle<st20p_rx_handle, st20p_rx_free>;

constexpr uint16_t kInitModeUdpPort = 22100;
constexpr uint8_t kInitModePt = 112;
constexpr int kInitModeMinRxFrames = 18;
constexpr int kInitModeTimeoutMs = 10000;

/* Scoped MTL log tap for verifying runtime mode activation (pacing, PTP, etc.). */
struct ModeLogTap {
  static inline std::atomic<bool> saw_dev_tsc{false};
  static inline std::atomic<bool> saw_dev_rl{false};
  static inline std::atomic<bool> saw_dev_be{false};
  static inline std::atomic<bool> saw_tx_tsc{false};
  static inline std::atomic<bool> saw_tx_rl{false};
  static inline std::atomic<bool> saw_tx_be{false};
  static inline std::atomic<bool> saw_fallback_tsc{false};
  static inline std::atomic<bool> saw_ptp_active_stat{false};

  static void printer(enum mtl_log_level /*level*/, const char* format, ...) {
    char buf[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (strstr(buf, "tx pacing tsc") != nullptr) {
      saw_dev_tsc.store(true, std::memory_order_release);
    }
    if (strstr(buf, "tx pacing ratelimit") != nullptr) {
      saw_dev_rl.store(true, std::memory_order_release);
    }
    if (strstr(buf, "tx pacing be") != nullptr) {
      saw_dev_be.store(true, std::memory_order_release);
    }
    if (strstr(buf, "pacing way: tsc") != nullptr) {
      saw_tx_tsc.store(true, std::memory_order_release);
    }
    if (strstr(buf, "pacing way: ratelimit") != nullptr) {
      saw_tx_rl.store(true, std::memory_order_release);
    }
    if (strstr(buf, "pacing way: be") != nullptr) {
      saw_tx_be.store(true, std::memory_order_release);
    }
    if (strstr(buf, "fallback to tsc") != nullptr) {
      saw_fallback_tsc.store(true, std::memory_order_release);
    }
    if (strstr(buf, "PTP(0): mode ") != nullptr) {
      saw_ptp_active_stat.store(true, std::memory_order_release);
    }
    fputs(buf, stderr);
  }

  ModeLogTap() {
    saw_dev_tsc.store(false, std::memory_order_relaxed);
    saw_dev_rl.store(false, std::memory_order_relaxed);
    saw_dev_be.store(false, std::memory_order_relaxed);
    saw_tx_tsc.store(false, std::memory_order_relaxed);
    saw_tx_rl.store(false, std::memory_order_relaxed);
    saw_tx_be.store(false, std::memory_order_relaxed);
    saw_fallback_tsc.store(false, std::memory_order_relaxed);
    saw_ptp_active_stat.store(false, std::memory_order_relaxed);
    mtl_set_log_printer(printer);
  }
  ~ModeLogTap() {
    mtl_set_log_printer(nullptr);
  }
  ModeLogTap(const ModeLogTap&) = delete;
  ModeLogTap& operator=(const ModeLogTap&) = delete;
};

/* Keep queue counts within the default 16-queue IAVF VF allocation so parallel
 * init_mode_* workers on sibling VFs do not trigger concurrent PF mailbox resets. */
void setCompactQueues(st_tests_context* ctx, uint16_t q = 4) {
  for (int i = 0; i < MTL_PORT_MAX; i++) {
    ctx->para.tx_queues_cnt[i] = q;
    ctx->para.rx_queues_cnt[i] = q;
  }
}

/* Counts distinct UDP source ports matching dst_port in a pcapng capture. */
size_t countDistinctUdpSrcPorts(const char* path, uint16_t dst_port) {
  std::set<uint16_t> src_ports;
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) return 0;
  const std::streamoff sz_off = file.tellg();
  if (sz_off < 28) return 0;
  const uint64_t file_size = static_cast<uint64_t>(sz_off);

  auto read_le32 = [](const uint8_t* p) -> uint32_t {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
  };
  auto read_be16 = [](const uint8_t* p) -> uint16_t {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
  };

  uint64_t off = 0;
  std::vector<uint8_t> pkt(128);
  while (off + 12u <= file_size) {
    uint8_t hdr[8];
    file.seekg(static_cast<std::streamoff>(off), std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(hdr), 8)) break;
    const uint32_t btype = read_le32(hdr);
    const uint32_t blen = read_le32(hdr + 4);
    if (blen < 12u || (blen % 4u) != 0u || off + blen > file_size) break;

    if (btype == 0x00000006u && blen >= 32u) {
      uint8_t cap_buf[4];
      file.seekg(static_cast<std::streamoff>(off + 20u), std::ios::beg);
      if (!file.read(reinterpret_cast<char*>(cap_buf), 4)) break;
      const uint32_t cap_len = read_le32(cap_buf);
      const size_t to_read = std::min<size_t>(cap_len, pkt.size());
      if (to_read >= 42u && 32u + cap_len <= blen) {
        file.seekg(static_cast<std::streamoff>(off + 28u), std::ios::beg);
        if (file.read(reinterpret_cast<char*>(pkt.data()),
                      static_cast<std::streamsize>(to_read))) {
          size_t eth_len = 14u;
          uint16_t eth_type = read_be16(pkt.data() + 12u);
          if (eth_type == 0x8100u && to_read >= eth_len + 4u) {
            eth_type = read_be16(pkt.data() + 16u);
            eth_len += 4u;
          }
          if (eth_type == 0x0800u && to_read >= eth_len + 20u) {
            const uint8_t ihl = static_cast<uint8_t>((pkt[eth_len] & 0x0fu) * 4u);
            const uint8_t proto = pkt[eth_len + 9u];
            if (proto == 17u && ihl >= 20u && to_read >= eth_len + ihl + 8u) {
              const uint16_t sport = read_be16(pkt.data() + eth_len + ihl);
              const uint16_t dport = read_be16(pkt.data() + eth_len + ihl + 2u);
              if (dport == dst_port) src_ports.insert(sport);
            }
          }
        }
      }
    }
    off += blen;
  }
  return src_ports.size();
}

struct St20pDigestLoopbackResult {
  int tx_frames = 0;
  int rx_frames = 0;
  int rx_before_start = 0;
  int sha_ok = 0;
  int sha_fail = 0;
  int tx_sch_idx = -1;
  int rx_sch_idx = -1;
  uint32_t vrx_pkts = 0;
  size_t distinct_src_ports = 0;
  uint64_t median_frame_span_ns = 0;
  double rx_fps = 0.0;
};

struct St20pLoopbackConfig {
  bool call_mtl_start = true;
  bool check_fps = false;
  bool check_multi_src_port = false;
  bool check_quiescent_before_start = false;
};

/* Runs a fast, deterministic 1080p59.94 ST20p loopback with SHA256 verification. */
St20pDigestLoopbackResult runSt20pDigestLoopback(struct st_tests_context* ctx,
                                                 uint16_t udp_port,
                                                 const St20pLoopbackConfig& cfg = {}) {
  St20pDigestLoopbackResult res;
  if (ctx->para.num_ports < 2) {
    ADD_FAILURE() << "init_mode loopback requires at least 2 ports";
    return res;
  }

  constexpr uint32_t kWidth = 1920;
  constexpr uint32_t kHeight = 1080;
  constexpr enum st_frame_fmt kFmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  constexpr enum st_fps kFps = ST_FPS_P59_94;
  constexpr uint16_t kFbCnt = 3;
  const size_t frame_sz = st_frame_size(kFmt, kWidth, kHeight, false);

  std::vector<std::vector<uint8_t>> patterns(TEST_SHA_HIST_NUM,
                                             std::vector<uint8_t>(frame_sz));
  unsigned char expected_shas[TEST_SHA_HIST_NUM][SHA256_DIGEST_LENGTH];
  for (int i = 0; i < TEST_SHA_HIST_NUM; i++) {
    st_test_rand_data(patterns[i].data(), frame_sz, 0x31u + static_cast<uint8_t>(i));
    SHA256(patterns[i].data(), frame_sz, expected_shas[i]);
  }

  struct st20p_tx_ops tx_ops = {};
  tx_ops.name = "init_mode_st20p_tx";
  mtl_test::fill_tx_port(tx_ops.port, ctx, udp_port, kInitModePt);
  tx_ops.width = kWidth;
  tx_ops.height = kHeight;
  tx_ops.fps = kFps;
  tx_ops.input_fmt = kFmt;
  tx_ops.transport_fmt = ST20_FMT_YUV_422_10BIT;
  tx_ops.framebuff_cnt = kFbCnt;
  tx_ops.flags = ST20P_TX_FLAG_BLOCK_GET;

  struct st20p_rx_ops rx_ops = {};
  rx_ops.name = "init_mode_st20p_rx";
  mtl_test::fill_rx_port(rx_ops.port, ctx, udp_port, kInitModePt);
  rx_ops.width = kWidth;
  rx_ops.height = kHeight;
  rx_ops.fps = kFps;
  rx_ops.output_fmt = kFmt;
  rx_ops.transport_fmt = ST20_FMT_YUV_422_10BIT;
  rx_ops.framebuff_cnt = kFbCnt;
  rx_ops.flags = ST20P_RX_FLAG_BLOCK_GET;

  ScopedSt20pRx rx(st20p_rx_create(ctx->handle, &rx_ops));
  EXPECT_TRUE(static_cast<bool>(rx)) << "st20p_rx_create failed";
  ScopedSt20pTx tx(st20p_tx_create(ctx->handle, &tx_ops));
  EXPECT_TRUE(static_cast<bool>(tx)) << "st20p_tx_create failed";
  if (!rx || !tx) return res;

  res.tx_sch_idx = st20p_tx_get_sch_idx(tx.get());
  res.rx_sch_idx = st20p_rx_get_sch_idx(rx.get());
  double tr_offset_ns = 0.0;
  double trs_ns = 0.0;
  EXPECT_EQ(st20p_tx_get_pacing_params(tx.get(), &tr_offset_ns, &trs_ns, &res.vrx_pkts),
            0);

  EXPECT_EQ(st20p_tx_set_block_timeout(tx.get(), 200 * NS_PER_MS), 0);
  EXPECT_EQ(st20p_rx_set_block_timeout(rx.get(), 200 * NS_PER_MS), 0);

  std::atomic<bool> stop{false};
  std::atomic<bool> fps_phase{false};
  std::atomic<bool> span_phase{false};
  std::atomic<int> tx_cnt{0};
  std::atomic<int> rx_cnt{0};
  std::atomic<int> sha_ok{0};
  std::atomic<int> sha_fail{0};
  std::vector<uint64_t> frame_spans_ns;
  frame_spans_ns.reserve(128);

  std::thread rx_th([&]() {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    while (!stop.load(std::memory_order_acquire)) {
      struct st_frame* frame = st20p_rx_get_frame(rx.get());
      if (!frame) continue;
      const uint64_t done_ns = mtl_ptp_read_time_raw(ctx->handle);
      if (span_phase.load(std::memory_order_acquire) && frame->receive_timestamp > 0 &&
          done_ns >= frame->receive_timestamp) {
        frame_spans_ns.push_back(done_ns - frame->receive_timestamp);
      }
      if (frame->addr[0] && frame->data_size >= frame_sz) {
        if (!fps_phase.load(std::memory_order_acquire)) {
          SHA256(static_cast<const unsigned char*>(frame->addr[0]), frame_sz, digest);
          bool matched = false;
          for (int i = 0; i < TEST_SHA_HIST_NUM; i++) {
            if (memcmp(digest, expected_shas[i], SHA256_DIGEST_LENGTH) == 0) {
              matched = true;
              break;
            }
          }
          if (matched) {
            sha_ok.fetch_add(1, std::memory_order_relaxed);
          } else {
            sha_fail.fetch_add(1, std::memory_order_relaxed);
          }
        } else {
          /* Fast edge-byte sanity check during full-FPS window without 15 ms SHA stall.
           */
          const auto* b = static_cast<const uint8_t*>(frame->addr[0]);
          bool edge_ok = false;
          for (int i = 0; i < TEST_SHA_HIST_NUM; i++) {
            if (b[0] == patterns[i][0] && b[frame_sz / 2] == patterns[i][frame_sz / 2] &&
                b[frame_sz - 1] == patterns[i][frame_sz - 1]) {
              edge_ok = true;
              break;
            }
          }
          if (!edge_ok) sha_fail.fetch_add(1, std::memory_order_relaxed);
        }
      } else {
        sha_fail.fetch_add(1, std::memory_order_relaxed);
      }
      EXPECT_GE(st20p_rx_put_frame(rx.get(), frame), 0);
      rx_cnt.fetch_add(1, std::memory_order_relaxed);
    }
  });

  std::thread tx_th([&]() {
    std::vector<void*> filled_addrs;
    filled_addrs.reserve(kFbCnt);
    int idx = 0;
    while (!stop.load(std::memory_order_acquire)) {
      struct st_frame* frame = st20p_tx_get_frame(tx.get());
      if (!frame) continue;
      if (frame->addr[0]) {
        bool already_filled = false;
        for (void* addr : filled_addrs) {
          if (addr == frame->addr[0]) {
            already_filled = true;
            break;
          }
        }
        if (!already_filled) {
          memcpy(frame->addr[0], patterns[idx % TEST_SHA_HIST_NUM].data(), frame_sz);
          filled_addrs.push_back(frame->addr[0]);
        }
      }
      frame->data_size = frame_sz;
      EXPECT_GE(st20p_tx_put_frame(tx.get(), frame), 0);
      idx++;
      tx_cnt.fetch_add(1, std::memory_order_relaxed);
    }
  });

  mtl_test::ScopedWorker rx_guard(nullptr, std::move(rx_th), [&]() {
    stop.store(true, std::memory_order_release);
    st20p_rx_wake_block(rx.get());
  });
  mtl_test::ScopedWorker tx_guard(nullptr, std::move(tx_th), [&]() {
    stop.store(true, std::memory_order_release);
    st20p_tx_wake_block(tx.get());
  });

  if (cfg.check_quiescent_before_start) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    res.rx_before_start = rx_cnt.load(std::memory_order_relaxed);
  }

  if (cfg.call_mtl_start) {
    EXPECT_GE(mtl_start(ctx->handle), 0);
  }

  /* Phase 1: full SHA256 data-integrity verification on >= kInitModeMinRxFrames frames.
   */
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return sha_ok.load(std::memory_order_relaxed) >= kInitModeMinRxFrames; },
      kInitModeTimeoutMs));

  /* Phase 2 (optional): steady-state full-rate FPS and intra-frame span window. */
  if (cfg.check_fps) {
    fps_phase.store(true, std::memory_order_release);
    const int warm_target = rx_cnt.load(std::memory_order_relaxed) + 12;
    EXPECT_TRUE(mtl_test::wait_until(
        [&]() { return rx_cnt.load(std::memory_order_relaxed) >= warm_target; },
        kInitModeTimeoutMs));
    span_phase.store(true, std::memory_order_release);
    const uint64_t t0 = st_test_get_monotonic_time();
    const int r0 = rx_cnt.load(std::memory_order_relaxed);
    EXPECT_TRUE(mtl_test::wait_until(
        [&]() {
          const uint64_t elapsed_ms = (st_test_get_monotonic_time() - t0) / NS_PER_MS;
          return elapsed_ms >= 1200 &&
                 (rx_cnt.load(std::memory_order_relaxed) - r0) >= 45;
        },
        kInitModeTimeoutMs));
    const double dt_s = static_cast<double>(st_test_get_monotonic_time() - t0) / NS_PER_S;
    if (dt_s > 0.0) {
      res.rx_fps =
          static_cast<double>(rx_cnt.load(std::memory_order_relaxed) - r0) / dt_s;
    }
  }

  /* Optional wire-level check for MTL_FLAG_MULTI_SRC_PORT (8 rotating UDP src ports),
   * run after FPS measurement so pcapng disk I/O does not perturb steady-state pacing. */
  if (cfg.check_multi_src_port) {
    struct st_pcap_dump_meta dump_meta = {};
    const int dump_ret = st20p_rx_pcapng_dump(rx.get(), 1536, true, &dump_meta);
    St20UnlinkGuard unlink_guard{dump_meta.file_name[MTL_SESSION_PORT_P]};
    EXPECT_EQ(dump_ret, 0);
    if (dump_ret == 0) {
      res.distinct_src_ports =
          countDistinctUdpSrcPorts(dump_meta.file_name[MTL_SESSION_PORT_P], udp_port);
    }
  }

  tx_guard.stop();
  rx_guard.stop();
  if (cfg.call_mtl_start) {
    EXPECT_GE(mtl_stop(ctx->handle), 0);
  }
  rx.reset();
  tx.reset();

  if (!frame_spans_ns.empty()) {
    const size_t mid = frame_spans_ns.size() / 2;
    std::nth_element(frame_spans_ns.begin(), frame_spans_ns.begin() + mid,
                     frame_spans_ns.end());
    res.median_frame_span_ns = frame_spans_ns[mid];
  }
  res.tx_frames = tx_cnt.load(std::memory_order_relaxed);
  res.rx_frames = rx_cnt.load(std::memory_order_relaxed);
  res.sha_ok = sha_ok.load(std::memory_order_relaxed);
  res.sha_fail = sha_fail.load(std::memory_order_relaxed);
  return res;
}

void expectDigestLoopbackOk(const St20pDigestLoopbackResult& res,
                            bool check_fps = false) {
  EXPECT_GE(res.tx_frames, kInitModeMinRxFrames);
  EXPECT_GE(res.rx_frames, kInitModeMinRxFrames);
  EXPECT_GE(res.sha_ok, kInitModeMinRxFrames);
  EXPECT_EQ(res.sha_fail, 0);
  if (check_fps) {
    const double expect_fps = st_frame_rate(ST_FPS_P59_94);
    EXPECT_NEAR(res.rx_fps, expect_fps, expect_fps * 0.15);
  }
}

void expectInitFlagsSet(mtl_handle handle, uint64_t required_flags) {
  struct mtl_fix_info fix = {};
  ASSERT_EQ(mtl_get_fix_info(handle, &fix), 0);
  EXPECT_EQ(fix.init_flags & required_flags, required_flags);
}

/* Exercises Main port IP query + St20p create/expect-fail/abort under a mode. */
void verifyModePortIpAndSt20pControl(struct st_tests_context* ctx, uint16_t udp_port,
                                     const uint8_t expected_p_ip[MTL_IP_ADDR_LEN]) {
  mtl_handle st = ctx->handle;
  uint8_t ip[MTL_IP_ADDR_LEN] = {};
  uint8_t mask[MTL_IP_ADDR_LEN] = {};
  uint8_t gw[MTL_IP_ADDR_LEN] = {};
  ASSERT_EQ(mtl_port_ip_info(st, MTL_PORT_P, ip, mask, gw), 0);
  EXPECT_EQ(memcmp(ip, expected_p_ip, MTL_IP_ADDR_LEN), 0);

  struct st20p_tx_ops bad_ops = {};
  bad_ops.name = "init_mode_bad_st20p";
  mtl_test::fill_tx_port(bad_ops.port, ctx, udp_port, kInitModePt);
  bad_ops.port.num_port = 0;
  bad_ops.width = 1920;
  bad_ops.height = 1080;
  bad_ops.fps = ST_FPS_P59_94;
  bad_ops.input_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  bad_ops.transport_fmt = ST20_FMT_YUV_422_10BIT;
  bad_ops.framebuff_cnt = 3;
  EXPECT_EQ(st20p_tx_create(st, &bad_ops), nullptr);

  bad_ops.port.num_port = 1;
  bad_ops.framebuff_cnt = 1;
  EXPECT_EQ(st20p_tx_create(st, &bad_ops), nullptr);

  bad_ops.framebuff_cnt = 3;
  bad_ops.flags = ST20P_TX_FLAG_BLOCK_GET;
  ScopedSt20pTx tx(st20p_tx_create(st, &bad_ops));
  ASSERT_TRUE(static_cast<bool>(tx));
  struct st_frame* f = st20p_tx_get_frame(tx.get());
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(st20p_tx_put_frame_abort(tx.get(), f), 0);
  EXPECT_LT(st20p_tx_put_frame_abort(tx.get(), f), 0);
  tx.reset();

  /* Exercise create/free multi-session capacity under the active init mode. */
  constexpr int kCapSessions = 3;
  std::vector<ScopedSt20pTx> cap_handles;
  cap_handles.reserve(kCapSessions);
  for (int i = 0; i < kCapSessions; i++) {
    bad_ops.port.udp_port[MTL_SESSION_PORT_P] = static_cast<uint16_t>(udp_port + 10 + i);
    ScopedSt20pTx h(st20p_tx_create(st, &bad_ops));
    ASSERT_TRUE(static_cast<bool>(h)) << "mode create/free slot " << i;
    cap_handles.push_back(std::move(h));
  }
  struct st_var_info st_var = {};
  ASSERT_EQ(st_get_var_info(st, &st_var), 0);
  EXPECT_EQ(st_var.st20_tx_sessions_cnt, kCapSessions);
  cap_handles.clear();
  ASSERT_EQ(st_get_var_info(st, &st_var), 0);
  EXPECT_EQ(st_var.st20_tx_sessions_cnt, 0);
}

/* Exercises multi-session (progressive + interlaced -> software RSS) and RTCP 10% loss
 * ST20p loopback under the active pacing/IOVA/RSS mode (addresses PR #1970 B7). */
void verifyModeSt20pMultiAndRtcp(struct st_tests_context* ctx, uint16_t base_port) {
  mtl_handle st = ctx->handle;
  constexpr uint16_t kFbCnt = 3;
  constexpr enum st_frame_fmt kFmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;

  /* 1. Concurrent 2-stream loopback: Stream 0 = 720p59.94 progressive,
   *    Stream 1 = 1080i50 interlaced (both active simultaneously for software RSS). */
  struct StreamDef {
    uint32_t w;
    uint32_t h;
    enum st_fps fps;
    bool interlaced;
    uint16_t port;
  };
  const StreamDef defs[2] = {
      {1280, 720, ST_FPS_P59_94, false, base_port},
      {1920, 1080, ST_FPS_P50, true, static_cast<uint16_t>(base_port + 2)},
  };

  std::vector<ScopedSt20pRx> rxs;
  std::vector<ScopedSt20pTx> txs;
  std::vector<size_t> fszs(2);
  std::vector<std::vector<uint8_t>> pats(2);
  std::vector<std::array<unsigned char, SHA256_DIGEST_LENGTH>> shas(2);
  for (int i = 0; i < 2; i++) {
    fszs[i] = st_frame_size(kFmt, defs[i].w, defs[i].h, defs[i].interlaced);
    pats[i].resize(fszs[i]);
    st_test_rand_data(pats[i].data(), fszs[i], 0x41u + static_cast<uint8_t>(i));
    SHA256(pats[i].data(), fszs[i], shas[i].data());

    struct st20p_rx_ops rops = {};
    rops.name = i == 0 ? "mode_multi_rx0" : "mode_multi_rx1";
    mtl_test::fill_rx_port(rops.port, ctx, defs[i].port, kInitModePt);
    rops.width = defs[i].w;
    rops.height = defs[i].h;
    rops.fps = defs[i].fps;
    rops.interlaced = defs[i].interlaced;
    rops.output_fmt = kFmt;
    rops.transport_fmt = ST20_FMT_YUV_422_10BIT;
    rops.framebuff_cnt = kFbCnt;
    rops.flags = ST20P_RX_FLAG_BLOCK_GET;
    ScopedSt20pRx r(st20p_rx_create(st, &rops));
    ASSERT_TRUE(static_cast<bool>(r)) << "multi rx " << i;
    EXPECT_EQ(st20p_rx_set_block_timeout(r.get(), 200 * NS_PER_MS), 0);
    rxs.push_back(std::move(r));

    struct st20p_tx_ops tops = {};
    tops.name = i == 0 ? "mode_multi_tx0" : "mode_multi_tx1";
    mtl_test::fill_tx_port(tops.port, ctx, defs[i].port, kInitModePt);
    tops.width = defs[i].w;
    tops.height = defs[i].h;
    tops.fps = defs[i].fps;
    tops.interlaced = defs[i].interlaced;
    tops.input_fmt = kFmt;
    tops.transport_fmt = ST20_FMT_YUV_422_10BIT;
    tops.framebuff_cnt = kFbCnt;
    tops.flags = ST20P_TX_FLAG_BLOCK_GET;
    ScopedSt20pTx t(st20p_tx_create(st, &tops));
    ASSERT_TRUE(static_cast<bool>(t)) << "multi tx " << i;
    EXPECT_EQ(st20p_tx_set_block_timeout(t.get(), 200 * NS_PER_MS), 0);
    for (uint16_t b = 0; b < kFbCnt; b++) {
      void* fb = st20p_tx_get_fb_addr(t.get(), b);
      ASSERT_NE(fb, nullptr);
      memcpy(fb, pats[i].data(), fszs[i]);
    }
    txs.push_back(std::move(t));
  }

  std::atomic<bool> stop{false};
  std::array<std::atomic<int>, 2> sha_ok{{{0}, {0}}};
  std::array<std::atomic<int>, 2> sha_fail{{{0}, {0}}};
  std::atomic<int> first_field{0}, second_field{0};
  std::vector<std::unique_ptr<mtl_test::ScopedWorker>> workers;
  for (int i = 0; i < 2; i++) {
    workers.push_back(std::make_unique<mtl_test::ScopedWorker>(
        nullptr, std::thread([&, i]() {
          unsigned char got[SHA256_DIGEST_LENGTH];
          while (!stop.load(std::memory_order_acquire)) {
            struct st_frame* f = st20p_rx_get_frame(rxs[i].get());
            if (!f) continue;
            if (defs[i].interlaced) {
              if (f->second_field)
                second_field.fetch_add(1, std::memory_order_relaxed);
              else
                first_field.fetch_add(1, std::memory_order_relaxed);
            }
            if (f->addr[0] && f->data_size >= fszs[i]) {
              SHA256(static_cast<const unsigned char*>(f->addr[0]), fszs[i], got);
              if (memcmp(got, shas[i].data(), SHA256_DIGEST_LENGTH) == 0)
                sha_ok[i].fetch_add(1, std::memory_order_relaxed);
              else
                sha_fail[i].fetch_add(1, std::memory_order_relaxed);
            } else {
              sha_fail[i].fetch_add(1, std::memory_order_relaxed);
            }
            EXPECT_GE(st20p_rx_put_frame(rxs[i].get(), f), 0);
          }
        }),
        [&, i]() {
          stop.store(true, std::memory_order_release);
          st20p_rx_wake_block(rxs[i].get());
        }));
    workers.push_back(std::make_unique<mtl_test::ScopedWorker>(
        nullptr, std::thread([&, i]() {
          int n = 0;
          while (!stop.load(std::memory_order_acquire)) {
            struct st_frame* f = st20p_tx_get_frame(txs[i].get());
            if (!f) continue;
            if (defs[i].interlaced) f->second_field = (n % 2) != 0;
            f->data_size = fszs[i];
            EXPECT_GE(st20p_tx_put_frame(txs[i].get(), f), 0);
            n++;
          }
        }),
        [&, i]() {
          stop.store(true, std::memory_order_release);
          st20p_tx_wake_block(txs[i].get());
        }));
  }

  ASSERT_GE(mtl_start(st), 0);
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() {
        return sha_ok[0].load(std::memory_order_relaxed) >= 12 &&
               sha_ok[1].load(std::memory_order_relaxed) >= 12;
      },
      kInitModeTimeoutMs));
  for (auto& w : workers) w->stop();
  workers.clear();
  EXPECT_GE(mtl_stop(st), 0);
  rxs.clear();
  txs.clear();

  for (int i = 0; i < 2; i++) {
    EXPECT_GE(sha_ok[i].load(std::memory_order_relaxed), 12) << "stream " << i;
    EXPECT_EQ(sha_fail[i].load(std::memory_order_relaxed), 0) << "stream " << i;
  }
  EXPECT_GT(first_field.load(std::memory_order_relaxed), 0);
  EXPECT_GT(second_field.load(std::memory_order_relaxed), 0);

  /* 2. RTCP 10% simulated loss recovery under the active mode. */
  const size_t rtcp_sz = st_frame_size(kFmt, 1280, 720, false);
  std::vector<uint8_t> rtcp_pat(rtcp_sz);
  unsigned char rtcp_sha[SHA256_DIGEST_LENGTH];
  st_test_rand_data(rtcp_pat.data(), rtcp_sz, 0x63u);
  SHA256(rtcp_pat.data(), rtcp_sz, rtcp_sha);

  struct st20p_rx_ops rtcp_rx_ops = {};
  rtcp_rx_ops.name = "mode_rtcp_rx";
  mtl_test::fill_rx_port(rtcp_rx_ops.port, ctx, static_cast<uint16_t>(base_port + 4),
                         kInitModePt);
  rtcp_rx_ops.width = 1280;
  rtcp_rx_ops.height = 720;
  rtcp_rx_ops.fps = ST_FPS_P59_94;
  rtcp_rx_ops.output_fmt = kFmt;
  rtcp_rx_ops.transport_fmt = ST20_FMT_YUV_422_10BIT;
  rtcp_rx_ops.framebuff_cnt = kFbCnt;
  rtcp_rx_ops.flags = ST20P_RX_FLAG_BLOCK_GET | ST20P_RX_FLAG_ENABLE_RTCP |
                      ST20P_RX_FLAG_SIMULATE_PKT_LOSS;
  rtcp_rx_ops.rtcp.nack_interval_us = 250;
  rtcp_rx_ops.rtcp.seq_bitmap_size = 64;
  rtcp_rx_ops.rtcp.seq_skip_window = 0;
  rtcp_rx_ops.rtcp.burst_loss_max = 1;
  rtcp_rx_ops.rtcp.sim_loss_rate = 0.1f;

  struct st20p_tx_ops rtcp_tx_ops = {};
  rtcp_tx_ops.name = "mode_rtcp_tx";
  mtl_test::fill_tx_port(rtcp_tx_ops.port, ctx, static_cast<uint16_t>(base_port + 4),
                         kInitModePt);
  rtcp_tx_ops.width = 1280;
  rtcp_tx_ops.height = 720;
  rtcp_tx_ops.fps = ST_FPS_P59_94;
  rtcp_tx_ops.input_fmt = kFmt;
  rtcp_tx_ops.transport_fmt = ST20_FMT_YUV_422_10BIT;
  rtcp_tx_ops.framebuff_cnt = kFbCnt;
  rtcp_tx_ops.flags = ST20P_TX_FLAG_BLOCK_GET | ST20P_TX_FLAG_ENABLE_RTCP;
  rtcp_tx_ops.rtcp.buffer_size = 1024;

  ScopedSt20pRx rtcp_rx(st20p_rx_create(st, &rtcp_rx_ops));
  ScopedSt20pTx rtcp_tx(st20p_tx_create(st, &rtcp_tx_ops));
  ASSERT_TRUE(static_cast<bool>(rtcp_rx));
  ASSERT_TRUE(static_cast<bool>(rtcp_tx));
  EXPECT_EQ(st20p_rx_set_block_timeout(rtcp_rx.get(), 200 * NS_PER_MS), 0);
  EXPECT_EQ(st20p_tx_set_block_timeout(rtcp_tx.get(), 200 * NS_PER_MS), 0);
  for (uint16_t b = 0; b < kFbCnt; b++) {
    void* fb = st20p_tx_get_fb_addr(rtcp_tx.get(), b);
    ASSERT_NE(fb, nullptr);
    memcpy(fb, rtcp_pat.data(), rtcp_sz);
  }

  std::atomic<bool> rtcp_stop{false};
  std::atomic<int> rtcp_ok{0}, rtcp_fail{0};
  mtl_test::ScopedWorker rtcp_rx_w(
      nullptr, std::thread([&]() {
        unsigned char got[SHA256_DIGEST_LENGTH];
        while (!rtcp_stop.load(std::memory_order_acquire)) {
          struct st_frame* f = st20p_rx_get_frame(rtcp_rx.get());
          if (!f) continue;
          if (st_is_frame_complete(f->status) && f->addr[0] && f->data_size >= rtcp_sz) {
            SHA256(static_cast<const unsigned char*>(f->addr[0]), rtcp_sz, got);
            if (memcmp(got, rtcp_sha, SHA256_DIGEST_LENGTH) == 0)
              rtcp_ok.fetch_add(1, std::memory_order_relaxed);
            else
              rtcp_fail.fetch_add(1, std::memory_order_relaxed);
          }
          EXPECT_GE(st20p_rx_put_frame(rtcp_rx.get(), f), 0);
        }
      }),
      [&]() {
        rtcp_stop.store(true, std::memory_order_release);
        st20p_rx_wake_block(rtcp_rx.get());
      });
  mtl_test::ScopedWorker rtcp_tx_w(nullptr, std::thread([&]() {
                                     while (!rtcp_stop.load(std::memory_order_acquire)) {
                                       struct st_frame* f =
                                           st20p_tx_get_frame(rtcp_tx.get());
                                       if (!f) continue;
                                       f->data_size = rtcp_sz;
                                       EXPECT_GE(st20p_tx_put_frame(rtcp_tx.get(), f), 0);
                                     }
                                   }),
                                   [&]() {
                                     rtcp_stop.store(true, std::memory_order_release);
                                     st20p_tx_wake_block(rtcp_tx.get());
                                   });

  ASSERT_GE(mtl_start(st), 0);
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return rtcp_ok.load(std::memory_order_relaxed) >= 12; },
      kInitModeTimeoutMs));
  struct st20_rx_user_stats rx_stats = {};
  EXPECT_EQ(st20p_rx_get_session_stats(rtcp_rx.get(), &rx_stats), 0);
  rtcp_tx_w.stop();
  rtcp_rx_w.stop();
  EXPECT_GE(mtl_stop(st), 0);

  EXPECT_GE(rtcp_ok.load(std::memory_order_relaxed), 12);
  EXPECT_EQ(rtcp_fail.load(std::memory_order_relaxed), 0);
  EXPECT_GT(rx_stats.stat_pkts_simulate_loss, 0u);
  EXPECT_GT(rx_stats.stat_pkts_retransmit, 0u);
}

struct st20_tx_ops makeBasicSt20TxOps(struct st_tests_context* ctx,
                                      tests_context* test_ctx, const char* name,
                                      uint16_t udp_port) {
  struct st20_tx_ops ops = {};
  ops.name = name;
  ops.priv = test_ctx;
  ops.num_port = 1;
  memcpy(ops.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops.udp_port[MTL_SESSION_PORT_P] = udp_port;
  ops.payload_type = kInitModePt;
  ops.type = ST20_TYPE_FRAME_LEVEL;
  ops.width = 1920;
  ops.height = 1080;
  ops.fps = ST_FPS_P59_94;
  ops.fmt = ST20_FMT_YUV_422_10BIT;
  ops.packing = ST20_PACKING_BPM;
  ops.framebuff_cnt = 2;
  ops.get_next_frame = [](void* priv, uint16_t* next_frame_idx,
                          struct st20_tx_frame_meta*) {
    return tx_next_frame(priv, next_frame_idx);
  };
  return ops;
}

}  // namespace

/* Replaces --queue_cnt 24 CLI profile (st2110_20_tx_queue_cnt). */
TEST_F(NoCtxTest, tx_queue_cnt_24) {
  MTL_TEST_REQUIRE_DPDK_PMD(ctx, MTL_PORT_P);
  if (ctx->para.flags & MTL_FLAG_SHARED_TX_QUEUE) {
    GTEST_SKIP() << "needs one dedicated DPDK tx queue per session";
  }

  constexpr uint16_t kWantQueues = 24;
  ctx->para.tx_queues_cnt[MTL_PORT_P] = kWantQueues;
  ctx->para.tx_queues_cnt[MTL_PORT_R] = kWantQueues;
  ctx->para.rx_queues_cnt[MTL_PORT_P] = kWantQueues;
  ctx->para.rx_queues_cnt[MTL_PORT_R] = kWantQueues;
  initDefaultContext();
  if (HasFatalFailure()) return;

  const int base_sch = st_test_sch_cnt(ctx);
  auto test_ctx = mtl_test::make_context(ctx, 0, 2);
  struct st20_tx_ops ops =
      makeBasicSt20TxOps(ctx, test_ctx.get(), "tx_queue_cnt_24", 15000);

  std::vector<ScopedSt20Tx> handles;
  handles.reserve(kWantQueues);
  for (uint16_t i = 0; i < kWantQueues; i++) {
    st20_tx_handle h = st20_tx_create(ctx->handle, &ops);
    if (!h) break;
    handles.emplace_back(h);
    ops.udp_port[MTL_SESSION_PORT_P]++;
  }
  const int created = static_cast<int>(handles.size());
  EXPECT_EQ(created, kWantQueues);
  struct st_var_info st_var = {};
  ASSERT_EQ(st_get_var_info(ctx->handle, &st_var), 0);
  EXPECT_EQ(st_var.st20_tx_sessions_cnt, created);
  if (created == kWantQueues) {
    ScopedSt20Tx overflow(st20_tx_create(ctx->handle, &ops));
    EXPECT_FALSE(static_cast<bool>(overflow))
        << "25th TX session must fail once all 24 dedicated TX queues are occupied";
  }

  handles.clear();
  EXPECT_EQ(st_test_sch_cnt(ctx), base_sch);
  ASSERT_EQ(st_get_var_info(ctx->handle, &st_var), 0);
  EXPECT_EQ(st_var.st20_tx_sessions_cnt, 0);

  /* Re-acquire all 24 queues so a partial TX-queue leak on free is caught. */
  std::vector<ScopedSt20Tx> reused;
  reused.reserve(kWantQueues);
  for (uint16_t i = 0; i < kWantQueues; i++) {
    st20_tx_handle h = st20_tx_create(ctx->handle, &ops);
    if (!h) break;
    reused.emplace_back(h);
    ops.udp_port[MTL_SESSION_PORT_P]++;
  }
  EXPECT_EQ(static_cast<int>(reused.size()), kWantQueues)
      << "all 24 TX queues must be reusable after freeing all 24 sessions";
}

/* Replaces --pacing_way tsc / --tsc + --iova_mode va + --rss_mode l3_l4 +
 * --multi_src_port (st20p_tsc_pacing profile). */
TEST_F(NoCtxTest, init_mode_pacing_tsc_va_rss_l3_l4_multi_src) {
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  MTL_TEST_REQUIRE_DPDK_PMD(ctx, MTL_PORT_P);

  ModeLogTap log_tap;
  setCompactQueues(ctx);
  uint8_t expected_p_ip[MTL_IP_ADDR_LEN];
  memcpy(expected_p_ip, ctx->para.sip_addr[MTL_PORT_P], MTL_IP_ADDR_LEN);
  ctx->para.pacing = ST21_TX_PACING_WAY_TSC;
  ctx->para.iova_mode = MTL_IOVA_MODE_VA;
  ctx->para.rss_mode = MTL_RSS_MODE_L3_L4;
  ctx->para.flags |= MTL_FLAG_MULTI_SRC_PORT | MTL_FLAG_CNI_TASKLET;
  initDefaultContext();
  if (HasFatalFailure()) return;

  EXPECT_EQ(mtl_iova_mode_get(ctx->handle), MTL_IOVA_MODE_VA);
  EXPECT_EQ(mtl_rss_mode_get(ctx->handle), MTL_RSS_MODE_L3_L4);
  expectInitFlagsSet(ctx->handle, MTL_FLAG_MULTI_SRC_PORT);

  St20pLoopbackConfig cfg;
  cfg.check_fps = true;
  cfg.check_multi_src_port = true;
  auto res = runSt20pDigestLoopback(ctx, kInitModeUdpPort, cfg);
  expectDigestLoopbackOk(res, /*check_fps=*/true);
  EXPECT_TRUE(ModeLogTap::saw_dev_tsc.load(std::memory_order_acquire));
  EXPECT_TRUE(ModeLogTap::saw_tx_tsc.load(std::memory_order_acquire));
  EXPECT_EQ(res.vrx_pkts, 6u);
  EXPECT_EQ(res.distinct_src_ports, 8u)
      << "MTL_FLAG_MULTI_SRC_PORT must rotate across 8 distinct UDP source ports";
  verifyModeSt20pMultiAndRtcp(ctx, kInitModeUdpPort + 60);
  verifyModePortIpAndSt20pControl(ctx, kInitModeUdpPort + 50, expected_p_ip);
}

/* Replaces --pacing_way rl / auto + --iova_mode va + --rss_mode l3_l4 +
 * --multi_src_port (st20p_auto_pacing_va profile). */
TEST_F(NoCtxTest, init_mode_pacing_rl_va_rss_l3_l4_multi_src) {
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  MTL_TEST_REQUIRE_DPDK_PMD(ctx, MTL_PORT_P);

  ModeLogTap log_tap;
  setCompactQueues(ctx);
  uint8_t expected_p_ip[MTL_IP_ADDR_LEN];
  memcpy(expected_p_ip, ctx->para.sip_addr[MTL_PORT_P], MTL_IP_ADDR_LEN);
  ctx->para.pacing = ST21_TX_PACING_WAY_RL;
  ctx->para.iova_mode = MTL_IOVA_MODE_VA;
  ctx->para.rss_mode = MTL_RSS_MODE_L3_L4;
  ctx->para.flags |= MTL_FLAG_MULTI_SRC_PORT | MTL_FLAG_CNI_TASKLET;
  initDefaultContext();
  if (HasFatalFailure()) return;

  EXPECT_EQ(mtl_iova_mode_get(ctx->handle), MTL_IOVA_MODE_VA);
  EXPECT_EQ(mtl_rss_mode_get(ctx->handle), MTL_RSS_MODE_L3_L4);
  expectInitFlagsSet(ctx->handle, MTL_FLAG_MULTI_SRC_PORT);

  St20pLoopbackConfig cfg;
  cfg.check_fps = true;
  cfg.check_multi_src_port = true;
  auto res = runSt20pDigestLoopback(ctx, kInitModeUdpPort + 2, cfg);
  expectDigestLoopbackOk(res, /*check_fps=*/true);
  EXPECT_TRUE(ModeLogTap::saw_dev_rl.load(std::memory_order_acquire));
  EXPECT_TRUE(ModeLogTap::saw_tx_rl.load(std::memory_order_acquire));
  EXPECT_FALSE(ModeLogTap::saw_fallback_tsc.load(std::memory_order_acquire));
  EXPECT_EQ(res.vrx_pkts, 5u);
  EXPECT_EQ(res.distinct_src_ports, 8u)
      << "MTL_FLAG_MULTI_SRC_PORT must rotate across 8 distinct UDP source ports";
  verifyModeSt20pMultiAndRtcp(ctx, kInitModeUdpPort + 70);
  verifyModePortIpAndSt20pControl(ctx, kInitModeUdpPort + 56, expected_p_ip);
}

/* Replaces --pacing_way be + --rss_mode l3. */
TEST_F(NoCtxTest, init_mode_pacing_be_rss_l3) {
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  MTL_TEST_REQUIRE_DPDK_PMD(ctx, MTL_PORT_P);

  ModeLogTap log_tap;
  setCompactQueues(ctx);
  ctx->para.pacing = ST21_TX_PACING_WAY_BE;
  ctx->para.rss_mode = MTL_RSS_MODE_L3;
  initDefaultContext();
  if (HasFatalFailure()) return;

  EXPECT_EQ(mtl_rss_mode_get(ctx->handle), MTL_RSS_MODE_L3);

  St20pLoopbackConfig cfg;
  cfg.check_fps = true;
  auto res = runSt20pDigestLoopback(ctx, kInitModeUdpPort + 4, cfg);
  expectDigestLoopbackOk(res, /*check_fps=*/true);
  EXPECT_TRUE(ModeLogTap::saw_dev_be.load(std::memory_order_acquire));
  EXPECT_TRUE(ModeLogTap::saw_tx_be.load(std::memory_order_acquire));
  EXPECT_FALSE(ModeLogTap::saw_dev_rl.load(std::memory_order_acquire));
  EXPECT_FALSE(ModeLogTap::saw_tx_rl.load(std::memory_order_acquire));
}

/* Replaces --iova_mode pa + st20p_auto_pacing_pa + Dma.iova_pa_rejects_map_and_mem_alloc.
 */
TEST_F(NoCtxTest, init_mode_iova_pa_dma_reject_and_st20p) {
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  MTL_TEST_REQUIRE_DPDK_PMD(ctx, MTL_PORT_P);

  setCompactQueues(ctx);
  uint8_t expected_p_ip[MTL_IP_ADDR_LEN];
  memcpy(expected_p_ip, ctx->para.sip_addr[MTL_PORT_P], MTL_IP_ADDR_LEN);
  ctx->para.pacing = ST21_TX_PACING_WAY_AUTO;
  ctx->para.iova_mode = MTL_IOVA_MODE_PA;
  ctx->para.rss_mode = MTL_RSS_MODE_L3_L4;
  /* Exercise MTL_FLAG_DMA_AUTO under IOVA PA mode (library must safely reject DMA). */
  ctx->para.flags |= MTL_FLAG_MULTI_SRC_PORT | MTL_FLAG_DMA_AUTO | MTL_FLAG_CNI_TASKLET;
  ctx->para.num_dma_dev_port = 0;
  initDefaultContext();
  if (HasFatalFailure()) return;

  ASSERT_EQ(mtl_iova_mode_get(ctx->handle), MTL_IOVA_MODE_PA);
  EXPECT_EQ(mtl_rss_mode_get(ctx->handle), MTL_RSS_MODE_L3_L4);
  verifyModePortIpAndSt20pControl(ctx, kInitModeUdpPort + 52, expected_p_ip);

  /* DMA mapping, DMA memory allocation, and UDMA creation must reject in PA mode. */
  mtl_handle st = ctx->handle;
  const size_t pg_sz = mtl_page_size(st);
  const size_t size = 64 * pg_sz;
  mtl_test::PageAlignedBuf buf(size, pg_sz);
  ASSERT_TRUE(buf);

  mtl_test::ScopedDmaMap mapping(st, buf.data(), size);
  EXPECT_EQ(mapping.iova(), MTL_BAD_IOVA);
  EXPECT_EQ(mtl_dma_unmap(st, buf.data(), static_cast<mtl_iova_t>(pg_sz), size), -EINVAL);

  mtl_test::ScopedDmaMem dma_mem(st, size);
  EXPECT_EQ(dma_mem.get(), nullptr);

  mtl_test::ScopedUdma dma(mtl_udma_create(st, 128, MTL_PORT_P));
  EXPECT_EQ(dma.get(), nullptr);

  /* Hugepage allocation and virt2iova must still work in IOVA PA mode. */
  mtl_test::ScopedHpBuf hp(st, 4096, MTL_PORT_P, true);
  ASSERT_TRUE(hp);
  EXPECT_NE(hp.iova(), MTL_BAD_IOVA);
  EXPECT_NE(hp.iova(), static_cast<mtl_iova_t>(0));

  /* Real ST20p data plane must pass SHA256 digest + multi-session/interlaced/RTCP in PA
   * mode. */
  auto res = runSt20pDigestLoopback(ctx, kInitModeUdpPort + 6);
  expectDigestLoopbackOk(res);
  verifyModeSt20pMultiAndRtcp(ctx, kInitModeUdpPort + 80);
}

/* Verifies both MTL_FLAG_DEV_AUTO_START_STOP polarities (OFF in child, ON in parent). */
TEST_F(NoCtxTest, init_mode_auto_start_stop_polarity) {
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  MTL_TEST_REQUIRE_DPDK_PMD(ctx, MTL_PORT_P);
  setCompactQueues(ctx);

#ifndef WINDOWSENV
  /* Phase 1 (child process): DEV_AUTO_START_STOP OFF requires explicit mtl_start(). */
  pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    alarm(60);
    ctx->para.ptp_get_time_fn = FakePtpClockNow;
    ctx->para.flags &= ~MTL_FLAG_DEV_AUTO_START_STOP;
    ctx->handle = mtl_init(&ctx->para);
    if (!ctx->handle) _exit(10);

    struct mtl_var_info var = {};
    if (mtl_get_var_info(ctx->handle, &var) != 0 || var.dev_started) _exit(11);
    if (mtl_start(ctx->handle) < 0) _exit(12);
    if (mtl_get_var_info(ctx->handle, &var) != 0 || !var.dev_started) _exit(13);
    if (mtl_stop(ctx->handle) < 0) _exit(14);
    if (mtl_get_var_info(ctx->handle, &var) != 0 || var.dev_started) _exit(15);

    St20pLoopbackConfig off_cfg;
    off_cfg.call_mtl_start = true;
    off_cfg.check_quiescent_before_start = true;
    auto res = runSt20pDigestLoopback(ctx, kInitModeUdpPort + 8, off_cfg);
    int uninit_rc = mtl_uninit(ctx->handle);
    ctx->handle = nullptr;
    if (res.rx_before_start != 0 || res.sha_ok < kInitModeMinRxFrames ||
        res.sha_fail != 0 || uninit_rc != 0)
      _exit(16);
    if (::testing::Test::HasFailure()) _exit(17);
    _exit(0);
  }
  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0)
      << "DEV_AUTO_START_STOP OFF child failed (exit=" << WEXITSTATUS(status)
      << ", sig=" << (WIFSIGNALED(status) ? WTERMSIG(status) : 0) << ")";
#endif

  /* Phase 2 (parent process): DEV_AUTO_START_STOP ON starts traffic without mtl_start(),
   * and explicit mtl_stop() must be a no-op until mtl_uninit(). */
  ctx->para.ptp_get_time_fn = FakePtpClockNow;
  ctx->para.flags |= MTL_FLAG_DEV_AUTO_START_STOP;
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_NE(ctx->handle, nullptr);
  expectInitFlagsSet(ctx->handle, MTL_FLAG_DEV_AUTO_START_STOP);

  struct mtl_var_info var = {};
  ASSERT_EQ(mtl_get_var_info(ctx->handle, &var), 0);
  EXPECT_TRUE(var.dev_started);
  EXPECT_EQ(mtl_stop(ctx->handle), 0);
  ASSERT_EQ(mtl_get_var_info(ctx->handle, &var), 0);
  EXPECT_TRUE(var.dev_started);

  St20pLoopbackConfig on_cfg;
  on_cfg.call_mtl_start = false;
  auto res = runSt20pDigestLoopback(ctx, kInitModeUdpPort + 10, on_cfg);
  expectDigestLoopbackOk(res);
}

/* Replaces --hdr_split CLI flag and always-skipped St20_rx.digest_hdr_split. */
TEST_F(NoCtxTest, init_mode_st20_rx_hdr_split) {
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  MTL_TEST_REQUIRE_DPDK_PMD(ctx, MTL_PORT_P);
  setCompactQueues(ctx);

  ctx->para.nb_rx_hdr_split_queues = 1;
  ctx->para.ptp_get_time_fn = FakePtpClockNow;
  ctx->para.flags &= ~MTL_FLAG_DEV_AUTO_START_STOP;
  ctx->handle = mtl_init(&ctx->para);
  if (!ctx->handle) {
    GTEST_SKIP() << "NIC/driver does not support nb_rx_hdr_split_queues=1";
  }

  constexpr uint16_t kHdrSplitPort = 6970;
  const St20StreamSpec st{ST20_TYPE_FRAME_LEVEL,
                          ST20_TYPE_FRAME_LEVEL,
                          ST20_PACKING_BPM,
                          ST_FPS_P59_94,
                          1920,
                          1080,
                          false,
                          ST20_FMT_YUV_422_10BIT};

  auto tx_ctx = mtl_test::make_context(ctx, 0, TEST_SHA_HIST_NUM, true);
  auto rx_ctx = mtl_test::make_context(ctx, 0, 3, true);
  tx_ctx->ready.store(false, std::memory_order_relaxed);

  struct st20_tx_ops ops_tx = {};
  st20_fill_loopback_tx_ops(ops_tx, tx_ctx.get(), st, "hdr_split_tx", kHdrSplitPort);
  ScopedSt20Tx tx_handle(st20_tx_create(ctx->handle, &ops_tx));
  ASSERT_TRUE(static_cast<bool>(tx_handle));
  st20_init_tx_digest_frames(ctx->handle, tx_ctx.get(), tx_handle.get(), st);
  if (HasFatalFailure()) return;

  struct st20_rx_ops ops_rx = {};
  st20_fill_loopback_rx_ops(ops_rx, rx_ctx.get(), st, "hdr_split_rx", kHdrSplitPort);
  ops_rx.notify_frame_ready = st20_digest_rx_frame_ready;
  ops_rx.flags |= ST20_RX_FLAG_HDR_SPLIT;
  ScopedSt20Rx rx_handle(st20_rx_create(ctx->handle, &ops_rx));
  ASSERT_TRUE(static_cast<bool>(rx_handle))
      << "mtl_init accepted nb_rx_hdr_split_queues=1; ST20_RX_FLAG_HDR_SPLIT "
         "session creation must succeed";

  rx_ctx->frame_time = static_cast<double>(NS_PER_S) / st_frame_rate(ops_rx.fps);
  st20_bind_rx_digest_ctx(rx_ctx.get(), tx_ctx.get(), rx_handle.get(), st);
  mtl_test::ScopedWorker rx_worker(rx_ctx.get(),
                                   std::thread(st20_digest_rx_frame_check, rx_ctx.get()));

  st20_set_handle(tx_ctx.get(), tx_handle.get());
  tx_ctx->ready.store(true, std::memory_order_release);

  ASSERT_GE(mtl_start(ctx->handle), 0);
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() {
        return mtl_test::rx_frames(rx_ctx.get()) >= kSt20MinRxFrames &&
               mtl_test::sha_checked_frames(rx_ctx.get()) >= kSt20MinShaFrames;
      },
      kInitModeTimeoutMs));
  tx_ctx->ready.store(false, std::memory_order_release);
  EXPECT_GE(mtl_stop(ctx->handle), 0);
  rx_worker.stop();
  st20_rx_drain_bufq_put_framebuff(rx_ctx.get());

  EXPECT_GT(mtl_test::rx_frames(rx_ctx.get()), 0);
  EXPECT_GT(mtl_test::sha_checked_frames(rx_ctx.get()), 0);
  EXPECT_EQ(mtl_test::relaxed_load(&rx_ctx->sha_fail_cnt), 0);
}

/* Replaces --ptp CLI flag: tests built-in PTP service (MTL_FLAG_PTP_ENABLE with no user
 * ptp_get_time_fn). */
TEST_F(NoCtxTest, init_mode_hw_ptp_clock) {
  MTL_TEST_REQUIRE_DPDK_PMD(ctx, MTL_PORT_P);
  setCompactQueues(ctx);

  ModeLogTap log_tap;
  std::atomic<bool> stat_dumped{false};
  ctx->para.flags |= MTL_FLAG_PTP_ENABLE;
  ctx->para.flags &= ~MTL_FLAG_PTP_SOURCE_TSC;
  ctx->para.ptp_get_time_fn = nullptr;
  ctx->para.flags &= ~MTL_FLAG_DEV_AUTO_START_STOP;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->para.dump_period_s = 1;
  ctx->para.priv = &stat_dumped;
  ctx->para.stat_dump_cb_fn = [](void* priv) {
    static_cast<std::atomic<bool>*>(priv)->store(true, std::memory_order_release);
  };
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_NE(ctx->handle, nullptr);
  stat_dumped.store(false, std::memory_order_release);
  expectInitFlagsSet(ctx->handle, MTL_FLAG_PTP_ENABLE);

  ASSERT_GE(mtl_start(ctx->handle), 0);
  const uint64_t t1 = mtl_ptp_read_time(ctx->handle);
  const uint64_t raw1 = mtl_ptp_read_time_raw(ctx->handle);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const uint64_t t2 = mtl_ptp_read_time(ctx->handle);
  const uint64_t raw2 = mtl_ptp_read_time_raw(ctx->handle);

  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return stat_dumped.load(std::memory_order_acquire); }, 3000));
  EXPECT_TRUE(ModeLogTap::saw_ptp_active_stat.load(std::memory_order_acquire))
      << "built-in PTP service did not report active PTP(0) status";
  ASSERT_GE(mtl_stop(ctx->handle), 0);

  EXPECT_GT(t1, 0u);
  EXPECT_GT(t2, t1);
  EXPECT_GT(raw1, 0u);
  EXPECT_GT(raw2, raw1);
  /* Check monotonic wall-time rate on the undisciplined raw clock so a live lab PTP
   * grandmaster step during the 100 ms window does not cause a false failure. */
  const uint64_t raw_delta_ms = (raw2 - raw1) / NS_PER_MS;
  EXPECT_GE(raw_delta_ms, 50u);
  EXPECT_LE(raw_delta_ms, 500u);
}

/* Replaces --rx_mono_pool, --tx_mono_pool, --mono_pool, --tx_no_chain,
 * --rx_separate_lcore. */
TEST_F(NoCtxTest, init_mode_mono_pool_no_chain_sep_lcore) {
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  MTL_TEST_REQUIRE_DPDK_PMD(ctx, MTL_PORT_P);

  setCompactQueues(ctx);
  const uint64_t mode_flags = MTL_FLAG_RX_MONO_POOL | MTL_FLAG_TX_MONO_POOL |
                              MTL_FLAG_TX_NO_CHAIN | MTL_FLAG_RX_SEPARATE_VIDEO_LCORE;
  ctx->para.flags |= mode_flags;
  initDefaultContext();
  if (HasFatalFailure()) return;

  expectInitFlagsSet(ctx->handle, mode_flags);
  auto res = runSt20pDigestLoopback(ctx, kInitModeUdpPort + 12);
  expectDigestLoopbackOk(res);
  EXPECT_GE(res.tx_sch_idx, 0);
  EXPECT_GE(res.rx_sch_idx, 0);
  EXPECT_NE(res.tx_sch_idx, res.rx_sch_idx)
      << "MTL_FLAG_RX_SEPARATE_VIDEO_LCORE must place RX and TX on distinct schedulers";
}

/* Replaces --cni_thread, --tasklet_thread, --nb_tx_desc, --nb_rx_desc. */
TEST_F(NoCtxTest, init_mode_cni_tasklet_thread_desc) {
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  MTL_TEST_REQUIRE_DPDK_PMD(ctx, MTL_PORT_P);

  const uint64_t mode_flags = MTL_FLAG_CNI_THREAD | MTL_FLAG_TASKLET_THREAD;
  setCompactQueues(ctx);
  ctx->para.flags &= ~MTL_FLAG_CNI_TASKLET;
  ctx->para.flags |= mode_flags;
  ctx->para.nb_tx_desc = 1024;
  ctx->para.nb_rx_desc = 1024;
  initDefaultContext();
  if (HasFatalFailure()) return;

  expectInitFlagsSet(ctx->handle, mode_flags);
  ASSERT_GE(mtl_start(ctx->handle), 0);
  struct mtl_var_info var = {};
  ASSERT_EQ(mtl_get_var_info(ctx->handle, &var), 0);
  EXPECT_TRUE(var.dev_started);
  EXPECT_GE(var.sch_cnt, 1u);
  EXPECT_EQ(var.lcore_cnt, 0u)
      << "MTL_FLAG_TASKLET_THREAD must run schedulers on pthreads with 0 EAL lcores";

  St20pLoopbackConfig cfg;
  cfg.call_mtl_start = false;
  auto res = runSt20pDigestLoopback(ctx, kInitModeUdpPort + 14, cfg);
  ASSERT_EQ(mtl_get_var_info(ctx->handle, &var), 0);
  EXPECT_EQ(var.lcore_cnt, 0u);
  EXPECT_GE(mtl_stop(ctx->handle), 0);
  expectDigestLoopbackOk(res);
}

/* Replaces --migrate_enable, --migrate_disable, --sch_session_quota,
 * --allow_across_numa_core. */
TEST_F(NoCtxTest, init_mode_migrate_quota_across_numa) {
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  MTL_TEST_REQUIRE_DPDK_PMD(ctx, MTL_PORT_P);

  setCompactQueues(ctx, 6);
  const uint64_t mode_flags = MTL_FLAG_RX_VIDEO_MIGRATE | MTL_FLAG_TX_VIDEO_MIGRATE |
                              MTL_FLAG_ALLOW_ACROSS_NUMA_CORE;
  ctx->para.flags |= mode_flags;
  ctx->para.data_quota_mbs_per_sch = 2 * st20_1080p59_yuv422_10bit_bandwidth_mps();
  initDefaultContext();
  if (HasFatalFailure()) return;

  expectInitFlagsSet(ctx->handle, mode_flags);

  /* 3x 1080p59.94 TX exceeds the custom 2x quota (but not default 12x), forcing a 2nd
   * scheduler; freeing all 3 must release it back to baseline. */
  const int base_sch = st_test_sch_cnt(ctx);
  auto tctx = mtl_test::make_context(ctx, 0, 2);
  struct st20_tx_ops ops = makeBasicSt20TxOps(ctx, tctx.get(), "quota_probe", 15200);
  std::vector<ScopedSt20Tx> quota_tx;
  for (int i = 0; i < 3; i++) {
    st20_tx_handle h = st20_tx_create(ctx->handle, &ops);
    ASSERT_NE(h, nullptr);
    quota_tx.emplace_back(h);
    ops.udp_port[MTL_SESSION_PORT_P]++;
  }
  EXPECT_EQ(st_test_sch_cnt(ctx), base_sch + 1);
  EXPECT_EQ(st20_tx_get_sch_idx(quota_tx[0].get()),
            st20_tx_get_sch_idx(quota_tx[1].get()));
  EXPECT_NE(st20_tx_get_sch_idx(quota_tx[2].get()),
            st20_tx_get_sch_idx(quota_tx[0].get()));
  quota_tx.clear();
  EXPECT_EQ(st_test_sch_cnt(ctx), base_sch);

  auto res = runSt20pDigestLoopback(ctx, kInitModeUdpPort + 16);
  expectDigestLoopbackOk(res);
}

/* Replaces --rxtx_simd_512. */
TEST_F(NoCtxTest, init_mode_rxtx_simd_512) {
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  MTL_TEST_REQUIRE_DPDK_PMD(ctx, MTL_PORT_P);
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  if (!__builtin_cpu_supports("avx512f")) {
    GTEST_SKIP() << "Host CPU does not support AVX-512";
  }
#else
  GTEST_SKIP() << "AVX-512 check requires x86_64";
#endif

  setCompactQueues(ctx);
  ctx->para.flags |= MTL_FLAG_RXTX_SIMD_512;
  initDefaultContext();
  if (HasFatalFailure()) return;

  expectInitFlagsSet(ctx->handle, MTL_FLAG_RXTX_SIMD_512);
#ifndef WINDOWSENV
  using GetMaxSimdFn = uint16_t (*)(void);
  GetMaxSimdFn get_max_simd = nullptr;
  void* sym = dlsym(RTLD_DEFAULT, "rte_vect_get_max_simd_bitwidth");
  ASSERT_NE(sym, nullptr) << "rte_vect_get_max_simd_bitwidth not resolved from DPDK EAL";
  std::memcpy(&get_max_simd, &sym, sizeof(get_max_simd));
  EXPECT_EQ(get_max_simd(), 512u)
      << "MTL_FLAG_RXTX_SIMD_512 must force DPDK max SIMD bitwidth to 512";
#endif
  auto res = runSt20pDigestLoopback(ctx, kInitModeUdpPort + 18);
  expectDigestLoopbackOk(res);
}

/* Covers TSC pacing + IOVA PA mode with ST20p converter timeout recovery. */
TEST_F(NoCtxTest, init_mode_pacing_tsc_iova_pa_st20p_timeout) {
  MTL_TEST_REQUIRE_PORTS(ctx, 2);
  MTL_TEST_REQUIRE_DPDK_PMD(ctx, MTL_PORT_P);

  ModeLogTap log_tap;
  setCompactQueues(ctx);
  uint8_t expected_p_ip[MTL_IP_ADDR_LEN];
  memcpy(expected_p_ip, ctx->para.sip_addr[MTL_PORT_P], MTL_IP_ADDR_LEN);
  ctx->para.pacing = ST21_TX_PACING_WAY_TSC;
  ctx->para.iova_mode = MTL_IOVA_MODE_PA;
  ctx->para.rss_mode = MTL_RSS_MODE_L3_L4;
  initDefaultContext();
  if (HasFatalFailure()) return;

  ASSERT_EQ(mtl_iova_mode_get(ctx->handle), MTL_IOVA_MODE_PA);
  EXPECT_EQ(mtl_rss_mode_get(ctx->handle), MTL_RSS_MODE_L3_L4);

  ASSERT_EQ(st_test_convert_plugin_register(ctx), 0);
  struct UnregGuard {
    st_tests_context* c;
    ~UnregGuard() {
      st_test_convert_plugin_unregister(c);
    }
  } unreg{ctx};
  mtl_test::ScopedPluginFaults faults(ctx, /*fail_interval=*/0, /*timeout_interval=*/3,
                                      /*timeout_ms=*/20);

  constexpr uint32_t kW = 1920, kH = 1080;
  constexpr enum st_frame_fmt kFmt = ST_FRAME_FMT_YUV422PLANAR10LE;
  const size_t frame_sz = st_frame_size(kFmt, kW, kH, false);

  struct st20p_tx_ops tx_ops = {};
  tx_ops.name = "tsc_pa_timeout_tx";
  mtl_test::fill_tx_port(tx_ops.port, ctx, kInitModeUdpPort + 20, kInitModePt);
  tx_ops.width = kW;
  tx_ops.height = kH;
  tx_ops.fps = ST_FPS_P59_94;
  tx_ops.input_fmt = kFmt;
  tx_ops.transport_fmt = ST20_FMT_YUV_422_10BIT;
  tx_ops.device = ST_PLUGIN_DEVICE_TEST;
  tx_ops.framebuff_cnt = 3;
  tx_ops.flags = ST20P_TX_FLAG_BLOCK_GET;

  struct st20p_rx_ops rx_ops = {};
  rx_ops.name = "tsc_pa_timeout_rx";
  mtl_test::fill_rx_port(rx_ops.port, ctx, kInitModeUdpPort + 20, kInitModePt);
  rx_ops.width = kW;
  rx_ops.height = kH;
  rx_ops.fps = ST_FPS_P59_94;
  rx_ops.output_fmt = kFmt;
  rx_ops.transport_fmt = ST20_FMT_YUV_422_10BIT;
  rx_ops.device = ST_PLUGIN_DEVICE_TEST;
  rx_ops.framebuff_cnt = 3;
  rx_ops.flags = ST20P_RX_FLAG_BLOCK_GET;

  ScopedSt20pRx rx(st20p_rx_create(ctx->handle, &rx_ops));
  ScopedSt20pTx tx(st20p_tx_create(ctx->handle, &tx_ops));
  ASSERT_TRUE(static_cast<bool>(rx));
  ASSERT_TRUE(static_cast<bool>(tx));
  EXPECT_EQ(st20p_tx_set_block_timeout(tx.get(), 200 * NS_PER_MS), 0);
  EXPECT_EQ(st20p_rx_set_block_timeout(rx.get(), 200 * NS_PER_MS), 0);

  unsigned char expect_sha[3][SHA256_DIGEST_LENGTH];
  for (uint16_t i = 0; i < 3; i++) {
    auto* fb = static_cast<uint8_t*>(st20p_tx_get_fb_addr(tx.get(), i));
    ASSERT_NE(fb, nullptr);
    st_test_rand_data(fb, frame_sz - SHA256_DIGEST_LENGTH, 0x70 + i);
    SHA256(fb, frame_sz - SHA256_DIGEST_LENGTH, expect_sha[i]);
    /* ST_PLUGIN_DEVICE_TEST forwards the trailing SHA-256 digest through TX and RX. */
    memcpy(fb + frame_sz - SHA256_DIGEST_LENGTH, expect_sha[i], SHA256_DIGEST_LENGTH);
  }

  std::atomic<bool> stop{false};
  std::atomic<int> rx_ok{0}, sha_fail{0};
  std::thread rx_th([&]() {
    while (!stop.load(std::memory_order_acquire)) {
      struct st_frame* f = st20p_rx_get_frame(rx.get());
      if (!f) continue;
      if (st_is_frame_complete(f->status) && f->addr[0] && f->data_size == frame_sz) {
        const auto* got_sha =
            static_cast<const uint8_t*>(f->addr[0]) + frame_sz - SHA256_DIGEST_LENGTH;
        bool matched = false;
        for (int k = 0; k < 3; k++) {
          if (memcmp(got_sha, expect_sha[k], SHA256_DIGEST_LENGTH) == 0) {
            matched = true;
            break;
          }
        }
        if (matched)
          rx_ok.fetch_add(1, std::memory_order_relaxed);
        else
          sha_fail.fetch_add(1, std::memory_order_relaxed);
      }
      EXPECT_GE(st20p_rx_put_frame(rx.get(), f), 0);
    }
  });
  std::thread tx_th([&]() {
    while (!stop.load(std::memory_order_acquire)) {
      struct st_frame* f = st20p_tx_get_frame(tx.get());
      if (!f) continue;
      EXPECT_GE(st20p_tx_put_frame(tx.get(), f), 0);
    }
  });
  mtl_test::ScopedWorker rx_w(nullptr, std::move(rx_th), [&]() {
    stop.store(true, std::memory_order_release);
    st20p_rx_wake_block(rx.get());
  });
  mtl_test::ScopedWorker tx_w(nullptr, std::move(tx_th), [&]() {
    stop.store(true, std::memory_order_release);
    st20p_tx_wake_block(tx.get());
  });

  ASSERT_GE(mtl_start(ctx->handle), 0);
  EXPECT_TRUE(mtl_test::wait_until(
      [&]() { return rx_ok.load(std::memory_order_relaxed) >= 24; }, kInitModeTimeoutMs));
  tx_w.stop();
  rx_w.stop();
  EXPECT_GE(mtl_stop(ctx->handle), 0);
  rx.reset();
  tx.reset();

  EXPECT_GE(rx_ok.load(std::memory_order_relaxed), 24);
  EXPECT_EQ(sha_fail.load(std::memory_order_relaxed), 0);
  EXPECT_TRUE(ModeLogTap::saw_dev_tsc.load(std::memory_order_acquire));
  EXPECT_TRUE(ModeLogTap::saw_tx_tsc.load(std::memory_order_acquire));
  verifyModePortIpAndSt20pControl(ctx, kInitModeUdpPort + 54, expected_p_ip);
}
