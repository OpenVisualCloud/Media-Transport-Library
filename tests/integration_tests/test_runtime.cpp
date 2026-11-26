/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "test_runtime.hpp"

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
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

/* Iterative wildcard matcher supporting GTest's '*' and '?' syntax. */
bool glob_matches(const std::string& pat, const std::string& str) {
  size_t pi = 0;
  size_t si = 0;
  size_t star_pi = std::string::npos;
  size_t star_si = 0;

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

/* Splits on ':' preserving empty tokens so ":" matches no test, per GoogleTest. */
std::vector<std::string> split_colon(const std::string& s) {
  std::vector<std::string> parts;
  if (s.empty()) return parts;
  size_t start = 0;
  while (true) {
    size_t pos = s.find(':', start);
    if (pos == std::string::npos) {
      parts.push_back(s.substr(start));
      break;
    }
    parts.push_back(s.substr(start, pos - start));
    start = pos + 1;
  }
  return parts;
}

bool is_disabled_name(const std::string& s) {
  return s.rfind("DISABLED_", 0) == 0 || s.find("/DISABLED_") != std::string::npos;
}

enum class ShardStatus { kNone, kActive, kInvalid };

ShardStatus parse_shard_env(int& total_shards, int& shard_index) {
  total_shards = 1;
  shard_index = 0;
  const char* total_s = std::getenv("GTEST_TOTAL_SHARDS");
  const char* index_s = std::getenv("GTEST_SHARD_INDEX");
  const bool has_total = (total_s != nullptr && *total_s != '\0');
  const bool has_index = (index_s != nullptr && *index_s != '\0');
  if (!has_total && !has_index) return ShardStatus::kNone;
  if (has_total != has_index) {
    err("Both GTEST_TOTAL_SHARDS and GTEST_SHARD_INDEX must be set together\n");
    return ShardStatus::kInvalid;
  }

  char* end_t = nullptr;
  char* end_i = nullptr;
  errno = 0;
  long t = std::strtol(total_s, &end_t, 10);
  if (errno != 0 || end_t == total_s || *end_t != '\0' || t <= 0 || t > INT_MAX) {
    err("Invalid GTEST_TOTAL_SHARDS='%s'\n", total_s);
    return ShardStatus::kInvalid;
  }
  errno = 0;
  long idx = std::strtol(index_s, &end_i, 10);
  if (errno != 0 || end_i == index_s || *end_i != '\0' || idx < 0 || idx >= t) {
    err("Invalid GTEST_SHARD_INDEX='%s' for GTEST_TOTAL_SHARDS=%ld\n", index_s, t);
    return ShardStatus::kInvalid;
  }
  total_shards = static_cast<int>(t);
  shard_index = static_cast<int>(idx);
  return ShardStatus::kActive;
}

bool validate_ports(const struct st_tests_context* ctx) {
  if (ctx->para.num_ports > 0 && ctx->para.port[MTL_PORT_P][0] == '\0') {
    err("Primary port is empty (--r_port requires --p_port, or use "
        "--port_list/--ports)\n");
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
  if (ctx->hdr_split) {
    ctx->para.nb_rx_hdr_split_queues = 1;
  }
  if (ctx->para.num_ports > 1 &&
      strcmp(ctx->para.port[MTL_PORT_P], ctx->para.port[MTL_PORT_R]) == 0) {
    /* for test with --p_port kernel:lo --r_port kernel:lo */
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
    mtl_port_ip_info(ctx->handle, (enum mtl_port)i, ctx->para.sip_addr[i],
                     ctx->para.netmask[i], ctx->para.gateway[i]);
    uint8_t* ip = ctx->para.sip_addr[i];
    info("%s, if ip %u.%u.%u.%u for port %s\n", __func__, ip[0], ip[1], ip[2], ip[3],
         ctx->para.port[i]);
  }

  ctx->iova = mtl_iova_mode_get(ctx->handle);
  ctx->rss_mode = mtl_rss_mode_get(ctx->handle);

  st_test_st22_plugin_register(ctx);
  st_test_convert_plugin_register(ctx);

  const uint64_t start_time_ns = st_test_get_monotonic_time();
  const int ret = RUN_ALL_TESTS();
  const uint64_t end_time_ns = st_test_get_monotonic_time();
  const int time_s = static_cast<int>((end_time_ns - start_time_ns) / NS_PER_S);
  constexpr int kLinkFlapMinS = 10;

  if (link_flap_wa && time_s < kLinkFlapMinS) {
    /* wa for linkFlapErrDisabled in the hub */
    const int wait_s = kLinkFlapMinS - time_s;
    info("%s, sleep %ds before disable the port\n", __func__, wait_s);
    sleep(wait_s);
  }

  st_test_st22_plugin_unregister(ctx);
  st_test_convert_plugin_unregister(ctx);
  if (ctx->handle) {
    mtl_uninit(ctx->handle);
    ctx->handle = nullptr;
  }
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
  if (child > 0) {
    kill(child, sig);
  }
}

std::string build_shared_filter(const RunPlan& plan) {
  if (!plan.sharded) {
    const std::string& orig = testing::GTEST_FLAG(filter);
    if (orig == "*") return "*-NoCtxTest.*";
    if (orig.find('-') == std::string::npos) return orig + "-NoCtxTest.*";
    return orig + ":NoCtxTest.*";
  }
  std::string joined;
  for (size_t i = 0; i < plan.shared_cases.size(); ++i) {
    if (i > 0) joined.push_back(':');
    joined += plan.shared_cases[i];
  }
  return joined;
}

enum class OutputKind { kNone, kXml, kUnsupported };

OutputKind resolve_output_spec(std::string& xml_path) {
  xml_path.clear();
  const std::string out = testing::GTEST_FLAG(output);
  if (out.empty()) return OutputKind::kNone;
  if (out == "xml") {
    xml_path = "test_detail.xml";
    return OutputKind::kXml;
  }
  if (out.rfind("xml:", 0) == 0) {
    xml_path = out.substr(4);
    if (xml_path.empty()) {
      xml_path = "test_detail.xml";
    } else if (xml_path.back() == '/') {
      xml_path += "KahawaiTest.xml";
    }
    return OutputKind::kXml;
  }
  err("Multi-phase mode only supports XML --gtest_output merging (got '%s'); "
      "filter to a single phase or use --gtest_output=xml:<path>.\n",
      out.c_str());
  return OutputKind::kUnsupported;
}

std::string xml_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

int extract_xml_int_attr(const std::string& tag, const char* attr) {
  const std::string key = std::string(" ") + attr + "=\"";
  size_t pos = tag.find(key);
  if (pos == std::string::npos) return 0;
  pos += key.size();
  size_t end = tag.find('"', pos);
  if (end == std::string::npos) return 0;
  return std::atoi(tag.substr(pos, end - pos).c_str());
}

double extract_xml_double_attr(const std::string& tag, const char* attr) {
  const std::string key = std::string(" ") + attr + "=\"";
  size_t pos = tag.find(key);
  if (pos == std::string::npos) return 0.0;
  pos += key.size();
  size_t end = tag.find('"', pos);
  if (end == std::string::npos) return 0.0;
  return std::strtod(tag.substr(pos, end - pos).c_str(), nullptr);
}

/* Finds the closing '>' of an XML start tag, ignoring '>' inside double quotes. */
size_t find_tag_end(const std::string& content, size_t start) {
  bool in_quotes = false;
  for (size_t i = start; i < content.size(); ++i) {
    if (content[i] == '"') {
      in_quotes = !in_quotes;
    } else if (content[i] == '>' && !in_quotes) {
      return i;
    }
  }
  return std::string::npos;
}

/* Finds "</testsuite>" outside CDATA sections. */
size_t find_testsuite_close(const std::string& content, size_t from) {
  constexpr const char* kClose = "</testsuite>";
  constexpr const char* kCdataOpen = "<![CDATA[";
  constexpr const char* kCdataClose = "]]>";
  size_t pos = from;
  while (pos < content.size()) {
    size_t close_pos = content.find(kClose, pos);
    if (close_pos == std::string::npos) return std::string::npos;
    size_t cdata_pos = content.find(kCdataOpen, pos);
    if (cdata_pos != std::string::npos && cdata_pos < close_pos) {
      size_t cdata_end = content.find(kCdataClose, cdata_pos + strlen(kCdataOpen));
      if (cdata_end == std::string::npos) return std::string::npos;
      pos = cdata_end + strlen(kCdataClose);
      continue;
    }
    return close_pos + strlen(kClose);
  }
  return std::string::npos;
}

struct PhaseResult {
  std::string label;
  std::string xml_path;
  int exit_code = 0;
};

void cleanup_temp_xmls(const std::vector<PhaseResult>& phases) {
  for (const auto& ph : phases) {
    if (!ph.xml_path.empty()) std::remove(ph.xml_path.c_str());
  }
}

std::string make_synthetic_failure_suite(const std::string& label, int exit_code) {
  std::string cls = label;
  std::string nm = "process";
  size_t dot = label.find('.');
  if (dot != std::string::npos) {
    cls = label.substr(0, dot);
    nm = label.substr(dot + 1);
  }
  std::ostringstream synth;
  synth << "  <testsuite name=\"" << xml_escape(cls)
        << "\" tests=\"1\" failures=\"1\" disabled=\"0\" errors=\"0\" time=\"0.000\">\n"
        << "    <testcase name=\"" << xml_escape(nm) << "\" classname=\""
        << xml_escape(cls) << "\" status=\"run\" result=\"completed\" time=\"0.000\">\n"
        << "      <failure message=\"Phase exited with code " << exit_code
        << " without failing test XML\" type=\"\"/>\n"
        << "    </testcase>\n"
        << "  </testsuite>";
  return synth.str();
}

bool merge_phase_xmls(const std::string& target_path,
                      const std::vector<PhaseResult>& phases) {
  int total_tests = 0;
  int total_failures = 0;
  int total_disabled = 0;
  int total_errors = 0;
  double total_time = 0.0;
  std::vector<std::string> suite_blocks;

  for (const auto& ph : phases) {
    int phase_suites = 0;
    int phase_failures = 0;

    std::ifstream in(ph.xml_path);
    if (in.is_open()) {
      std::ostringstream buf;
      buf << in.rdbuf();
      const std::string content = buf.str();
      in.close();

      size_t root_pos = content.find("<testsuites");
      if (root_pos != std::string::npos) {
        size_t root_end = find_tag_end(content, root_pos);
        if (root_end != std::string::npos) {
          const std::string root_tag = content.substr(root_pos, root_end - root_pos + 1);
          total_tests += extract_xml_int_attr(root_tag, "tests");
          const int f = extract_xml_int_attr(root_tag, "failures");
          const int e = extract_xml_int_attr(root_tag, "errors");
          total_failures += f;
          total_disabled += extract_xml_int_attr(root_tag, "disabled");
          total_errors += e;
          total_time += extract_xml_double_attr(root_tag, "time");
          phase_failures += f + e;
        }
      }

      size_t pos = 0;
      while ((pos = content.find("<testsuite ", pos)) != std::string::npos) {
        size_t tag_end = find_tag_end(content, pos);
        if (tag_end == std::string::npos) break;
        if (tag_end > pos && content[tag_end - 1] == '/') {
          suite_blocks.push_back("  " + content.substr(pos, tag_end - pos + 1));
          ++phase_suites;
          pos = tag_end + 1;
          continue;
        }
        size_t close_end = find_testsuite_close(content, tag_end + 1);
        if (close_end == std::string::npos) break;
        suite_blocks.push_back("  " + content.substr(pos, close_end - pos));
        ++phase_suites;
        pos = close_end;
      }
    }

    if (ph.exit_code != 0 && (phase_suites == 0 || phase_failures == 0)) {
      suite_blocks.push_back(make_synthetic_failure_suite(ph.label, ph.exit_code));
      total_tests += 1;
      total_failures += 1;
    }
  }

  std::ofstream out(target_path, std::ios::trunc);
  if (!out.is_open()) {
    err("Failed to open merged JUnit XML target '%s': %s\n", target_path.c_str(),
        std::strerror(errno));
    cleanup_temp_xmls(phases);
    return false;
  }
  out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  out << std::fixed << std::setprecision(3);
  out << "<testsuites tests=\"" << total_tests << "\" failures=\"" << total_failures
      << "\" disabled=\"" << total_disabled << "\" errors=\"" << total_errors
      << "\" time=\"" << total_time << "\" name=\"AllTests\">\n";
  for (const auto& blk : suite_blocks) {
    out << blk << "\n";
  }
  out << "</testsuites>\n";
  out.flush();
  const bool ok = out.good();
  out.close();
  cleanup_temp_xmls(phases);
  if (!ok) {
    err("Failed writing merged JUnit XML target '%s'\n", target_path.c_str());
  }
  return ok;
}

int spawn_child_phase(const std::vector<std::string>& orig_argv,
                      const std::string& phase_filter, const std::string& xml_path,
                      const std::string& label) {
  std::vector<std::string> args;
  args.reserve(orig_argv.size() + 4);
  args.push_back(orig_argv.empty() ? std::string("KahawaiTest") : orig_argv[0]);

  for (size_t i = 1; i < orig_argv.size(); ++i) {
    const std::string& a = orig_argv[i];
    if (a.rfind("--gtest_filter=", 0) == 0 || a.rfind("--gtest_output=", 0) == 0 ||
        a == "--gtest_output" || a == "--single_process" || a == "--no_ctx_tests" ||
        a == "--no_ctx") {
      continue;
    }
    args.push_back(a);
  }
  args.push_back("--single_process");
  args.push_back("--gtest_filter=" + phase_filter);
  if (!xml_path.empty()) {
    args.push_back("--gtest_output=xml:" + xml_path);
  }

  std::vector<char*> c_argv;
  c_argv.reserve(args.size() + 1);
  for (auto& s : args) c_argv.push_back(const_cast<char*>(s.c_str()));
  c_argv.push_back(nullptr);

  std::fflush(stdout);
  std::fflush(stderr);
  pid_t pid = fork();
  if (pid < 0) {
    err("fork() failed for phase '%s': %s\n", label.c_str(), std::strerror(errno));
    return 1;
  }
  if (pid == 0) {
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    unsetenv("GTEST_TOTAL_SHARDS");
    unsetenv("GTEST_SHARD_INDEX");
    unsetenv("GTEST_SHARD_STATUS_FILE");
    unsetenv("GTEST_OUTPUT");
    execv("/proc/self/exe", c_argv.data());
    if (!orig_argv.empty()) execvp(orig_argv[0].c_str(), c_argv.data());
    _exit(127);
  }

  g_active_child_pid = pid;
  if (g_caught_signal != 0) {
    kill(pid, static_cast<int>(g_caught_signal));
  }

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

  if (g_caught_signal != 0) {
    return 128 + static_cast<int>(g_caught_signal);
  }
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) {
    err("Phase '%s' terminated by signal %d\n", label.c_str(), WTERMSIG(status));
    return 128 + WTERMSIG(status);
  }
  return 1;
}

void touch_shard_status_file_if_requested() {
  const char* status_file = std::getenv("GTEST_SHARD_STATUS_FILE");
  if (!status_file || !*status_file) return;
  std::ofstream out(status_file, std::ios::app);
}

int run_multi_phase(const RunPlan& plan, const std::vector<std::string>& orig_argv) {
  std::string xml_target;
  const OutputKind out_kind = resolve_output_spec(xml_target);
  if (out_kind == OutputKind::kUnsupported) return 1;

  touch_shard_status_file_if_requested();

  g_active_child_pid = 0;
  g_caught_signal = 0;
  struct sigaction sa{};
  sa.sa_handler = multi_phase_signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  struct sigaction prev_int{};
  struct sigaction prev_term{};
  sigaction(SIGINT, &sa, &prev_int);
  sigaction(SIGTERM, &sa, &prev_term);

  const bool fail_fast = testing::GTEST_FLAG(fail_fast);
  const pid_t self_pid = getpid();
  std::vector<PhaseResult> results;
  int overall_rc = 0;
  size_t phase_idx = 0;

  auto run_one = [&](const std::string& label, const std::string& flt) {
    std::string phase_xml;
    if (out_kind == OutputKind::kXml) {
      phase_xml = xml_target + ".p" + std::to_string(self_pid) + "_" +
                  std::to_string(phase_idx) + ".tmp.xml";
    }
    ++phase_idx;
    info("=== KahawaiTest phase %zu: %s ===\n", phase_idx, label.c_str());
    const int rc = spawn_child_phase(orig_argv, flt, phase_xml, label);
    results.push_back({label, phase_xml, rc});
    if (rc != 0) overall_rc = rc;
    return rc == 0;
  };

  if (!plan.shared_cases.empty() && g_caught_signal == 0) {
    const bool ok = run_one("shared", build_shared_filter(plan));
    if (!ok && fail_fast) {
      sigaction(SIGINT, &prev_int, nullptr);
      sigaction(SIGTERM, &prev_term, nullptr);
      if (g_caught_signal != 0) {
        cleanup_temp_xmls(results);
        return 128 + static_cast<int>(g_caught_signal);
      }
      if (out_kind == OutputKind::kXml && !merge_phase_xmls(xml_target, results)) {
        return 1;
      }
      return overall_rc;
    }
  }

  for (const auto& own_case : plan.own_cases) {
    if (g_caught_signal != 0) break;
    const bool ok = run_one(own_case, own_case);
    if (!ok && fail_fast) break;
  }

  sigaction(SIGINT, &prev_int, nullptr);
  sigaction(SIGTERM, &prev_term, nullptr);

  if (g_caught_signal != 0) {
    cleanup_temp_xmls(results);
    return 128 + static_cast<int>(g_caught_signal);
  }
  if (out_kind == OutputKind::kXml && !merge_phase_xmls(xml_target, results)) {
    return 1;
  }
  return overall_rc;
}

#endif /* !WINDOWSENV */

}  // namespace

bool is_own_ctx_suite(const std::string& suite_name) {
  return suite_name == "NoCtxTest";
}

bool gtest_filter_matches(const std::string& filter, const std::string& full_name) {
  if (filter.empty()) return false;
  if (filter == "*") return true;

  const size_t dash = filter.find('-');
  const std::string pos_str =
      (dash == std::string::npos) ? filter : filter.substr(0, dash);
  const std::string neg_str =
      (dash == std::string::npos) ? std::string() : filter.substr(dash + 1);

  if (dash != 0) {
    const auto pos_pats = split_colon(pos_str);
    bool matched_pos = false;
    for (const auto& pat : pos_pats) {
      if (glob_matches(pat, full_name)) {
        matched_pos = true;
        break;
      }
    }
    if (!matched_pos) return false;
  }

  const auto neg_pats = split_colon(neg_str);
  for (const auto& pat : neg_pats) {
    if (!pat.empty() && glob_matches(pat, full_name)) return false;
  }
  return true;
}

RunPlan build_run_plan() {
  RunPlan plan;
  int total_shards = 1;
  int shard_index = 0;
  const ShardStatus shard_st = parse_shard_env(total_shards, shard_index);
  if (shard_st == ShardStatus::kInvalid) {
    plan.invalid = true;
    return plan;
  }
  plan.sharded = (shard_st == ShardStatus::kActive && total_shards > 1);

  const std::string& filter = testing::GTEST_FLAG(filter);
  const bool run_disabled = testing::GTEST_FLAG(also_run_disabled_tests);
  const auto* ut = testing::UnitTest::GetInstance();
  int shard_counter = 0;

  for (int i = 0; i < ut->total_test_suite_count(); ++i) {
    const auto* ts = ut->GetTestSuite(i);
    const std::string suite = ts->name();
    const bool suite_disabled = is_disabled_name(suite);
    for (int j = 0; j < ts->total_test_count(); ++j) {
      const auto* ti = ts->GetTestInfo(j);
      const std::string name = ti->name();
      const std::string full = suite + "." + name;
      if (!gtest_filter_matches(filter, full)) continue;
      if (!run_disabled && (suite_disabled || is_disabled_name(name))) continue;
      const int idx = shard_counter++;
      if (plan.sharded && (idx % total_shards) != shard_index) continue;
      if (is_own_ctx_suite(suite)) {
        plan.own_cases.push_back(full);
      } else {
        plan.shared_cases.push_back(full);
      }
    }
  }
  return plan;
}

void print_cases_json() {
  const auto* ut = testing::UnitTest::GetInstance();
  printf("[\n");
  bool first = true;
  for (int i = 0; i < ut->total_test_suite_count(); ++i) {
    const auto* ts = ut->GetTestSuite(i);
    const std::string suite = ts->name();
    const bool own = is_own_ctx_suite(suite);
    for (int j = 0; j < ts->total_test_count(); ++j) {
      const auto* ti = ts->GetTestInfo(j);
      const std::string name = ti->name();
      const std::string full = suite + "." + name;
      const bool is_pf = own && name.find("_pf_") != std::string::npos;
      const bool is_red = name.find("redundant") != std::string::npos;
      const bool needs_iso =
          own && (name.find("default_timestamps") != std::string::npos ||
                  name.find("user_pacing") != std::string::npos ||
                  name.find("vrx_margin") != std::string::npos ||
                  name.find("rl_staged_overload") != std::string::npos);
      const char* topo =
          is_pf ? "pf_pair" : (own ? "cross_pf_quad" : (is_red ? "vf_quad" : "vf_pair"));
      if (!first) printf(",\n");
      first = false;
      printf(
          "  {\"case\":\"%s\",\"suite\":\"%s\",\"instance\":\"%s\","
          "\"topology\":\"%s\",\"isolated\":%s}",
          full.c_str(), suite.c_str(), own ? "own" : "shared", topo,
          needs_iso ? "true" : "false");
    }
  }
  printf("\n]\n");
}

int run_unified_tests(struct st_tests_context* ctx,
                      const std::vector<std::string>& orig_argv) {
  const RunPlan plan = build_run_plan();
  if (plan.invalid) return 1;

  if (plan.shared_cases.empty() && plan.own_cases.empty()) {
    return RUN_ALL_TESTS();
  }

  /* Single-process fast path: either all shared tests or a single own-ctx test. */
  if (plan.own_cases.empty()) {
    return run_shared_phase(ctx);
  }
  if (plan.shared_cases.empty() && plan.own_cases.size() == 1) {
    return run_own_phase(ctx);
  }

  /* Multiple own-ctx tests or mixed shared + own-ctx tests require per-phase
   * processes because DPDK EAL cannot initialize twice in one process. */
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
