/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_RX_VIDEO_SESSION_HEAD_H_
#define _ST_LIB_RX_VIDEO_SESSION_HEAD_H_

#include "st_main.h"

#define ST_RX_VIDEO_BURTS_SIZE (128)

#define ST_RX_VIDEO_DMA_MIN_SIZE (1024)

#define ST_RV_EBU_TSC_SYNC_MS (100) /* sync tsc with ptp period(ms) */
#define ST_RV_EBU_TSC_SYNC_NS (ST_RV_EBU_TSC_SYNC_MS * 1000 * 1000)

int st_rx_video_sessions_sch_init(struct st_main_impl* impl, struct st_sch_impl* sch);

int st_rx_video_sessions_sch_uinit(struct st_main_impl* impl, struct st_sch_impl* sch);

void st_rx_video_sessions_stat(struct st_main_impl* impl);

/* call rx_video_session_put always if get successfully */
static inline struct st_rx_video_session_impl* rx_video_session_get(
    struct st_rx_video_sessions_mgr* mgr, int idx) {
  rte_spinlock_lock(&mgr->mutex[idx]);
  struct st_rx_video_session_impl* s = mgr->sessions[idx];
  if (!s) rte_spinlock_unlock(&mgr->mutex[idx]);
  return s;
}

/* call rx_video_session_put always if get successfully */
static inline struct st_rx_video_session_impl* rx_video_session_try_get(
    struct st_rx_video_sessions_mgr* mgr, int idx) {
  if (!rte_spinlock_trylock(&mgr->mutex[idx])) return NULL;
  struct st_rx_video_session_impl* s = mgr->sessions[idx];
  if (!s) rte_spinlock_unlock(&mgr->mutex[idx]);
  return s;
}

/* call rx_video_session_put always if get successfully */
static inline bool rx_video_session_get_empty(struct st_rx_video_sessions_mgr* mgr,
                                              int idx) {
  rte_spinlock_lock(&mgr->mutex[idx]);
  struct st_rx_video_session_impl* s = mgr->sessions[idx];
  if (s) {
    rte_spinlock_unlock(&mgr->mutex[idx]); /* not null, unlock it */
    return false;
  } else {
    return true;
  }
}

static inline void rx_video_session_put(struct st_rx_video_sessions_mgr* mgr, int idx) {
  rte_spinlock_unlock(&mgr->mutex[idx]);
}

void rx_video_session_cal_cpu_busy(struct st_rx_video_session_impl* s);
void rx_video_session_clear_cpu_busy(struct st_rx_video_session_impl* s);

static inline bool rx_video_session_is_cpu_busy(struct st_rx_video_session_impl* s) {
  if (s->dma_dev && (s->dma_busy_score > 90)) return true;

  if (s->cpu_busy_score > 95.0) return true;

  return false;
}

static inline float rx_video_session_get_cpu_busy(struct st_rx_video_session_impl* s) {
  return s->cpu_busy_score;
}

int st_rx_video_session_migrate(struct st_main_impl* impl,
                                struct st_rx_video_sessions_mgr* mgr,
                                struct st_rx_video_session_impl* s, int idx);

#endif
