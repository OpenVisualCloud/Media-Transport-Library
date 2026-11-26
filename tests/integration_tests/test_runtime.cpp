/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "test_runtime.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef WINDOWSENV
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "log.h"

namespace mtl_test {

namespace {

bool test_port_is_vf(const char* port) {
#ifdef WINDOWSENV
  (void)port;
  return false;
#else
  if (!port || !port[0]) return false;
  char path[256];
  snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/physfn", port);
  return access(path, F_OK) == 0;
#endif
}

bool is_disabled_name(const std::string& s) {
  return s.rfind("DISABLED_", 0) == 0 || s.find("/DISABLED_") != std::string::npos;
}

/* Iterative wildcard matcher supporting GoogleTest '*' and '?' syntax. */
bool glob_matches(const std::string& pat, const std::string& str) {
  size_t pi = 0, si = 0, star_pi = std::string::npos, star_si = 0;
  while (si < str.size()) {
    if (pi < pat.size() && (pat[pi] == '?' || pat[pi] == str[si])) {
      ++pi;
      ++si;
    } else if (pi < pat.size() && pat[pi] == '*') {
      star_pi = pi++;
      star_si = si;
    } else if (star_pi != std::string::npos) {
      pi = star_pi + 1;
      si = ++star_si;
    } else {
      return false;
    }
  }
  while (pi < pat.size() && pat[pi] == '*') ++pi;
  return pi == pat.size();
}

bool matches_any_colon_pat(const std::string& pats, const std::string& full_name,
                           bool skip_empty) {
  if (pats.empty()) return false;
  size_t start = 0;
  while (true) {
    const size_t pos = pats.find(':', start);
    const std::string pat =
        (pos == std::string::npos) ? pats.substr(start) : pats.substr(start, pos - start);
    if ((!skip_empty || !pat.empty()) && glob_matches(pat, full_name)) return true;
    if (pos == std::string::npos) return false;
    start = pos + 1;
  }
}

bool gtest_filter_matches(const std::string& filter, const std::string& full_name) {
  if (filter.empty()) return false;
  if (filter == "*") return true;
  const size_t dash = filter.find('-');
  const std::string pos_str =
      (dash == std::string::npos) ? filter : filter.substr(0, dash);
  const std::string neg_str =
      (dash == std::string::npos) ? std::string() : filter.substr(dash + 1);
  if (dash != 0 && !matches_any_colon_pat(pos_str, full_name, false)) return false;
  return !matches_any_colon_pat(neg_str, full_name, true);
}

struct RunPlan {
  bool has_shared = false;
  std::vector<std::string> own_cases;
};

RunPlan build_run_plan() {
  RunPlan plan;
  const std::string& filter = testing::GTEST_FLAG(filter);
  const bool run_disabled = testing::GTEST_FLAG(also_run_disabled_tests);
  const auto* ut = testing::UnitTest::GetInstance();

  for (int i = 0; i < ut->total_test_suite_count(); ++i) {
    const auto* ts = ut->GetTestSuite(i);
    const std::string suite = ts->name();
    const bool suite_disabled = is_disabled_name(suite);
    for (int j = 0; j < ts->total_test_count(); ++j) {
      const std::string name = ts->GetTestInfo(j)->name();
      const std::string full = suite + "." + name;
      if (!gtest_filter_matches(filter, full)) continue;
      if (!run_disabled && (suite_disabled || is_disabled_name(name))) continue;
      if (suite == "NoCtxTest") {
        plan.own_cases.push_back(full);
      } else {
        plan.has_shared = true;
      }
    }
  }
  return plan;
}

bool validate_ports(const struct st_tests_context* ctx) {
  if (ctx->para.num_ports > 0 && ctx->para.port[MTL_PORT_P][0] == '\0') {
    err("Primary port is empty (--r_port requires --p_port, or use --port_list)\n");
    return false;
  }
  return true;
}

bool prepare_common_params(struct st_tests_context* ctx) {
  bool link_flap_wa = false;
  for (int i = 0; i < ctx->para.num_ports && i < MTL_PORT_MAX; i++) {
    ctx->para.pmd[i] = mtl_pmd_by_port_name(ctx->para.port[i]);
    if (ctx->para.pmd[i] != MTL_PMD_DPDK_USER) {
      ctx->para.flags |= MTL_FLAG_RX_SEPARATE_VIDEO_LCORE;
    } else if (!test_port_is_vf(ctx->para.port[i])) {
      /* Only physical functions flap the physical switch port on DPDK close. */
      link_flap_wa = true;
    }
  }
  if (ctx->hdr_split) ctx->para.nb_rx_hdr_split_queues = 1;
  if (ctx->para.num_ports > 1 &&
      strcmp(ctx->para.port[MTL_PORT_P], ctx->para.port[MTL_PORT_R]) == 0) {
    ctx->same_dual_port = true;
  }
  return link_flap_wa;
}

int run_shared_phase(struct st_tests_context* ctx) {
  if (!validate_ports(ctx)) return 1;
  const bool link_flap_wa = prepare_common_params(ctx);

  ctx->handle = mtl_init(&ctx->para);
  if (!ctx->handle) {
    err("%s, mtl_init fail\n", __func__);
    return 1;
  }

  for (int i = 0; i < ctx->para.num_ports && i < MTL_PORT_MAX; i++) {
    mtl_port_ip_info(ctx->handle, static_cast<enum mtl_port>(i), ctx->para.sip_addr[i],
                     ctx->para.netmask[i], ctx->para.gateway[i]);
    uint8_t* ip = ctx->para.sip_addr[i];
    info("%s, if ip %u.%u.%u.%u for port %s\n", __func__, ip[0], ip[1], ip[2], ip[3],
         ctx->para.port[i]);
  }

  ctx->iova = mtl_iova_mode_get(ctx->handle);
  ctx->rss_mode = mtl_rss_mode_get(ctx->handle);

  int ret = 1;
  if (st_test_st22_plugin_register(ctx) < 0) {
    err("%s, st22 plugin register fail\n", __func__);
  } else if (st_test_convert_plugin_register(ctx) < 0 || !ctx->converter_dev_handle) {
    err("%s, convert plugin register fail\n", __func__);
  } else {
    const uint64_t start_ns = st_test_get_monotonic_time();
    ret = RUN_ALL_TESTS();
    const int elapsed_s =
        static_cast<int>((st_test_get_monotonic_time() - start_ns) / NS_PER_S);
    constexpr int kLinkFlapMinS = 10;
    if (link_flap_wa && elapsed_s < kLinkFlapMinS) {
      /* Workaround for linkFlapErrDisabled on physical switch ports. */
      const int wait_s = kLinkFlapMinS - elapsed_s;
      info("%s, sleep %ds before disable the port\n", __func__, wait_s);
      sleep(wait_s);
    }
  }

  st_test_convert_plugin_unregister(ctx);
  st_test_st22_plugin_unregister(ctx);
  mtl_uninit(ctx->handle);
  ctx->handle = nullptr;
  return ret;
}

int run_own_phase(struct st_tests_context* ctx) {
  if (!validate_ports(ctx)) return 1;
  prepare_common_params(ctx);
  ctx->handle = nullptr;
  return RUN_ALL_TESTS();
}

#ifndef WINDOWSENV

volatile sig_atomic_t g_active_child_pid = 0;
volatile sig_atomic_t g_caught_signal = 0;

void multi_phase_signal_handler(int sig) {
  g_caught_signal = sig;
  const pid_t child = static_cast<pid_t>(g_active_child_pid);
  if (child > 0) kill(child, sig);
}

std::string build_shared_filter() {
  const std::string& orig = testing::GTEST_FLAG(filter);
  if (orig == "*") return "*-NoCtxTest.*";
  if (orig.find('-') == std::string::npos) return orig + "-NoCtxTest.*";
  return orig + ":NoCtxTest.*";
}

int spawn_child_phase(const std::vector<std::string>& orig_argv,
                      const std::string& phase_filter, const std::string& label) {
  std::vector<std::string> args;
  args.reserve(orig_argv.size() + 3);
  args.push_back(orig_argv.empty() ? std::string("KahawaiTest") : orig_argv[0]);

  for (size_t i = 1; i < orig_argv.size(); ++i) {
    const std::string& a = orig_argv[i];
    if (a == "--gtest_filter") {
      if (i + 1 < orig_argv.size() && orig_argv[i + 1].rfind("-", 0) != 0) ++i;
      continue;
    }
    if (a.rfind("--gtest_filter=", 0) == 0 || a == "--single_process") continue;
    args.push_back(a);
  }
  args.push_back("--single_process");
  args.push_back("--gtest_filter=" + phase_filter);

  std::vector<char*> c_argv;
  c_argv.reserve(args.size() + 1);
  for (auto& s : args) c_argv.push_back(s.data());
  c_argv.push_back(nullptr);

  std::fflush(stdout);
  std::fflush(stderr);
  const pid_t pid = fork();
  if (pid < 0) {
    err("fork() failed for phase '%s': %s\n", label.c_str(), std::strerror(errno));
    return 1;
  }
  if (pid == 0) {
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    unsetenv("GTEST_FILTER");
    execv("/proc/self/exe", c_argv.data());
    if (!orig_argv.empty()) execvp(orig_argv[0].c_str(), c_argv.data());
    _exit(127);
  }

  g_active_child_pid = pid;
  if (g_caught_signal != 0) kill(pid, static_cast<int>(g_caught_signal));

  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR) {
      if (g_caught_signal != 0) kill(pid, static_cast<int>(g_caught_signal));
      continue;
    }
    g_active_child_pid = 0;
    err("waitpid() failed for phase '%s': %s\n", label.c_str(), std::strerror(errno));
    return 1;
  }
  g_active_child_pid = 0;

  if (g_caught_signal != 0) return 128 + static_cast<int>(g_caught_signal);
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) {
    err("Phase '%s' terminated by signal %d\n", label.c_str(), WTERMSIG(status));
    return 128 + WTERMSIG(status);
  }
  return 1;
}

int run_multi_phase(const RunPlan& plan, const std::vector<std::string>& orig_argv) {
  g_active_child_pid = 0;
  g_caught_signal = 0;
  struct sigaction sa = {};
  sa.sa_handler = multi_phase_signal_handler;
  sigemptyset(&sa.sa_mask);
  struct sigaction prev_int = {};
  struct sigaction prev_term = {};
  sigaction(SIGINT, &sa, &prev_int);
  sigaction(SIGTERM, &sa, &prev_term);

  const bool fail_fast = testing::GTEST_FLAG(fail_fast);
  int overall_rc = 0;
  size_t phase_idx = 0;

  auto run_one = [&](const std::string& label, const std::string& flt) {
    info("=== KahawaiTest phase %zu: %s ===\n", ++phase_idx, label.c_str());
    const int rc = spawn_child_phase(orig_argv, flt, label);
    if (rc != 0) overall_rc = rc;
    return rc == 0;
  };

  bool stop = false;
  if (plan.has_shared && g_caught_signal == 0) {
    if (!run_one("shared", build_shared_filter()) && fail_fast) stop = true;
  }
  for (const auto& own_case : plan.own_cases) {
    if (stop || g_caught_signal != 0) break;
    if (!run_one(own_case, own_case) && fail_fast) break;
  }

  sigaction(SIGINT, &prev_int, nullptr);
  sigaction(SIGTERM, &prev_term, nullptr);
  if (g_caught_signal != 0) return 128 + static_cast<int>(g_caught_signal);
  return overall_rc;
}

#endif /* !WINDOWSENV */

}  // namespace

int run_unified_tests(struct st_tests_context* ctx,
                      const std::vector<std::string>& orig_argv) {
  const RunPlan plan = build_run_plan();
  if (!plan.has_shared && plan.own_cases.empty()) return RUN_ALL_TESTS();

  /* Single-process fast path: shared suites only or one NoCtxTest case. */
  if (plan.own_cases.empty()) return run_shared_phase(ctx);
  if (!plan.has_shared && plan.own_cases.size() == 1) return run_own_phase(ctx);

  /* Multiple own-ctx tests or mixed suites require separate processes because
   * DPDK EAL cannot initialize twice in one process. */
  if (ctx->single_process) {
    err("Cannot run mixed or multiple NoCtxTest cases inside --single_process\n");
    return 1;
  }
#ifdef WINDOWSENV
  (void)orig_argv;
  err("Running multiple NoCtxTest cases or mixed suites in one invocation requires POSIX "
      "process spawning; filter to one NoCtxTest case or shared suites only.\n");
  return 1;
#else
  if (!validate_ports(ctx)) return 1;
  return run_multi_phase(plan, orig_argv);
#endif
}

}  // namespace mtl_test
