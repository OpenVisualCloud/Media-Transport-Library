/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright(c) 2026 Intel Corporation */

#include <gtest/gtest.h>

#include "core/test_fixture.hpp"
#include "handlers/st40p_handler.hpp"

namespace {

class St40pAutoDetectStrategy : public FrameTestStrategy {
 public:
  St40pAutoDetectStrategy() : FrameTestStrategy(nullptr, false, true) {
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

TEST_F(NoCtxTest, st40p_rx_auto_detect_interlace) {
  initDefaultContext();

  auto bundle = createSt40pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St40pHandler*) { return new St40pAutoDetectStrategy(); },
      [](St40pHandler* handler) {
        handler->sessionsOpsTx.interlaced = true;  // emit F bits
        handler->sessionsOpsRx.interlaced =
            false;  // unknown at start, auto-detect default
      });

  auto* handler = bundle.handler;
  auto* strategy = static_cast<St40pAutoDetectStrategy*>(bundle.strategy);
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
