/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins tv_train_pacing() in st_tx_video_session.c: the RL training at session create
 * fills the queue with a whole ring of pads and keeps it full, and the session then
 * paces packet 0 of the first frames as if that queue were idle. Training traffic still
 * queued at return makes them leave late, behind it.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*St20TxRl*Train*Test.*'
 */

#include <gtest/gtest.h>

#include "session/st20_tx_harness.h"

namespace {
/* shaper time of one pad: the 1080p59.94 GPM_SL trs, ~3707 ns, a pad interval of ~440
 * short */
constexpr long double kPadNs = 3699;
/* the 1080p59.94 BPM trs, ~3892 ns, a pad interval of ~500 short */
constexpr long double kBpmPadNs = 3884;
}  // namespace

class St20TxRlTrainingTest : public ::testing::TestWithParam<uint16_t> {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

TEST_P(St20TxRlTrainingTest, TrainingTrafficDrainsBeforeTheSessionStarts) {
  ASSERT_EQ(0, ut_txv_train_rl_pacing(ctx_, GetParam(), kPadNs, false));
  ASSERT_GT(ut_txv_pad_interval(ctx_), 32); /* the training measured the queue */

  EXPECT_EQ(0u, ut_txv_train_queued_ns(ctx_));
  /* as long as the ring needs, rounded up to the next us */
  EXPECT_GE(ut_txv_slept_ns(ctx_), GetParam() * kPadNs);
  EXPECT_LE(ut_txv_slept_ns(ctx_), GetParam() * kPadNs + 1000);
}

/* the pre-arm queues NORMAL pads only, but BPM trains on a frame's pads, a quarter of
 * them EXTRA and 6 bytes longer: the plan needs the NORMAL pad's drain, not the mean */
TEST_P(St20TxRlTrainingTest, PreArmDrainIsTheNormalPads) {
  ASSERT_EQ(0, ut_txv_train_rl_pacing(ctx_, GetParam(), kBpmPadNs, true));
  ASSERT_GT(ut_txv_pad_interval(ctx_), 32);

  /* 1 ps a pad, 0.13 ns over a 128-pad warm-up; the mock times a frame to the ns */
  EXPECT_NEAR(ut_txv_rl_drain(ctx_, 0), kBpmPadNs, 0.001L);
}

/* a double resolves a ~1.79e18 ns TAI instant to 256 ns, no divisor of a BPM frame */
TEST_P(St20TxRlTrainingTest, TaiScalePtpTimeKeepsTheDrainExact) {
  ut_txv_set_mock_ptp_time(ctx_, 1790000000123456789ull);
  ASSERT_EQ(0, ut_txv_train_rl_pacing(ctx_, GetParam(), kBpmPadNs, true));
  ASSERT_GT(ut_txv_pad_interval(ctx_), 32);

  EXPECT_NEAR(ut_txv_rl_drain(ctx_, 0), kBpmPadNs, 0.001L);
}

INSTANTIATE_TEST_SUITE_P(NbTxDesc, St20TxRlTrainingTest, ::testing::Values(512, 2048));

/* a pre-trained pad_interval is the mean of the training pads of one mix: a session of
 * another mix at the same rate trains its own */
class St20TxRlPreTrainTest : public St20TxRlTrainingTest {
 protected:
  /* trains a BPM frame-level session, a quarter of its pads EXTRA */
  void TrainBpmFrames() {
    ASSERT_EQ(0, ut_txv_train_rl_pacing(ctx_, 512, kBpmPadNs, true));
    trained_ = ut_txv_pad_interval(ctx_);
    slept_ns_ = ut_txv_slept_ns(ctx_);
  }
  float trained_ = 0;
  uint64_t slept_ns_ = 0;
};

TEST_F(St20TxRlPreTrainTest, SamePadMixReusesThePreTrainedInterval) {
  TrainBpmFrames();

  /* a faster queue would train another pad_interval */
  ASSERT_EQ(0, ut_txv_train_rl_pacing(ctx_, 512, kBpmPadNs - 5, true));

  EXPECT_EQ(ut_txv_pad_interval(ctx_), trained_);
  EXPECT_EQ(ut_txv_slept_ns(ctx_), slept_ns_);
  EXPECT_NEAR(ut_txv_rl_drain(ctx_, 0), kBpmPadNs, 0.001L);
}

/* RTP level trains on NORMAL pads only, at the same rate */
TEST_F(St20TxRlPreTrainTest, PreTrainedIntervalOfAnotherPadMixIsNotReused) {
  TrainBpmFrames();
  ut_txv_set_rtp_level(ctx_, true);

  ASSERT_EQ(0, ut_txv_train_rl_pacing(ctx_, 512, kBpmPadNs, true));

  EXPECT_GT(ut_txv_slept_ns(ctx_), slept_ns_);
  EXPECT_NEAR(ut_txv_rl_drain(ctx_, 0), kBpmPadNs, 0.001L);
}

/* the rate was retrained for the first mix already: the session falls back to TSC */
TEST_F(St20TxRlPreTrainTest, AnotherPadMixOnARetrainedRateGivesUpTheRl) {
  TrainBpmFrames();
  ASSERT_EQ(0, ut_txv_add_trained_bps(ctx_, 1));
  ut_txv_set_rtp_level(ctx_, true);

  /* a queue slower than the rate */
  EXPECT_EQ(-EINVAL, ut_txv_train_rl_pacing(ctx_, 512, 3900, true));
}

/* A retrain of the rate for another mix programs later sessions of the first mix on
 * faster queues too, which drain faster than it trained: they train their own. */
TEST_F(St20TxRlPreTrainTest, PreTrainedIntervalIsKeyedByTheQueueRate) {
  TrainBpmFrames();
  const uint64_t trained_bps = ut_txv_rl_queue_bps(ctx_, 0);
  ut_txv_set_rtp_level(ctx_, true);
  /* a queue slower than the rate: retrained */
  ASSERT_EQ(0, ut_txv_train_rl_pacing(ctx_, 512, 3900, true));
  const uint64_t retrained_bps = ut_txv_rl_queue_bps(ctx_, 0);
  ASSERT_GT(retrained_bps, trained_bps);
  ut_txv_set_rtp_level(ctx_, false);
  const uint64_t slept_ns = ut_txv_slept_ns(ctx_);
  const long double pad_ns = kBpmPadNs * trained_bps / retrained_bps;

  ASSERT_EQ(0, ut_txv_train_rl_pacing(ctx_, 512, pad_ns, true));

  EXPECT_EQ(ut_txv_rl_queue_bps(ctx_, 0), retrained_bps);
  EXPECT_GT(ut_txv_slept_ns(ctx_), slept_ns);
  EXPECT_NEAR(ut_txv_rl_drain(ctx_, 0), pad_ns, 0.001L);
}

TEST_F(St20TxRlPreTrainTest, PreTrainedIntervalMatchesTheRatesAndTheWholeMix) {
  float pad_interval = 0;
  ASSERT_EQ(0, ut_txv_pad_result_add(ctx_, 1000, 1005, 5000, 1200, 4, 512.5f));

  EXPECT_EQ(0, ut_txv_pad_result_search(ctx_, 1000, 1005, 5000, 1200, 4, &pad_interval));
  EXPECT_EQ(pad_interval, 512.5f);
  EXPECT_EQ(-EINVAL,
            ut_txv_pad_result_search(ctx_, 1001, 1005, 5000, 1200, 4, &pad_interval));
  EXPECT_EQ(-EINVAL,
            ut_txv_pad_result_search(ctx_, 1000, 1000, 5000, 1200, 4, &pad_interval));
  EXPECT_EQ(-EINVAL,
            ut_txv_pad_result_search(ctx_, 1000, 1005, 5001, 1200, 4, &pad_interval));
  EXPECT_EQ(-EINVAL,
            ut_txv_pad_result_search(ctx_, 1000, 1005, 5000, 1201, 4, &pad_interval));
  EXPECT_EQ(-EINVAL,
            ut_txv_pad_result_search(ctx_, 1000, 1005, 5000, 1200, 5, &pad_interval));
}

/* GPM_SL: a line whose bytes do not split evenly into pkts ends in a shorter LINE_TAIL
 * pkt, which the shaper drains by its bytes; training on NORMAL pads only overstates a
 * frame's time, so the trained pad_interval leaves the frame short of its active time */
namespace {

struct LineTailCase {
  const char* name;
  enum st20_fmt fmt;
  uint32_t width;
  uint32_t height;
  uint32_t line_tail_pkts;
};

class St20TxRlTrainLineTailTest : public ::testing::TestWithParam<LineTailCase> {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

/* the queue drains a NORMAL pad 0.3% faster than trs, which the pads fill */
TEST_P(St20TxRlTrainLineTailTest, FrameFillsItsActiveTime) {
  const LineTailCase& c = GetParam();
  ASSERT_EQ(0, ut_txv_train_rl_gpm_sl(ctx_, c.fmt, c.width, c.height, 0.997L));
  ASSERT_EQ(ut_txv_line_tail_pkts(ctx_), c.line_tail_pkts);
  ASSERT_GT(ut_txv_pad_interval(ctx_), 32);
  const long double trs = ut_txv_pacing_trs(ctx_);

  /* a frame short by more than a pkt pushes VRX up a pkt by its end */
  EXPECT_NEAR(ut_txv_gpm_sl_frame_ns(ctx_), trs * ut_txv_pipe_total_pkts(ctx_), trs)
      << "pad_interval " << ut_txv_pad_interval(ctx_);
  /* the pre-arm queues NORMAL pads: the plan keeps the NORMAL pad's drain */
  EXPECT_NEAR(ut_txv_rl_drain(ctx_, 0), trs * 0.997L, 0.001L);
}

INSTANTIATE_TEST_SUITE_P(
    GpmSl, St20TxRlTrainLineTailTest,
    ::testing::Values(
        LineTailCase{"Yuv422_10bit1080p", ST20_FMT_YUV_422_10BIT, 1920, 1080, 0},
        LineTailCase{"Yuv444_12bit1080p", ST20_FMT_YUV_444_12BIT, 1920, 1080, 1080},
        LineTailCase{"Rgb12bit1080p", ST20_FMT_RGB_12BIT, 1920, 1080, 1080},
        LineTailCase{"Yuv422_10bit720p", ST20_FMT_YUV_422_10BIT, 1280, 720, 720},
        LineTailCase{"Yuv422_10bit2160p", ST20_FMT_YUV_422_10BIT, 3840, 2160, 2160}),
    [](const ::testing::TestParamInfo<LineTailCase>& info) { return info.param.name; });
}  // namespace
