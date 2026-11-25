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

/**
 * TAI of frame frame_num of one session: (E0 + frame_num) * frame_time + offset.
 * *epoch is the per-session E0; 0 before the first frame, which sets it from now_tai
 * two epochs ahead as legacy/tx_video_app.c does. now_tai is read only then.
 * Long double keeps the product exact to 0.125 ns at a 2026 TAI; the double
 * frame_time gives a constant grid error below tai * 2^-52 (~0.4 us), with no drift.
 */
static inline uint64_t st_app_user_time_calc(const struct st_user_time* user_time,
                                             uint64_t* epoch, uint64_t now_tai,
                                             uint64_t frame_num, double frame_time) {
  if (!*epoch) *epoch = (uint64_t)(now_tai / (long double)frame_time) + 2;
  uint64_t tai = (uint64_t)((long double)(*epoch + frame_num) * frame_time);
  return (uint64_t)((int64_t)tai + user_time->user_time_offset);
}

#endif
