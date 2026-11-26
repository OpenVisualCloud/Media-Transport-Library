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
constexpr int64_t kOffset = (int64_t)(kTaiNow - kClockNow);

struct gst_mtl_tai_sync synced() {
  struct gst_mtl_tai_sync sync = {};
  gst_mtl_tai_sync_update(&sync, kBaseTime, kClockNow, kOffset);
  return sync;
}

/* 48 kHz stereo 16 bit: 4 bytes per sample frame, a 1920 byte frame is 10 ms */
struct gst_mtl_audio_framer framer() {
  struct gst_mtl_audio_framer f = {};
  f.frame_size = 1920;
  f.bpf = 4;
  f.rate = 48000;
  return f;
}

constexpr uint64_t kNs1000Bytes = 5208333; /* 250 samples */
constexpr uint64_t kNs920Bytes = 4791666;  /* 230 samples */

}  // namespace

TEST(GstMtlPtsTai, OffsetComesFromTheMidpointOfTheClockReads) {
  int64_t offset = gst_mtl_clock_to_tai(kClockNow, kTaiNow, kClockNow + 2000);

  EXPECT_EQ(offset, (int64_t)(kTaiNow - (kClockNow + 1000)));
}

TEST(GstMtlPtsTai, RunningTimeGetsTheTaiOfThePipelineClock) {
  struct gst_mtl_tai_sync sync = synced();
  struct gst_mtl_tai_sync ahead = {};
  uint64_t clock_ahead = kTaiNow + 37 * kNsPerS;
  gst_mtl_tai_sync_update(&ahead, clock_ahead - kNsPerS, clock_ahead,
                          gst_mtl_clock_to_tai(clock_ahead, kTaiNow, clock_ahead));

  EXPECT_EQ(gst_mtl_buffer_tai(&sync, false, 0, 2 * kNsPerS, 0), kTaiNow);
  EXPECT_EQ(gst_mtl_buffer_tai(&sync, false, 0, 0, 1080), kTaiNow - 2 * kNsPerS + 1080);
  EXPECT_EQ(gst_mtl_buffer_tai(&ahead, false, 0, kNsPerS, 0), kTaiNow);
}

TEST(GstMtlPtsTai, RunningTimeWithNoClockOrOutsideTheSegmentGivesMtlPacing) {
  struct gst_mtl_tai_sync sync = synced();

  EXPECT_EQ(gst_mtl_buffer_tai(NULL, false, kNsPerS, kNsPerS, 1080), 0u);
  EXPECT_EQ(gst_mtl_buffer_tai(&sync, false, kNsPerS, GST_MTL_TIME_NONE, 1080), 0u);
}

TEST(GstMtlPtsTai, PtsIsTaiSendsThePtsWithNoClock) {
  EXPECT_EQ(gst_mtl_buffer_tai(NULL, true, kTaiNow, GST_MTL_TIME_NONE, 1080),
            kTaiNow + 1080);
  EXPECT_EQ(gst_mtl_buffer_tai(NULL, true, GST_MTL_TIME_NONE, GST_MTL_TIME_NONE, 1080),
            0u);
}

TEST(GstMtlPtsTai, OffsetIsMeasuredAgainEachSecond) {
  struct gst_mtl_tai_sync sync = synced();

  EXPECT_FALSE(gst_mtl_tai_sync_due(&sync, kBaseTime, kClockNow + kNsPerS - 1));
  EXPECT_TRUE(gst_mtl_tai_sync_due(&sync, kBaseTime, kClockNow + kNsPerS));
}

TEST(GstMtlPtsTai, NewBaseTimeTakesTheNewMeasureAsFirst) {
  struct gst_mtl_tai_sync sync = synced();
  uint64_t resumed = kClockNow + 5 * kNsPerS;

  EXPECT_TRUE(gst_mtl_tai_sync_due(&sync, resumed, resumed));
  EXPECT_EQ(gst_mtl_tai_sync_update(&sync, resumed, resumed, kOffset + 37 * kNsPerS),
            GST_MTL_TAI_FIRST);
  EXPECT_EQ(sync.clock_to_tai, kOffset + 37 * (int64_t)kNsPerS);
}

TEST(GstMtlPtsTai, SmallChangeKeepsTheOffsetAndTheCadence) {
  struct gst_mtl_tai_sync sync = synced();
  uint64_t later = kClockNow + kNsPerS;

  EXPECT_EQ(gst_mtl_tai_sync_update(&sync, kBaseTime, later, kOffset + 50000),
            GST_MTL_TAI_KEPT);
  EXPECT_EQ(gst_mtl_tai_sync_update(&sync, kBaseTime, later + kNsPerS, kOffset - 50000),
            GST_MTL_TAI_KEPT);
  EXPECT_EQ(sync.clock_to_tai, kOffset);
  EXPECT_FALSE(gst_mtl_tai_sync_due(&sync, kBaseTime, later + kNsPerS + 1));
}

TEST(GstMtlPtsTai, StepOfTheTaiSourceIsApplied) {
  struct gst_mtl_tai_sync sync = synced();
  int64_t stepped = kOffset + 37 * (int64_t)kNsPerS;

  EXPECT_EQ(gst_mtl_tai_sync_update(&sync, kBaseTime, kClockNow + kNsPerS, stepped),
            GST_MTL_TAI_STEP);
  EXPECT_EQ(sync.clock_to_tai, stepped);
}

TEST(GstMtlPtsTai, PtpIsLockedOnlyUnder30Us) {
  EXPECT_TRUE(gst_mtl_ptp_locked(29999));
  EXPECT_TRUE(gst_mtl_ptp_locked(-29999));
  EXPECT_FALSE(gst_mtl_ptp_locked(30000));
  EXPECT_FALSE(gst_mtl_ptp_locked(-5 * (int64_t)kNsPerS));
}

TEST(GstMtlAudioFramer, FrameSpanningTwoBuffersKeepsItsFirstSampleTime) {
  struct gst_mtl_audio_framer f = framer();

  gst_mtl_audio_framer_buffer(&f, kTaiNow);
  EXPECT_EQ(gst_mtl_audio_framer_take(&f, 1000), 1000u);
  gst_mtl_audio_framer_buffer(&f, kTaiNow + kNs1000Bytes);
  EXPECT_EQ(gst_mtl_audio_framer_take(&f, 2000), 920u);

  EXPECT_EQ(f.fill, f.frame_size);
  EXPECT_EQ(f.frame_tai, kTaiNow);
  gst_mtl_audio_framer_reset(&f);
  gst_mtl_audio_framer_take(&f, 1080);
  EXPECT_EQ(f.frame_tai, kTaiNow + kNs1000Bytes + kNs920Bytes);
}

TEST(GstMtlAudioFramer, FrameStartingInTheSecondMemoryCountsTheFirst) {
  struct gst_mtl_audio_framer f = framer();

  gst_mtl_audio_framer_buffer(&f, kTaiNow);
  gst_mtl_audio_framer_take(&f, 1000);
  EXPECT_EQ(gst_mtl_audio_framer_take(&f, 1000), 920u);
  gst_mtl_audio_framer_reset(&f);
  gst_mtl_audio_framer_take(&f, 80);

  /* the 80 byte frame that EOS sends */
  EXPECT_EQ(f.fill, 80u);
  EXPECT_EQ(f.frame_tai, kTaiNow + 10000000);
}

TEST(GstMtlAudioFramer, PartFrameTakesItsTimeFromTheBufferAfterAPause) {
  struct gst_mtl_audio_framer f = framer();
  uint64_t resumed = kTaiNow + 5 * kNsPerS + kNs1000Bytes;

  gst_mtl_audio_framer_buffer(&f, kTaiNow);
  gst_mtl_audio_framer_take(&f, 1000);
  gst_mtl_audio_framer_buffer(&f, resumed);

  EXPECT_EQ(f.frame_tai, resumed - kNs1000Bytes);
}

TEST(GstMtlAudioFramer, FlushDropsThePartFrame) {
  struct gst_mtl_audio_framer f = framer();
  uint64_t after_seek = kTaiNow + 60 * kNsPerS;

  gst_mtl_audio_framer_buffer(&f, kTaiNow);
  gst_mtl_audio_framer_take(&f, 1000);
  gst_mtl_audio_framer_reset(&f);
  gst_mtl_audio_framer_buffer(&f, after_seek);

  EXPECT_EQ(gst_mtl_audio_framer_take(&f, 1920), 1920u);
  EXPECT_EQ(f.frame_tai, after_seek);
}

TEST(GstMtlAudioFramer, BufferWithNoTaiGivesMtlPacing) {
  struct gst_mtl_audio_framer f = framer();

  gst_mtl_audio_framer_buffer(&f, kTaiNow);
  gst_mtl_audio_framer_take(&f, 1000);
  gst_mtl_audio_framer_buffer(&f, 0);

  EXPECT_EQ(f.frame_tai, 0u);
}
