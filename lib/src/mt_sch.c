/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_sch.h"

#include "mt_dev.h"
#include "mt_log.h"
#include "mt_stat.h"
#include "st2110/st_rx_video_session.h"
#include "st2110/st_tx_video_session.h"

static inline void sch_mgr_lock(struct mt_sch_mgr* mgr) {
  mt_pthread_mutex_lock(&mgr->mgr_mutex);
}

static inline void sch_mgr_unlock(struct mt_sch_mgr* mgr) {
  mt_pthread_mutex_unlock(&mgr->mgr_mutex);
}

static inline void sch_lock(struct mt_sch_impl* sch) {
  mt_pthread_mutex_lock(&sch->mutex);
}

static inline void sch_unlock(struct mt_sch_impl* sch) {
  mt_pthread_mutex_unlock(&sch->mutex);
}

static void sch_sleep_wakeup(struct mt_sch_impl* sch) {
  mt_pthread_mutex_lock(&sch->sleep_wake_mutex);
  mt_pthread_cond_signal(&sch->sleep_wake_cond);
  mt_pthread_mutex_unlock(&sch->sleep_wake_mutex);
}

static void sch_sleep_alarm_handler(void* param) {
  struct mt_sch_impl* sch = param;

  sch_sleep_wakeup(sch);
}

static int sch_tasklet_sleep(struct mtl_main_impl* impl, struct mt_sch_impl* sch) {
  /* get sleep us */
  uint64_t sleep_us = mt_sch_default_sleep_us(impl);
  uint64_t force_sleep_us = mt_sch_force_sleep_us(impl);
  int num_tasklet = sch->max_tasklet_idx;
  struct mt_sch_tasklet_impl* tasklet;
  uint64_t advice_sleep_us;

  if (force_sleep_us) {
    sleep_us = force_sleep_us;
  } else {
    for (int i = 0; i < num_tasklet; i++) {
      tasklet = sch->tasklet[i];
      if (!tasklet) continue;
      advice_sleep_us = tasklet->ops.advice_sleep_us;
      if (advice_sleep_us && (advice_sleep_us < sleep_us)) sleep_us = advice_sleep_us;
    }
  }
  dbg("%s(%d), sleep_us %" PRIu64 "\n", __func__, sch->idx, sleep_us);

  /* sleep now */
  uint64_t start = mt_get_tsc(impl);
  if (sleep_us < mt_sch_zero_sleep_thresh_us(impl)) {
    mt_sleep_ms(0);
  } else {
    struct timespec abs_time;
    clock_gettime(MT_THREAD_TIMEDWAIT_CLOCK_ID, &abs_time);
    abs_time.tv_sec += 1; /* timeout 1s */

    rte_eal_alarm_set(sleep_us, sch_sleep_alarm_handler, sch);
    mt_pthread_mutex_lock(&sch->sleep_wake_mutex);
    mt_pthread_cond_timedwait(&sch->sleep_wake_cond, &sch->sleep_wake_mutex, &abs_time);
    mt_pthread_mutex_unlock(&sch->sleep_wake_mutex);
  }
  uint64_t end = mt_get_tsc(impl);
  uint64_t delta = end - start;
  sch->stat_sleep_ns += delta;
  sch->stat_sleep_cnt++;
  sch->stat_sleep_ns_min = RTE_MIN(delta, sch->stat_sleep_ns_min);
  sch->stat_sleep_ns_max = RTE_MAX(delta, sch->stat_sleep_ns_max);
  /* cal cpu sleep ratio on every 5s */
  sch->sleep_ratio_sleep_ns += delta;
  uint64_t sleep_ratio_dur_ns = end - sch->sleep_ratio_start_ns;
  if (sleep_ratio_dur_ns > (5 * (uint64_t)NS_PER_S)) {
    dbg("%s(%d), sleep %" PRIu64 "ns, total %" PRIu64 "ns\n", __func__, idx,
        sch->sleep_ratio_sleep_ns, sleep_ratio_dur_ns);
    dbg("%s(%d), end %" PRIu64 "ns, start %" PRIu64 "ns\n", __func__, idx, end,
        sch->sleep_ratio_start_ns);
    sch->sleep_ratio_score =
        (float)sch->sleep_ratio_sleep_ns * 100.0 / sleep_ratio_dur_ns;
    sch->sleep_ratio_sleep_ns = 0;
    sch->sleep_ratio_start_ns = end;
  }

  return 0;
}

static int sch_tasklet_func(void* args) {
  struct mt_sch_impl* sch = args;
  struct mtl_main_impl* impl = sch->parent;
  int idx = sch->idx;
  int num_tasklet, i;
  struct mt_sch_tasklet_ops* ops;
  struct mt_sch_tasklet_impl* tasklet;
  bool time_measure = mt_has_tasklet_time_measure(impl);
  uint64_t tsc_s = 0;

  num_tasklet = sch->max_tasklet_idx;
  info("%s(%d), start with %d tasklets\n", __func__, idx, num_tasklet);

  for (i = 0; i < num_tasklet; i++) {
    tasklet = sch->tasklet[i];
    if (!tasklet) continue;
    ops = &tasklet->ops;
    if (ops->pre_start) ops->pre_start(ops->priv);
  }

  for (i = 0; i < num_tasklet; i++) {
    tasklet = sch->tasklet[i];
    if (!tasklet) continue;
    ops = &tasklet->ops;
    if (ops->start) ops->start(ops->priv);
  }

  sch->sleep_ratio_start_ns = mt_get_tsc(impl);

  while (rte_atomic32_read(&sch->request_stop) == 0) {
    int pending = MT_TASKLET_ALL_DONE;

    num_tasklet = sch->max_tasklet_idx;
    for (i = 0; i < num_tasklet; i++) {
      tasklet = sch->tasklet[i];
      if (!tasklet) continue;
      if (tasklet->request_exit) {
        tasklet->ack_exit = true;
        sch->tasklet[i] = NULL;
        dbg("%s(%d), tasklet %s(%d) exit\n", __func__, idx, tasklet->name, i);
        continue;
      }
      ops = &tasklet->ops;
      if (time_measure) tsc_s = mt_get_tsc(impl);
      pending += ops->handler(ops->priv);
      if (time_measure) {
        uint32_t delta_us = (mt_get_tsc(impl) - tsc_s) / 1000;
        tasklet->stat_max_time_us = RTE_MAX(tasklet->stat_max_time_us, delta_us);
        tasklet->stat_min_time_us = RTE_MIN(tasklet->stat_min_time_us, delta_us);
        tasklet->stat_sum_time_us += delta_us;
        tasklet->stat_time_cnt++;
      }
    }
    if (sch->allow_sleep && (pending == MT_TASKLET_ALL_DONE)) {
      sch_tasklet_sleep(impl, sch);
    }
  }

  num_tasklet = sch->max_tasklet_idx;
  for (i = 0; i < num_tasklet; i++) {
    tasklet = sch->tasklet[i];
    if (!tasklet) continue;
    ops = &tasklet->ops;
    if (ops->stop) ops->stop(ops->priv);
  }

  rte_atomic32_set(&sch->stopped, 1);
  info("%s(%d), end with %d tasklets\n", __func__, idx, num_tasklet);
  return 0;
}

static void* sch_tasklet_thread(void* arg) {
  sch_tasklet_func(arg);
  return NULL;
}

static int sch_start(struct mt_sch_impl* sch) {
  int idx = sch->idx;
  int ret;

  sch_lock(sch);

  if (mt_sch_started(sch)) {
    warn("%s(%d), started already\n", __func__, idx);
    sch_unlock(sch);
    return -EIO;
  }

  mt_sch_set_cpu_busy(sch, false);
  rte_atomic32_set(&sch->request_stop, 0);
  rte_atomic32_set(&sch->stopped, 0);

  if (!sch->run_in_thread) {
    ret = mt_dev_get_lcore(sch->parent, &sch->lcore);
    if (ret < 0) {
      err("%s(%d), get lcore fail %d\n", __func__, idx, ret);
      sch_unlock(sch);
      return ret;
    }
    ret = rte_eal_remote_launch(sch_tasklet_func, sch, sch->lcore);
  } else {
    ret = pthread_create(&sch->tid, NULL, sch_tasklet_thread, sch);
  }
  if (ret < 0) {
    err("%s(%d), fail %d to launch\n", __func__, idx, ret);
    sch_unlock(sch);
    return ret;
  }

  rte_atomic32_set(&sch->started, 1);
  if (!sch->run_in_thread)
    info("%s(%d), succ on lcore %u\n", __func__, idx, sch->lcore);
  else
    info("%s(%d), succ on tid %" PRIu64 "\n", __func__, idx, sch->tid);
  sch_unlock(sch);
  return 0;
}

static int sch_stop(struct mt_sch_impl* sch) {
  int idx = sch->idx;

  sch_lock(sch);

  if (!mt_sch_started(sch)) {
    warn("%s(%d), not started\n", __func__, idx);
    sch_unlock(sch);
    return 0;
  }

  rte_atomic32_set(&sch->request_stop, 1);
  while (rte_atomic32_read(&sch->stopped) == 0) {
    mt_sleep_ms(10);
  }
  if (!sch->run_in_thread) {
    rte_eal_wait_lcore(sch->lcore);
    mt_dev_put_lcore(sch->parent, sch->lcore);
  } else {
    pthread_join(sch->tid, NULL);
  }
  rte_atomic32_set(&sch->started, 0);

  mt_sch_set_cpu_busy(sch, false);

  info("%s(%d), succ\n", __func__, idx);
  sch_unlock(sch);
  return 0;
}

static struct mt_sch_impl* sch_request(struct mtl_main_impl* impl, enum mt_sch_type type,
                                       mt_sch_mask_t mask) {
  struct mt_sch_impl* sch;

  for (int sch_idx = 0; sch_idx < MT_MAX_SCH_NUM; sch_idx++) {
    /* mask check */
    if (!(mask & MTL_BIT64(sch_idx))) continue;

    sch = mt_sch_instance(impl, sch_idx);

    sch_lock(sch);
    if (!mt_sch_is_active(sch)) { /* find one free sch */
      sch->type = type;
      rte_atomic32_inc(&sch->active);
      rte_atomic32_inc(&mt_sch_get_mgr(impl)->sch_cnt);
      sch_unlock(sch);
      return sch;
    }
    sch_unlock(sch);
  }

  err("%s, fail as no free sch\n", __func__);
  return NULL;
}

static int sch_free(struct mt_sch_impl* sch) {
  int idx = sch->idx;

  if (!mt_sch_is_active(sch)) {
    err("%s, sch %d is not allocated\n", __func__, idx);
    return -EIO;
  }

  sch_lock(sch);
  for (int i = 0; i < sch->nb_tasklets; i++) {
    if (sch->tasklet[i]) {
      warn("%s(%d), tasklet %d still active\n", __func__, idx, i);
      mt_sch_unregister_tasklet(sch->tasklet[i]);
    }
  }
  rte_atomic32_dec(&mt_sch_get_mgr(sch->parent)->sch_cnt);
  rte_atomic32_dec(&sch->active);
  sch_unlock(sch);
  return 0;
}

static int sch_free_quota(struct mt_sch_impl* sch, int quota_mbs) {
  int idx = sch->idx;

  if (!mt_sch_is_active(sch)) {
    err("%s(%d), sch is not allocated\n", __func__, idx);
    return -ENOMEM;
  }

  sch_lock(sch);
  sch->data_quota_mbs_total -= quota_mbs;
  if (!sch->data_quota_mbs_total) {
    /* no tx/rx video, change to default */
    sch->type = MT_SCH_TYPE_DEFAULT;
  }
  sch_unlock(sch);
  info("%s(%d), quota %d total now %d\n", __func__, idx, quota_mbs,
       sch->data_quota_mbs_total);
  return 0;
}

static bool sch_is_capable(struct mt_sch_impl* sch, int quota_mbs,
                           enum mt_sch_type type) {
  if (!quota_mbs) { /* zero quota_mbs can be applied to any type */
    return true;
  }
  if ((type == MT_SCH_TYPE_RX_VIDEO_ONLY) && (sch->type == MT_SCH_TYPE_DEFAULT)) {
    sch_lock(sch);
    if (!sch->data_quota_mbs_total) {
      /* change type to rx video only since no quota on this */
      sch->type = MT_SCH_TYPE_RX_VIDEO_ONLY;
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

static void sch_tasklet_stat_clear(struct mt_sch_tasklet_impl* tasklet) {
  tasklet->stat_max_time_us = 0;
  tasklet->stat_min_time_us = (uint32_t)-1;
  tasklet->stat_sum_time_us = 0;
  tasklet->stat_time_cnt = 0;
}

static int sch_stat(void* priv) {
  struct mt_sch_impl* sch = priv;
  int num_tasklet = sch->max_tasklet_idx;
  struct mt_sch_tasklet_impl* tasklet;
  int idx = sch->idx;
  uint32_t avg_us;

  if (mt_has_tasklet_time_measure(sch->parent)) {
    for (int i = 0; i < num_tasklet; i++) {
      tasklet = sch->tasklet[i];
      if (!tasklet) continue;

      if (tasklet->stat_time_cnt) {
        avg_us = tasklet->stat_sum_time_us / tasklet->stat_time_cnt;
        notice("SCH(%d): tasklet %s, avg %uus max %uus min %uus\n", idx, tasklet->name,
               avg_us, tasklet->stat_max_time_us, tasklet->stat_min_time_us);
        sch_tasklet_stat_clear(tasklet);
      }
    }
  }

  if (sch->allow_sleep) {
    notice("SCH(%d): sleep %fms(ratio:%f), cnt %u, min %" PRIu64 "us, max %" PRIu64
           "us\n",
           idx, (double)sch->stat_sleep_ns / NS_PER_MS, sch->sleep_ratio_score,
           sch->stat_sleep_cnt, sch->stat_sleep_ns_min / NS_PER_US,
           sch->stat_sleep_ns_max / NS_PER_US);
    sch->stat_sleep_ns = 0;
    sch->stat_sleep_cnt = 0;
    sch->stat_sleep_ns_min = -1;
    sch->stat_sleep_ns_max = 0;
  }
  if (mt_sch_is_active(sch) && !mt_sch_started(sch)) {
    notice("SCH(%d): active but still not started\n", idx);
  }

  return 0;
}

int mt_sch_unregister_tasklet(struct mt_sch_tasklet_impl* tasklet) {
  struct mt_sch_impl* sch = tasklet->sch;
  int sch_idx = sch->idx;
  int idx = tasklet->idx;

  sch_lock(sch);

  if (sch->tasklet[idx] != tasklet) {
    err("%s(%d), invalid tasklet on %d\n", __func__, sch_idx, idx);
    sch_unlock(sch);
    return -EIO;
  }

  if (mt_sch_started(sch)) {
    int retry = 0;
    /* wait sch ack this exit */
    dbg("%s(%d), tasklet %s(%d) runtime unregistered\n", __func__, sch_idx, tasklet->name,
        idx);
    tasklet->ack_exit = false;
    tasklet->request_exit = true;
    do {
      mt_sleep_ms(1);
      retry++;
      if (retry > 1000) {
        err("%s(%d), tasklet %s(%d) runtime unregistered timeout\n", __func__, sch_idx,
            tasklet->name, idx);
        sch_unlock(sch);
        return -EIO;
      }
    } while (!tasklet->ack_exit);
    info("%s(%d), tasklet %s(%d) unregistered, retry %d\n", __func__, sch_idx,
         tasklet->name, idx, retry);
  } else {
    /* safe to directly remove */
    sch->tasklet[idx] = NULL;
    info("%s(%d), tasklet %s(%d) unregistered\n", __func__, sch_idx, tasklet->name, idx);
  }

  mt_rte_free(tasklet);

  int max_idx = 0;
  for (int i = 0; i < sch->nb_tasklets; i++) {
    if (sch->tasklet[i]) max_idx = i + 1;
  }
  sch->max_tasklet_idx = max_idx;

  sch_unlock(sch);
  return 0;
}

struct mt_sch_tasklet_impl* mt_sch_register_tasklet(
    struct mt_sch_impl* sch, struct mt_sch_tasklet_ops* tasklet_ops) {
  int idx = sch->idx;
  struct mtl_main_impl* impl = sch->parent;
  struct mt_sch_tasklet_impl* tasklet;

  sch_lock(sch);

  /* find one empty slot in the mgr */
  for (int i = 0; i < sch->nb_tasklets; i++) {
    if (sch->tasklet[i]) continue;

    /* find one empty tasklet slot */
    tasklet = mt_rte_zmalloc_socket(sizeof(*tasklet), mt_socket_id(impl, MTL_PORT_P));
    if (!tasklet) {
      err("%s(%d), tasklet malloc fail on %d\n", __func__, idx, i);
      sch_unlock(sch);
      return NULL;
    }

    tasklet->ops = *tasklet_ops;
    strncpy(tasklet->name, tasklet_ops->name, ST_MAX_NAME_LEN - 1);
    tasklet->sch = sch;
    tasklet->idx = i;
    sch_tasklet_stat_clear(tasklet);

    sch->tasklet[i] = tasklet;
    sch->max_tasklet_idx = RTE_MAX(sch->max_tasklet_idx, i + 1);

    if (mt_sch_started(sch)) {
      if (tasklet_ops->pre_start) tasklet_ops->pre_start(tasklet_ops->priv);
      if (tasklet_ops->start) tasklet_ops->start(tasklet_ops->priv);
    }

    sch_unlock(sch);
    info("%s(%d), tasklet %s registered into slot %d\n", __func__, idx, tasklet_ops->name,
         i);
    return tasklet;
  }

  err("%s(%d), no space on this sch, max %d\n", __func__, idx, sch->nb_tasklets);
  sch_unlock(sch);
  return NULL;
}

int mt_sch_mrg_init(struct mtl_main_impl* impl, int data_quota_mbs_limit) {
  struct mt_sch_impl* sch;
  struct mt_sch_mgr* mgr = mt_sch_get_mgr(impl);
  int socket = mt_socket_id(impl, MTL_PORT_P);
  int nb_tasklets = impl->tasklets_nb_per_sch;

  mt_pthread_mutex_init(&mgr->mgr_mutex, NULL);

  for (int sch_idx = 0; sch_idx < MT_MAX_SCH_NUM; sch_idx++) {
    sch = mt_sch_instance(impl, sch_idx);
    mt_pthread_mutex_init(&sch->mutex, NULL);
    sch->parent = impl;
    sch->idx = sch_idx;
    rte_atomic32_set(&sch->started, 0);
    rte_atomic32_set(&sch->ref_cnt, 0);
    rte_atomic32_set(&sch->active, 0);
    sch->max_tasklet_idx = 0;
    sch->nb_tasklets = nb_tasklets;
    sch->data_quota_mbs_total = 0;
    sch->data_quota_mbs_limit = data_quota_mbs_limit;
    sch->run_in_thread = mt_tasklet_has_thread(impl);

    /* sleep info init */
    sch->allow_sleep = mt_tasklet_has_sleep(impl);
#if MT_THREAD_TIMEDWAIT_CLOCK_ID != CLOCK_REALTIME
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, MT_THREAD_TIMEDWAIT_CLOCK_ID);
    mt_pthread_cond_init(&sch->sleep_wake_cond, &attr);
#else
    mt_pthread_cond_init(&sch->sleep_wake_cond, NULL);
#endif
    mt_pthread_mutex_init(&sch->sleep_wake_mutex, NULL);

    sch->stat_sleep_ns_min = -1;
    /* init mgr lock for video */
    mt_pthread_mutex_init(&sch->tx_video_mgr_mutex, NULL);
    mt_pthread_mutex_init(&sch->rx_video_mgr_mutex, NULL);

    mt_stat_register(impl, sch_stat, sch, "sch");

    sch->tasklet =
        mt_rte_zmalloc_socket(sizeof(*sch->tasklet) * sch->nb_tasklets, socket);
    if (!sch->tasklet) {
      err("%s(%d), tasklet malloc fail\n", __func__, sch_idx);
      mt_sch_mrg_uinit(impl);
      return -ENOMEM;
    }
  }

  info("%s, succ with data quota %d M, nb_tasklets %d\n", __func__, data_quota_mbs_limit,
       nb_tasklets);
  return 0;
}

int mt_sch_mrg_uinit(struct mtl_main_impl* impl) {
  struct mt_sch_impl* sch;
  struct mt_sch_mgr* mgr = mt_sch_get_mgr(impl);

  for (int sch_idx = 0; sch_idx < MT_MAX_SCH_NUM; sch_idx++) {
    sch = mt_sch_instance(impl, sch_idx);

    if (sch->tasklet) {
      mt_rte_free(sch->tasklet);
      sch->tasklet = NULL;
    }

    mt_stat_unregister(impl, sch_stat, sch);

    mt_pthread_mutex_destroy(&sch->tx_video_mgr_mutex);
    mt_pthread_mutex_destroy(&sch->rx_video_mgr_mutex);

    mt_pthread_mutex_destroy(&sch->sleep_wake_mutex);
    mt_pthread_cond_destroy(&sch->sleep_wake_cond);

    mt_pthread_mutex_destroy(&sch->mutex);
  }

  mt_pthread_mutex_destroy(&mgr->mgr_mutex);
  return 0;
};

int mt_sch_add_quota(struct mt_sch_impl* sch, int quota_mbs) {
  int idx = sch->idx;

  if (!mt_sch_is_active(sch)) {
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

int mt_sch_put(struct mt_sch_impl* sch, int quota_mbs) {
  int sidx = sch->idx, ret;
  struct mtl_main_impl* impl = sch->parent;

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
    }
    mt_pthread_mutex_lock(&sch->tx_video_mgr_mutex);
    st_tx_video_sessions_sch_uinit(impl, sch);
    mt_pthread_mutex_unlock(&sch->tx_video_mgr_mutex);

    mt_pthread_mutex_lock(&sch->rx_video_mgr_mutex);
    st_rx_video_sessions_sch_uinit(impl, sch);
    mt_pthread_mutex_unlock(&sch->rx_video_mgr_mutex);

    sch_free(sch);
  }

  return 0;
}

struct mt_sch_impl* mt_sch_get(struct mtl_main_impl* impl, int quota_mbs,
                               enum mt_sch_type type, mt_sch_mask_t mask) {
  int ret, idx;
  struct mt_sch_impl* sch;
  struct mt_sch_mgr* mgr = mt_sch_get_mgr(impl);

  sch_mgr_lock(mgr);

  /* first try to find one sch capable with quota */
  for (idx = 0; idx < MT_MAX_SCH_NUM; idx++) {
    sch = mt_sch_instance(impl, idx);
    /* mask check */
    if (!(mask & MTL_BIT64(idx))) continue;
    /* active and busy check */
    if (!mt_sch_is_active(sch) || sch->cpu_busy) continue;
    /* quota check */
    if (!sch_is_capable(sch, quota_mbs, type)) continue;
    ret = mt_sch_add_quota(sch, quota_mbs);
    if (ret >= 0) {
      info("%s(%d), succ with quota_mbs %d\n", __func__, idx, quota_mbs);
      rte_atomic32_inc(&sch->ref_cnt);
      sch_mgr_unlock(mgr);
      return sch;
    }
  }

  /* no quota, try to create one */
  sch = sch_request(impl, type, mask);
  if (!sch) {
    err("%s, no free sch\n", __func__);
    sch_mgr_unlock(mgr);
    return NULL;
  }
  idx = sch->idx;
  ret = mt_sch_add_quota(sch, quota_mbs);
  if (ret < 0) {
    err("%s(%d), mt_sch_add_quota fail %d\n", __func__, idx, ret);
    sch_free(sch);
    sch_mgr_unlock(mgr);
    return NULL;
  }

  /* start the sch if instance is started */
  if (mt_started(impl)) {
    ret = sch_start(sch);
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

int mt_sch_start_all(struct mtl_main_impl* impl) {
  int ret = 0;
  struct mt_sch_impl* sch;

  /* start active sch */
  for (int sch_idx = 0; sch_idx < MT_MAX_SCH_NUM; sch_idx++) {
    sch = mt_sch_instance(impl, sch_idx);
    if (mt_sch_is_active(sch) && !mt_sch_started(sch)) {
      ret = sch_start(sch);
      if (ret < 0) {
        err("%s(%d), sch_start fail %d\n", __func__, sch_idx, ret);
        mt_sch_stop_all(impl);
        return ret;
      }
    }
  }

  return 0;
}

int mt_sch_stop_all(struct mtl_main_impl* impl) {
  int ret;
  struct mt_sch_impl* sch;

  /* stop active sch */
  for (int sch_idx = 0; sch_idx < MT_MAX_SCH_NUM; sch_idx++) {
    sch = mt_sch_instance(impl, sch_idx);
    if (mt_sch_is_active(sch) && mt_sch_started(sch)) {
      ret = sch_stop(sch);
      if (ret < 0) {
        err("%s(%d), sch_stop fail %d\n", __func__, sch_idx, ret);
      }
    }
  }

  info("%s, succ\n", __func__);
  return 0;
}
