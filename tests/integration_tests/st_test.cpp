/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef WINDOWSENV
#include <pthread.h>
#include <sched.h>
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
  mtl_test::ScopedHpBuf buf(ctx->handle, size, port, zero);
  EXPECT_EQ(static_cast<bool>(buf), expect_succ);
  if (buf) {
    if (mtl_pmd_is_dpdk_based(ctx->handle, port)) {
      EXPECT_NE(buf.iova(), MTL_BAD_IOVA);
    }
    if (zero) {
      std::vector<uint8_t> zeros(size, 0);
      EXPECT_EQ(0, memcmp(buf.data(), zeros.data(), size));
    }
    /* Dirty the allocation before freeing so a subsequent zmalloc verifies zeroing. */
    memset(buf.data(), 0x5A, size);
  }
}

void hp_malloc_tests(struct st_tests_context* ctx, enum mtl_port port, bool zero) {
  for (size_t sz : {size_t{1}, size_t{1024}, size_t{1024 + 3}}) {
    hp_malloc_test(ctx, sz, port, zero, true);
  }
}

std::atomic<int> g_log_prefix_calls{0};
std::atomic<int> g_log_printer_calls{0};
std::atomic<bool> g_log_printer_saw_prefix{false};

void test_log_prefix_formatter(char* buf, size_t buf_sz) {
  g_log_prefix_calls.fetch_add(1, std::memory_order_relaxed);
  snprintf(buf, buf_sz, "TEST_PFX: ");
}

void test_log_printer(enum mtl_log_level level, const char* format, ...) {
  MTL_MAY_UNUSED(level);
  g_log_printer_calls.fetch_add(1, std::memory_order_relaxed);
  char msg[512];
  va_list args;
  va_start(args, format);
  vsnprintf(msg, sizeof(msg), format, args);
  va_end(args);
  if (strstr(msg, "TEST_PFX:") != nullptr) {
    g_log_printer_saw_prefix.store(true, std::memory_order_release);
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
  struct mtl_fix_info fix = {};
  struct mtl_var_info var = {};
  ASSERT_EQ(mtl_get_fix_info(handle, &fix), 0);
  ASSERT_EQ(mtl_get_var_info(handle, &var), 0);
  EXPECT_LE(var.dma_dev_cnt, fix.dma_dev_cnt_max);
  EXPECT_EQ(var.dev_started, (ctx->para.flags & MTL_FLAG_DEV_AUTO_START_STOP) != 0);
}

TEST(Main, lcore) {
  test_lcore_one(st_test_ctx());
}

TEST(Main, bind_to_lcore) {
  auto* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  unsigned int lcore = 0;

  ASSERT_GE(mtl_get_lcore(handle, &lcore), 0);
  mtl_test::ScopedLcore guard(handle, lcore);

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

  EXPECT_EQ(mtl_bind_to_lcore(handle, tid, lcore), 0);
#ifndef WINDOWSENV
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  ASSERT_EQ(pthread_getaffinity_np(tid, sizeof(cpuset), &cpuset), 0);
  EXPECT_EQ(CPU_COUNT(&cpuset), 1);
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
  ASSERT_LT(mtl_put_lcore(ctx->handle, 10000), 0);

  unsigned int lcore = 0;
  ASSERT_GE(mtl_get_lcore(ctx->handle, &lcore), 0);
  mtl_test::ScopedLcore guard(ctx->handle, lcore);
  EXPECT_EQ(guard.reset(), 0);
  EXPECT_LT(mtl_put_lcore(ctx->handle, lcore), 0);

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
  struct mtl_port_status stats = {};

  for (int port = 0; port < ctx->para.num_ports; port++) {
    auto p = static_cast<enum mtl_port>(port);
    EXPECT_EQ(mtl_get_port_stats(handle, p, &stats), 0);
    EXPECT_EQ(mtl_reset_port_stats(handle, p), 0);
    EXPECT_EQ(mtl_get_port_stats(handle, p, &stats), 0);
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
  EXPECT_EQ(mtl_iova_mode_get(handle), ctx->iova);
  EXPECT_EQ(mtl_rss_mode_get(handle), ctx->rss_mode);

  for (int port = 0; port < ctx->para.num_ports; port++) {
    auto p = static_cast<enum mtl_port>(port);
    uint8_t ip[MTL_IP_ADDR_LEN] = {};
    uint8_t netmask[MTL_IP_ADDR_LEN] = {};
    uint8_t gateway[MTL_IP_ADDR_LEN] = {};
    EXPECT_EQ(mtl_port_ip_info(handle, p, ip, netmask, gateway), 0);
    if (!ctx->dhcp) {
      EXPECT_EQ(memcmp(ip, ctx->para.sip_addr[port], MTL_IP_ADDR_LEN), 0);
    }
    const bool expected_dpdk = (ctx->para.pmd[port] == MTL_PMD_DPDK_USER) ||
                               (ctx->para.pmd[port] == MTL_PMD_DPDK_AF_XDP) ||
                               (ctx->para.pmd[port] == MTL_PMD_DPDK_AF_PACKET);
    EXPECT_EQ(mtl_pmd_is_dpdk_based(handle, p), expected_dpdk);
  }

  uint8_t ip[MTL_IP_ADDR_LEN] = {};
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

  EXPECT_GE(mtl_get_numa_id(handle, MTL_PORT_P), 0);
  if (ctx->para.num_ports > 1) {
    EXPECT_GE(mtl_get_numa_id(handle, MTL_PORT_R), 0);
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
  hp_malloc_test(ctx, 1024, static_cast<enum mtl_port>(MTL_PORT_MAX), false, false);
  EXPECT_EQ(mtl_hp_virt2iova(ctx->handle, nullptr), MTL_BAD_IOVA);
}

TEST(Misc, hp_zmalloc_expect_fail) {
  auto* ctx = st_test_ctx();
  hp_malloc_test(ctx, 0, MTL_PORT_P, true, false);
  if (ctx->para.num_ports > 1) hp_malloc_test(ctx, 0, MTL_PORT_R, true, false);
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

  /* Uncached raw read exercises the PTP source directly. */
  if (user_ptp) {
    const uint64_t cb_before = __atomic_load_n(&ctx->ptp_time, __ATOMIC_ACQUIRE);
    const uint64_t ptp_raw = mtl_ptp_read_time_raw(handle);
    const uint64_t cb_after = __atomic_load_n(&ctx->ptp_time, __ATOMIC_ACQUIRE);
    EXPECT_GT(ptp_raw, 0u);
    EXPECT_GT(cb_after, 0u);
    EXPECT_GE(ptp_raw, cb_before);
    EXPECT_LE(ptp_raw, cb_after);
  } else {
    const uint64_t ptp_raw = mtl_ptp_read_time_raw(handle);
    EXPECT_GT(ptp_raw, 0u);
  }

  /* Cached reads must stay strictly monotonic over 5 x 2 ms polls. */
  uint64_t prev = mtl_ptp_read_time(handle);
  EXPECT_GT(prev, 0u);
  for (int i = 0; i < 5; i++) {
    st_usleep(2 * 1000);
    const uint64_t cur = mtl_ptp_read_time(handle);
    EXPECT_GT(cur, prev);
    if (user_ptp) {
      const uint64_t real_time = st_test_get_realtime_ns();
      const uint64_t diff = (cur > real_time) ? (cur - real_time) : (real_time - cur);
      EXPECT_LT(diff, static_cast<uint64_t>(5) * NS_PER_MS);
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
    const uint64_t real_time = st_test_get_realtime_ns();
    const uint64_t diff =
        (refreshed > real_time) ? (refreshed - real_time) : (real_time - refreshed);
    EXPECT_LT(diff, static_cast<uint64_t>(5) * NS_PER_MS);
  }
}

TEST(Misc, log_level) {
  mtl_handle handle = st_test_ctx()->handle;
  ScopedLogHooks guard(handle);

  EXPECT_GE(mtl_set_log_level(handle, MTL_LOG_LEVEL_INFO), 0);
  EXPECT_EQ(mtl_get_log_level(handle), MTL_LOG_LEVEL_INFO);
  EXPECT_GE(mtl_set_log_level(handle, MTL_LOG_LEVEL_ERR), 0);
  EXPECT_EQ(mtl_get_log_level(handle), MTL_LOG_LEVEL_ERR);
  EXPECT_LT(mtl_set_log_level(handle, MTL_LOG_LEVEL_MAX), 0);
  EXPECT_EQ(mtl_get_log_level(handle), MTL_LOG_LEVEL_ERR);
  EXPECT_GE(mtl_set_log_level(handle, guard.orig()), 0);
  EXPECT_EQ(mtl_get_log_level(handle), guard.orig());
}

TEST(Misc, log_callbacks_and_stream) {
  mtl_handle handle = st_test_ctx()->handle;
  ScopedLogHooks guard(handle);

  g_log_prefix_calls.store(0, std::memory_order_relaxed);
  g_log_printer_calls.store(0, std::memory_order_relaxed);
  g_log_printer_saw_prefix.store(false, std::memory_order_relaxed);

  /* Phase 1: custom prefix formatter + custom printer. */
  ASSERT_EQ(mtl_set_log_level(handle, MTL_LOG_LEVEL_INFO), 0);
  ASSERT_EQ(mtl_set_log_prefix_formatter(test_log_prefix_formatter), 0);
  ASSERT_EQ(mtl_set_log_printer(test_log_printer), 0);
  EXPECT_EQ(mtl_sch_set_sleep_us(handle, 0), 0);

  EXPECT_GT(g_log_prefix_calls.load(std::memory_order_acquire), 0);
  EXPECT_GT(g_log_printer_calls.load(std::memory_order_acquire), 0);
  EXPECT_TRUE(g_log_printer_saw_prefix.load(std::memory_order_acquire));

  /* Phase 2: default RTE_LOG path redirected via mtl_openlog_stream. */
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
  const bool first = mtl_is_manager_alive();
  const bool second = mtl_is_manager_alive();
  EXPECT_EQ(first, second);
}
