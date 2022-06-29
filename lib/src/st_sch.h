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

int st_sch_mrg_init(struct st_main_impl* impl, int data_quota_mbs_limit);

int st_sch_register_tasklet(struct st_sch_impl* sch,
                            struct st_sch_tasklet_ops* tasklet_ops);

int st_sch_add_quota(struct st_sch_impl* sch, int quota_mbs);

struct st_sch_impl* st_sch_get(struct st_main_impl* impl, int quota_mbs,
                               enum st_sch_type type);
int st_sch_put(struct st_sch_impl* sch, int quota_mbs);

int st_sch_start_all(struct st_main_impl* impl);
int st_sch_stop_all(struct st_main_impl* impl);

static inline void st_sch_set_cpu_busy(struct st_sch_impl* sch, bool busy) {
  sch->cpu_busy = busy;
}

#endif
