/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>
#include <stdint.h>

#include "../../../ecosystem/gstreamer_plugin/gst_mtl_pts_tai.h"

namespace {

constexpr uint64_t kNsPerS = 1000000000ULL;
constexpr uint64_t kTaiNow = 1795636655000000000ULL;
/* a monotonic pipeline clock one hour after boot, PLAYING for two seconds */
constexpr uint64_t kClockNow = 3600 * kNsPerS;
constexpr uint64_t kBaseTime = kClockNow - 2 * kNsPerS;
constexpr uint64_t kFrameNsP59 = 16683333;

int64_t clock_to_tai_now() {
  return gst_mtl_clock_to_tai(kClockNow, kTaiNow, kClockNow);
}

}  // namespace

TEST(GstMtlPtsTai, BufferDueNowGetsTaiNow) {
  uint64_t tai = gst_mtl_running_time_to_tai(kClockNow - kBaseTime, kBaseTime,
                                             clock_to_tai_now(), 0);

  EXPECT_EQ(tai, kTaiNow);
}

TEST(GstMtlPtsTai, FirstBufferIsNotDecadesEarly) {
  uint64_t tai = gst_mtl_running_time_to_tai(0, kBaseTime, clock_to_tai_now(), 1080);

  EXPECT_EQ(tai, kTaiNow - 2 * kNsPerS + 1080);
}

TEST(GstMtlPtsTai, OffsetComesFromTheMidpointOfTheClockReads) {
  int64_t offset = gst_mtl_clock_to_tai(kClockNow, kTaiNow, kClockNow + 2000);

  EXPECT_EQ(offset, (int64_t)(kTaiNow - (kClockNow + 1000)));
}

TEST(GstMtlPtsTai, PacingOffsetDelaysTheStamp) {
  uint64_t on_time =
      gst_mtl_running_time_to_tai(kNsPerS, kBaseTime, clock_to_tai_now(), 0);
  uint64_t delayed =
      gst_mtl_running_time_to_tai(kNsPerS, kBaseTime, clock_to_tai_now(), 33366666);

  EXPECT_EQ(delayed - on_time, 33366666u);
}

TEST(GstMtlPtsTai, StampsKeepThePtsCadence) {
  int64_t offset = clock_to_tai_now();
  uint64_t tai0 = gst_mtl_running_time_to_tai(kNsPerS, kBaseTime, offset, 1080);
  uint64_t tai1 =
      gst_mtl_running_time_to_tai(kNsPerS + kFrameNsP59, kBaseTime, offset, 1080);

  EXPECT_EQ(tai1 - tai0, kFrameNsP59);
}

TEST(GstMtlPtsTai, PipelineClockAheadOfTaiConverts) {
  uint64_t clock_now = kTaiNow + 37 * kNsPerS;
  uint64_t base_time = clock_now - kNsPerS;
  int64_t offset = gst_mtl_clock_to_tai(clock_now, kTaiNow, clock_now);

  EXPECT_LT(offset, 0);
  EXPECT_EQ(gst_mtl_running_time_to_tai(kNsPerS, base_time, offset, 0), kTaiNow);
}

TEST(GstMtlPtsTai, AudioFrameStartsAtItsFirstSample) {
  /* 48 kHz, stereo, 16 bit: 4 bytes per sample frame */
  EXPECT_EQ(gst_mtl_audio_bytes_to_ns(1920, 4, 48000), 10000000u);
  EXPECT_EQ(gst_mtl_audio_bytes_to_ns(1764, 4, 44100), 10000000u);
  EXPECT_EQ(gst_mtl_audio_bytes_to_ns(4, 4, 44100), 22675u);
}
