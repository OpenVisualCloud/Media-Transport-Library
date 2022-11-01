/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_SCH_HEAD_H_
#define _ST_LIB_SCH_HEAD_H_

#include "st_main.h"

static inline struct st_sch_mgr* st_sch_get_mgr(struct st_main_impl* impl) {
  return &impl->sch_mgr;
}

static inline struct st_sch_impl* st_sch_instance(struct st_main_impl* impl, int i) {
  return &st_sch_get_mgr(impl)->sch[i];
}

static inline bool st_sch_is_active(struct st_sch_impl* sch) {
  if (rte_atomic32_read(&sch->active))
    return true;
  else
    return false;
}

static inline bool st_sch_started(struct st_sch_impl* sch) {
  if (rte_atomic32_read(&sch->started))
    return true;
  else
    return false;
}

static inline void st_sch_enable_allow_sleep(struct st_sch_impl* sch, bool enable) {
  sch->allow_sleep = enable;
}

static inline bool st_sch_has_busy(struct st_sch_impl* sch) {
  if (!sch->allow_sleep || sch->sleep_ratio_score > 70.0)
    return true;
  else
    return false;
}

int st_sch_mrg_init(struct st_main_impl* impl, int data_quota_mbs_limit);
int st_sch_mrg_uinit(struct st_main_impl* impl);

struct st_sch_tasklet_impl* st_sch_register_tasklet(
    struct st_sch_impl* sch, struct st_sch_tasklet_ops* tasklet_ops);
int st_sch_unregister_tasklet(struct st_sch_tasklet_impl* tasklet);

static inline void st_tasklet_set_sleep(struct st_sch_tasklet_impl* tasklet,
                                        uint64_t advice_sleep_us) {
  tasklet->ops.advice_sleep_us = advice_sleep_us;
}

int st_sch_add_quota(struct st_sch_impl* sch, int quota_mbs);

struct st_sch_impl* st_sch_get(struct st_main_impl* impl, int quota_mbs,
                               enum st_sch_type type, st_sch_mask_t mask);
int st_sch_put(struct st_sch_impl* sch, int quota_mbs);

int st_sch_start_all(struct st_main_impl* impl);
int st_sch_stop_all(struct st_main_impl* impl);

void st_sch_stat(struct st_main_impl* impl);

static inline void st_sch_set_cpu_busy(struct st_sch_impl* sch, bool busy) {
  sch->cpu_busy = busy;
}

#endif
