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
  if (mt_atomic32_read(&sch->active))
    return true;
  else
    return false;
}

static inline bool mt_sch_started(struct mtl_sch_impl* sch) {
  if (mt_atomic32_read_acquire(&sch->started))
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

int mt_sch_put_lcore(struct mtl_main_impl* impl, unsigned int lcore);
int mt_sch_get_lcore(struct mtl_main_impl* impl, unsigned int* lcore,
                     enum mt_lcore_type type, int socket);
bool mt_sch_lcore_valid(struct mtl_main_impl* impl, unsigned int lcore);

#endif
