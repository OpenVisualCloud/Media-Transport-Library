/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

#ifndef _ST_LIB_TX_VIDEO_SESSION_HEAD_H_
#define _ST_LIB_TX_VIDEO_SESSION_HEAD_H_

#include "st_main.h"

struct st_tx_video_session_impl* st_tx_video_sessions_mgr_attach(
    struct st_tx_video_sessions_mgr* mgr, struct st20_tx_ops* ops,
    enum st_session_type s_type, struct st22_tx_ops* st22_frame_ops);
int st_tx_video_sessions_mgr_detach(struct st_tx_video_sessions_mgr* mgr,
                                    struct st_tx_video_session_impl* s);

int st_tx_video_sessions_mgr_update(struct st_tx_video_sessions_mgr* mgr);

void st_tx_video_sessions_stat(struct st_main_impl* impl);

int st_tx_video_sessions_sch_init(struct st_main_impl* impl, struct st_sch_impl* sch);

int st_tx_video_sessions_sch_uinit(struct st_main_impl* impl, struct st_sch_impl* sch);

/* call tx_video_session_put always if get successfully */
static inline struct st_tx_video_session_impl* tx_video_session_get(
    struct st_tx_video_sessions_mgr* mgr, int idx) {
  rte_spinlock_lock(&mgr->mutex[idx]);
  struct st_tx_video_session_impl* s = mgr->sessions[idx];
  if (!s) rte_spinlock_unlock(&mgr->mutex[idx]);
  return s;
}

/* call tx_video_session_put always if get successfully */
static inline struct st_tx_video_session_impl* tx_video_session_try_get(
    struct st_tx_video_sessions_mgr* mgr, int idx) {
  if (!rte_spinlock_trylock(&mgr->mutex[idx])) return NULL;
  struct st_tx_video_session_impl* s = mgr->sessions[idx];
  if (!s) rte_spinlock_unlock(&mgr->mutex[idx]);
  return s;
}

/* call tx_video_session_put always if get successfully */
static inline bool tx_video_session_get_empty(struct st_tx_video_sessions_mgr* mgr,
                                              int idx) {
  rte_spinlock_lock(&mgr->mutex[idx]);
  struct st_tx_video_session_impl* s = mgr->sessions[idx];
  if (s) {
    rte_spinlock_unlock(&mgr->mutex[idx]); /* not null, unlock it */
    return false;
  } else {
    return true;
  }
}

static inline void tx_video_session_put(struct st_tx_video_sessions_mgr* mgr, int idx) {
  rte_spinlock_unlock(&mgr->mutex[idx]);
}

void tx_video_session_cal_cpu_busy(struct st_tx_video_session_impl* s);
void tx_video_session_clear_cpu_busy(struct st_tx_video_session_impl* s);

static inline bool tx_video_session_is_cpu_busy(struct st_tx_video_session_impl* s) {
  if (s->cpu_busy_score > 95.0) return true;

  return false;
}

static inline float tx_video_session_get_cpu_busy(struct st_tx_video_session_impl* s) {
  return s->cpu_busy_score;
}

int st_tx_video_session_migrate(struct st_main_impl* impl,
                                struct st_tx_video_sessions_mgr* mgr,
                                struct st_tx_video_session_impl* s, int idx);

#endif
