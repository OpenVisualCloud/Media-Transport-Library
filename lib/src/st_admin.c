/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_admin.h"

#include "st_log.h"
#include "st_rx_video_session.h"
#include "st_sch.h"
#include "st_tx_video_session.h"

static inline struct st_admin* st_get_admin(struct st_main_impl* impl) {
  return &impl->admin;
}

static int admin_cal_cpu_busy(struct st_main_impl* impl) {
  struct st_sch_impl* sch;
  struct st_tx_video_sessions_mgr* tx_mgr;
  struct st_tx_video_session_impl* tx_s;
  struct st_rx_video_sessions_mgr* rx_mgr;
  struct st_rx_video_session_impl* rx_s;

  for (int sch_idx = 0; sch_idx < ST_MAX_SCH_NUM; sch_idx++) {
    sch = st_sch_instance(impl, sch_idx);
    if (!st_sch_started(sch)) continue;

    /* cal tx video cpu */
    tx_mgr = &sch->tx_video_mgr;
    for (int j = 0; j < tx_mgr->max_idx; j++) {
      tx_s = tx_video_session_get(tx_mgr, j);
      if (tx_s) {
        tx_video_session_cal_cpu_busy(tx_s);
        tx_video_session_put(tx_mgr, j);
      }
    }

    /* cal rx video cpu */
    rx_mgr = &sch->rx_video_mgr;
    for (int j = 0; j < rx_mgr->max_idx; j++) {
      rx_s = rx_video_session_get(rx_mgr, j);
      if (rx_s) {
        rx_video_session_cal_cpu_busy(rx_s);
        rx_video_session_put(rx_mgr, j);
      }
    }
  }

  return 0;
}

static int admin_clear_cpu_busy(struct st_main_impl* impl) {
  struct st_sch_impl* sch;
  struct st_tx_video_sessions_mgr* tx_mgr;
  struct st_tx_video_session_impl* tx_s;
  struct st_rx_video_sessions_mgr* rx_mgr;
  struct st_rx_video_session_impl* rx_s;

  for (int sch_idx = 0; sch_idx < ST_MAX_SCH_NUM; sch_idx++) {
    sch = st_sch_instance(impl, sch_idx);
    if (!st_sch_started(sch)) continue;

    /* cal tx video cpu */
    tx_mgr = &sch->tx_video_mgr;
    for (int j = 0; j < tx_mgr->max_idx; j++) {
      tx_s = tx_video_session_get(tx_mgr, j);
      if (tx_s) {
        tx_video_session_clear_cpu_busy(tx_s);
        tx_video_session_put(tx_mgr, j);
      }
    }

    /* cal rx video cpu */
    rx_mgr = &sch->rx_video_mgr;
    for (int j = 0; j < rx_mgr->max_idx; j++) {
      rx_s = rx_video_session_get(rx_mgr, j);
      if (rx_s) {
        rx_video_session_clear_cpu_busy(rx_s);
        rx_video_session_put(rx_mgr, j);
      }
    }
  }

  return 0;
}

static inline int tx_video_quota_mbs(struct st_tx_video_session_impl* s) {
  if (s->st22_handle)
    return s->st22_handle->quota_mbs;
  else
    return s->st20_handle->quota_mbs;
}

static inline struct st_sch_impl* tx_video_get_sch(struct st_tx_video_session_impl* s) {
  if (s->st22_handle)
    return s->st22_handle->sch;
  else
    return s->st20_handle->sch;
}

static inline void tx_video_set_sch(struct st_tx_video_session_impl* s,
                                    struct st_sch_impl* sch) {
  if (s->st22_handle)
    s->st22_handle->sch = sch;
  else
    s->st20_handle->sch = sch;
}

static int tx_video_migrate_to(struct st_main_impl* impl,
                               struct st_tx_video_session_impl* s,
                               struct st_sch_impl* from_sch, struct st_sch_impl* to_sch) {
  struct st_tx_video_sessions_mgr* to_tx_mgr = &to_sch->tx_video_mgr;
  int to_midx = to_tx_mgr->idx;
  struct st_tx_video_sessions_mgr* from_tx_mgr = &from_sch->tx_video_mgr;
  int from_midx = from_tx_mgr->idx;
  int from_idx = s->idx;

  st_pthread_mutex_lock(&to_sch->tx_video_mgr_mutex);
  st_pthread_mutex_lock(&from_sch->tx_video_mgr_mutex);
  if (!tx_video_session_get(from_tx_mgr, from_idx)) {
    err("%s, get session(%d,%d) fail\n", __func__, from_midx, from_idx);
    st_pthread_mutex_unlock(&from_sch->tx_video_mgr_mutex);
    st_pthread_mutex_unlock(&to_sch->tx_video_mgr_mutex);
    return -EIO;
  }
  int i;
  /* find one empty slot in the new sch */
  for (i = 0; i < ST_SCH_MAX_TX_VIDEO_SESSIONS; i++) {
    if (!tx_video_session_get_empty(to_tx_mgr, i)) continue;
    /* remove from old sch */
    from_tx_mgr->sessions[from_idx] = NULL;
    /* migrate resource */
    st_tx_video_session_migrate(impl, to_tx_mgr, s, i);
    /* link to new sch */
    to_tx_mgr->sessions[i] = s;
    to_tx_mgr->max_idx = RTE_MAX(to_tx_mgr->max_idx, i + 1);
    tx_video_set_sch(s, to_sch);
    tx_video_session_put(to_tx_mgr, i);
    break;
  }
  tx_video_session_put(from_tx_mgr, from_idx);
  st_pthread_mutex_unlock(&from_sch->tx_video_mgr_mutex);
  st_pthread_mutex_unlock(&to_sch->tx_video_mgr_mutex);

  info("%s, session(%d,%d,%f) move to (%d,%d)\n", __func__, from_midx, from_idx,
       tx_video_session_get_cpu_busy(s), to_midx, i);

  return 0;
}

static int admin_tx_video_migrate(struct st_main_impl* impl, bool* migrated) {
  struct st_tx_video_session_impl* busy_s = NULL;
  struct st_sch_impl* from_sch = NULL;
  int ret;

  for (int sch_idx = 0; sch_idx < ST_MAX_SCH_NUM; sch_idx++) {
    struct st_sch_impl* sch = st_sch_instance(impl, sch_idx);
    if (!st_sch_started(sch)) continue;
    if (!st_sch_has_busy(sch)) continue;

    /* check if any busy session in this cpu */
    struct st_tx_video_session_impl* busy_s_in_sch = NULL;
    struct st_tx_video_sessions_mgr* tx_mgr = &sch->tx_video_mgr;
    for (int j = 0; j < tx_mgr->max_idx; j++) {
      struct st_tx_video_session_impl* tx_s = tx_video_session_get(tx_mgr, j);
      if (!tx_s) continue;
      if (tx_video_session_is_cpu_busy(tx_s)) {
        busy_s_in_sch = tx_s; /* last one as the busy one */
      }
      tx_video_session_put(tx_mgr, j);
    }

    if (busy_s_in_sch) {
      st_sch_set_cpu_busy(sch, true);
      busy_s = busy_s_in_sch; /* last one as the busy one */
      from_sch = sch;
    }
  }

  if (!busy_s) return 0; /* no busy session */

  int quota_mbs = tx_video_quota_mbs(busy_s);
  if (quota_mbs >= from_sch->data_quota_mbs_total) {
    return 0; /* only one video session in this sch */
  }

  dbg("%s, find one busy session(%d,%d)\n", __func__, from_sch->idx, busy_s->idx);
  struct st_sch_impl* to_sch = st_sch_get(impl, quota_mbs, from_sch->type);
  if (!to_sch) {
    err("%s, no idle sch for session(%d,%d)\n", __func__, from_sch->idx, busy_s->idx);
    return -EIO;
  }

  st_pthread_mutex_lock(&to_sch->tx_video_mgr_mutex);
  st_tx_video_sessions_sch_init(impl, to_sch); /* ensure video sch context */
  st_pthread_mutex_unlock(&to_sch->tx_video_mgr_mutex);

  ret = tx_video_migrate_to(impl, busy_s, from_sch, to_sch);
  if (ret < 0) {
    err("%s, session(%d,%d) migrate to fail\n", __func__, from_sch->idx, busy_s->idx);
    st_sch_put(to_sch, quota_mbs);
    return ret;
  }
  *migrated = true;
  return 0;
}

static inline int rx_video_quota_mbs(struct st_rx_video_session_impl* s) {
  if (s->st22_handle)
    return s->st22_handle->quota_mbs;
  else
    return s->st20_handle->quota_mbs;
}

static inline struct st_sch_impl* rx_video_get_sch(struct st_rx_video_session_impl* s) {
  if (s->st22_handle)
    return s->st22_handle->sch;
  else
    return s->st20_handle->sch;
}

static inline void rx_video_set_sch(struct st_rx_video_session_impl* s,
                                    struct st_sch_impl* sch) {
  if (s->st22_handle)
    s->st22_handle->sch = sch;
  else
    s->st20_handle->sch = sch;
}

static int rx_video_migrate_to(struct st_main_impl* impl,
                               struct st_rx_video_session_impl* s,
                               struct st_sch_impl* from_sch, struct st_sch_impl* to_sch) {
  struct st_rx_video_sessions_mgr* to_rx_mgr = &to_sch->rx_video_mgr;
  int to_midx = to_rx_mgr->idx;
  struct st_rx_video_sessions_mgr* from_rx_mgr = &from_sch->rx_video_mgr;
  int from_midx = from_rx_mgr->idx;
  int from_idx = s->idx;

  st_pthread_mutex_lock(&to_sch->rx_video_mgr_mutex);
  st_pthread_mutex_lock(&from_sch->rx_video_mgr_mutex);
  if (!rx_video_session_get(from_rx_mgr, from_idx)) {
    err("%s, get session(%d,%d) fail\n", __func__, from_midx, from_idx);
    st_pthread_mutex_unlock(&from_sch->rx_video_mgr_mutex);
    st_pthread_mutex_unlock(&to_sch->rx_video_mgr_mutex);
    return -EIO;
  }
  int i;
  /* find one empty slot in the new sch */
  for (i = 0; i < ST_SCH_MAX_RX_VIDEO_SESSIONS; i++) {
    if (!rx_video_session_get_empty(to_rx_mgr, i)) continue;
    /* remove from old sch */
    from_rx_mgr->sessions[from_idx] = NULL;
    /* migrate resource */
    st_rx_video_session_migrate(impl, to_rx_mgr, s, i);
    /* link to new sch */
    to_rx_mgr->sessions[i] = s;
    to_rx_mgr->max_idx = RTE_MAX(to_rx_mgr->max_idx, i + 1);
    rx_video_set_sch(s, to_sch);
    rx_video_session_put(to_rx_mgr, i);
    break;
  }
  rx_video_session_put(from_rx_mgr, from_idx);
  st_pthread_mutex_unlock(&from_sch->rx_video_mgr_mutex);
  st_pthread_mutex_unlock(&to_sch->rx_video_mgr_mutex);

  info("%s, session(%d,%d,%f) move to (%d,%d)\n", __func__, from_midx, from_idx,
       rx_video_session_get_cpu_busy(s), to_midx, i);

  return 0;
}

static int admin_rx_video_migrate(struct st_main_impl* impl, bool* migrated) {
  struct st_rx_video_session_impl* busy_s = NULL;
  struct st_sch_impl* from_sch = NULL;
  int ret;

  for (int sch_idx = 0; sch_idx < ST_MAX_SCH_NUM; sch_idx++) {
    struct st_sch_impl* sch = st_sch_instance(impl, sch_idx);
    if (!st_sch_started(sch)) continue;
    if (!st_sch_has_busy(sch)) continue;

    /* check if any busy session in this cpu */
    struct st_rx_video_session_impl* busy_s_in_sch = NULL;
    struct st_rx_video_sessions_mgr* rx_mgr = &sch->rx_video_mgr;
    for (int j = 0; j < rx_mgr->max_idx; j++) {
      struct st_rx_video_session_impl* rx_s = rx_video_session_get(rx_mgr, j);
      if (!rx_s) continue;
      if (rx_video_session_is_cpu_busy(rx_s)) {
        busy_s_in_sch = rx_s; /* last one as the busy one */
      }
      rx_video_session_put(rx_mgr, j);
    }

    if (busy_s_in_sch) {
      st_sch_set_cpu_busy(sch, true);
      busy_s = busy_s_in_sch; /* last one as the busy one */
      from_sch = sch;
    }
  }

  if (!busy_s) return 0; /* no busy session */

  int quota_mbs = rx_video_quota_mbs(busy_s);
  if (quota_mbs >= from_sch->data_quota_mbs_total) {
    return 0; /* only one video session in this sch */
  }

  dbg("%s, find one busy session(%d,%d)\n", __func__, from_sch->idx, busy_s->idx);
  struct st_sch_impl* to_sch = st_sch_get(impl, quota_mbs, from_sch->type);
  if (!to_sch) {
    err("%s, no idle sch for session(%d,%d)\n", __func__, from_sch->idx, busy_s->idx);
    return -EIO;
  }

  st_pthread_mutex_lock(&to_sch->rx_video_mgr_mutex);
  st_rx_video_sessions_sch_init(impl, to_sch); /* ensure video sch context */
  st_pthread_mutex_unlock(&to_sch->rx_video_mgr_mutex);

  ret = rx_video_migrate_to(impl, busy_s, from_sch, to_sch);
  if (ret < 0) {
    err("%s, session(%d,%d) migrate fail\n", __func__, from_sch->idx, busy_s->idx);
    st_sch_put(to_sch, quota_mbs);
    return ret;
  }
  *migrated = true;
  return 0;
}

static void admin_wakeup_thread(struct st_admin* admin) {
  st_pthread_mutex_lock(&admin->admin_wake_mutex);
  st_pthread_cond_signal(&admin->admin_wake_cond);
  st_pthread_mutex_unlock(&admin->admin_wake_mutex);
}

static void admin_alarm_handler(void* param) {
  struct st_main_impl* impl = param;
  struct st_admin* admin = st_get_admin(impl);

  admin_wakeup_thread(admin);
}

static int admin_func(struct st_main_impl* impl) {
  struct st_admin* admin = st_get_admin(impl);

  dbg("%s, start\n", __func__);

  admin_cal_cpu_busy(impl);

  bool migrated = false;
  /* only one migrate(both tx and rx) for this period */
  if (st_has_tx_video_migrate(impl)) {
    admin_tx_video_migrate(impl, &migrated);
  }
  if (!migrated && st_has_rx_video_migrate(impl)) {
    admin_rx_video_migrate(impl, &migrated);
  }

  if (migrated) admin_clear_cpu_busy(impl);

  rte_eal_alarm_set(admin->period_us, admin_alarm_handler, impl);

  return 0;
}

static void* admin_thread(void* arg) {
  struct st_main_impl* impl = arg;
  struct st_admin* admin = st_get_admin(impl);

  info("%s, start\n", __func__);
  while (rte_atomic32_read(&admin->admin_stop) == 0) {
    st_pthread_mutex_lock(&admin->admin_wake_mutex);
    if (!rte_atomic32_read(&admin->admin_stop))
      st_pthread_cond_wait(&admin->admin_wake_cond, &admin->admin_wake_mutex);
    st_pthread_mutex_unlock(&admin->admin_wake_mutex);

    if (!rte_atomic32_read(&admin->admin_stop)) admin_func(impl);
  }
  info("%s, stop\n", __func__);

  return NULL;
}

int st_admin_init(struct st_main_impl* impl) {
  struct st_admin* admin = st_get_admin(impl);

  admin->period_us = 5 * US_PER_S; /* 5s */
  st_pthread_mutex_init(&admin->admin_wake_mutex, NULL);
  st_pthread_cond_init(&admin->admin_wake_cond, NULL);
  rte_atomic32_set(&admin->admin_stop, 0);

  pthread_create(&admin->admin_tid, NULL, admin_thread, impl);
  rte_eal_alarm_set(admin->period_us, admin_alarm_handler, impl);

  return 0;
}

int st_admin_uinit(struct st_main_impl* impl) {
  struct st_admin* admin = st_get_admin(impl);

  if (admin->admin_tid) {
    rte_atomic32_set(&admin->admin_stop, 1);
    admin_wakeup_thread(admin);
    pthread_join(admin->admin_tid, NULL);
    admin->admin_tid = 0;
  }
  rte_eal_alarm_cancel(admin_alarm_handler, impl);

  st_pthread_mutex_destroy(&admin->admin_wake_mutex);
  st_pthread_cond_destroy(&admin->admin_wake_cond);
  return 0;
}
