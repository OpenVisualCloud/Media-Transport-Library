/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/* IPMX sender timing, TR-10-1 section 8.1: an IPMX st20p TX paced by default RL keeps
 * every frame within CMAX and the IPMX VRX, as the library's RX timing parser measures
 * it on NIC RX timestamps. Catalogue: README.md here; the KahawaiTest counterpart is
 * Ipmx_st20p.*_tp_*.
 *
 * Common to every test here:
 * Config:    initStrictPacingContext(); one session TX TEST_PORT_1 -> RX TEST_PORT_2,
 *            RFC 4175 frames (no conversion), BPM, 3 buffers, UDP 20000, default RL
 *            pacing; TX ST20P_TX_FLAG_IPMX, RX ST20P_RX_FLAG_IPMX |
 *            ST20P_RX_FLAG_TIMING_PARSER_META.
 * Plan:      default pacing for 20 s (sleepUntilFailure()).
 * Expect:    on every RX frame, St20pIpmxTpRecorder:
 *            1. receive_timestamp is a NIC RX timestamp (RxPhcClock)
 *            after stop, from ipmx_st20p_rx_get_tp_stat() on port P:
 *            2. returns 0; cmax and vrx_full are the format's TR-10-1 limits
 *            3. measured (compliant + failed) frames >= kMinMeasuredFrames and >= 50 %
 *               of the frames the parser saw
 *            4. frames_failed == 0
 * Skip/Fail: TX and RX on one physical port, no RX PHC or no NIC RX timestamp on
 *            frame 0: SKIP, FAIL with NOCTX_REQUIRE_STRICT=1. A software RX time
 *            after frame 0: FAIL (RxPhcClock).
 */

#ifdef MTL_HAS_IPMX

#include <cstdio>

#include "core/constants.hpp"
#include "core/test_fixture.hpp"
#include "handlers/st20p_handler.hpp"
#include "ipmx/ipmx_api.h"
#include "strategies/st20p_strategies.hpp"

namespace {
/* 10 s of 60p, half the run */
constexpr uint32_t kMinMeasuredFrames = 600;

class St20pIpmxTpRecorder : public FrameTestStrategy {
 public:
  explicit St20pIpmxTpRecorder(St20pHandler* parentHandler)
      : FrameTestStrategy(parentHandler, false, true) {
    rxPhc.start(parentHandler->sessionsOpsRx.port.port[MTL_SESSION_PORT_P]);
  }

  void rxTestFrameModifier(void* frame, size_t /*frame_size*/) override {
    auto* f = static_cast<st_frame*>(frame);
    auto* handler = static_cast<St20pHandler*>(parent);
    RxTime rx;

    /* for its NIC RX timestamp check; the time itself is unused */
    rxPhc.receiveTimeMonotonicRaw(idx_rx, f->receive_timestamp,
                                  handler->sessionsOpsRx.port.port[MTL_SESSION_PORT_P],
                                  handler->ctx->handle, &rx);
    idx_rx++;
  }

 private:
  RxPhcClock rxPhc;
};

void runIpmxTp(NoCtxTest* test, st_tests_context* ctx, enum st_frame_fmt frame_fmt,
               enum st20_fmt fmt, uint width, uint height, int32_t cmax) {
  auto bundle = test->createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St20pHandler* handler) { return new St20pIpmxTpRecorder(handler); },
      [=](St20pHandler* handler) {
        handler->fillSt20Ops(20000, 3, fmt, width, height, 112, ST_FPS_P60);
        handler->sessionsOpsTx.input_fmt = frame_fmt;
        handler->sessionsOpsTx.transport_fmt = fmt;
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_IPMX;
        handler->sessionsOpsRx.output_fmt = frame_fmt;
        handler->sessionsOpsRx.transport_fmt = fmt;
        handler->sessionsOpsRx.flags |=
            ST20P_RX_FLAG_IPMX | ST20P_RX_FLAG_TIMING_PARSER_META;
      });
  ASSERT_NE(bundle.handler->sessionsHandleRx, nullptr);

  bundle.handler->startSession();
  ASSERT_EQ(mtl_start(ctx->handle), 0);
  test->sleepUntilFailure();
  bundle.handler->stopSession();
  if (::testing::Test::IsSkipped()) return;

  struct ipmx_rx_tp_stat stat;
  ASSERT_EQ(ipmx_st20p_rx_get_tp_stat(bundle.handler->sessionsHandleRx,
                                      MTL_SESSION_PORT_P, &stat),
            0);
  const uint32_t measured = stat.frames_compliant + stat.frames_failed;
  (void)fprintf(
      stderr,
      "NoCtx ipmx: rx frames %u, compliant %u failed %u unmeasured %u, cinst max "
      "%d cmax %d, vrx min %d max %d full %d, last failed cause %s\n",
      bundle.strategy->idx_rx, stat.frames_compliant, stat.frames_failed,
      stat.frames_unmeasured, stat.cinst_max, stat.cmax, stat.vrx_min, stat.vrx_max,
      stat.vrx_full, stat.failed_cause);
  EXPECT_EQ(stat.cmax, cmax);
  EXPECT_EQ(stat.vrx_full, cmax * 2);
  EXPECT_GE(measured, kMinMeasuredFrames);
  EXPECT_GE(measured * 2, measured + stat.frames_unmeasured);
  EXPECT_EQ(stat.frames_failed, 0u) << "last failed cause: " << stat.failed_cause;
}
}  // namespace

/* st20p_ipmx_tp_1080p60
 * 1080p60 YUV 4:2:2 10-bit: 4115 packets, CMAX 16, VRXFULL 32. */
TEST_F(NoCtxTest, st20p_ipmx_tp_1080p60) {
  initStrictPacingContext();
  if (IsSkipped() || HasFatalFailure()) return;
  runIpmxTp(this, ctx, ST_FRAME_FMT_YUV422RFC4175PG2BE10, ST20_FMT_YUV_422_10BIT, 1920,
            1080, 16);
}

/* st20p_ipmx_tp_2160p60
 * 2160p60 YUV 4:2:2 10-bit: 16458 packets, CMAX 45, VRXFULL 90. */
TEST_F(NoCtxTest, st20p_ipmx_tp_2160p60) {
  initStrictPacingContext();
  if (IsSkipped() || HasFatalFailure()) return;
  runIpmxTp(this, ctx, ST_FRAME_FMT_YUV422RFC4175PG2BE10, ST20_FMT_YUV_422_10BIT, 3840,
            2160, 45);
}

/* st20p_ipmx_tp_rgb10_2160p60
 * 2160p60 RGB 4:4:4 10-bit, the heaviest D6 rate (~16 Gb/s): 24686 packets, CMAX 68,
 * VRXFULL 136. */
TEST_F(NoCtxTest, st20p_ipmx_tp_rgb10_2160p60) {
  initStrictPacingContext();
  if (IsSkipped() || HasFatalFailure()) return;
  runIpmxTp(this, ctx, ST_FRAME_FMT_RGBRFC4175PG4BE10, ST20_FMT_RGB_10BIT, 3840, 2160,
            68);
}

#endif
