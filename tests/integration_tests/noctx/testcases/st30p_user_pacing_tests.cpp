/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/* ST30p default pacing (frame 0 on the packet grid, exact RTP step) and strict user
 * pacing (exact RTP, NIC RX time within +-40 us of the request).
 * See README.md, "Test catalogue".
 */

#include <cstdlib>
#include <fstream>
#include <string>

#include "core/constants.hpp"
#include "core/test_fixture.hpp"
#include "handlers/st30p_handler.hpp"
#include "strategies/st30p_strategies.hpp"

namespace {
/* Empty if this process runs in an isolated cgroup v2 cpuset partition, else why not. */
std::string exclusivePartitionError() {
  std::string path;
  std::ifstream cgroup("/proc/self/cgroup");
  for (std::string line; std::getline(cgroup, line);)
    if (line.rfind("0::", 0) == 0) path = line.substr(3);

  std::string partition;
  std::ifstream state("/sys/fs/cgroup" + path + "/cpuset.cpus.partition");
  if (!std::getline(state, partition)) partition = "<absent>";
  if (partition == "isolated") return "";

  const char* isolation = getenv("MTL_CPU_ISOLATION");
  return "st30p_user_pacing (software TSC paced audio, +-40 us) needs an exclusive CPU "
         "partition: cgroup " +
         path + ", partition '" + partition +
         "', MTL_CPU_ISOLATION=" + (isolation ? isolation : "unset") +
         "; run via tests/integration_tests/noctx/run.sh "
         "(tests/tools/isolate/isolate.sh) on a host that allows it; see "
         "tests/tools/isolate/README.md";
}
}  // namespace

TEST_F(NoCtxTest, st30p_default_timestamps) {
  ctx->para.ptp_get_time_fn = NoCtxTest::FakePtpClockNow;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);

  auto bundle = createSt30pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St30pHandler* handler) { return new St30pDefaultTimestamp(handler); });
  auto* handler = bundle.handler;
  auto* strategy = static_cast<St30pDefaultTimestamp*>(bundle.strategy);
  ASSERT_NE(handler, nullptr);
  ASSERT_NE(strategy, nullptr);

  handler->startSession();
  sleepUntilFailure();
  handler->stopSession();
}

TEST_F(NoCtxTest, st30p_user_pacing) {
  initStrictPacingContext();
  if (IsSkipped() || HasFatalFailure()) return;

  const std::string isolation_error = exclusivePartitionError();
  if (!isolation_error.empty()) FAIL() << isolation_error;

  auto bundle = createSt30pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St30pHandler* handler) { return new St30pUserTimestamp(handler); },
      [](St30pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST30P_TX_FLAG_USER_PACING;
      });
  auto* handler = bundle.handler;
  auto* strategy = static_cast<St30pUserTimestamp*>(bundle.strategy);
  ASSERT_NE(handler, nullptr);
  ASSERT_NE(strategy, nullptr);

  strategy->initializeTiming(handler);
  sleep(1);

  /* The plan counts from PTP zero; restarting before mtl_start() steps no tasklet. */
  StartFakePtpClock();
  mtl_start(ctx->handle);
  handler->startSession();

  sleepUntilFailure();

  handler->stopSession();

  ASSERT_GT(strategy->idx_tx, 0u) << "st30p_user_pacing did not transmit any frames";
  ASSERT_GT(strategy->idx_rx, 0u) << "st30p_user_pacing did not receive any frames";
  ASSERT_EQ(strategy->idx_tx, strategy->idx_rx) << "TX/RX frame count mismatch";
}
