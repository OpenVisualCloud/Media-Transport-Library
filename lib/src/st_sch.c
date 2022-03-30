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

#include "st_sch.h"

#include "st_log.h"

static inline void sch_lock(struct st_sch_impl* sch) { pthread_mutex_lock(&sch->mutex); }

static inline void sch_unlock(struct st_sch_impl* sch) {
  pthread_mutex_unlock(&sch->mutex);
}

static int sch_lcore_func(void* args) {
  struct st_sch_impl* sch = args;
  int idx = sch->idx;
  int num_tasklet, i;
  struct st_sch_tasklet_ops* ops;

  num_tasklet = sch->num_tasklet;
  info("%s(%d), start with %d tasklets\n", __func__, idx, num_tasklet);

  for (i = 0; i < num_tasklet; i++) {
    ops = &sch->tasklet[i].ops;
    if (ops->pre_start) ops->pre_start(ops->priv);
  }

  for (i = 0; i < num_tasklet; i++) {
    ops = &sch->tasklet[i].ops;
    if (ops->start) ops->start(ops->priv);
  }

  while (rte_atomic32_read(&sch->request_stop) == 0) {
    num_tasklet = sch->num_tasklet;
    for (i = 0; i < num_tasklet; i++) {
      ops = &sch->tasklet[i].ops;
      ops->handler(ops->priv);
    }
  }

  for (i = 0; i < num_tasklet; i++) {
    ops = &sch->tasklet[i].ops;
    if (ops->stop) ops->stop(ops->priv);
  }

  rte_atomic32_set(&sch->stopped, 1);
  info("%s(%d), end with %d tasklets\n", __func__, idx, num_tasklet);
  return 0;
}

int st_sch_start(struct st_sch_impl* sch, unsigned int lcore) {
  int idx = sch->idx;
  int ret;

  sch_lock(sch);

  if (rte_atomic32_read(&sch->started)) {
    err("%s(%d), started already\n", __func__, idx);
    sch_unlock(sch);
    return -EIO;
  }

  rte_atomic32_set(&sch->request_stop, 0);
  rte_atomic32_set(&sch->stopped, 0);

  ret = rte_eal_remote_launch(sch_lcore_func, sch, lcore);
  if (ret < 0) {
    err("%s(%d), fail %d on lcore %d\n", __func__, idx, ret, lcore);
    sch_unlock(sch);
    return ret;
  }

  sch->lcore = lcore;
  rte_atomic32_set(&sch->started, 1);
  info("%s(%d), succ on lcore %d\n", __func__, idx, lcore);
  sch_unlock(sch);
  return 0;
}

int st_sch_stop(struct st_sch_impl* sch) {
  int idx = sch->idx;

  sch_lock(sch);

  if (!rte_atomic32_read(&sch->started)) {
    info("%s(%d), not started\n", __func__, idx);
    sch_unlock(sch);
    return 0;
  }

  rte_atomic32_set(&sch->request_stop, 1);
  while (rte_atomic32_read(&sch->stopped) == 0) {
    st_sleep_ms(10);
  }
  rte_atomic32_set(&sch->started, 0);

  info("%s(%d), succ\n", __func__, idx);
  sch_unlock(sch);
  return 0;
}

/* todo: support unregister */
int st_sch_register_tasklet(struct st_sch_impl* sch,
                            struct st_sch_tasklet_ops* tasklet_ops) {
  int idx = sch->idx;
  int cur_tasklet_idx = sch->num_tasklet;
  struct st_sch_tasklet_impl* sch_tasklet;

  sch_lock(sch);

  if (cur_tasklet_idx >= ST_MAX_TASKLET_PER_SCH) {
    err("%s(%d), no space as sch is full(%d)\n", __func__, idx, cur_tasklet_idx);
    sch_unlock(sch);
    return -ENOMEM;
  }

  sch_tasklet = &sch->tasklet[cur_tasklet_idx];
  sch_tasklet->ops = *tasklet_ops;
  strncpy(sch_tasklet->name, tasklet_ops->name, ST_MAX_NAME_LEN);
  sch->num_tasklet++;

  info("%s(%d), tasklet %s registered into slot %d\n", __func__, sch->idx,
       sch_tasklet->name, cur_tasklet_idx);
  sch_unlock(sch);
  return 0;
}

int st_sch_init(struct st_main_impl* impl, int data_quota_mbs_limit) {
  struct st_sch_impl* sch;

  for (int sch_idx = 0; sch_idx < ST_MAX_SCH_NUM; sch_idx++) {
    sch = st_get_sch(impl, sch_idx);
    pthread_mutex_init(&sch->mutex, NULL);
    sch->parnet = impl;
    sch->idx = sch_idx;
    rte_atomic32_set(&sch->started, 0);
    rte_atomic32_set(&sch->ref_cnt, 0);
    rte_atomic32_set(&sch->active, 0);
    sch->num_tasklet = 0;
    sch->data_quota_mbs_total = 0;
    sch->data_quota_mbs_limit = data_quota_mbs_limit;
    /* init mgr lock for video */
    pthread_mutex_init(&sch->tx_video_mgr_mutex, NULL);
    pthread_mutex_init(&sch->rx_video_mgr_mutex, NULL);
  }

  info("%s, succ with data quota %d M\n", __func__, data_quota_mbs_limit);
  return 0;
}

struct st_sch_impl* st_sch_request(struct st_main_impl* impl, enum st_sch_type type) {
  struct st_sch_impl* sch;

  for (int sch_idx = 0; sch_idx < ST_MAX_SCH_NUM; sch_idx++) {
    sch = st_get_sch(impl, sch_idx);

    sch_lock(sch);
    if (!st_sch_is_active(sch)) { /* find one free sch */
      sch->type = type;
      rte_atomic32_inc(&sch->active);
      rte_atomic32_inc(&impl->sch_cnt);
      sch_unlock(sch);
      return sch;
    }
    sch_unlock(sch);
  }

  err("%s, fail as no free sch\n", __func__);
  return NULL;
}

int st_sch_free(struct st_sch_impl* sch) {
  if (!st_sch_is_active(sch)) {
    err("%s, sch %d is not allocated\n", __func__, sch->idx);
    return -EIO;
  }

  sch_lock(sch);
  /* todo: free all registered tasklet */
  rte_atomic32_dec(&sch->parnet->sch_cnt);
  rte_atomic32_dec(&sch->active);
  sch_unlock(sch);
  return 0;
}

int st_sch_add_quota(struct st_sch_impl* sch, int quota_mbs) {
  int idx = sch->idx;

  if (!st_sch_is_active(sch)) {
    dbg("%s(%d), sch is not allocated\n", __func__, idx);
    return -ENOMEM;
  }

  sch_lock(sch);
  /* either the first quota request or sch is capable the quota */
  if (!sch->data_quota_mbs_total ||
      ((sch->data_quota_mbs_total + quota_mbs) <= sch->data_quota_mbs_limit)) {
    /* find one sch capable with quota */
    sch->data_quota_mbs_total += quota_mbs;
    info("%s(%d:%d), quota %d total now %d\n", __func__, idx, sch->type, quota_mbs,
         sch->data_quota_mbs_total);
    sch_unlock(sch);
    return 0;
  }
  sch_unlock(sch);

  return -ENOMEM;
}

int st_sch_free_quota(struct st_sch_impl* sch, int quota_mbs) {
  int idx = sch->idx;

  if (!st_sch_is_active(sch)) {
    err("%s(%d), sch is not allocated\n", __func__, idx);
    return -ENOMEM;
  }

  sch_lock(sch);
  sch->data_quota_mbs_total -= quota_mbs;
  sch_unlock(sch);
  info("%s(%d), quota %d total now %d\n", __func__, idx, quota_mbs,
       sch->data_quota_mbs_total);
  return 0;
}
