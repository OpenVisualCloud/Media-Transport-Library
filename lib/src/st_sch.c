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

#include "st_dev.h"
#include "st_log.h"
#include "st_rx_video_session.h"
#include "st_tx_video_session.h"

static inline void sch_mgr_lock(struct st_sch_mgr* mgr) {
  st_pthread_mutex_lock(&mgr->mgr_mutex);
}

static inline void sch_mgr_unlock(struct st_sch_mgr* mgr) {
  st_pthread_mutex_unlock(&mgr->mgr_mutex);
}

static inline void sch_lock(struct st_sch_impl* sch) {
  st_pthread_mutex_lock(&sch->mutex);
}

static inline void sch_unlock(struct st_sch_impl* sch) {
  st_pthread_mutex_unlock(&sch->mutex);
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

static int sch_start(struct st_sch_impl* sch, unsigned int lcore) {
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

static int sch_stop(struct st_sch_impl* sch) {
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

  st_sch_set_cpu_busy(sch, false);

  info("%s(%d), succ\n", __func__, idx);
  sch_unlock(sch);
  return 0;
}

static struct st_sch_impl* sch_request(struct st_main_impl* impl, enum st_sch_type type) {
  struct st_sch_impl* sch;

  for (int sch_idx = 0; sch_idx < ST_MAX_SCH_NUM; sch_idx++) {
    sch = st_sch_instance(impl, sch_idx);

    sch_lock(sch);
    if (!st_sch_is_active(sch)) { /* find one free sch */
      sch->type = type;
      rte_atomic32_inc(&sch->active);
      rte_atomic32_inc(&st_sch_get_mgr(impl)->sch_cnt);
      sch_unlock(sch);
      return sch;
    }
    sch_unlock(sch);
  }

  err("%s, fail as no free sch\n", __func__);
  return NULL;
}

static int sch_free(struct st_sch_impl* sch) {
  if (!st_sch_is_active(sch)) {
    err("%s, sch %d is not allocated\n", __func__, sch->idx);
    return -EIO;
  }

  sch_lock(sch);
  /* todo: free all registered tasklet */
  rte_atomic32_dec(&st_sch_get_mgr(sch->parnet)->sch_cnt);
  rte_atomic32_dec(&sch->active);
  sch_unlock(sch);
  return 0;
}

static int sch_free_quota(struct st_sch_impl* sch, int quota_mbs) {
  int idx = sch->idx;

  if (!st_sch_is_active(sch)) {
    err("%s(%d), sch is not allocated\n", __func__, idx);
    return -ENOMEM;
  }

  sch_lock(sch);
  sch->data_quota_mbs_total -= quota_mbs;
  if (!sch->data_quota_mbs_total) {
    /* no tx/rx video, change to default */
    sch->type = ST_SCH_TYPE_DEFAULT;
  }
  sch_unlock(sch);
  info("%s(%d), quota %d total now %d\n", __func__, idx, quota_mbs,
       sch->data_quota_mbs_total);
  return 0;
}

static int sch_dev_start(struct st_main_impl* impl, struct st_sch_impl* sch) {
  int sidx = sch->idx;
  int ret;
  unsigned int lcore;

  st_sch_set_cpu_busy(sch, false);

  ret = st_dev_get_lcore(impl, &lcore);
  if (ret < 0) {
    err("%s(%d), dev_get_lcore fail %d\n", __func__, sidx, ret);
    return ret;
  }
  ret = sch_start(sch, lcore);
  if (ret < 0) {
    err("%s(%d), st_sch_start fail %d\n", __func__, sidx, ret);
    return ret;
  }

  return 0;
}

static bool sch_is_capable(struct st_sch_impl* sch, int quota_mbs,
                           enum st_sch_type type) {
  if (!quota_mbs) { /* zero quota_mbs can be applied to any type */
    return true;
  }
  if ((type == ST_SCH_TYPE_RX_VIDEO_ONLY) && (sch->type == ST_SCH_TYPE_DEFAULT)) {
    sch_lock(sch);
    if (!sch->data_quota_mbs_total) {
      /* change type to rx video only since no quota on this */
      sch->type = ST_SCH_TYPE_RX_VIDEO_ONLY;
      sch_unlock(sch);
      return true;
    }
    sch_unlock(sch);
  }
  if (sch->type != type)
    return false;
  else
    return true;
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
  strncpy(sch_tasklet->name, tasklet_ops->name, ST_MAX_NAME_LEN - 1);
  sch->num_tasklet++;

  info("%s(%d), tasklet %s registered into slot %d\n", __func__, sch->idx,
       sch_tasklet->name, cur_tasklet_idx);
  sch_unlock(sch);
  return 0;
}

int st_sch_mrg_init(struct st_main_impl* impl, int data_quota_mbs_limit) {
  struct st_sch_impl* sch;
  struct st_sch_mgr* mgr = st_sch_get_mgr(impl);

  st_pthread_mutex_init(&mgr->mgr_mutex, NULL);

  for (int sch_idx = 0; sch_idx < ST_MAX_SCH_NUM; sch_idx++) {
    sch = st_sch_instance(impl, sch_idx);
    st_pthread_mutex_init(&sch->mutex, NULL);
    sch->parnet = impl;
    sch->idx = sch_idx;
    rte_atomic32_set(&sch->started, 0);
    rte_atomic32_set(&sch->ref_cnt, 0);
    rte_atomic32_set(&sch->active, 0);
    sch->num_tasklet = 0;
    sch->data_quota_mbs_total = 0;
    sch->data_quota_mbs_limit = data_quota_mbs_limit;
    /* init mgr lock for video */
    st_pthread_mutex_init(&sch->tx_video_mgr_mutex, NULL);
    st_pthread_mutex_init(&sch->rx_video_mgr_mutex, NULL);
  }

  info("%s, succ with data quota %d M\n", __func__, data_quota_mbs_limit);
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

int st_sch_put(struct st_sch_impl* sch, int quota_mbs) {
  int sidx = sch->idx, ret;
  struct st_main_impl* impl = sch->parnet;

  sch_free_quota(sch, quota_mbs);

  if (rte_atomic32_dec_and_test(&sch->ref_cnt)) {
    info("%s(%d), ref_cnt now zero\n", __func__, sidx);
    if (sch->data_quota_mbs_total)
      err("%s(%d), still has %d data_quota_mbs_total\n", __func__, sidx,
          sch->data_quota_mbs_total);
    /* stop and free sch */
    ret = sch_stop(sch);
    if (ret < 0) {
      err("%s(%d), sch_stop fail %d\n", __func__, sidx, ret);
    } else {
      /* put the sch lcore if dev is started */
      if (rte_atomic32_read(&impl->started)) {
        rte_eal_wait_lcore(sch->lcore);
        st_dev_put_lcore(impl, sch->lcore);
      }
    }
    st_pthread_mutex_lock(&sch->tx_video_mgr_mutex);
    st_tx_video_sessions_sch_uinit(impl, sch);
    st_pthread_mutex_unlock(&sch->tx_video_mgr_mutex);

    st_pthread_mutex_lock(&sch->rx_video_mgr_mutex);
    st_rx_video_sessions_sch_uinit(impl, sch);
    st_pthread_mutex_unlock(&sch->rx_video_mgr_mutex);

    sch_free(sch);
  }

  return 0;
}

struct st_sch_impl* st_sch_get(struct st_main_impl* impl, int quota_mbs,
                               enum st_sch_type type) {
  int ret, idx;
  struct st_sch_impl* sch;
  struct st_sch_mgr* mgr = st_sch_get_mgr(impl);

  sch_mgr_lock(mgr);

  /* first try to find one sch capable with quota */
  for (idx = 0; idx < ST_MAX_SCH_NUM; idx++) {
    sch = st_sch_instance(impl, idx);
    if (!st_sch_is_active(sch) || sch->cpu_busy) continue;
    if (!sch_is_capable(sch, quota_mbs, type)) continue;
    ret = st_sch_add_quota(sch, quota_mbs);
    if (ret >= 0) {
      info("%s(%d), succ with quota_mbs %d\n", __func__, idx, quota_mbs);
      rte_atomic32_inc(&sch->ref_cnt);
      sch_mgr_unlock(mgr);
      return sch;
    }
  }

  /* no quota, try to create one */
  sch = sch_request(impl, type);
  if (!sch) {
    err("%s, no free sch\n", __func__);
    sch_mgr_unlock(mgr);
    return NULL;
  }
  idx = sch->idx;
  ret = st_sch_add_quota(sch, quota_mbs);
  if (ret < 0) {
    err("%s(%d), st_sch_add_quota fail %d\n", __func__, idx, ret);
    sch_free(sch);
    sch_mgr_unlock(mgr);
    return NULL;
  }

  /* start the sch if dev is started */
  if (rte_atomic32_read(&impl->started)) {
    ret = sch_dev_start(impl, sch);
    if (ret < 0) {
      err("%s(%d), start sch fail %d\n", __func__, idx, ret);
      sch_free(sch);
      sch_mgr_unlock(mgr);
      return NULL;
    }
  }

  rte_atomic32_inc(&sch->ref_cnt);
  sch_mgr_unlock(mgr);
  return sch;
}

int st_sch_start_all(struct st_main_impl* impl) {
  int ret = 0;
  struct st_sch_impl* sch;

  /* start active sch */
  for (int sch_idx = 0; sch_idx < ST_MAX_SCH_NUM; sch_idx++) {
    sch = st_sch_instance(impl, sch_idx);
    if (st_sch_is_active(sch)) {
      ret = sch_dev_start(impl, sch);
      if (ret < 0) {
        err("%s(%d), sch_dev_start fail %d\n", __func__, sch_idx, ret);
        st_dev_stop(impl);
        return ret;
      }
    }
  }

  return 0;
}

int st_sch_stop_all(struct st_main_impl* impl) {
  int ret;
  struct st_sch_impl* sch;

  /* stop active sch */
  for (int sch_idx = 0; sch_idx < ST_MAX_SCH_NUM; sch_idx++) {
    sch = st_sch_instance(impl, sch_idx);
    if (st_sch_is_active(sch)) {
      ret = sch_stop(sch);
      if (ret < 0) {
        err("%s(%d), sch_stop fail %d\n", __func__, sch_idx, ret);
      } else {
        rte_eal_wait_lcore(sch->lcore);
        st_dev_put_lcore(impl, sch->lcore);
      }
    }
  }

  info("%s, succ\n", __func__);
  return 0;
}
