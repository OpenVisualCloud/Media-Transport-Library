/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef _ST_APP_USER_TIME_HEAD_H_
#define _ST_APP_USER_TIME_HEAD_H_

#include <stdint.h>

struct st_user_time {
  /* signed: a negative offset starts the pacing clock in the past */
  int64_t user_time_offset;
};

struct st_user_time_state {
  uint64_t epoch;
  uint64_t resync_cnt;
};

/**
 * TAI of frame frame_num of one session: (E0 + frame_num) * frame_time + offset.
 * state->epoch is the per-session E0, 0 before the first frame. The first frame, and
 * a frame whose time without the offset is before now_tai or whose stamped time is
 * more than 1 s - frame_time after it, move E0 so the frame lands two epochs after
 * now_tai, as legacy/tx_video_app.c does; each move after the first counts a resync.
 * The offset stays out of the late check, so a negative offset stamps past times.
 * Long double keeps the product exact to 0.125 ns at a 2026 TAI; the double
 * frame_time gives a constant grid error below tai * 2^-52 (~0.4 us), with no drift.
 */
static inline uint64_t st_app_user_time_calc(const struct st_user_time* user_time,
                                             struct st_user_time_state* state,
                                             uint64_t now_tai, uint64_t frame_num,
                                             double frame_time) {
  long double ft = frame_time;
  uint64_t base = (uint64_t)((state->epoch + frame_num) * ft);
  uint64_t tai = (uint64_t)((int64_t)base + user_time->user_time_offset);

  if (!state->epoch || base < now_tai || (long double)tai - now_tai > 1e9L - ft) {
    if (state->epoch) state->resync_cnt++;
    state->epoch = (uint64_t)(now_tai / ft) + 2 - frame_num;
    base = (uint64_t)((state->epoch + frame_num) * ft);
    tai = (uint64_t)((int64_t)base + user_time->user_time_offset);
  }
  return tai;
}

#endif
