/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright(c) 2026 Intel Corporation */

#include <gtest/gtest.h>

#include "core/test_fixture.hpp"
#include "handlers/st40p_handler.hpp"

namespace {

/* Records whether any RX frame reported interlaced; the test asserts on it. */
class St40pInterlaceFlagRecorder : public FrameTestStrategy {
 public:
  St40pInterlaceFlagRecorder() : FrameTestStrategy(nullptr, false, true) {
  }

  void rxTestFrameModifier(void* frame, size_t /*frame_size*/) override {
    auto* info = static_cast<st40_frame_info*>(frame);
    ASSERT_NE(info, nullptr);
    if (!info) return;

    if (info->interlaced) {
      saw_interlaced = true;
      last_second_field = info->second_field;
      second_field_sampled = true;
    }
  }

  bool saw_interlaced = false;
  bool last_second_field = false;
  bool second_field_sampled = false;
};

}  // namespace

/* st40p_rx_auto_detect_interlace
 * Config:    initDefaultContext(); one session TX TEST_PORT_1 -> RX TEST_PORT_2, 60p,
 *            4 buffers (fillSt40pOps()); TX kTxInterlaced (sets F bits), RX
 *            kRxInterlaced = false, left to auto-detect.
 * Plan:      default pacing.
 * Expect:    after stop:
 *            1. txFrames() > 0, rxFrames() > 0, equal
 *            2. some RX frame reported interlaced   St40pInterlaceFlagRecorder
 *            Every TX and RX frame also passes the St40pHandler thread checks.
 * Skip/Fail: none.
 */
TEST_F(NoCtxTest, st40p_rx_auto_detect_interlace) {
  initDefaultContext();

  constexpr bool kTxInterlaced = true;
  constexpr bool kRxInterlaced = false;

  auto bundle = createSt40pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St40pHandler*) { return new St40pInterlaceFlagRecorder(); },
      [](St40pHandler* handler) {
        handler->sessionsOpsTx.interlaced = kTxInterlaced;
        handler->sessionsOpsRx.interlaced = kRxInterlaced;
      });

  auto* handler = bundle.handler;
  auto* strategy = static_cast<St40pInterlaceFlagRecorder*>(bundle.strategy);
  ASSERT_NE(handler, nullptr);
  ASSERT_NE(strategy, nullptr);

  StartFakePtpClock();
  handler->startSession();
  mtl_start(ctx->handle);

  sleepUntilFailure();

  handler->stopSession();

  ASSERT_GT(handler->txFrames(), 0u) << "No frames transmitted";
  ASSERT_GT(handler->rxFrames(), 0u) << "No frames received";
  EXPECT_EQ(handler->txFrames(), handler->rxFrames()) << "TX/RX frame count mismatch";
  EXPECT_TRUE(strategy->saw_interlaced) << "Auto-detect did not see interlaced F bits";
  EXPECT_TRUE(strategy->second_field_sampled)
      << "Auto-detect did not surface field cadence metadata";
}
