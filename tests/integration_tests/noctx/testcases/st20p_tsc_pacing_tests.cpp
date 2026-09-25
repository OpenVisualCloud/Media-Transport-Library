/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>

#include "core/test_fixture.hpp"
#include "handlers/st20p_handler.hpp"

/* The ice PF keeps VF queue rates across processes, so an RL child runs first */
TEST_F(NoCtxTest, st20p_tsc_pacing_after_rl_process_full_fps) {
  pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    alarm(60);
    ctx->para.pacing = ST21_TX_PACING_WAY_RL;
    mtl_handle rl = mtl_init(&ctx->para);
    if (!rl) _exit(2);
    _exit(mtl_uninit(rl) == 0 ? 0 : 1);
  }
  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 2)
    GTEST_SKIP() << "RL pacing init failed, no stale queue rate to reproduce";
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0)
      << "RL child failed, exit " << WEXITSTATUS(status) << " signal "
      << (WIFSIGNALED(status) ? WTERMSIG(status) : 0);

  ctx->para.pacing = ST21_TX_PACING_WAY_TSC;
  initDefaultContext();

  auto bundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true, nullptr, [](St20pHandler* handler) {
        handler->fillSt20Ops(20000, 3, ST20_FMT_YUV_422_10BIT, 1920, 1080, 112,
                             ST_FPS_P59_94);
      });
  auto* handler = bundle.handler;

  StartFakePtpClock();
  handler->startSession();
  ASSERT_GE(mtl_start(ctx->handle), 0);

  const int measureSeconds = 10;
  std::this_thread::sleep_for(std::chrono::seconds(3));
  const uint32_t tx0 = handler->txFrames();
  const uint32_t rx0 = handler->rxFrames();
  std::this_thread::sleep_for(std::chrono::seconds(measureSeconds));
  const double txFps = (double)(handler->txFrames() - tx0) / measureSeconds;
  const double rxFps = (double)(handler->rxFrames() - rx0) / measureSeconds;
  handler->stopSession();
  RecordProperty("tx_fps", std::to_string(txFps));
  RecordProperty("rx_fps", std::to_string(rxFps));

  const double minFps = st_frame_rate(ST_FPS_P59_94) * 0.9;
  EXPECT_GE(txFps, minFps);
  EXPECT_GE(rxFps, minFps);
}
