/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_TX_VIDEO_SESSION_HEAD_H_
#define _ST_LIB_TX_VIDEO_SESSION_HEAD_H_

#include "st_main.h"

#define ST_TX_VIDEO_PREFIX "TV_"

#define ST_TX_VIDEO_RTCP_BURST_SIZE (32)
#define ST_TX_VIDEO_RTCP_RING_SIZE (1024)

int st_tx_video_sessions_sch_init(struct mtl_main_impl* impl, struct mtl_sch_impl* sch);

int st_tx_video_sessions_sch_uinit(struct mtl_main_impl* impl, struct mtl_sch_impl* sch);

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
static inline struct st_tx_video_session_impl* tx_video_session_get_timeout(
    struct st_tx_video_sessions_mgr* mgr, int idx, int timeout_us) {
  if (!mt_spinlock_lock_timeout(mgr->parent, &mgr->mutex[idx], timeout_us)) return NULL;
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

void tx_video_session_cal_cpu_busy(struct mtl_sch_impl* sch,
                                   struct st_tx_video_session_impl* s);
void tx_video_session_clear_cpu_busy(struct st_tx_video_session_impl* s);

static inline bool tx_video_session_is_cpu_busy(struct st_tx_video_session_impl* s) {
  if (s->cpu_busy_score > ST_SESSION_MIGRATE_CPU_BUSY_THRESHOLD) return true;

  return false;
}

static inline float tx_video_session_get_cpu_busy(struct st_tx_video_session_impl* s) {
  return s->cpu_busy_score;
}

int st_tx_video_session_migrate(struct st_tx_video_sessions_mgr* mgr,
                                struct st_tx_video_session_impl* s, int idx);

int st20_pacing_static_profiling(struct mtl_main_impl* impl,
                                 struct st_tx_video_session_impl* s,
                                 enum mtl_session_port s_port);

#endif
