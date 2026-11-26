/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pure-function pin for st_frame_period_ns(), the frame period the pipeline TX
 * sessions cache at create and their DROP_WHEN_LATE window measures against.
 * Expected values are 1e9 * den / mul truncated, so a swapped mul/den moves
 * them; they are bit-identical to the double form the callers used before.
 *
 * Also pins st_frame_rate_to_st_fps()'s snap warning: its match windows are up to a full
 * fps wide, so an off-list rate is reclassified as a neighbour and paced at the wrong
 * period. The warning is the only signal the caller gets, so both directions of
 * ST_FPS_SNAP_TOLERANCE matter - silent for every rate a conformant caller can express,
 * loud for every rate that is really being substituted.
 *
 * Run:   ./build_unit/tests/unit/UnitTest
 *          --gtest_filter='StFramePeriodNs*:StEpochTai*:StFrameRateToStFps*'
 */

#include <errno.h>
#include <gtest/gtest.h>
#include <stdarg.h>
#include <stdio.h>

extern "C" {
#include <mtl_api.h>
#include <st30_api.h>
}

namespace {

uint64_t g_mock_ptp_now_ns = 0;

uint64_t ut_mock_ptp_read_time_raw(mtl_handle /*mt*/) {
  return g_mock_ptp_now_ns;
}

}  // namespace

/* Redirect mtl_ptp_read_time_raw() inside static inline st_frame_is_late() before
 * st_fmt.h pulls in st_pipeline_api.h. */
#define mtl_ptp_read_time_raw ut_mock_ptp_read_time_raw
#include <st_pipeline_api.h>
#undef mtl_ptp_read_time_raw

extern "C" {
#include "st2110/st_fmt.h"
}

TEST(StFramePeriodNs, EveryFpsInTheTimingTable) {
  const struct {
    enum st_fps fps;
    uint64_t period_ns;
  } cases[] = {
      {ST_FPS_P120, 8333333}, {ST_FPS_P119_88, 8341666}, {ST_FPS_P100, 10000000},
      {ST_FPS_P60, 16666666}, {ST_FPS_P59_94, 16683333}, {ST_FPS_P50, 20000000},
      {ST_FPS_P30, 33333333}, {ST_FPS_P29_97, 33366666}, {ST_FPS_P25, 40000000},
      {ST_FPS_P24, 41666666}, {ST_FPS_P23_98, 41708333},
  };

  for (auto& c : cases) {
    uint64_t period_ns = 0;
    ASSERT_EQ(st_frame_period_ns(c.fps, &period_ns), 0) << "fps " << c.fps;
    EXPECT_EQ(period_ns, c.period_ns) << "fps " << c.fps;
  }
}

TEST(StFramePeriodNs, InvalidFpsLeavesPeriodUntouched) {
  uint64_t period_ns = 0xdeadbeef;

  EXPECT_EQ(st_frame_period_ns(ST_FPS_MAX, &period_ns), -EINVAL);
  EXPECT_EQ(period_ns, 0xdeadbeefu);
}

/* The epoch math every TX session paces from: epoch k of a den/mul s period starts at
 * the last ns at or before k * 1e9 * den / mul, kept exact at today's ~1e11 epochs. */
static constexpr uint64_t kNsPerS = 1000000000;

static uint64_t ExactEpochTai(uint64_t k, uint64_t mul, uint64_t den) {
  return (uint64_t)((unsigned __int128)k * kNsPerS * den / mul);
}

TEST(StEpochTai, RoundTripsAtTodaysTaiForEveryFps) {
  for (int fps = 0; fps < ST_FPS_MAX; fps++) {
    struct st_fps_timing tm;
    if (st_get_fps_timing((enum st_fps)fps, &tm) < 0) continue;
    const uint64_t period_floor = kNsPerS * tm.den / tm.mul;
    const uint64_t first = 1790000000ull * tm.mul / tm.den;

    for (uint64_t k = first; k < first + 10000; k++) {
      const uint64_t tai = st_epoch_to_tai(k, tm.mul, tm.den);
      ASSERT_EQ(tai, ExactEpochTai(k, tm.mul, tm.den)) << tm.name << " epoch " << k;
      ASSERT_EQ(st_tai_to_epoch(tai, tm.mul, tm.den), k) << tm.name << " epoch " << k;
      ASSERT_EQ(st_tai_to_epoch(tai - 1, tm.mul, tm.den), k - 1)
          << tm.name << " epoch " << k;
      const uint64_t step = st_epoch_to_tai(k + 1, tm.mul, tm.den) - tai;
      ASSERT_TRUE(step == period_floor || step == period_floor + 1)
          << tm.name << " epoch " << k << " step " << step;
    }
  }
}

/* the 90 kHz tick of an epoch is the exact instant's, ties down as
 * st10_tai_to_media_clk() rounds: odd 59.94 epochs sit on a half tick */
TEST(StEpochTai, EpochKeepsTheExactMediaClockTick) {
  for (int fps = 0; fps < ST_FPS_MAX; fps++) {
    struct st_fps_timing tm;
    if (st_get_fps_timing((enum st_fps)fps, &tm) < 0) continue;
    const uint64_t first = 1790000000ull * tm.mul / tm.den;
    const unsigned __int128 tick_den = tm.mul;

    for (uint64_t k = first; k < first + 10000; k++) {
      const unsigned __int128 num =
          (unsigned __int128)k * tm.sampling_clock_rate * tm.den;
      uint64_t tick = (uint64_t)(num / tick_den);
      if (2 * (num % tick_den) > tick_den) tick++;
      ASSERT_EQ(st10_tai_to_media_clk(st_epoch_to_tai(k, tm.mul, tm.den),
                                      tm.sampling_clock_rate),
                (uint32_t)tick)
          << tm.name << " epoch " << k;
    }
  }
}

TEST(StEpochTai, NearestEpochTiesToTheLater) {
  constexpr uint64_t kEpoch = 44750000000ull; /* 25 fps, 40 ms */
  const uint64_t half = kEpoch * 40000000 + 20000000;

  EXPECT_EQ(st_tai_to_nearest_epoch(half - 1, 25, 1), kEpoch);
  EXPECT_EQ(st_tai_to_nearest_epoch(half, 25, 1), kEpoch + 1);
}

TEST(StEpochTai, SaturatesPastTheLastNs) {
  EXPECT_EQ(st_epoch_to_tai(UINT64_MAX / 1000, 60000, 1001), UINT64_MAX);
  EXPECT_EQ(st_tai_to_epoch(UINT64_MAX, 60000, 1001),
            (uint64_t)((unsigned __int128)UINT64_MAX * 60000 /
                       ((unsigned __int128)kNsPerS * 1001)));
}

static int g_warn_cnt;
static char g_warn_msg[256];

static void ut_fps_log_printer(enum mtl_log_level level, const char* format, ...) {
  va_list args;

  if (level != MTL_LOG_LEVEL_WARNING) return;

  g_warn_cnt++;
  va_start(args, format);
  vsnprintf(g_warn_msg, sizeof(g_warn_msg), format, args);
  va_end(args);
}

class StFrameRateToStFps : public ::testing::Test {
 protected:
  void SetUp() override {
    g_warn_cnt = 0;
    g_warn_msg[0] = '\0';
    mtl_set_log_printer(ut_fps_log_printer);
  }
  void TearDown() override {
    mtl_set_log_printer(NULL);
  }
};

/* Every rate a conformant caller can hand over: the integer rates, the four fractional
 * rates as the exact rational a GStreamer/FFmpeg caller passes, and the same four as the
 * rounded decimal the table names them by. None of these may warn. */
TEST_F(StFrameRateToStFps, ConformantRatesAreSilent) {
  const struct {
    double framerate;
    enum st_fps fps;
  } cases[] = {
      {120.0, ST_FPS_P120},
      {120000.0 / 1001, ST_FPS_P119_88},
      {119.88, ST_FPS_P119_88},
      {100.0, ST_FPS_P100},
      {60.0, ST_FPS_P60},
      {60000.0 / 1001, ST_FPS_P59_94},
      {59.94, ST_FPS_P59_94},
      {50.0, ST_FPS_P50},
      {30.0, ST_FPS_P30},
      {30000.0 / 1001, ST_FPS_P29_97},
      {29.97, ST_FPS_P29_97},
      {25.0, ST_FPS_P25},
      {24.0, ST_FPS_P24},
      {24000.0 / 1001, ST_FPS_P23_98},
      {23.98, ST_FPS_P23_98},
      /* Roundings of mul/den that differ from the name the table carries, so unlike
       * 119.880 or 29.970 these are not the name in disguise and do reach the tolerance.
       * 23.9765 is the closest conformant input to it: 4.76e-4 of 1e-3. */
      {119.8801, ST_FPS_P119_88},
      {59.9401, ST_FPS_P59_94},
      {23.976, ST_FPS_P23_98},
      {23.9765, ST_FPS_P23_98},
  };

  for (auto& c : cases) {
    g_warn_cnt = 0;
    EXPECT_EQ(st_frame_rate_to_st_fps(c.framerate), c.fps) << c.framerate;
    EXPECT_EQ(g_warn_cnt, 0) << c.framerate << " warned: " << g_warn_msg;
  }
}

/* The snapped return values themselves are asserted by Main.frame_rate_to_st_fps in
 * st_fmt_api_test.cpp and are deliberately unchanged; what is pinned here is
 * that snapping is no longer silent, which is all a GStreamer or FFmpeg caller
 * ever sees. */
TEST_F(StFrameRateToStFps, OffListRatesWarnAndStillSnap) {
  const struct {
    double framerate;
    enum st_fps fps;
  } cases[] = {
      {26.0, ST_FPS_P25},
      {51.0, ST_FPS_P50},
      {31.0, ST_FPS_P30},
      {23.0, ST_FPS_P23_98},
      {49.0, ST_FPS_P50},
      {24.99, ST_FPS_P24},
      /* the near misses: a fraction of an fps off is still a wrong pacing period, and a
       * tolerance sized to the table's rounded names would let all of these through */
      {29.99, ST_FPS_P29_97},
      {29.98, ST_FPS_P29_97},
      {25.01, ST_FPS_P25},
      {119.94, ST_FPS_P119_88},
      {120.06, ST_FPS_P120},
      {59.95, ST_FPS_P59_94},
      /* the far side of the tolerance from 23.9765 above: 1.476e-3 of 1e-3, so the two
       * together bracket ST_FPS_SNAP_TOLERANCE to within ~3x */
      {23.9775, ST_FPS_P23_98},
  };

  for (auto& c : cases) {
    g_warn_cnt = 0;
    EXPECT_EQ(st_frame_rate_to_st_fps(c.framerate), c.fps) << c.framerate;
    EXPECT_EQ(g_warn_cnt, 1) << c.framerate << " did not warn";
  }
}

/* Outside every window the existing err() already fires, so no warning is added. */
TEST_F(StFrameRateToStFps, RateOutsideEveryWindowIsRejected) {
  EXPECT_EQ(st_frame_rate_to_st_fps(48.0), ST_FPS_MAX);
  EXPECT_EQ(g_warn_cnt, 0);
}

/* The pacer sends one packet per st30_get_packet_time() and the RTP clock advances by
 * st30_get_sample_num() per packet, so the two must agree with the sampling rate. */
TEST(St30PacketTime, CarriesExactlyTheSamplesOfOnePacket) {
  for (int sampling = 0; sampling < ST30_SAMPLING_MAX; sampling++) {
    for (int ptime = 0; ptime < ST30_PTIME_MAX; ptime++) {
      int samples =
          st30_get_sample_num((enum st30_ptime)ptime, (enum st30_sampling)sampling);
      if (samples < 0) continue;
      double rate = st30_get_sample_rate((enum st30_sampling)sampling);
      double pkt_time_ns = st30_get_packet_time((enum st30_ptime)ptime);
      EXPECT_NEAR(pkt_time_ns, samples * 1e9 / rate, 1.0)
          << "ptime " << ptime << " sampling " << sampling;
    }
  }
}

TEST(StFrameFmtIsCodestream, EndSentinelIsNotACodestream) {
  EXPECT_TRUE(st_frame_fmt_is_codestream(ST_FRAME_FMT_JPEGXS_CODESTREAM));
  EXPECT_TRUE(st_frame_fmt_is_codestream(ST_FRAME_FMT_H265_CODESTREAM));
  EXPECT_FALSE(st_frame_fmt_is_codestream(ST_FRAME_FMT_CODESTREAM_END));
  EXPECT_FALSE(st_frame_fmt_is_codestream(ST_FRAME_FMT_MAX));
  EXPECT_FALSE(st_frame_fmt_is_codestream(ST_FRAME_FMT_RGB_END));
}

/* A codestream frame has no size of its own: st_frame_size() gives 0, so there is no
 * buffer to hand out, as with st_frame_create(). */
TEST(StFrameCreateByMalloc, FormatWithoutASizeReturnsNull) {
  ASSERT_EQ(st_frame_size(ST_FRAME_FMT_JPEGXS_CODESTREAM, 1920, 1080, false), 0u);
  EXPECT_EQ(st_frame_create_by_malloc(ST_FRAME_FMT_JPEGXS_CODESTREAM, 1920, 1080, false),
            nullptr);
}

/* st_frame_free() frees addr[0], so every frame the call hands out must keep its buffer
 * there. */
TEST(StFrameCreateByMalloc, EveryFormatWithASizeKeepsItsBufferInPlaneZero) {
  for (int fmt = 0; fmt < ST_FRAME_FMT_MAX; fmt++) {
    size_t size = st_frame_size((enum st_frame_fmt)fmt, 1920, 1080, false);
    if (!size) continue;
    struct st_frame* frame =
        st_frame_create_by_malloc((enum st_frame_fmt)fmt, 1920, 1080, false);
    ASSERT_NE(frame, nullptr) << "fmt " << fmt;
    EXPECT_NE(frame->addr[0], nullptr) << "fmt " << fmt;
    EXPECT_EQ(frame->buffer_size, size) << "fmt " << fmt;
    EXPECT_EQ(st_frame_free(frame), 0) << "fmt " << fmt;
  }
}

TEST(St30FormatHelpers, SampleSizeValidAndInvalid) {
  const struct {
    enum st30_fmt fmt;
    int expected;
  } valid_cases[] = {
      {ST30_FMT_PCM8, 1},
      {ST30_FMT_PCM16, 2},
      {ST30_FMT_PCM24, 3},
      {ST31_FMT_AM824, 4},
  };

  for (const auto& c : valid_cases) {
    EXPECT_EQ(st30_get_sample_size(c.fmt), c.expected) << "fmt " << c.fmt;
  }

  EXPECT_EQ(st30_get_sample_size(ST30_FMT_MAX), -EINVAL);
  EXPECT_EQ(st30_get_sample_size(static_cast<enum st30_fmt>(-1)), -EINVAL);
}

TEST(St30FormatHelpers, PacketSizeMatrixAndInvalid) {
  const struct {
    enum st30_fmt fmt;
    int sample_bytes;
  } fmts[] = {
      {ST30_FMT_PCM8, 1},
      {ST30_FMT_PCM16, 2},
      {ST30_FMT_PCM24, 3},
      {ST31_FMT_AM824, 4},
  };
  const struct {
    enum st30_sampling sampling;
    enum st30_ptime ptime;
    int samples;
  } rates[] = {
      {ST30_SAMPLING_48K, ST30_PTIME_1MS, 48},
      {ST30_SAMPLING_48K, ST30_PTIME_125US, 6},
      {ST30_SAMPLING_48K, ST30_PTIME_250US, 12},
      {ST30_SAMPLING_48K, ST30_PTIME_333US, 16},
      {ST30_SAMPLING_48K, ST30_PTIME_4MS, 192},
      {ST30_SAMPLING_48K, ST31_PTIME_80US, 4},
      {ST30_SAMPLING_96K, ST30_PTIME_1MS, 96},
      {ST30_SAMPLING_96K, ST30_PTIME_125US, 12},
      {ST30_SAMPLING_96K, ST30_PTIME_250US, 24},
      {ST30_SAMPLING_96K, ST30_PTIME_333US, 32},
      {ST30_SAMPLING_96K, ST30_PTIME_4MS, 384},
      {ST30_SAMPLING_96K, ST31_PTIME_80US, 8},
      {ST31_SAMPLING_44K, ST31_PTIME_1_09MS, 48},
      {ST31_SAMPLING_44K, ST31_PTIME_0_14MS, 6},
      {ST31_SAMPLING_44K, ST31_PTIME_0_09MS, 4},
  };
  const uint16_t channels[] = {1, 2, 4, 8};

  for (const auto& f : fmts) {
    for (const auto& r : rates) {
      for (uint16_t ch : channels) {
        EXPECT_EQ(st30_get_packet_size(f.fmt, r.ptime, r.sampling, ch),
                  f.sample_bytes * r.samples * ch)
            << "fmt " << f.fmt << " ptime " << r.ptime << " sampling " << r.sampling
            << " ch " << ch;
      }
    }
  }

  /* Invalid format, ptime, sampling, mismatched ptime/sampling, and zero channels. */
  const struct {
    enum st30_fmt fmt;
    enum st30_ptime ptime;
    enum st30_sampling sampling;
    uint16_t channel;
  } invalid_pkt[] = {
      {ST30_FMT_MAX, ST30_PTIME_1MS, ST30_SAMPLING_48K, 2},
      {static_cast<enum st30_fmt>(-1), ST30_PTIME_1MS, ST30_SAMPLING_48K, 2},
      {ST30_FMT_PCM24, ST30_PTIME_MAX, ST30_SAMPLING_48K, 2},
      {ST30_FMT_PCM24, ST30_PTIME_1MS, ST30_SAMPLING_MAX, 2},
      {ST30_FMT_PCM24, ST30_PTIME_1MS, ST31_SAMPLING_44K, 2},
      {ST30_FMT_PCM24, ST30_PTIME_1MS, ST30_SAMPLING_48K, 0},
  };
  for (const auto& c : invalid_pkt) {
    EXPECT_EQ(st30_get_packet_size(c.fmt, c.ptime, c.sampling, c.channel), -EINVAL);
  }
}

TEST(St30FormatHelpers, SampleNumRateAndPacketTimeInvalid) {
  const struct {
    enum st30_ptime ptime;
    enum st30_sampling sampling;
  } bad_pairs[] = {
      {ST30_PTIME_MAX, ST30_SAMPLING_48K},
      {static_cast<enum st30_ptime>(-1), ST30_SAMPLING_48K},
      {ST30_PTIME_MAX, ST30_SAMPLING_96K},
      {ST30_PTIME_MAX, ST31_SAMPLING_44K},
      {ST30_PTIME_1MS, ST30_SAMPLING_MAX},
      {ST30_PTIME_1MS, static_cast<enum st30_sampling>(-1)},
      /* Mismatched 44.1 kHz vs 48/96 kHz ptime and sampling pairs. */
      {ST31_PTIME_1_09MS, ST30_SAMPLING_48K},
      {ST31_PTIME_0_14MS, ST30_SAMPLING_48K},
      {ST31_PTIME_0_09MS, ST30_SAMPLING_96K},
      {ST30_PTIME_1MS, ST31_SAMPLING_44K},
      {ST30_PTIME_125US, ST31_SAMPLING_44K},
      {ST31_PTIME_80US, ST31_SAMPLING_44K},
  };
  for (const auto& c : bad_pairs) {
    EXPECT_EQ(st30_get_sample_num(c.ptime, c.sampling), -EINVAL);
  }

  for (enum st30_sampling s : {ST30_SAMPLING_MAX, static_cast<enum st30_sampling>(-1)}) {
    EXPECT_EQ(st30_get_sample_rate(s), -EINVAL);
  }
  for (enum st30_ptime p : {ST30_PTIME_MAX, static_cast<enum st30_ptime>(-1)}) {
    EXPECT_DOUBLE_EQ(st30_get_packet_time(p), static_cast<double>(-EINVAL));
  }
}

TEST(St30FormatHelpers, CalculateFramebuffSizeValidMatrix) {
  const struct {
    enum st30_fmt fmt;
    enum st30_ptime ptime;
    enum st30_sampling sampling;
    uint16_t channel;
    uint64_t desired_frame_time_ns;
    int expected_size;
    double expected_fps;
  } cases[] = {
      /* 10 ms at 1 ms/48k/PCM24/2ch: 10 pkts * 288 B = 2880 B, 100.0 fps */
      {ST30_FMT_PCM24, ST30_PTIME_1MS, ST30_SAMPLING_48K, 2, 10000000ull, 2880, 100.0},
      /* 10 ms at 125 us/48k/PCM16/4ch: 80 pkts * 48 B = 3840 B, 100.0 fps */
      {ST30_FMT_PCM16, ST30_PTIME_125US, ST30_SAMPLING_48K, 4, 10000000ull, 3840, 100.0},
      /* 10 ms at 1.09 ms/44.1k/AM824/2ch: floor(9.1875) = 9 pkts * 384 B = 3456 B */
      {ST31_FMT_AM824, ST31_PTIME_1_09MS, ST31_SAMPLING_44K, 2, 10000000ull, 3456,
       44100.0 / (48 * 9)},
      /* desired_frame_time_ns < pkt_time clamps to 1 pkt: 500 us at 1 ms -> 192 B */
      {ST30_FMT_PCM16, ST30_PTIME_1MS, ST30_SAMPLING_48K, 2, 500000ull, 192, 1000.0},
      /* exact 1-pkt frame: 4 ms at 4 ms/96k/PCM16/2ch -> 1536 B, 250.0 fps */
      {ST30_FMT_PCM16, ST30_PTIME_4MS, ST30_SAMPLING_96K, 2, 4000000ull, 1536, 250.0},
  };

  for (const auto& c : cases) {
    double fps = 0.0;
    const int size_with_fps = st30_calculate_framebuff_size(
        c.fmt, c.ptime, c.sampling, c.channel, c.desired_frame_time_ns, &fps);
    EXPECT_EQ(size_with_fps, c.expected_size)
        << "fmt " << c.fmt << " ptime " << c.ptime << " sampling " << c.sampling;
    EXPECT_NEAR(fps, c.expected_fps, 1e-6)
        << "fmt " << c.fmt << " ptime " << c.ptime << " sampling " << c.sampling;

    const int size_null_fps = st30_calculate_framebuff_size(
        c.fmt, c.ptime, c.sampling, c.channel, c.desired_frame_time_ns, nullptr);
    EXPECT_EQ(size_null_fps, c.expected_size)
        << "nullptr fps mismatch for fmt " << c.fmt << " ptime " << c.ptime;
  }
}

TEST(StFrameIsLate, BasicComparisons) {
  g_mock_ptp_now_ns = 1700000000ULL * kNsPerS;
  struct st_frame frame = {};
  frame.tfmt = ST10_TIMESTAMP_FMT_TAI;

  frame.timestamp = g_mock_ptp_now_ns - kNsPerS;
  EXPECT_TRUE(st_frame_is_late(nullptr, &frame));
  frame.timestamp = g_mock_ptp_now_ns - 1;
  EXPECT_TRUE(st_frame_is_late(nullptr, &frame));

  frame.timestamp = g_mock_ptp_now_ns;
  EXPECT_FALSE(st_frame_is_late(nullptr, &frame));
  frame.timestamp = g_mock_ptp_now_ns + 10 * kNsPerS;
  EXPECT_FALSE(st_frame_is_late(nullptr, &frame));

  frame.tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
  frame.timestamp = 0;
  EXPECT_FALSE(st_frame_is_late(nullptr, &frame));
}
