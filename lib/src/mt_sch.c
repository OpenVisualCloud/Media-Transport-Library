/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_sch.h"

#include <signal.h>

#include "mt_instance.h"
#include "mt_log.h"
#include "mt_stat.h"
#include "mtl_lcore_shm_api.h"
#include "st2110/st_rx_ancillary_session.h"
#include "st2110/st_rx_audio_session.h"
#include "st2110/st_rx_fastmetadata_session.h"
#include "st2110/st_rx_video_session.h"
#include "st2110/st_tx_ancillary_session.h"
#include "st2110/st_tx_audio_session.h"
#include "st2110/st_tx_fastmetadata_session.h"
#include "st2110/st_tx_video_session.h"

static inline void sch_mgr_lock(struct mt_sch_mgr* mgr) {
  mt_pthread_mutex_lock(&mgr->mgr_mutex);
}

static inline void sch_mgr_unlock(struct mt_sch_mgr* mgr) {
  mt_pthread_mutex_unlock(&mgr->mgr_mutex);
}

static inline void sch_lock(struct mtl_sch_impl* sch) {
  mt_pthread_mutex_lock(&sch->mutex);
}

static inline void sch_unlock(struct mtl_sch_impl* sch) {
  mt_pthread_mutex_unlock(&sch->mutex);
}

static const char* lcore_type_names[MT_LCORE_TYPE_MAX] = {
    "lib_sch", "lib_tap", "lib_rxv_ring", "app_allocated", "lib_app_sch",
};

static const char* lcore_type_name(enum mt_lcore_type type) {
  if (type >= MT_LCORE_TYPE_SCH && type < MT_LCORE_TYPE_MAX)
    return lcore_type_names[type];
  else
    return "unknown";
}

static void sch_sleep_wakeup(struct mtl_sch_impl* sch) {
  mt_pthread_mutex_lock(&sch->sleep_wake_mutex);
  mt_pthread_cond_signal(&sch->sleep_wake_cond);
  mt_pthread_mutex_unlock(&sch->sleep_wake_mutex);
}

static void sch_sleep_alarm_handler(void* param) {
  struct mtl_sch_impl* sch = param;

  sch_sleep_wakeup(sch);
}

static int sch_tasklet_sleep(struct mtl_main_impl* impl, struct mtl_sch_impl* sch) {
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
    rte_eal_alarm_set(sleep_us, sch_sleep_alarm_handler, sch);
    mt_pthread_mutex_lock(&sch->sleep_wake_mutex);
    /* timeout 1s */
    mt_pthread_cond_timedwait_ns(&sch->sleep_wake_cond, &sch->sleep_wake_mutex, NS_PER_S);
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
    dbg("%s(%d), sleep %" PRIu64 "ns, total %" PRIu64 "ns\n", __func__, sch->idx,
        sch->sleep_ratio_sleep_ns, sleep_ratio_dur_ns);
    dbg("%s(%d), end %" PRIu64 "ns, start %" PRIu64 "ns\n", __func__, sch->idx, end,
        sch->sleep_ratio_start_ns);
    sch->sleep_ratio_score =
        (float)sch->sleep_ratio_sleep_ns * 100.0 / sleep_ratio_dur_ns;
    sch->sleep_ratio_sleep_ns = 0;
    sch->sleep_ratio_start_ns = end;
  }

  return 0;
}

static bool sch_tasklet_time_measure(struct mtl_main_impl* impl) {
  bool enabled = mt_user_tasklet_time_measure(impl);
  if (MT_USDT_TASKLET_TIME_MEASURE_ENABLED()) enabled = true;
  return enabled;
}

static int sch_tasklet_func(struct mtl_sch_impl* sch) {
  struct mtl_main_impl* impl = sch->parent;
  int idx = sch->idx;
  int num_tasklet, i;
  struct mtl_tasklet_ops* ops;
  struct mt_sch_tasklet_impl* tasklet;
  uint64_t loop_cal_start_ns;
  uint64_t loop_cnt = 0;

  num_tasklet = sch->max_tasklet_idx;
  info("%s(%d), start with %d tasklets, t_pid %d\n", __func__, idx, num_tasklet,
       sch->t_pid);

  char thread_name[32];
  snprintf(thread_name, sizeof(thread_name), "mtl_sch_%d", idx);
  mtl_thread_setname(sch->tid, thread_name);

  for (i = 0; i < num_tasklet; i++) {
    tasklet = sch->tasklet[i];
    if (!tasklet) continue;
    ops = &tasklet->ops;
    if (ops->start) ops->start(ops->priv);
  }

  sch->sleep_ratio_start_ns = mt_get_tsc(impl);
  loop_cal_start_ns = mt_get_tsc(impl);

  while (mt_atomic32_read_acquire(&sch->request_stop) == 0) {
    int pending = MTL_TASKLET_ALL_DONE;
    bool time_measure = sch_tasklet_time_measure(impl);
    uint64_t tm_sch_tsc_s = 0; /* for sch time_measure */

    if (time_measure) tm_sch_tsc_s = mt_get_tsc(impl);

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

      uint64_t tm_tasklet_tsc_s = 0; /* for tasklet time_measure */
      if (time_measure) tm_tasklet_tsc_s = mt_get_tsc(impl);
      pending += ops->handler(ops->priv);
      if (time_measure) {
        uint64_t delta_ns = mt_get_tsc(impl) - tm_tasklet_tsc_s;
        mt_stat_u64_update(&tasklet->stat_time, delta_ns);
      }
    }
    if (sch->allow_sleep && (pending == MTL_TASKLET_ALL_DONE)) {
      sch_tasklet_sleep(impl, sch);
    }

    loop_cnt++;
    /* cal avg_ns_per_loop per two second */
    uint64_t delta_loop_ns = mt_get_tsc(impl) - loop_cal_start_ns;
    if (delta_loop_ns > ((uint64_t)NS_PER_S * 2)) {
      sch->avg_ns_per_loop = delta_loop_ns / loop_cnt;
      loop_cnt = 0;
      loop_cal_start_ns = mt_get_tsc(impl);
    }

    if (time_measure) {
      uint64_t delta_ns = mt_get_tsc(impl) - tm_sch_tsc_s;
      mt_stat_u64_update(&sch->stat_time, delta_ns);
    }
  }

  num_tasklet = sch->max_tasklet_idx;
  for (i = 0; i < num_tasklet; i++) {
    tasklet = sch->tasklet[i];
    if (!tasklet) continue;
    ops = &tasklet->ops;
    if (ops->stop) ops->stop(ops->priv);
  }

  mt_atomic32_set_release(&sch->stopped, 1);
  info("%s(%d), end with %d tasklets\n", __func__, idx, num_tasklet);
  return 0;
}

static int sch_tasklet_lcore(void* arg) {
  struct mtl_sch_impl* sch = arg;
  sch->tid = pthread_self();
  sch->t_pid = rte_sys_gettid();
  sch_tasklet_func(sch);
  return 0;
}

static void* sch_tasklet_thread(void* arg) {
  struct mtl_sch_impl* sch = arg;
  sch->t_pid = rte_sys_gettid();
  sch_tasklet_func(sch);
  return NULL;
}

static int sch_start(struct mtl_sch_impl* sch) {
  int idx = sch->idx;
  int ret;

  sch_lock(sch);

  if (mt_sch_started(sch)) {
    warn("%s(%d), started already\n", __func__, idx);
    sch_unlock(sch);
    return -EIO;
  }

  mt_sch_set_cpu_busy(sch, false);
  mt_atomic32_set(&sch->request_stop, 0);
  mt_atomic32_set(&sch->stopped, 0);

  if (!sch->run_in_thread) {
    ret = mt_sch_get_lcore(
        sch->parent, &sch->lcore,
        (sch->type == MT_SCH_TYPE_APP) ? MT_LCORE_TYPE_SCH_USER : MT_LCORE_TYPE_SCH,
        mt_sch_socket_id(sch));
    if (ret < 0) {
      err("%s(%d), get lcore fail %d\n", __func__, idx, ret);
      sch_unlock(sch);
      return ret;
    }
    ret = rte_eal_remote_launch(sch_tasklet_lcore, sch, sch->lcore);
  } else {
    ret = pthread_create(&sch->tid, NULL, sch_tasklet_thread, sch);
  }
  if (ret < 0) {
    err("%s(%d), fail %d to launch\n", __func__, idx, ret);
    sch_unlock(sch);
    return ret;
  }

  mt_atomic32_set_release(&sch->started, 1);
  if (!sch->run_in_thread)
    info("%s(%d), succ on lcore %u socket %d\n", __func__, idx, sch->lcore,
         mt_sch_socket_id(sch));
  else
    info("%s(%d), succ on tid %" PRIu64 "\n", __func__, idx, sch->tid);
  sch_unlock(sch);
  return 0;
}

static int sch_stop(struct mtl_sch_impl* sch) {
  int idx = sch->idx;

  sch_lock(sch);

  if (!mt_sch_started(sch)) {
    warn("%s(%d), not started\n", __func__, idx);
    sch_unlock(sch);
    return 0;
  }

  mt_atomic32_set_release(&sch->request_stop, 1);
  while (mt_atomic32_read_acquire(&sch->stopped) == 0) {
    mt_sleep_ms(10);
  }
  if (!sch->run_in_thread) {
    rte_eal_wait_lcore(sch->lcore);
    mt_sch_put_lcore(sch->parent, sch->lcore);
  } else {
    pthread_join(sch->tid, NULL);
  }
  mt_atomic32_set_release(&sch->started, 0);

  mt_sch_set_cpu_busy(sch, false);

  info("%s(%d), succ\n", __func__, idx);
  sch_unlock(sch);
  return 0;
}

static struct mtl_sch_impl* sch_request(struct mtl_main_impl* impl, enum mt_sch_type type,
                                        mt_sch_mask_t mask, struct mtl_sch_ops* ops,
                                        int socket) {
  struct mtl_sch_impl* sch;

  for (int sch_idx = 0; sch_idx < MT_MAX_SCH_NUM; sch_idx++) {
    /* mask check */
    if (!(mask & MTL_BIT64(sch_idx))) continue;

    sch = mt_sch_instance(impl, sch_idx);

    sch_lock(sch);
    if (!mt_sch_is_active(sch)) { /* find one free sch */
      sch->type = type;
      if (ops && ops->name) {
        snprintf(sch->name, sizeof(sch->name), "%s", ops->name);
      } else {
        snprintf(sch->name, sizeof(sch->name), "sch_%d", sch_idx);
      }
      if (ops && ops->nb_tasklets)
        sch->nb_tasklets = ops->nb_tasklets;
      else
        sch->nb_tasklets = impl->tasklets_nb_per_sch;
      sch->tasklet =
          mt_rte_zmalloc_socket(sizeof(*sch->tasklet) * sch->nb_tasklets, socket);
      if (!sch->tasklet) {
        err("%s(%d), %u tasklet malloc fail\n", __func__, sch_idx, sch->nb_tasklets);
        sch_unlock(sch);
        return NULL;
      }
      mt_atomic32_inc(&sch->active);
      mt_atomic32_inc(&mt_sch_get_mgr(impl)->sch_cnt);
      sch_unlock(sch);

      /* set the socket id */
      sch->socket_id = socket;
      info("%s(%d), name %s with %u tasklets, type %d socket %d\n", __func__, sch_idx,
           sch->name, sch->nb_tasklets, type, sch->socket_id);
      return sch;
    }
    sch_unlock(sch);
  }

  err("%s, fail as no free sch\n", __func__);
  return NULL;
}

static int sch_free(struct mtl_sch_impl* sch) {
  int idx = sch->idx;

  if (!mt_sch_is_active(sch)) {
    err("%s, sch %d is not allocated\n", __func__, idx);
    return -EIO;
  }

  info("%s(%d), start to free sch: %s \n", __func__, idx, sch->name);
  sch_lock(sch);
  if (sch->tasklet) {
    for (int i = 0; i < sch->nb_tasklets; i++) {
      if (sch->tasklet[i]) {
        warn("%s(%d), tasklet %d still active\n", __func__, idx, i);
        sch_unlock(sch); /* unlock */
        mtl_sch_unregister_tasklet(sch->tasklet[i]);
        sch_lock(sch);
      }
    }
    mt_rte_free(sch->tasklet);
    sch->tasklet = NULL;
  }
  sch->nb_tasklets = 0;
  mt_atomic32_dec(&mt_sch_get_mgr(sch->parent)->sch_cnt);
  mt_atomic32_dec(&sch->active);
  sch_unlock(sch);
  return 0;
}

static int sch_free_quota(struct mtl_sch_impl* sch, int quota_mbs) {
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

static bool sch_is_capable(struct mtl_sch_impl* sch, int quota_mbs,
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

static int sch_stat(void* priv) {
  struct mtl_sch_impl* sch = priv;
  int num_tasklet = sch->max_tasklet_idx;
  struct mt_sch_tasklet_impl* tasklet;
  int idx = sch->idx;

  if (!mt_sch_is_active(sch)) return 0;

  notice("SCH(%d:%s): tasklets %d, lcore %u(t_pid: %d), avg loop %" PRIu64 " ns\n", idx,
         sch->name, num_tasklet, sch->lcore, sch->t_pid, mt_sch_avg_ns_loop(sch));

  /* print the stat time info */
  struct mt_stat_u64* stat_time = &sch->stat_time;
  if (stat_time->cnt) {
    uint64_t avg_ns = stat_time->sum / stat_time->cnt;
    notice("SCH(%d): time avg %.2fus max %.2fus min %.2fus\n", idx,
           (float)avg_ns / NS_PER_US, (float)stat_time->max / NS_PER_US,
           (float)stat_time->min / NS_PER_US);
    mt_stat_u64_init(stat_time);
  }
  for (int i = 0; i < num_tasklet; i++) {
    tasklet = sch->tasklet[i];
    if (!tasklet) continue;

    dbg("SCH(%d): tasklet %s at %d\n", idx, tasklet->name, i);
    stat_time = &tasklet->stat_time;
    if (stat_time->cnt) {
      uint64_t avg_ns = stat_time->sum / stat_time->cnt;
      notice("SCH(%d,%d): tasklet %s, avg %.2fus max %.2fus min %.2fus\n", idx, i,
             tasklet->name, (float)avg_ns / NS_PER_US, (float)stat_time->max / NS_PER_US,
             (float)stat_time->min / NS_PER_US);
      mt_stat_u64_init(stat_time);
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
  if (!mt_sch_started(sch)) {
    notice("SCH(%d): active but still not started\n", idx);
  }

  return 0;
}

static int sch_filelock_lock(struct mt_sch_mgr* mgr) {
  int fd = open(MT_FLOCK_PATH, O_RDONLY | O_CREAT, 0666);
  if (fd < 0) {
    /* sometimes may fail due to user permission, try open read-only */
    fd = open(MT_FLOCK_PATH, O_RDONLY);
    if (fd < 0) {
      err("%s, failed to open %s, %s\n", __func__, MT_FLOCK_PATH, strerror(errno));
      return -EIO;
    }
  }
  mgr->lcore_lock_fd = fd;
  /* wait until locked */
  if (flock(fd, LOCK_EX) != 0) {
    err("%s, can not lock file\n", __func__);
    close(fd);
    mgr->lcore_lock_fd = -1;
    return -EIO;
  }

  return 0;
}

static int sch_filelock_unlock(struct mt_sch_mgr* mgr) {
  int fd = mgr->lcore_lock_fd;

  if (fd < 0) {
    err("%s, wrong lock file fd %d\n", __func__, fd);
    return -EIO;
  }

  if (flock(fd, LOCK_UN) != 0) {
    err("%s, can not unlock file\n", __func__);
    return -EIO;
  }
  close(fd);
  mgr->lcore_lock_fd = -1;
  return 0;
}

static int sch_lcore_shm_init(struct mt_lcore_mgr* mgr, bool clear_on_first) {
  struct mt_lcore_shm* lcore_shm = NULL;
  int ret;

  mgr->lcore_shm_id = -1;

  key_t key = ftok("/dev/null", 21);
  if (key < 0) {
    err("%s, ftok error: %s\n", __func__, strerror(errno));
    return -EIO;
  }
  int shm_id = shmget(key, sizeof(*lcore_shm), 0666 | IPC_CREAT);
  if (shm_id < 0) {
    err("%s, can not get shared memory for lcore, %s\n", __func__, strerror(errno));
    return -EIO;
  }
  mgr->lcore_shm_id = shm_id;

  lcore_shm = shmat(shm_id, NULL, 0);
  if (lcore_shm == (void*)-1) {
    err("%s, can not attach shared memory for lcore, %s\n", __func__, strerror(errno));
    return -EIO;
  }

  struct shmid_ds stat;
  ret = shmctl(shm_id, IPC_STAT, &stat);
  if (ret < 0) {
    err("%s, shmctl fail\n", __func__);
    shmdt(lcore_shm);
    return ret;
  }

  if (clear_on_first && (stat.shm_nattch == 1)) {
    info("%s, clear shm as we are the first user\n", __func__);
    memset(lcore_shm, 0, sizeof(*lcore_shm));
  }

  mgr->lcore_shm = lcore_shm;
  info("%s, shared memory attached at %p nattch %d shm_id %d key 0x%x\n", __func__,
       mgr->lcore_shm, (int)stat.shm_nattch, shm_id, (int)key);
  return 0;
}

static int sch_lcore_shm_uinit(struct mt_lcore_mgr* mgr) {
  int ret;

  if (mgr->lcore_shm) {
    ret = shmdt(mgr->lcore_shm);
    if (ret < 0) err("%s, shared memory detach failed, %s\n", __func__, strerror(errno));
    mgr->lcore_shm = NULL;
  }

  if (mgr->lcore_shm_id >= 0) {
    struct shmid_ds shmds;
    ret = shmctl(mgr->lcore_shm_id, IPC_STAT, &shmds);
    if (ret < 0) {
      err("%s, can not stat shared memory, %s\n", __func__, strerror(errno));
    } else {
      if (shmds.shm_nattch == 0) { /* remove ipc if we are the last user */
        notice("%s, remove shared memory as we are the last user\n", __func__);
        ret = shmctl(mgr->lcore_shm_id, IPC_RMID, NULL);
        if (ret < 0) {
          warn("%s, can not remove shared memory, %s\n", __func__, strerror(errno));
        }
      }
    }
    mgr->lcore_shm_id = -1;
  }

  return 0;
}

static int sch_uinit_lcores(struct mtl_main_impl* impl, struct mt_sch_mgr* mgr) {
  int ret;

  for (unsigned int lcore = 0; lcore < RTE_MAX_LCORE; lcore++) {
    if (mgr->local_lcores_active[lcore]) {
      warn("%s, lcore %d still active\n", __func__, lcore);
      mt_sch_put_lcore(impl, lcore);
    }
  }

  ret = sch_filelock_lock(mgr);
  if (ret < 0) {
    err("%s, sch_filelock_lock fail\n", __func__);
    return ret;
  }

  ret = sch_lcore_shm_uinit(&mgr->lcore_mgr);
  if (ret < 0) {
    err("%s, lcore shm uinit fail %d\n", __func__, ret);
  }

  ret = sch_filelock_unlock(mgr);
  if (ret < 0) {
    err("%s, sch_filelock_unlock fail\n", __func__);
    return ret;
  }

  return 0;
}

static int sch_init_lcores(struct mt_sch_mgr* mgr) {
  int ret;

  if (mgr->lcore_mgr.lcore_shm) {
    err("%s, lcore_shm attached\n", __func__);
    return -EIO;
  }

  ret = sch_filelock_lock(mgr);
  if (ret < 0) {
    err("%s, sch_filelock_lock fail %d\n", __func__, ret);
    return ret;
  }

  ret = sch_lcore_shm_init(&mgr->lcore_mgr, true);
  if (ret < 0) {
    err("%s, lcore init fail %d\n", __func__, ret);
    sch_filelock_unlock(mgr);
    return ret;
  }

  ret = sch_filelock_unlock(mgr);
  if (ret < 0) {
    err("%s, sch_filelock_unlock fail %d\n", __func__, ret);
    return ret;
  }
  return 0;
}

static inline bool sch_socket_match(int cpu_socket, int dev_socket,
                                    bool skip_numa_check) {
  if (skip_numa_check) return true;
  return mt_socket_match(cpu_socket, dev_socket);
}

/*
 * check if the lcore has not been properly released by previous(currently dead) process,
 * if so, clean it up
 */
static void lcore_shm_check_and_clean(struct mt_lcore_shm_entry* shm_entry,
                                      struct mt_user_info* info) {
  if (!shm_entry->active) return;
  struct mt_user_info* u_info = &shm_entry->u_info;
  if (0 != strncmp(u_info->hostname, info->hostname, sizeof(u_info->hostname))) return;
  if (0 != strncmp(u_info->user, info->user, sizeof(u_info->user))) return;
  if (kill(shm_entry->pid, 0) != 0) {
    shm_entry->active = false;
    info("%s, releasing lcore for dead process pid %d \n", __func__, shm_entry->pid);
  }
}

int mt_sch_get_lcore(struct mtl_main_impl* impl, unsigned int* lcore,
                     enum mt_lcore_type type, int socket) {
  unsigned int cur_lcore;
  int ret;
  bool skip_numa_check = false;
  struct mt_sch_mgr* mgr = mt_sch_get_mgr(impl);
  int tried = 0;

  if (mt_user_not_bind_numa(impl)) skip_numa_check = true;

again:
  cur_lcore = 0;
  if (mt_is_manager_connected(impl)) {
    do {
      cur_lcore = rte_get_next_lcore(cur_lcore, 1, 0);
      if ((cur_lcore < RTE_MAX_LCORE) &&
          sch_socket_match(rte_lcore_to_socket_id(cur_lcore), socket, skip_numa_check)) {
        ret = mt_instance_get_lcore(impl, cur_lcore);
        if (ret == 0) {
          *lcore = cur_lcore;
          mt_atomic32_inc(&impl->lcore_cnt);
          /* set local lcores info */
          mgr->local_lcores_active[cur_lcore] = true;
          mgr->local_lcores_type[cur_lcore] = type;
          info("%s, succ on manager lcore %d for %s socket %d\n", __func__, cur_lcore,
               lcore_type_name(type), socket);
          return 0;
        }
      }
      tried++;
    } while (cur_lcore < RTE_MAX_LCORE);
  } else {
    struct mt_user_info* info = &impl->u_info;

    struct mt_lcore_shm* lcore_shm = mgr->lcore_mgr.lcore_shm;
    struct mt_lcore_shm_entry* shm_entry;

    ret = sch_filelock_lock(mgr);
    if (ret < 0) {
      err("%s, sch_filelock_lock fail\n", __func__);
      return ret;
    }

    do {
      cur_lcore = rte_get_next_lcore(cur_lcore, 1, 0);
      shm_entry = &lcore_shm->lcores_info[cur_lcore];

      if ((cur_lcore < RTE_MAX_LCORE) &&
          sch_socket_match(rte_lcore_to_socket_id(cur_lcore), socket, skip_numa_check)) {
        lcore_shm_check_and_clean(shm_entry, info);
        if (!shm_entry->active) {
          *lcore = cur_lcore;
          shm_entry->active = true;
          struct mt_user_info* u_info = &shm_entry->u_info;
          strncpy(u_info->hostname, info->hostname, sizeof(u_info->hostname));
          strncpy(u_info->user, info->user, sizeof(u_info->user));
          strncpy(u_info->comm, info->comm, sizeof(u_info->comm));
          shm_entry->type = type;
          shm_entry->pid = info->pid;
          lcore_shm->used++;
          mt_atomic32_inc(&impl->lcore_cnt);
          /* set local lcores info */
          mgr->local_lcores_active[cur_lcore] = true;
          mgr->local_lcores_type[cur_lcore] = type;
          ret = sch_filelock_unlock(mgr);
          info("%s, succ on shm lcore %d for %s socket %d\n", __func__, cur_lcore,
               lcore_type_name(type), socket);
          if (ret < 0) {
            err("%s, sch_filelock_unlock fail\n", __func__);
            return ret;
          }
          return 0;
        }
      }
      tried++;
    } while (cur_lcore < RTE_MAX_LCORE);

    sch_filelock_unlock(mgr);
  }

  if (!skip_numa_check && mt_user_across_numa_core(impl)) {
    warn("%s, can't find available lcore from socket %d, try with other numa cpu\n",
         __func__, socket);
    skip_numa_check = true;
    goto again;
  }

  err("%s, no available lcore, type %s tried %d\n", __func__, lcore_type_name(type),
      tried);
  return -EIO;
}

int mt_sch_put_lcore(struct mtl_main_impl* impl, unsigned int lcore) {
  int ret;
  struct mt_sch_mgr* mgr = mt_sch_get_mgr(impl);

  if (mt_is_manager_connected(impl)) {
    ret = mt_instance_put_lcore(impl, lcore);
    if (ret == 0) {
      mt_atomic32_dec(&impl->lcore_cnt);
      mgr->local_lcores_active[lcore] = false;
      info("%s, succ on manager lcore %d for %s\n", __func__, lcore,
           lcore_type_name(mgr->local_lcores_type[lcore]));
      return 0;
    } else if (lcore >= RTE_MAX_LCORE) {
      err("%s, err %d on manager invalid lcore %d\n", __func__, ret, lcore);
      return -EIO;
    } else {
      err("%s, err %d on manager lcore %d for %s\n", __func__, ret, lcore,
          lcore_type_name(mgr->local_lcores_type[lcore]));
      return ret;
    }
  } else {
    struct mt_lcore_shm* lcore_shm = mgr->lcore_mgr.lcore_shm;

    if (lcore >= RTE_MAX_LCORE) {
      err("%s, invalid lcore %d\n", __func__, lcore);
      return -EIO;
    }
    if (!lcore_shm) {
      err("%s, no lcore shm attached\n", __func__);
      return -EIO;
    }
    ret = sch_filelock_lock(mgr);
    if (ret < 0) {
      err("%s, sch_filelock_lock fail\n", __func__);
      return ret;
    }
    if (!lcore_shm->lcores_info[lcore].active) {
      err("%s, lcore %d not active\n", __func__, lcore);
      ret = -EIO;
      goto err_unlock;
    }

    lcore_shm->lcores_info[lcore].active = false;
    lcore_shm->used--;
    mt_atomic32_dec(&impl->lcore_cnt);
    mgr->local_lcores_active[lcore] = false;
    ret = sch_filelock_unlock(mgr);
    info("%s, succ on shm lcore %d for %s\n", __func__, lcore,
         lcore_type_name(mgr->local_lcores_type[lcore]));
    if (ret < 0) {
      err("%s, sch_filelock_unlock fail\n", __func__);
      return ret;
    }
    return 0;

  err_unlock:
    sch_filelock_unlock(mgr);
  }
  return ret;
}

bool mt_sch_lcore_valid(struct mtl_main_impl* impl, unsigned int lcore) {
  struct mt_sch_mgr* mgr = mt_sch_get_mgr(impl);
  struct mt_lcore_shm* lcore_shm = mgr->lcore_mgr.lcore_shm;

  if (lcore >= RTE_MAX_LCORE) {
    err("%s, invalid lcore %d\n", __func__, lcore);
    return -EIO;
  }

  if (mt_is_manager_connected(impl)) return true;

  if (!lcore_shm) {
    err("%s, no lcore shm attached\n", __func__);
    return -EIO;
  }

  return lcore_shm->lcores_info[lcore].active;
}

int mtl_sch_unregister_tasklet(mtl_tasklet_handle tasklet) {
  struct mtl_sch_impl* sch = tasklet->sch;
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
    /* call the stop for runtime path */
    if (tasklet->ops.stop) tasklet->ops.stop(tasklet->ops.priv);
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

mtl_tasklet_handle mtl_sch_register_tasklet(struct mtl_sch_impl* sch,
                                            struct mtl_tasklet_ops* tasklet_ops) {
  int idx = sch->idx;
  struct mt_sch_tasklet_impl* tasklet;

  sch_lock(sch);

  /* find one empty slot in the mgr */
  for (int i = 0; i < sch->nb_tasklets; i++) {
    if (sch->tasklet[i]) continue;

    /* find one empty tasklet slot */
    tasklet = mt_rte_zmalloc_socket(sizeof(*tasklet), mt_sch_socket_id(sch));
    if (!tasklet) {
      err("%s(%d), tasklet malloc fail on %d\n", __func__, idx, i);
      sch_unlock(sch);
      return NULL;
    }

    tasklet->ops = *tasklet_ops;
    snprintf(tasklet->name, ST_MAX_NAME_LEN - 1, "%s", tasklet_ops->name);
    tasklet->sch = sch;
    tasklet->idx = i;
    mt_stat_u64_init(&tasklet->stat_time);

    sch->tasklet[i] = tasklet;
    sch->max_tasklet_idx = RTE_MAX(sch->max_tasklet_idx, i + 1);

    if (mt_sch_started(sch)) {
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
  struct mtl_sch_impl* sch;
  struct mt_sch_mgr* mgr = mt_sch_get_mgr(impl);
  int ret;

  mt_pthread_mutex_init(&mgr->mgr_mutex, NULL);

  mgr->lcore_lock_fd = -1;

  if (!mt_is_manager_connected(impl)) {
    ret = sch_init_lcores(mgr);
    if (ret < 0) return ret;
  }

  for (int sch_idx = 0; sch_idx < MT_MAX_SCH_NUM; sch_idx++) {
    sch = mt_sch_instance(impl, sch_idx);
    mt_pthread_mutex_init(&sch->mutex, NULL);
    sch->parent = impl;
    sch->idx = sch_idx;
    mt_atomic32_set(&sch->started, 0);
    mt_atomic32_set(&sch->ref_cnt, 0);
    mt_atomic32_set(&sch->active, 0);
    sch->max_tasklet_idx = 0;
    sch->data_quota_mbs_total = 0;
    sch->data_quota_mbs_limit = data_quota_mbs_limit;
    sch->run_in_thread = mt_user_tasklet_thread(impl);
    mt_stat_u64_init(&sch->stat_time);

    /* sleep info init */
    sch->allow_sleep = mt_user_tasklet_sleep(impl);
    mt_pthread_cond_wait_init(&sch->sleep_wake_cond);
    mt_pthread_mutex_init(&sch->sleep_wake_mutex, NULL);

    sch->stat_sleep_ns_min = -1;
    /* init mgr lock for video */
    mt_pthread_mutex_init(&sch->tx_video_mgr_mutex, NULL);
    mt_pthread_mutex_init(&sch->rx_video_mgr_mutex, NULL);
    /* init mgr lock for audio */
    mt_pthread_mutex_init(&sch->tx_a_mgr_mutex, NULL);
    mt_pthread_mutex_init(&sch->rx_a_mgr_mutex, NULL);
    /* init mgr lock for anc */
    mt_pthread_mutex_init(&sch->tx_anc_mgr_mutex, NULL);
    mt_pthread_mutex_init(&sch->rx_anc_mgr_mutex, NULL);
    /* init mgr lock for fmd */
    mt_pthread_mutex_init(&sch->tx_fmd_mgr_mutex, NULL);
    mt_pthread_mutex_init(&sch->rx_fmd_mgr_mutex, NULL);

    mt_stat_register(impl, sch_stat, sch, "sch");
  }

  info("%s, succ with data quota %d M\n", __func__, data_quota_mbs_limit);
  return 0;
}

int mt_sch_mrg_uinit(struct mtl_main_impl* impl) {
  struct mtl_sch_impl* sch;
  struct mt_sch_mgr* mgr = mt_sch_get_mgr(impl);

  if (!mt_is_manager_connected(impl)) sch_uinit_lcores(impl, mgr);

  for (int sch_idx = 0; sch_idx < MT_MAX_SCH_NUM; sch_idx++) {
    sch = mt_sch_instance(impl, sch_idx);

    if (mt_sch_is_active(sch)) {
      warn("%s(%d), sch:%s still active\n", __func__, sch_idx, sch->name);
      mtl_sch_free(sch);
    }

    mt_stat_unregister(impl, sch_stat, sch);

    mt_pthread_mutex_destroy(&sch->tx_video_mgr_mutex);
    mt_pthread_mutex_destroy(&sch->rx_video_mgr_mutex);

    mt_pthread_mutex_destroy(&sch->tx_a_mgr_mutex);
    mt_pthread_mutex_destroy(&sch->rx_a_mgr_mutex);

    mt_pthread_mutex_destroy(&sch->tx_anc_mgr_mutex);
    mt_pthread_mutex_destroy(&sch->rx_anc_mgr_mutex);

    mt_pthread_mutex_destroy(&sch->tx_fmd_mgr_mutex);
    mt_pthread_mutex_destroy(&sch->rx_fmd_mgr_mutex);

    mt_pthread_mutex_destroy(&sch->sleep_wake_mutex);
    mt_pthread_cond_destroy(&sch->sleep_wake_cond);

    mt_pthread_mutex_destroy(&sch->mutex);
  }

  mt_pthread_mutex_destroy(&mgr->mgr_mutex);
  return 0;
};

int mt_sch_add_quota(struct mtl_sch_impl* sch, int quota_mbs) {
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

int mt_sch_put(struct mtl_sch_impl* sch, int quota_mbs) {
  int sidx = sch->idx, ret;
  struct mtl_main_impl* impl = sch->parent;

  sch_free_quota(sch, quota_mbs);

  if (mt_atomic32_dec_and_test(&sch->ref_cnt)) {
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

    mt_pthread_mutex_lock(&sch->tx_a_mgr_mutex);
    st_tx_audio_sessions_sch_uinit(sch);
    mt_pthread_mutex_unlock(&sch->tx_a_mgr_mutex);

    mt_pthread_mutex_lock(&sch->rx_a_mgr_mutex);
    st_rx_audio_sessions_sch_uinit(sch);
    mt_pthread_mutex_unlock(&sch->rx_a_mgr_mutex);

    mt_pthread_mutex_lock(&sch->tx_anc_mgr_mutex);
    st_tx_ancillary_sessions_sch_uinit(sch);
    mt_pthread_mutex_unlock(&sch->tx_anc_mgr_mutex);

    mt_pthread_mutex_lock(&sch->rx_anc_mgr_mutex);
    st_rx_ancillary_sessions_sch_uinit(sch);
    mt_pthread_mutex_unlock(&sch->rx_anc_mgr_mutex);

    mt_pthread_mutex_lock(&sch->tx_fmd_mgr_mutex);
    st_tx_fastmetadata_sessions_sch_uinit(sch);
    mt_pthread_mutex_unlock(&sch->tx_fmd_mgr_mutex);

    mt_pthread_mutex_lock(&sch->rx_fmd_mgr_mutex);
    st_rx_fastmetadata_sessions_sch_uinit(sch);
    mt_pthread_mutex_unlock(&sch->rx_fmd_mgr_mutex);

    sch_free(sch);
  }

  return 0;
}

struct mtl_sch_impl* mt_sch_get_by_socket(struct mtl_main_impl* impl, int quota_mbs,
                                          enum mt_sch_type type, mt_sch_mask_t mask,
                                          int socket) {
  int ret, idx;
  struct mtl_sch_impl* sch;
  struct mt_sch_mgr* mgr = mt_sch_get_mgr(impl);

  sch_mgr_lock(mgr);

  /* first try to find one sch capable with quota */
  for (idx = 0; idx < MT_MAX_SCH_NUM; idx++) {
    sch = mt_sch_instance(impl, idx);
    if (socket != mt_sch_socket_id(sch)) continue;
    /* mask check */
    if (!(mask & MTL_BIT64(idx))) continue;
    /* active and busy check */
    if (!mt_sch_is_active(sch) || sch->cpu_busy) continue;
    /* quota check */
    if (!sch_is_capable(sch, quota_mbs, type)) continue;
    ret = mt_sch_add_quota(sch, quota_mbs);
    if (ret >= 0) {
      info("%s(%d), succ with quota_mbs %d socket %d\n", __func__, idx, quota_mbs,
           socket);
      mt_atomic32_inc(&sch->ref_cnt);
      sch_mgr_unlock(mgr);
      return sch;
    }
  }

  /* no quota, try to create one */
  sch = sch_request(impl, type, mask, NULL, socket);
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

  mt_atomic32_inc(&sch->ref_cnt);
  sch_mgr_unlock(mgr);
  return sch;
}

int mt_sch_start_all(struct mtl_main_impl* impl) {
  int ret = 0;
  struct mtl_sch_impl* sch;

  /* start active sch */
  for (int sch_idx = 0; sch_idx < MT_MAX_SCH_NUM; sch_idx++) {
    sch = mt_sch_instance(impl, sch_idx);
    /* not start the app created sch, app should do the mtl_sch_start */
    if (sch->type == MT_SCH_TYPE_APP) continue;

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
  struct mtl_sch_impl* sch;

  /* stop active sch */
  for (int sch_idx = 0; sch_idx < MT_MAX_SCH_NUM; sch_idx++) {
    sch = mt_sch_instance(impl, sch_idx);
    /* not stop the app created sch, app should do the mtl_sch_stop */
    if (sch->type == MT_SCH_TYPE_APP) continue;

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

int mtl_lcore_shm_print(void) {
  struct mt_lcore_mgr lcore_mgr;
  struct mt_lcore_shm_entry* shm_entry;

  int ret = sch_lcore_shm_init(&lcore_mgr, false);
  if (ret < 0) return ret;

  struct mt_lcore_shm* lcore_shm = lcore_mgr.lcore_shm;
  info("%s, MTL used lcores %d\n", __func__, lcore_shm->used);

  int cpu_ids[RTE_MAX_LCORE];
  int found = 0;

  for (int i = 0; i < RTE_MAX_LCORE; i++) {
    shm_entry = &lcore_shm->lcores_info[i];

    if (!shm_entry->active) continue;
    struct mt_user_info* u_info = &shm_entry->u_info;
    info("%s, lcore %d active by %s@%s, pid: %d(comm: %s) type: %s\n", __func__, i,
         u_info->user, u_info->hostname, (int)shm_entry->pid, u_info->comm,
         lcore_type_name(shm_entry->type));
    cpu_ids[found] = i;
    found++;
  }

  if (found > 0) { /* read the cpu usage */
    struct mt_cpu_usage prev[found];
    struct mt_cpu_usage cur[found];

    info("%s, collecting cpu usage...\n", __func__);
    ret = mt_read_cpu_usage(prev, cpu_ids, found);
    if (ret != found) {
      err("%s, read cpu prev usage fail, expect %d but only %d get\n", __func__, found,
          ret);
    } else {
      mt_sleep_ms(1000 * 1);
      ret = mt_read_cpu_usage(cur, cpu_ids, found);
      if (ret != found) {
        err("%s, read cpu curr usage fail, expect %d but only %d get\n", __func__, found,
            ret);
      } else {
        /* print the result */
        for (int i = 0; i < found; i++) {
          double usage = mt_calculate_cpu_usage(&prev[i], &cur[i]);
          info("%s, lcore %d cpu usage %.2f%%\n", __func__, cpu_ids[i], usage);
        }
      }
    }
  }

  sch_lcore_shm_uinit(&lcore_mgr);
  return 0;
}

#ifdef WINDOWSENV
static int lcore_shm_clean_auto_pid(struct mt_lcore_mgr* lcore_mgr) {
  MTL_MAY_UNUSED(lcore_mgr);
  err("%s, not support on windows\n", __func__);
  return -EINVAL;
}
#else

static int lcore_shm_clean_auto_pid(struct mt_lcore_mgr* lcore_mgr) {
  struct mt_user_info info;
  int clean = 0;

  memset(&info, 0, sizeof(info));
  mt_user_info_init(&info);

  struct mt_lcore_shm* lcore_shm = lcore_mgr->lcore_shm;
  struct mt_lcore_shm_entry* shm_entry;
  for (int i = 0; i < RTE_MAX_LCORE; i++) {
    shm_entry = &lcore_shm->lcores_info[i];

    if (!shm_entry->active) continue;
    struct mt_user_info* u_info = &shm_entry->u_info;
    if (0 != strncmp(u_info->hostname, info.hostname, sizeof(u_info->hostname))) continue;
    if (0 != strncmp(u_info->user, info.user, sizeof(u_info->user))) continue;
    /* now check if PID is active with zero signal */
    int result = kill(shm_entry->pid, 0);
    if (0 == result) continue;
    clean++;
    notice("%s, delete dead lcore %d from the shared mem, PID %d\n", __func__, i,
           (int)shm_entry->pid);
  }

  return clean;
}
#endif

static int lcore_shm_clean_id(struct mt_lcore_mgr* lcore_mgr, void* args,
                              size_t args_sz) {
  struct mtl_lcore_clean_pid_info* info = args;

  if (!args) {
    err("%s, NULL args\n", __func__);
    return -EINVAL;
  }
  if (args_sz != sizeof(*info)) {
    err("%s, error args_sz %" PRIu64 "\n", __func__, args_sz);
    return -EINVAL;
  }
  uint32_t lcore = info->lcore;
  if (lcore >= RTE_MAX_LCORE) {
    err("%s, invalid lcore %u\n", __func__, lcore);
    return -EINVAL;
  }

  struct mt_lcore_shm* lcore_shm = lcore_mgr->lcore_shm;
  struct mt_lcore_shm_entry* shm_entry = &lcore_shm->lcores_info[lcore];
  if (!shm_entry->active) {
    err("%s, lcore %u is inactive\n", __func__, lcore);
    return -EINVAL;
  }

  shm_entry->active = false;
  notice("%s, delete lcore %u from the shared mem, PID %d\n", __func__, lcore,
         (int)shm_entry->pid);
  return 0;
}

int mtl_lcore_shm_clean(enum mtl_lcore_clean_action action, void* args, size_t args_sz) {
  struct mt_lcore_mgr lcore_mgr;

  int ret = sch_lcore_shm_init(&lcore_mgr, false);
  if (ret < 0) return ret;

  MTL_MAY_UNUSED(args);
  MTL_MAY_UNUSED(args_sz);
  switch (action) {
    case MTL_LCORE_CLEAN_PID_AUTO_CHECK:
      ret = lcore_shm_clean_auto_pid(&lcore_mgr);
      break;
    case MTL_LCORE_CLEAN_LCORE:
      ret = lcore_shm_clean_id(&lcore_mgr, args, args_sz);
      break;
    default:
      err("%s, unknown action %d\n", __func__, action);
      ret = -EINVAL;
      break;
  }

  sch_lcore_shm_uinit(&lcore_mgr);
  return ret;
}

/* below for public interface for application */

mtl_sch_handle mtl_sch_create(mtl_handle mt, struct mtl_sch_ops* ops) {
  struct mtl_main_impl* impl = mt;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  if (!ops) {
    err("%s, NULL ops\n", __func__);
    return NULL;
  }

  /* request sch on the MTL_PORT_P socket */
  struct mtl_sch_impl* sch = sch_request(impl, MT_SCH_TYPE_APP, MT_SCH_MASK_ALL, ops,
                                         mt_socket_id(impl, MTL_PORT_P));
  if (!sch) {
    err("%s, sch request fail\n", __func__);
    return NULL;
  }

  info("%s, succ on %d\n", __func__, sch->idx);
  return sch;
}

int mtl_sch_free(mtl_sch_handle sch) {
  int idx = sch->idx;
  /* stop incase user not dp the mtl_sch_stop */
  if (mt_sch_started(sch)) sch_stop(sch);
  int ret = sch_free(sch);
  if (ret < 0) {
    err("%s(%d), sch free fail %d\n", __func__, idx, ret);
    return ret;
  }
  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

int mtl_sch_start(mtl_sch_handle sch) {
  int idx = sch->idx;
  int ret = sch_start(sch);
  if (ret < 0) {
    err("%s(%d), sch start fail %d\n", __func__, idx, ret);
    return ret;
  }
  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

int mtl_sch_stop(mtl_sch_handle sch) {
  int idx = sch->idx;
  int ret = sch_stop(sch);
  if (ret < 0) {
    err("%s(%d), sch start fail %d\n", __func__, idx, ret);
    return ret;
  }
  info("%s(%d), succ\n", __func__, idx);
  return 0;
}
