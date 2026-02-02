/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_RX_VIDEO_SESSION_HEAD_H_
#define _ST_LIB_RX_VIDEO_SESSION_HEAD_H_

#include "st_main.h"

#define ST_RX_VIDEO_DMA_MIN_SIZE (1024)
/* Number of slots for out of order packet recovery for RTCP retransmission
   cannot be bigger than ST_VIDEO_RX_REC_NUM_OFO*/
#define ST_RX_VIDEO_RTCP_SLOT_NUM 2

/* Number of slots for redundant support */
#define ST_RX_VIDEO_REDUNDANT_SLOT_NUM 2

#define ST_RV_TP_TSC_SYNC_MS (100) /* sync tsc with ptp period(ms) */
#define ST_RV_TP_TSC_SYNC_NS (ST_RV_TP_TSC_SYNC_MS * 1000 * 1000)

#define ST_RX_VIDEO_PREFIX "RV_"

int st_rx_video_sessions_sch_init(struct mtl_main_impl* impl, struct mtl_sch_impl* sch);

int st_rx_video_sessions_sch_uinit(struct mtl_main_impl* impl, struct mtl_sch_impl* sch);

/* call rx_video_session_put always if get successfully */
static inline struct st_rx_video_session_impl* rx_video_session_get(
    struct st_rx_video_sessions_mgr* mgr, int idx) {
  rte_spinlock_lock(&mgr->mutex[idx]);
  struct st_rx_video_session_impl* s = mgr->sessions[idx];
  if (!s) rte_spinlock_unlock(&mgr->mutex[idx]);
  return s;
}

/* call rx_video_session_put always if get successfully */
static inline struct st_rx_video_session_impl* rx_video_session_get_timeout(
    struct st_rx_video_sessions_mgr* mgr, int idx, int timeout_us) {
  if (!mt_spinlock_lock_timeout(mgr->parent, &mgr->mutex[idx], timeout_us)) return NULL;
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

st20_rx_handle st20_rx_create_with_mask(struct mtl_main_impl* impl,
                                        struct st20_rx_ops* ops, mt_sch_mask_t sch_mask);

void rx_video_session_cal_cpu_busy(struct mtl_sch_impl* sch,
                                   struct st_rx_video_session_impl* s);
void rx_video_session_clear_cpu_busy(struct st_rx_video_session_impl* s);

static inline bool rx_video_session_is_cpu_busy(struct st_rx_video_session_impl* s) {
  if (s->dma_dev && (s->dma_busy_score > 90)) return true;
  if (s->imiss_busy_score > 95.0) return true;
  if (s->cpu_busy_score > 95.0) return true;

  return false;
}

static inline float rx_video_session_get_cpu_busy(struct st_rx_video_session_impl* s) {
  return s->cpu_busy_score;
}

static inline bool rx_video_session_can_migrate(struct st_rx_video_session_impl* s) {
  if (s->ops.flags & ST20_RX_FLAG_DISABLE_MIGRATE)
    return false;
  else
    return true;
}

int st_rx_video_session_migrate(struct mtl_main_impl* impl,
                                struct st_rx_video_sessions_mgr* mgr,
                                struct st_rx_video_session_impl* s, int idx);

#if defined(MTL_ENABLE_FUZZING_ST20) || defined(MTL_ENABLE_FUZZING_ST22)
int st_rx_video_session_fuzz_handle_pkt(struct st_rx_video_session_impl* s,
                                        struct rte_mbuf* mbuf,
                                        enum mtl_session_port s_port);
void st_rx_video_session_fuzz_reset(struct st_rx_video_session_impl* s);
#endif

#endif
