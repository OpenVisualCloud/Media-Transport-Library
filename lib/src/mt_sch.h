/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_SCH_HEAD_H_
#define _MT_LIB_SCH_HEAD_H_

#include "mt_main.h"

static inline struct mt_sch_mgr* mt_sch_get_mgr(struct mtl_main_impl* impl) {
  return &impl->sch_mgr;
}

static inline struct mtl_sch_impl* mt_sch_instance(struct mtl_main_impl* impl, int i) {
  return &mt_sch_get_mgr(impl)->sch[i];
}

static inline bool mt_sch_is_active(struct mtl_sch_impl* sch) {
  if (rte_atomic32_read(&sch->active))
    return true;
  else
    return false;
}

static inline bool mt_sch_started(struct mtl_sch_impl* sch) {
  if (rte_atomic32_read(&sch->started))
    return true;
  else
    return false;
}

static inline int mt_sch_socket_id(struct mtl_sch_impl* sch) {
  return sch->socket_id;
}

static inline void mt_sch_enable_allow_sleep(struct mtl_sch_impl* sch, bool enable) {
  sch->allow_sleep = enable;
}

static inline bool mt_sch_has_busy(struct mtl_sch_impl* sch) {
  if (!sch->allow_sleep || sch->sleep_ratio_score > 70.0)
    return true;
  else
    return false;
}

int mt_sch_mrg_init(struct mtl_main_impl* impl, int data_quota_mbs_limit);
int mt_sch_mrg_uinit(struct mtl_main_impl* impl);

static inline void mt_tasklet_set_sleep(struct mt_sch_tasklet_impl* tasklet,
                                        uint64_t advice_sleep_us) {
  tasklet->ops.advice_sleep_us = advice_sleep_us;
}

int mt_sch_add_quota(struct mtl_sch_impl* sch, int quota_mbs);

struct mtl_sch_impl* mt_sch_get_by_socket(struct mtl_main_impl* impl, int quota_mbs,
                                          enum mt_sch_type type, mt_sch_mask_t mask,
                                          int socket);
static inline struct mtl_sch_impl* mt_sch_get(struct mtl_main_impl* impl, int quota_mbs,
                                              enum mt_sch_type type, mt_sch_mask_t mask) {
  /* use the default socket of MTL_PORT_P */
  return mt_sch_get_by_socket(impl, quota_mbs, type, mask,
                              mt_socket_id(impl, MTL_PORT_P));
}
int mt_sch_put(struct mtl_sch_impl* sch, int quota_mbs);

int mt_sch_start_all(struct mtl_main_impl* impl);
int mt_sch_stop_all(struct mtl_main_impl* impl);

static inline void mt_sch_set_cpu_busy(struct mtl_sch_impl* sch, bool busy) {
  sch->cpu_busy = busy;
}

static inline uint64_t mt_sch_avg_ns_loop(struct mtl_sch_impl* sch) {
  return sch->avg_ns_per_loop;
}

/* Tasklets only: into slot idx before reading its lock, leaving the last slot. */
static inline void mt_sch_dp_slot_enter(struct mt_sch_dp_slot* dp, int idx) {
  atomic_store_explicit(&dp->idx, idx, memory_order_release);
  if (dp->fence)
    atomic_thread_fence(memory_order_seq_cst);
  else /* the membarrier(2) in mt_sch_quiescent_wait() orders the store before the load */
    atomic_signal_fence(memory_order_seq_cst);
}

static inline void mt_sch_dp_slot_leave(struct mt_sch_dp_slot* dp) {
  atomic_store_explicit(&dp->idx, -1, memory_order_release);
}

void mt_sch_dp_slot_init(struct mt_sch_dp_slot* dp);

/* 0 once no pass of sch begun before the call still runs, or, if that pass is long and dp
 * is set, once its tasklets are out of slot idx of dp; at once if sch is offline or the
 * caller is its thread. -ETIMEDOUT after timeout_us unless that is 0. */
int mt_sch_quiescent_wait(struct mtl_sch_impl* sch, const struct mt_sch_dp_slot* dp,
                          int idx, int timeout_us);

/* Whether the caller is sch's own thread, e.g. in a callback of one of its sessions. */
static inline bool mt_sch_on_own_thread(struct mtl_sch_impl* sch) {
  return rte_gettid() == sch->t_pid;
}

int mt_sch_put_lcore(struct mtl_main_impl* impl, unsigned int lcore);
int mt_sch_get_lcore(struct mtl_main_impl* impl, unsigned int* lcore,
                     enum mt_lcore_type type, int socket);
bool mt_sch_lcore_valid(struct mtl_main_impl* impl, unsigned int lcore);

#endif
