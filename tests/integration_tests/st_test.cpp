/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef WINDOWSENV
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "log.h"
#include "test_harness.hpp"

int st_test_sch_cnt(struct st_tests_context* ctx) {
  struct mtl_var_info var;
  int ret = mtl_get_var_info(ctx->handle, &var);
  if (ret < 0) return ret;
  return var.sch_cnt;
}

bool st_test_dma_available(struct st_tests_context* ctx) {
  if (ctx->iova == MTL_IOVA_MODE_PA) {
    info("%s, DMA not full supported under IOVA PA mode\n", __func__);
    return false;
  }

  struct mtl_var_info var;
  if (mtl_get_var_info(ctx->handle, &var) < 0) return false;

  struct mtl_fix_info fix;
  if (mtl_get_fix_info(ctx->handle, &fix) < 0) return false;

  return var.dma_dev_cnt < fix.dma_dev_cnt_max;
}

namespace {

int test_lcore_cnt(struct st_tests_context* ctx) {
  struct mtl_var_info var;
  int ret = mtl_get_var_info(ctx->handle, &var);
  if (ret < 0) return ret;
  return var.lcore_cnt;
}

bool test_dev_started(struct st_tests_context* ctx) {
  struct mtl_var_info var;
  if (mtl_get_var_info(ctx->handle, &var) < 0) return false;
  return var.dev_started;
}

void start_stop_test(int repeat) {
  mtl_handle handle = st_test_ctx()->handle;
  mtl_test::ScopedMtlStart guard(handle);
  for (int i = 0; i < repeat; i++) {
    EXPECT_GE(guard.start(), 0);
    EXPECT_GE(guard.stop(), 0);
  }
}

void test_lcore_one(struct st_tests_context* ctx) {
  mtl_handle handle = ctx->handle;
  const int base_cnt = test_lcore_cnt(ctx);
  unsigned int lcore = 0;

  ASSERT_GE(mtl_get_lcore(handle, &lcore), 0);
  mtl_test::ScopedLcore guard(handle, lcore);
  EXPECT_EQ(test_lcore_cnt(ctx), base_cnt + 1);
  EXPECT_GE(guard.reset(), 0);
  EXPECT_EQ(test_lcore_cnt(ctx), base_cnt);
}

void hp_malloc_test(struct st_tests_context* ctx, size_t size, enum mtl_port port,
                    bool zero, bool expect_succ) {
  if (zero && expect_succ) {
    /* Dirty a same-sized allocation first so zmalloc proves it actually zeroes memory. */
    mtl_test::ScopedHpBuf dirty(ctx->handle, size, port, false);
    ASSERT_TRUE(static_cast<bool>(dirty));
    memset(dirty.data(), 0x5A, size);
    dirty.reset();
  }
  mtl_test::ScopedHpBuf buf(ctx->handle, size, port, zero);
  EXPECT_EQ(static_cast<bool>(buf), expect_succ);
  if (buf) {
    if (mtl_pmd_is_dpdk_based(ctx->handle, port)) {
      EXPECT_NE(buf.iova(), static_cast<mtl_iova_t>(0));
      EXPECT_NE(buf.iova(), MTL_BAD_IOVA);
    }
    if (zero) {
      std::vector<uint8_t> zeros(size, 0);
      EXPECT_EQ(0, memcmp(buf.data(), zeros.data(), size));
    }
    memset(buf.data(), 0x5A, size);
  }
}

void hp_malloc_tests(struct st_tests_context* ctx, enum mtl_port port, bool zero) {
  for (size_t sz : {size_t{1}, size_t{1024}, size_t{1024 + 3}}) {
    hp_malloc_test(ctx, sz, port, zero, true);
  }
}

int port_stats_tx_next_frame(void* priv, uint16_t* next_frame_idx,
                             struct st30_tx_frame_meta* /*meta*/) {
  return tx_next_frame(priv, next_frame_idx);
}

int port_stats_rx_frame_ready(void* priv, void* frame,
                              struct st30_rx_frame_meta* /*meta*/) {
  auto* c = static_cast<tests_context*>(priv);
  auto h = static_cast<st30_rx_handle>(__atomic_load_n(&c->handle, __ATOMIC_ACQUIRE));
  if (!h) return -EIO;
  (void)st30_rx_put_framebuff(h, frame);
  __atomic_fetch_add(&c->fb_rec, 1, __ATOMIC_RELAXED);
  return 0;
}

std::atomic<int> g_log_prefix_calls{0};
std::atomic<int> g_log_printer_calls{0};
std::atomic<bool> g_log_printer_saw_prefix{false};
std::atomic<bool> g_log_printer_saw_sleep_info{false};

void reset_log_probe() {
  g_log_prefix_calls.store(0, std::memory_order_relaxed);
  g_log_printer_calls.store(0, std::memory_order_relaxed);
  g_log_printer_saw_prefix.store(false, std::memory_order_relaxed);
  g_log_printer_saw_sleep_info.store(false, std::memory_order_relaxed);
}

void test_log_prefix_formatter(char* buf, size_t buf_sz) {
  g_log_prefix_calls.fetch_add(1, std::memory_order_relaxed);
  snprintf(buf, buf_sz, "TEST_PFX: ");
}

void test_log_printer(enum mtl_log_level level, const char* format, ...) {
  g_log_printer_calls.fetch_add(1, std::memory_order_relaxed);
  char msg[512];
  va_list args;
  va_start(args, format);
  vsnprintf(msg, sizeof(msg), format, args);
  va_end(args);
  if (strstr(msg, "TEST_PFX:") != nullptr) {
    g_log_printer_saw_prefix.store(true, std::memory_order_release);
  }
  if (level == MTL_LOG_LEVEL_INFO && strstr(msg, "mtl_sch_set_sleep_us") != nullptr) {
    g_log_printer_saw_sleep_info.store(true, std::memory_order_release);
  }
}

class ScopedLogHooks {
 public:
  explicit ScopedLogHooks(mtl_handle handle)
      : handle_(handle), orig_level_(mtl_get_log_level(handle)) {
  }
  ~ScopedLogHooks() {
    mtl_set_log_printer(nullptr);
    mtl_set_log_prefix_formatter(nullptr);
    if (handle_) mtl_set_log_level(handle_, orig_level_);
    mtl_openlog_stream(nullptr);
    if (log_fp_) {
      fclose(log_fp_);
      log_fp_ = nullptr;
    }
  }
  ScopedLogHooks(const ScopedLogHooks&) = delete;
  ScopedLogHooks& operator=(const ScopedLogHooks&) = delete;

  enum mtl_log_level orig() const {
    return orig_level_;
  }

  void set_stream(FILE* fp) {
    log_fp_ = fp;
  }

 private:
  mtl_handle handle_ = nullptr;
  enum mtl_log_level orig_level_ = MTL_LOG_LEVEL_INFO;
  FILE* log_fp_ = nullptr;
};

}  // namespace

TEST(Main, init_expect_fail) {
  auto* ctx = st_test_ctx();
  struct mtl_init_params para = {};

  /* Empty params must fail. */
  EXPECT_EQ(mtl_init(&para), nullptr);

  /* Missing port name and IP must fail. */
  para.num_ports = 1;
  EXPECT_EQ(mtl_init(&para), nullptr);

  memcpy(mtl_p_sip_addr(&para), ctx->para.sip_addr[MTL_PORT_P], MTL_IP_ADDR_LEN);
  EXPECT_EQ(mtl_init(&para), nullptr);

  /* Invalid PCI BDF must fail. */
  snprintf(para.port[MTL_PORT_P], sizeof(para.port[MTL_PORT_P]), "0000:55:00.0");
  EXPECT_EQ(mtl_init(&para), nullptr);

  memcpy(mtl_r_sip_addr(&para), ctx->para.sip_addr[MTL_PORT_R], MTL_IP_ADDR_LEN);

  /* Invalid num_ports values must fail. */
  for (int invalid_ports : {0, 100, -1}) {
    para.num_ports = invalid_ports;
    EXPECT_EQ(mtl_init(&para), nullptr);
  }
}

TEST(Main, re_init_fail) {
  auto* ctx = st_test_ctx();
  EXPECT_EQ(mtl_init(&ctx->para), nullptr);
}

TEST(Main, start_stop_single) {
  start_stop_test(1);
}

TEST(Main, start_stop_multi) {
  start_stop_test(5);
}

TEST(Main, start_idempotent_and_null) {
  mtl_handle handle = st_test_ctx()->handle;
  EXPECT_LT(mtl_start(nullptr), 0);
  EXPECT_LT(mtl_uninit(nullptr), 0);
  mtl_test::ScopedMtlStart guard(handle);
  EXPECT_GE(guard.start(), 0);
  EXPECT_GE(mtl_start(handle), 0);
  EXPECT_GE(guard.stop(), 0);
}

TEST(Main, stop_idempotent_and_null) {
  mtl_handle handle = st_test_ctx()->handle;
  EXPECT_LT(mtl_stop(nullptr), 0);
  mtl_test::ScopedMtlStart guard(handle);
  EXPECT_GE(mtl_stop(handle), 0);
  EXPECT_GE(guard.start(), 0);
  EXPECT_GE(guard.stop(), 0);
  EXPECT_GE(mtl_stop(handle), 0);
}

TEST(Main, get_fix) {
  auto* ctx = st_test_ctx();
  struct mtl_fix_info fix = {};
  ASSERT_GE(mtl_get_fix_info(ctx->handle, &fix), 0);
  EXPECT_EQ(fix.num_ports, ctx->para.num_ports);
  EXPECT_EQ(fix.init_flags & MTL_FLAG_BIND_NUMA, ctx->para.flags & MTL_FLAG_BIND_NUMA);
  info("dma dev count %u\n", fix.dma_dev_cnt_max);
  info("init_flags 0x%" PRIx64 "\n", fix.init_flags);
}

TEST(Main, get_var) {
  auto* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  const bool auto_start = (ctx->para.flags & MTL_FLAG_DEV_AUTO_START_STOP) != 0;
  struct mtl_fix_info fix = {};
  struct mtl_var_info var;
  ASSERT_EQ(mtl_get_fix_info(handle, &fix), 0);

  memset(&var, 0xFF, sizeof(var));
  ASSERT_EQ(mtl_get_var_info(handle, &var), 0);
  EXPECT_LE(var.dma_dev_cnt, fix.dma_dev_cnt_max);
  EXPECT_EQ(var.dev_started, auto_start);
  const uint8_t base_lcore_cnt = var.lcore_cnt;

  {
    mtl_test::ScopedMtlStart start_guard(handle);
    ASSERT_GE(start_guard.start(), 0);
    memset(&var, 0xFF, sizeof(var));
    ASSERT_EQ(mtl_get_var_info(handle, &var), 0);
    EXPECT_TRUE(var.dev_started);

    ASSERT_GE(start_guard.stop(), 0);
    memset(&var, 0xFF, sizeof(var));
    ASSERT_EQ(mtl_get_var_info(handle, &var), 0);
    EXPECT_EQ(var.dev_started, auto_start);
  }

  unsigned int lcore = 0;
  ASSERT_GE(mtl_get_lcore(handle, &lcore), 0);
  mtl_test::ScopedLcore lcore_guard(handle, lcore);
  memset(&var, 0xFF, sizeof(var));
  ASSERT_EQ(mtl_get_var_info(handle, &var), 0);
  EXPECT_EQ(var.lcore_cnt, base_lcore_cnt + 1);

  EXPECT_EQ(lcore_guard.reset(), 0);
  memset(&var, 0xFF, sizeof(var));
  ASSERT_EQ(mtl_get_var_info(handle, &var), 0);
  EXPECT_EQ(var.lcore_cnt, base_lcore_cnt);
}

TEST(Main, lcore) {
  test_lcore_one(st_test_ctx());
}

TEST(Main, bind_to_lcore) {
  auto* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  unsigned int lcore1 = 0;
  unsigned int lcore2 = 0;

  ASSERT_GE(mtl_get_lcore(handle, &lcore1), 0);
  mtl_test::ScopedLcore guard1(handle, lcore1);
  ASSERT_GE(mtl_get_lcore(handle, &lcore2), 0);
  mtl_test::ScopedLcore guard2(handle, lcore2);
  EXPECT_NE(lcore1, lcore2);

  std::atomic<bool> done{false};
  std::thread worker([&done]() {
    while (!done.load(std::memory_order_acquire)) {
      mtl_sleep_us(1000);
    }
  });
  const auto tid = worker.native_handle();
  mtl_test::ScopedWorker worker_guard(nullptr, std::move(worker), [&done]() {
    done.store(true, std::memory_order_release);
  });

  EXPECT_EQ(mtl_bind_to_lcore(handle, tid, lcore1), 0);
#ifndef WINDOWSENV
  /* Verify single-CPU pinning without assuming lcore_id == OS cpu_id
   * (false under --remap-lcore-ids). */
  cpu_set_t set1;
  CPU_ZERO(&set1);
  ASSERT_EQ(pthread_getaffinity_np(tid, sizeof(set1), &set1), 0);
  EXPECT_EQ(CPU_COUNT(&set1), 1);
#endif

  EXPECT_EQ(mtl_bind_to_lcore(handle, tid, lcore2), 0);
#ifndef WINDOWSENV
  cpu_set_t set2;
  CPU_ZERO(&set2);
  ASSERT_EQ(pthread_getaffinity_np(tid, sizeof(set2), &set2), 0);
  EXPECT_EQ(CPU_COUNT(&set2), 1);
  EXPECT_FALSE(CPU_EQUAL(&set1, &set2));
#endif

  EXPECT_LT(mtl_bind_to_lcore(handle, tid, 10000), 0);
}

TEST(Main, lcore_max) {
  auto* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  const int base_cnt = test_lcore_cnt(ctx);
  constexpr int kMaxLcores = 100;

  std::vector<mtl_test::ScopedLcore> lcores;
  lcores.reserve(kMaxLcores);
  for (int i = 0; i < kMaxLcores; i++) {
    unsigned int lcore = 0;
    if (mtl_get_lcore(handle, &lcore) < 0) break;
    lcores.emplace_back(handle, lcore);
  }
  EXPECT_EQ(test_lcore_cnt(ctx), base_cnt + static_cast<int>(lcores.size()));
  lcores.clear();
  EXPECT_EQ(test_lcore_cnt(ctx), base_cnt);

  test_lcore_one(ctx);
}

TEST(Main, lcore_expect_fail) {
  auto* ctx = st_test_ctx();
  /* Avoid double-putting a released low lcore ID because another process may acquire it
   * via MtlManager in between. Validate out-of-range IDs instead. */
  EXPECT_LT(mtl_put_lcore(ctx->handle, 10000), 0);
  EXPECT_LT(mtl_put_lcore(ctx->handle, 0xFFFFFFFFu), 0);

  test_lcore_one(ctx);
}

TEST(Main, dev_started) {
  auto* ctx = st_test_ctx();
  const bool auto_start = (ctx->para.flags & MTL_FLAG_DEV_AUTO_START_STOP) != 0;
  mtl_test::ScopedMtlStart guard(ctx->handle);
  EXPECT_EQ(test_dev_started(ctx), auto_start);
  EXPECT_GE(guard.start(), 0);
  EXPECT_TRUE(test_dev_started(ctx));
  EXPECT_GE(guard.stop(), 0);
  EXPECT_EQ(test_dev_started(ctx), auto_start);
}

TEST(Main, port_stats_and_reset) {
  auto* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  struct mtl_port_status stats;

  /* Run a brief self-contained ST30 loopback session so port TX/RX counters are non-zero
   * before testing mtl_reset_port_stats(). */
  const int pkt_size =
      st30_get_packet_size(ST30_FMT_PCM16, ST30_PTIME_1MS, ST30_SAMPLING_48K, 2);
  ASSERT_GT(pkt_size, 0);

  auto tx_ctx = mtl_test::make_context(ctx, 0, 3);
  auto rx_ctx = mtl_test::make_context(ctx, 0, 3);
  ASSERT_TRUE(tx_ctx != nullptr);
  ASSERT_TRUE(rx_ctx != nullptr);

  constexpr uint16_t kPortStatsUdpPort = 20500;
  constexpr uint8_t kPortStatsPt = 111;
  struct st30_tx_ops tx_ops = {};
  tx_ops.name = "port_stats_tx";
  tx_ops.priv = tx_ctx.get();
  if (ctx->para.num_ports >= 2) {
    mtl_test::fill_raw_tx_port(tx_ops, ctx, kPortStatsUdpPort, kPortStatsPt);
  } else {
    tx_ops.num_port = 1;
    memcpy(tx_ops.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(tx_ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    tx_ops.udp_port[MTL_SESSION_PORT_P] = kPortStatsUdpPort;
    tx_ops.payload_type = kPortStatsPt;
  }
  tx_ops.type = ST30_TYPE_FRAME_LEVEL;
  tx_ops.fmt = ST30_FMT_PCM16;
  tx_ops.channel = 2;
  tx_ops.sampling = ST30_SAMPLING_48K;
  tx_ops.ptime = ST30_PTIME_1MS;
  tx_ops.framebuff_cnt = static_cast<uint16_t>(tx_ctx->fb_cnt);
  tx_ops.framebuff_size = static_cast<uint32_t>(pkt_size);
  tx_ops.get_next_frame = port_stats_tx_next_frame;

  mtl_test::ScopedHandle<st30_tx_handle, st30_tx_free> tx(
      st30_tx_create(handle, &tx_ops));
  ASSERT_TRUE(static_cast<bool>(tx));
  __atomic_store_n(&tx_ctx->handle, tx.get(), __ATOMIC_RELEASE);

  mtl_test::ScopedHandle<st30_rx_handle, st30_rx_free> rx;
  if (ctx->para.num_ports >= 2) {
    struct st30_rx_ops rx_ops = {};
    rx_ops.name = "port_stats_rx";
    rx_ops.priv = rx_ctx.get();
    mtl_test::fill_raw_rx_port(rx_ops, ctx, kPortStatsUdpPort, kPortStatsPt);
    rx_ops.type = ST30_TYPE_FRAME_LEVEL;
    rx_ops.fmt = ST30_FMT_PCM16;
    rx_ops.channel = 2;
    rx_ops.sampling = ST30_SAMPLING_48K;
    rx_ops.ptime = ST30_PTIME_1MS;
    rx_ops.framebuff_cnt = static_cast<uint16_t>(rx_ctx->fb_cnt);
    rx_ops.framebuff_size = static_cast<uint32_t>(pkt_size);
    rx_ops.notify_frame_ready = port_stats_rx_frame_ready;
    rx.reset(st30_rx_create(handle, &rx_ops));
    ASSERT_TRUE(static_cast<bool>(rx));
    __atomic_store_n(&rx_ctx->handle, rx.get(), __ATOMIC_RELEASE);
  }

  {
    mtl_test::ScopedMtlStart mtl_guard(handle);
    ASSERT_GE(mtl_guard.start(), 0);
    EXPECT_TRUE(mtl_test::wait_until(
        [&]() {
          return mtl_test::tx_frames(tx_ctx.get()) >= 30 &&
                 (!rx || mtl_test::rx_frames(rx_ctx.get()) >= 15);
        },
        3000, 5));
    EXPECT_GE(mtl_guard.stop(), 0);
  }
  rx.reset();
  tx.reset();

  memset(&stats, 0xFF, sizeof(stats));
  ASSERT_EQ(mtl_get_port_stats(handle, MTL_PORT_P, &stats), 0);
  EXPECT_GT(stats.tx_packets, 0u);
  EXPECT_GT(stats.tx_bytes, 0u);

  if (ctx->para.num_ports >= 2) {
    memset(&stats, 0xFF, sizeof(stats));
    ASSERT_EQ(mtl_get_port_stats(handle, MTL_PORT_R, &stats), 0);
    EXPECT_GT(stats.rx_packets, 0u);
    EXPECT_GT(stats.rx_bytes, 0u);
  }

  EXPECT_EQ(mtl_reset_port_stats(handle, MTL_PORT_P), 0);
  memset(&stats, 0xFF, sizeof(stats));
  ASSERT_EQ(mtl_get_port_stats(handle, MTL_PORT_P, &stats), 0);
  EXPECT_EQ(stats.tx_packets, 0u);
  EXPECT_EQ(stats.tx_bytes, 0u);

  for (int port = 1; port < ctx->para.num_ports; port++) {
    auto p = static_cast<enum mtl_port>(port);
    EXPECT_EQ(mtl_reset_port_stats(handle, p), 0);
    memset(&stats, 0xFF, sizeof(stats));
    EXPECT_EQ(mtl_get_port_stats(handle, p, &stats), 0);
  }

  if (ctx->para.num_ports < MTL_PORT_MAX) {
    auto oob = static_cast<enum mtl_port>(ctx->para.num_ports);
    EXPECT_LT(mtl_get_port_stats(handle, oob, &stats), 0);
    EXPECT_LT(mtl_reset_port_stats(handle, oob), 0);
  }
  EXPECT_LT(mtl_get_port_stats(handle, static_cast<enum mtl_port>(MTL_PORT_MAX), &stats),
            0);
  EXPECT_LT(mtl_reset_port_stats(handle, static_cast<enum mtl_port>(MTL_PORT_MAX)), 0);
}

TEST(Main, mode_and_port_info_queries) {
  auto* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;

  EXPECT_EQ(mtl_rss_mode_get(nullptr), MTL_RSS_MODE_MAX);
  EXPECT_EQ(mtl_iova_mode_get(nullptr), MTL_IOVA_MODE_MAX);
  const enum mtl_rss_mode rss = mtl_rss_mode_get(handle);
  EXPECT_LT(rss, MTL_RSS_MODE_MAX);
  if (ctx->para.pmd[MTL_PORT_P] == MTL_PMD_DPDK_USER ||
      ctx->para.rss_mode != MTL_RSS_MODE_NONE) {
    EXPECT_EQ(rss, ctx->para.rss_mode);
  }
  const enum mtl_iova_mode iova = mtl_iova_mode_get(handle);
  if (ctx->para.iova_mode != MTL_IOVA_MODE_AUTO) {
    EXPECT_EQ(iova, ctx->para.iova_mode);
  } else {
    EXPECT_TRUE(iova == MTL_IOVA_MODE_VA || iova == MTL_IOVA_MODE_PA);
  }
  EXPECT_EQ(iova, ctx->iova);

  for (int port = 0; port < ctx->para.num_ports; port++) {
    auto p = static_cast<enum mtl_port>(port);
    uint8_t ip[MTL_IP_ADDR_LEN];
    uint8_t netmask[MTL_IP_ADDR_LEN];
    uint8_t gateway[MTL_IP_ADDR_LEN];
    memset(ip, 0xFF, sizeof(ip));
    memset(netmask, 0xFF, sizeof(netmask));
    memset(gateway, 0xFF, sizeof(gateway));
    EXPECT_EQ(mtl_port_ip_info(handle, p, ip, netmask, gateway), 0);
    const uint32_t mask = (static_cast<uint32_t>(netmask[0]) << 24) |
                          (static_cast<uint32_t>(netmask[1]) << 16) |
                          (static_cast<uint32_t>(netmask[2]) << 8) | netmask[3];
    EXPECT_NE(mask, 0u);
    EXPECT_NE(mask, 0xFFFFFFFFu);
    EXPECT_EQ((~mask + 1u) & (~mask), 0u);
    const uint8_t kSentinel[MTL_IP_ADDR_LEN] = {0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_NE(memcmp(gateway, kSentinel, MTL_IP_ADDR_LEN), 0);
    if (!ctx->dhcp && ctx->para.pmd[port] == MTL_PMD_DPDK_USER) {
      /* Compare against pre-init configured IP rather than para.sip_addr populated by
       * mtl_port_ip_info() so a zero/corrupt mtl_port_ip_info() fails. */
      EXPECT_EQ(memcmp(ip, ctx->init_sip_addr[port], MTL_IP_ADDR_LEN), 0);
      const uint8_t kDefaultMask[MTL_IP_ADDR_LEN] = {255, 255, 255, 0};
      const uint8_t kZeroGw[MTL_IP_ADDR_LEN] = {0, 0, 0, 0};
      EXPECT_EQ(memcmp(netmask, kDefaultMask, MTL_IP_ADDR_LEN), 0);
      EXPECT_EQ(memcmp(gateway, kZeroGw, MTL_IP_ADDR_LEN), 0);
    } else if (!ctx->dhcp) {
      EXPECT_EQ(memcmp(ip, ctx->para.sip_addr[port], MTL_IP_ADDR_LEN), 0);
    }
    const bool expected_dpdk = (ctx->para.pmd[port] == MTL_PMD_DPDK_USER) ||
                               (ctx->para.pmd[port] == MTL_PMD_DPDK_AF_XDP) ||
                               (ctx->para.pmd[port] == MTL_PMD_DPDK_AF_PACKET);
    EXPECT_EQ(mtl_pmd_is_dpdk_based(handle, p), expected_dpdk);
  }

  uint8_t ip[MTL_IP_ADDR_LEN];
  memset(ip, 0xFF, sizeof(ip));
  /* Optional output pointers may be null independently. */
  EXPECT_EQ(mtl_port_ip_info(handle, MTL_PORT_P, ip, nullptr, nullptr), 0);
  EXPECT_EQ(mtl_port_ip_info(handle, MTL_PORT_P, nullptr, nullptr, nullptr), 0);
  if (!ctx->dhcp) {
    EXPECT_EQ(memcmp(ip, ctx->para.sip_addr[MTL_PORT_P], MTL_IP_ADDR_LEN), 0);
  }

  EXPECT_LT(mtl_port_ip_info(nullptr, MTL_PORT_P, ip, nullptr, nullptr), 0);
  EXPECT_LT(mtl_port_ip_info(handle, static_cast<enum mtl_port>(MTL_PORT_MAX), ip,
                             nullptr, nullptr),
            0);
}

TEST(Main, get_numa_id) {
  auto* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;

  for (int port = 0; port < ctx->para.num_ports; port++) {
    auto p = static_cast<enum mtl_port>(port);
    const int numa = mtl_get_numa_id(handle, p);
    EXPECT_GE(numa, 0);
#ifndef WINDOWSENV
    if (ctx->para.pmd[port] == MTL_PMD_DPDK_USER &&
        !(ctx->para.port_params[port].flags & MTL_PORT_FLAG_FORCE_NUMA)) {
      char path[128];
      snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/numa_node",
               ctx->para.port[port]);
      FILE* fp = fopen(path, "r");
      if (fp) {
        int sys_numa = -1;
        if (fscanf(fp, "%d", &sys_numa) == 1) {
          if (sys_numa < 0) sys_numa = 0;
          EXPECT_EQ(numa, sys_numa);
        }
        fclose(fp);
      }
    }
#endif
  }
  if (ctx->para.num_ports < MTL_PORT_MAX) {
    EXPECT_LT(mtl_get_numa_id(handle, static_cast<enum mtl_port>(ctx->para.num_ports)),
              0);
  }
  EXPECT_LT(mtl_get_numa_id(handle, static_cast<enum mtl_port>(MTL_PORT_MAX)), 0);
}

TEST(Misc, hp_malloc) {
  auto* ctx = st_test_ctx();
  hp_malloc_tests(ctx, MTL_PORT_P, false);
  if (ctx->para.num_ports > 1) hp_malloc_tests(ctx, MTL_PORT_R, false);
}

TEST(Misc, hp_zmalloc) {
  auto* ctx = st_test_ctx();
  hp_malloc_tests(ctx, MTL_PORT_P, true);
  if (ctx->para.num_ports > 1) hp_malloc_tests(ctx, MTL_PORT_R, true);
}

TEST(Misc, hp_malloc_expect_fail) {
  auto* ctx = st_test_ctx();
  hp_malloc_test(ctx, 0, MTL_PORT_P, false, false);
  if (ctx->para.num_ports > 1) hp_malloc_test(ctx, 0, MTL_PORT_R, false, false);
  if (ctx->para.num_ports < MTL_PORT_MAX) {
    hp_malloc_test(ctx, 1024, static_cast<enum mtl_port>(ctx->para.num_ports), false,
                   false);
  }
  hp_malloc_test(ctx, 1024, static_cast<enum mtl_port>(MTL_PORT_MAX), false, false);
  EXPECT_EQ(mtl_hp_virt2iova(ctx->handle, nullptr), MTL_BAD_IOVA);
}

TEST(Misc, hp_zmalloc_expect_fail) {
  auto* ctx = st_test_ctx();
  hp_malloc_test(ctx, 0, MTL_PORT_P, true, false);
  if (ctx->para.num_ports > 1) hp_malloc_test(ctx, 0, MTL_PORT_R, true, false);
  if (ctx->para.num_ports < MTL_PORT_MAX) {
    hp_malloc_test(ctx, 1024, static_cast<enum mtl_port>(ctx->para.num_ports), true,
                   false);
  }
  hp_malloc_test(ctx, 1024, static_cast<enum mtl_port>(MTL_PORT_MAX), true, false);
}

TEST(Misc, page_size) {
  const size_t pg_sz = mtl_page_size(st_test_ctx()->handle);
  EXPECT_GT(pg_sz, 0u);
  EXPECT_EQ(pg_sz & (pg_sz - 1), 0u);
#ifndef WINDOWSENV
  const long sys_pg_sz = sysconf(_SC_PAGESIZE);
  ASSERT_GT(sys_pg_sz, 0);
  EXPECT_EQ(pg_sz, static_cast<size_t>(sys_pg_sz));
#endif
}

TEST(Misc, ptp) {
  auto* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  const bool user_ptp = (ctx->para.ptp_get_time_fn != nullptr);

  /* Wait > 10 ms to guarantee a fresh sync before testing the 10 ms TSC cache hit. */
  st_usleep(12 * 1000);
  const uint64_t cb_before_sync = __atomic_load_n(&ctx->ptp_time, __ATOMIC_ACQUIRE);
  uint64_t prev = mtl_ptp_read_time(handle);
  const uint64_t cb_after_sync = __atomic_load_n(&ctx->ptp_time, __ATOMIC_ACQUIRE);
  EXPECT_GT(prev, 0u);
  if (user_ptp) {
    EXPECT_GT(cb_after_sync, cb_before_sync);
    /* First read on cache miss must match the PTP callback value within 5 us. */
    const uint64_t cb_diff =
        (prev > cb_after_sync) ? (prev - cb_after_sync) : (cb_after_sync - prev);
    EXPECT_LT(cb_diff, static_cast<uint64_t>(5) * NS_PER_US);
  }

  /* Within the 10 ms cache window (2 ms sleep), mtl_ptp_read_time() must hit the TSC
   * cache without invoking the user PTP callback while advancing >= 1 ms. */
  st_usleep(2 * 1000);
  const uint64_t cached = mtl_ptp_read_time(handle);
  const uint64_t cb_after_2ms = __atomic_load_n(&ctx->ptp_time, __ATOMIC_ACQUIRE);
  EXPECT_GT(cached, prev);
  EXPECT_GE(cached - prev, static_cast<uint64_t>(NS_PER_MS));
  if (user_ptp) {
    EXPECT_EQ(cb_after_2ms, cb_after_sync);
    const uint64_t real_time = st_test_get_realtime_ns();
    const uint64_t diff =
        (cached > real_time) ? (cached - real_time) : (real_time - cached);
    EXPECT_LT(diff, static_cast<uint64_t>(50) * NS_PER_US);
  }
  prev = cached;

  /* Uncached raw read must bypass the active 10 ms cache and invoke the callback. */
  if (user_ptp) {
    const uint64_t cb_before_raw = __atomic_load_n(&ctx->ptp_time, __ATOMIC_ACQUIRE);
    st_usleep(100);
    const uint64_t rt_before = st_test_get_realtime_ns();
    const uint64_t ptp_raw = mtl_ptp_read_time_raw(handle);
    const uint64_t rt_after = st_test_get_realtime_ns();
    const uint64_t cb_after_raw = __atomic_load_n(&ctx->ptp_time, __ATOMIC_ACQUIRE);
    EXPECT_GT(ptp_raw, 0u);
    EXPECT_GT(cb_after_raw, cb_before_raw);
    EXPECT_EQ(ptp_raw, cb_after_raw);
    EXPECT_GE(ptp_raw, rt_before);
    EXPECT_LE(ptp_raw, rt_after);
    EXPECT_GT(ptp_raw, prev);
  } else {
    const uint64_t ptp_raw = mtl_ptp_read_time_raw(handle);
    EXPECT_GT(ptp_raw, 0u);
  }

  /* Cached reads must stay strictly monotonic and accurate over repeated 2 ms polls. */
  for (int i = 0; i < 4; i++) {
    st_usleep(2 * 1000);
    const uint64_t cur = mtl_ptp_read_time(handle);
    EXPECT_GT(cur, prev);
    if (user_ptp) {
      const uint64_t real_time = st_test_get_realtime_ns();
      const uint64_t diff = (cur > real_time) ? (cur - real_time) : (real_time - cur);
      EXPECT_LT(diff, static_cast<uint64_t>(50) * NS_PER_US);
    }
    prev = cur;
  }

  /* Wait > 10 ms cache window so mtl_ptp_read_time() refreshes from source. */
  const uint64_t cb_before_refresh = __atomic_load_n(&ctx->ptp_time, __ATOMIC_ACQUIRE);
  st_usleep(12 * 1000);
  const uint64_t refreshed = mtl_ptp_read_time(handle);
  const uint64_t cb_after_refresh = __atomic_load_n(&ctx->ptp_time, __ATOMIC_ACQUIRE);
  EXPECT_GT(refreshed, prev);
  if (user_ptp) {
    EXPECT_GT(cb_after_refresh, cb_before_refresh);
    const uint64_t cb_diff = (refreshed > cb_after_refresh)
                                 ? (refreshed - cb_after_refresh)
                                 : (cb_after_refresh - refreshed);
    EXPECT_LT(cb_diff, static_cast<uint64_t>(5) * NS_PER_US);
  }
}

TEST(Misc, log_level) {
  mtl_handle handle = st_test_ctx()->handle;
  ScopedLogHooks guard(handle);

  EXPECT_GE(mtl_set_log_level(handle, MTL_LOG_LEVEL_INFO), 0);
  EXPECT_EQ(mtl_get_log_level(handle), MTL_LOG_LEVEL_INFO);
  ASSERT_EQ(mtl_set_log_printer(test_log_printer), 0);

  reset_log_probe();
  EXPECT_EQ(mtl_sch_set_sleep_us(handle, 0), 0);
  EXPECT_GT(g_log_printer_calls.load(std::memory_order_acquire), 0);
  EXPECT_TRUE(g_log_printer_saw_sleep_info.load(std::memory_order_acquire));

  EXPECT_GE(mtl_set_log_level(handle, MTL_LOG_LEVEL_ERR), 0);
  EXPECT_EQ(mtl_get_log_level(handle), MTL_LOG_LEVEL_ERR);
  reset_log_probe();
  EXPECT_EQ(mtl_sch_set_sleep_us(handle, 0), 0);
  EXPECT_EQ(g_log_printer_calls.load(std::memory_order_acquire), 0);

  EXPECT_LT(mtl_set_log_level(handle, MTL_LOG_LEVEL_MAX), 0);
  EXPECT_EQ(mtl_get_log_level(handle), MTL_LOG_LEVEL_ERR);
  EXPECT_GE(mtl_set_log_level(handle, guard.orig()), 0);
  EXPECT_EQ(mtl_get_log_level(handle), guard.orig());
}

TEST(Misc, log_callbacks_and_stream) {
  mtl_handle handle = st_test_ctx()->handle;
  ScopedLogHooks guard(handle);

  /* Phase 1: custom prefix formatter + custom printer. */
  ASSERT_EQ(mtl_set_log_level(handle, MTL_LOG_LEVEL_INFO), 0);
  ASSERT_EQ(mtl_set_log_prefix_formatter(test_log_prefix_formatter), 0);
  ASSERT_EQ(mtl_set_log_printer(test_log_printer), 0);
  reset_log_probe();
  EXPECT_EQ(mtl_sch_set_sleep_us(handle, 0), 0);

  EXPECT_GT(g_log_prefix_calls.load(std::memory_order_acquire), 0);
  EXPECT_GT(g_log_printer_calls.load(std::memory_order_acquire), 0);
  EXPECT_TRUE(g_log_printer_saw_prefix.load(std::memory_order_acquire));
  EXPECT_TRUE(g_log_printer_saw_sleep_info.load(std::memory_order_acquire));

  /* Phase 1b: resetting prefix formatter to null stops emitting TEST_PFX:. */
  ASSERT_EQ(mtl_set_log_prefix_formatter(nullptr), 0);
  reset_log_probe();
  EXPECT_EQ(mtl_sch_set_sleep_us(handle, 0), 0);
  EXPECT_EQ(g_log_prefix_calls.load(std::memory_order_acquire), 0);
  EXPECT_GT(g_log_printer_calls.load(std::memory_order_acquire), 0);
  EXPECT_FALSE(g_log_printer_saw_prefix.load(std::memory_order_acquire));
  EXPECT_TRUE(g_log_printer_saw_sleep_info.load(std::memory_order_acquire));

  /* Phase 2: default RTE_LOG path redirected via mtl_openlog_stream. */
  ASSERT_EQ(mtl_set_log_prefix_formatter(test_log_prefix_formatter), 0);
  ASSERT_EQ(mtl_set_log_printer(nullptr), 0);
  FILE* log_fp = tmpfile();
  ASSERT_NE(log_fp, nullptr);
  guard.set_stream(log_fp);
  ASSERT_EQ(mtl_openlog_stream(log_fp), 0);
  EXPECT_EQ(mtl_sch_set_sleep_us(handle, 0), 0);

  EXPECT_EQ(fflush(log_fp), 0);
  const long written = ftell(log_fp);
  ASSERT_GT(written, 0);
  rewind(log_fp);
  std::string buf(static_cast<size_t>(written), '\0');
  const size_t nread = fread(buf.data(), 1, buf.size(), log_fp);
  EXPECT_EQ(nread, buf.size());
  EXPECT_NE(buf.find("TEST_PFX:"), std::string::npos);
  EXPECT_NE(buf.find("mtl_sch_set_sleep_us"), std::string::npos);
}

TEST(Misc, thread_setname) {
  std::mutex mtx;
  std::condition_variable cv;
  bool started = false;
  bool stop = false;

  std::thread worker([&]() {
    std::unique_lock<std::mutex> lck(mtx);
    started = true;
    cv.notify_one();
    cv.wait(lck, [&]() { return stop; });
  });
  const auto tid = worker.native_handle();
  mtl_test::ScopedWorker worker_guard(nullptr, std::move(worker), [&]() {
    std::lock_guard<std::mutex> lck(mtx);
    stop = true;
    cv.notify_all();
  });

  {
    std::unique_lock<std::mutex> lck(mtx);
    cv.wait(lck, [&]() { return started; });
  }

  constexpr const char* kThreadName = "mtl_test_thr";
  EXPECT_EQ(mtl_thread_setname(tid, kThreadName), 0);
#ifndef WINDOWSENV
  char name[16] = {};
  ASSERT_EQ(pthread_getname_np(tid, name, sizeof(name)), 0);
  EXPECT_STREQ(name, kThreadName);
#endif
}

TEST(Misc, manager_probe) {
#ifndef WINDOWSENV
  constexpr const char* kManagerSockPath = "/var/run/imtl/mtl_manager.sock";
  const int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_GE(sock, 0);
  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", kManagerSockPath);
  const bool expected_alive =
      (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0);
  close(sock);
  EXPECT_EQ(mtl_is_manager_alive(), expected_alive);
#else
  EXPECT_FALSE(mtl_is_manager_alive());
#endif
}
