/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>
#include <math.h>
#include <stdint.h>

#include "../src/app_user_time.h"

namespace {

constexpr uint64_t kNow = 1795000000123456789ULL;
/* the frame times as the RxTxApp TX threads compute them: NS_PER_S / expect_fps */
const double kFrameTimeP59 = 1e9 / (60000.0 / 1001);
const double kFrameTimeP50 = 1e9 / 50.0;
const double kFrameTimeAudio1ms = 1e9 / 1000.0;

/* frame_time is a double, so its error times ~1e11 epochs can reach 0.4 us */
constexpr long double kGridToleranceNs = 1000;

/* frame n of a session that started at kNow, built half a frame before its time */
uint64_t on_time_now(uint64_t n) {
  return kNow + (uint64_t)((n + 0.5) * kFrameTimeP59);
}

/* ns from tai to the nearest multiple of the frame period den / mul s */
long double grid_error_ns(uint64_t tai, uint32_t mul, uint32_t den) {
  __int128 period_num = (__int128)1000000000 * den;
  __int128 epoch = ((__int128)2 * tai * mul + period_num) / (2 * period_num);
  return (long double)((__int128)tai * mul - epoch * period_num) / mul;
}

}  // namespace

TEST(RxTxAppUserTime, SessionStaysOnItsGridWhenAnotherStartsLater) {
  struct st_user_time user_time = {};
  struct st_user_time_state video = {}, audio = {};

  uint64_t video0 = st_app_user_time_calc(&user_time, &video, kNow, 0, kFrameTimeP59);
  uint64_t audio0 =
      st_app_user_time_calc(&user_time, &audio, kNow + 5300000, 0, kFrameTimeAudio1ms);
  uint64_t video1 =
      st_app_user_time_calc(&user_time, &video, kNow + 16000000, 1, kFrameTimeP59);

  EXPECT_LT(fabsl(grid_error_ns(video0, 60000, 1001)), kGridToleranceNs);
  EXPECT_LT(fabsl(grid_error_ns(audio0, 1000, 1)), kGridToleranceNs);
  EXPECT_LT(fabsl(grid_error_ns(video1, 60000, 1001)), kGridToleranceNs);
}

TEST(RxTxAppUserTime, LaterSessionDoesNotMoveEarlierOne) {
  struct st_user_time user_time = {};
  struct st_user_time_state video = {}, other = {};
  uint64_t elapsed = (uint64_t)(100 * kFrameTimeP59);

  uint64_t video0 = st_app_user_time_calc(&user_time, &video, kNow, 0, kFrameTimeP59);
  st_app_user_time_calc(&user_time, &other, kNow + elapsed, 0, kFrameTimeP50);
  uint64_t video100 =
      st_app_user_time_calc(&user_time, &video, kNow + elapsed, 100, kFrameTimeP59);

  EXPECT_NEAR((double)(video100 - video0), 100 * kFrameTimeP59, 1.0);
}

TEST(RxTxAppUserTime, FirstFrameLeadsNowByOneToTwoFrameTimes) {
  struct st_user_time user_time = {};
  struct st_user_time_state session = {};

  uint64_t tai0 = st_app_user_time_calc(&user_time, &session, kNow, 0, kFrameTimeP59);

  EXPECT_GE((double)(tai0 - kNow), kFrameTimeP59 - 1);
  EXPECT_LE((double)(tai0 - kNow), 2 * kFrameTimeP59);
}

TEST(RxTxAppUserTime, NegativeOffsetPutsFramesInThePast) {
  struct st_user_time on_time = {}, late = {};
  struct st_user_time_state on_time_session = {}, late_session = {};
  late.user_time_offset = -2000000000LL;

  uint64_t on_time0 =
      st_app_user_time_calc(&on_time, &on_time_session, kNow, 0, kFrameTimeP59);
  uint64_t late0 = st_app_user_time_calc(&late, &late_session, kNow, 0, kFrameTimeP59);

  EXPECT_EQ(late0, on_time0 - 2000000000ULL);
  EXPECT_LT(late0, kNow);
}

TEST(RxTxAppUserTime, PositiveOffsetDelaysFrames) {
  struct st_user_time on_time = {}, delayed = {};
  struct st_user_time_state on_time_session = {}, delayed_session = {};
  delayed.user_time_offset = 10000000;

  uint64_t on_time0 =
      st_app_user_time_calc(&on_time, &on_time_session, kNow, 0, kFrameTimeP59);
  uint64_t delayed0 =
      st_app_user_time_calc(&delayed, &delayed_session, kNow, 0, kFrameTimeP59);

  EXPECT_EQ(delayed0, on_time0 + 10000000ULL);
}

TEST(RxTxAppUserTime, FrameTimesDoNotDriftOverTenHours) {
  struct st_user_time user_time = {};
  struct st_user_time_state session = {};
  uint64_t frames = 10ULL * 3600 * 60000 / 1001;
  uint64_t exact_ns = (uint64_t)((__int128)frames * 1001 * 1000000000 / 60000);

  uint64_t tai0 = st_app_user_time_calc(&user_time, &session, kNow, 0, kFrameTimeP59);
  uint64_t tai_n =
      st_app_user_time_calc(&user_time, &session, kNow + exact_ns, frames, kFrameTimeP59);

  EXPECT_NEAR((double)(tai_n - tai0), (double)exact_ns, 2.0);
  EXPECT_LT(fabsl(grid_error_ns(tai_n, 60000, 1001)), kGridToleranceNs);
}

TEST(RxTxAppUserTime, LateSessionResyncsOntoGridAfterStartLag) {
  struct st_user_time user_time = {};
  struct st_user_time_state session = {};
  uint64_t start = kNow + 5000000000ULL;

  for (uint64_t n = 0; n < 3; n++)
    st_app_user_time_calc(&user_time, &session, kNow, n, kFrameTimeP59);
  uint64_t tai3 = st_app_user_time_calc(&user_time, &session, start, 3, kFrameTimeP59);

  EXPECT_GE((double)(tai3 - start), kFrameTimeP59 - 1);
  EXPECT_LE((double)(tai3 - start), 2 * kFrameTimeP59);
  EXPECT_LT(fabsl(grid_error_ns(tai3, 60000, 1001)), kGridToleranceNs);
  EXPECT_EQ(session.resync_cnt, 1u);
}

TEST(RxTxAppUserTime, ResyncedSessionKeepsItsNewEpoch) {
  struct st_user_time user_time = {};
  struct st_user_time_state session = {};
  uint64_t start = kNow + 5000000000ULL;

  st_app_user_time_calc(&user_time, &session, kNow, 0, kFrameTimeP59);
  uint64_t tai1 = st_app_user_time_calc(&user_time, &session, start, 1, kFrameTimeP59);
  uint64_t tai2 = st_app_user_time_calc(
      &user_time, &session, start + (uint64_t)kFrameTimeP59, 2, kFrameTimeP59);

  EXPECT_NEAR((double)(tai2 - tai1), kFrameTimeP59, 1.0);
  EXPECT_EQ(session.resync_cnt, 1u);
}

TEST(RxTxAppUserTime, OnTimeSessionNeverResyncs) {
  struct st_user_time user_time = {};
  struct st_user_time_state session = {};

  for (uint64_t n = 0; n < 36000; n++)
    st_app_user_time_calc(&user_time, &session, on_time_now(n), n, kFrameTimeP59);

  EXPECT_EQ(session.resync_cnt, 0u);
}

TEST(RxTxAppUserTime, NegativeOffsetDoesNotResync) {
  struct st_user_time user_time = {};
  struct st_user_time_state session = {};
  user_time.user_time_offset = -2000000000LL;
  bool all_late = true;

  for (uint64_t n = 0; n < 120; n++) {
    uint64_t now = on_time_now(n);
    if (st_app_user_time_calc(&user_time, &session, now, n, kFrameTimeP59) >= now)
      all_late = false;
  }

  EXPECT_TRUE(all_late);
  EXPECT_EQ(session.resync_cnt, 0u);
}

/* drop-when-late frees each late frame at once, so the base runs ahead of now */
TEST(RxTxAppUserTime, NegativeOffsetCatchUpDoesNotResync) {
  struct st_user_time user_time = {};
  struct st_user_time_state session = {};
  user_time.user_time_offset = -2000000000LL;
  uint64_t frames = 90;

  uint64_t tai0 = st_app_user_time_calc(&user_time, &session, kNow, 0, kFrameTimeP59);
  uint64_t tai_n = 0;
  for (uint64_t n = 1; n <= frames; n++)
    tai_n =
        st_app_user_time_calc(&user_time, &session, kNow + 10000000, n, kFrameTimeP59);

  EXPECT_NEAR((double)(tai_n - tai0), frames * kFrameTimeP59, 1.0);
  EXPECT_EQ(session.resync_cnt, 0u);
}

TEST(RxTxAppUserTime, PtpStepBackResyncs) {
  struct st_user_time user_time = {};
  struct st_user_time_state session = {};
  uint64_t stepped = kNow - 2000000000ULL;

  st_app_user_time_calc(&user_time, &session, kNow, 0, kFrameTimeP59);
  uint64_t tai1 = st_app_user_time_calc(&user_time, &session, stepped, 1, kFrameTimeP59);

  EXPECT_GE((double)(tai1 - stepped), kFrameTimeP59 - 1);
  EXPECT_LE((double)(tai1 - stepped), 2 * kFrameTimeP59);
  EXPECT_EQ(session.resync_cnt, 1u);
}
