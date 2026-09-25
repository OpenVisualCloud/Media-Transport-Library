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

static_assert(kSt30pRxToleranceNs == 40 * NS_PER_US, "comments quote +-40 us");
static_assert(kSt30pFirstBufferRxToleranceNs == 80 * NS_PER_US, "comments quote 80 us");
static_assert(kSt30pUserPacingStartBuffers == 60, "comments quote 600 ms");

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

/* st30p_default_timestamps
 * Config:    mtl_init() with FakePtpClockNow and INFO log, DEV_AUTO_START_STOP as on
 *            the command line (run.sh: on; the test never calls mtl_start()); one
 *            session TX TEST_PORT_1 -> RX TEST_PORT_2, PCM16 48 kHz stereo, 1 ms
 *            packets, 10 ms buffers (B), 3 buffers (fillSt30pOps()); default pacing.
 * Plan:      none.
 * Expect:    on every buffer, St30pDefaultPacingOracle
 *            1. buffer 0 RTP, as TAI, on the 1 ms grid, not after its software RX
 *               time and less than B before it     expectFirstFrameOnPacketGrid()
 *            2. RTP step 480                       rxTestFrameModifier()
 *            Every TX and RX buffer also passes the St30pHandler thread checks.
 * Skip/Fail: none.
 */
TEST_F(NoCtxTest, st30p_default_timestamps) {
  ctx->para.ptp_get_time_fn = NoCtxTest::FakePtpClockNow;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);

  auto bundle = createSt30pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St30pHandler* handler) { return new St30pDefaultPacingOracle(handler); });
  auto* handler = bundle.handler;
  auto* strategy = static_cast<St30pDefaultPacingOracle*>(bundle.strategy);
  ASSERT_NE(handler, nullptr);
  ASSERT_NE(strategy, nullptr);

  handler->startSession();
  sleepUntilFailure();
  handler->stopSession();
}

/* st30p_user_pacing
 * Config:    initStrictPacingContext(); one session TX TEST_PORT_1 -> RX TEST_PORT_2,
 *            PCM16 48 kHz stereo, 1 ms packets, 10 ms buffers (B), 3 buffers
 *            (fillSt30pOps()); TX kTxFlags = USER_PACING.
 * Plan:      t_user(n) = (60 + n) * B from PTP zero (kSt30pUserPacingStartBuffers):
 *            initializeTiming(), sleep 1 s, StartFakePtpClock(), mtl_start(), then
 *            the session threads.
 * Expect:    on every buffer, St30pUserPacingOracle
 *            1. |NIC RX time - t_user(n)| <= 40 us, 80 us for buffer 0
 *               verifyReceiveTiming(), kSt30pRxToleranceNs,
 *               kSt30pFirstBufferRxToleranceNs
 *            2. RTP == tick48k(t_user(n))          verifyMediaClock()
 *            3. RTP step 480                       verifyTimestampStep()
 *            after stop, in the test:
 *            4. idx_tx > 0, idx_rx > 0, idx_tx == idx_rx
 *            Every TX and RX buffer also passes the St30pHandler thread checks.
 * Skip/Fail: strict topology as the ST20p strict tests (SKIP, FAIL with
 *            NOCTX_REQUIRE_STRICT=1); then FAIL, never SKIP, without an exclusive
 *            CPU partition (exclusivePartitionError()).
 */
TEST_F(NoCtxTest, st30p_user_pacing) {
  initStrictPacingContext();
  if (IsSkipped() || HasFatalFailure()) return;

  const std::string isolation_error = exclusivePartitionError();
  if (!isolation_error.empty()) FAIL() << isolation_error;

  constexpr uint32_t kTxFlags = ST30P_TX_FLAG_USER_PACING;
  auto bundle = createSt30pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St30pHandler* handler) { return new St30pUserPacingOracle(handler); },
      [](St30pHandler* handler) { handler->sessionsOpsTx.flags |= kTxFlags; });
  auto* handler = bundle.handler;
  auto* strategy = static_cast<St30pUserPacingOracle*>(bundle.strategy);
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
