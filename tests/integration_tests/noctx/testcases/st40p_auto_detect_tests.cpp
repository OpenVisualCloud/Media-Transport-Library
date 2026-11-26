/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright(c) 2026 Intel Corporation */

#include "core/test_fixture.hpp"

namespace {

/* Records whether RX auto-detect observed interlaced F bits and both field polarities. */
class St40pInterlaceFlagRecorder : public FrameTestStrategy {
 public:
  St40pInterlaceFlagRecorder() : FrameTestStrategy(nullptr, false, true) {
  }

  void rxTestFrameModifier(void* frame, size_t /*frame_size*/) override {
    auto* info = static_cast<st40_frame_info*>(frame);
    ASSERT_NE(info, nullptr);

    if (info->interlaced) {
      saw_interlaced = true;
      if (info->second_field)
        saw_second_field = true;
      else
        saw_first_field = true;
    }
  }

  bool saw_interlaced = false;
  bool saw_first_field = false;
  bool saw_second_field = false;
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

  auto bundle = createSt40pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St40pHandler*) { return new St40pInterlaceFlagRecorder(); },
      [](St40pHandler* handler) {
        handler->sessionsOpsTx.interlaced = true;
        handler->sessionsOpsRx.interlaced = false;
      });

  auto* handler = bundle.handler;
  auto* strategy = static_cast<St40pInterlaceFlagRecorder*>(bundle.strategy);

  StartFakePtpClock();
  handler->startSession();
  ASSERT_EQ(mtl_start(ctx->handle), 0);

  /* 2 s at 60 fields/s delivers ~120 fields, enough to verify interlaced F-bit detection.
   */
  sleepUntilFailure(2);

  handler->stopSession();

  ASSERT_GT(handler->txFrames(), 0u) << "No frames transmitted";
  ASSERT_GT(handler->rxFrames(), 0u) << "No frames received";
  EXPECT_EQ(handler->txFrames(), handler->rxFrames()) << "TX/RX frame count mismatch";
  EXPECT_TRUE(strategy->saw_interlaced) << "Auto-detect did not see interlaced F bits";
  EXPECT_TRUE(strategy->saw_first_field) << "Auto-detect did not see first field";
  EXPECT_TRUE(strategy->saw_second_field) << "Auto-detect did not see second field";
}
