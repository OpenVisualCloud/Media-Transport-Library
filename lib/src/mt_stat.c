/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_stat.h"

// #define DEBUG
#include "mt_log.h"

#define MT_STAT_INTERVAL_S_DEFAULT (4) /* 10s */

static inline struct mt_stat_mgr* get_stat_mgr(struct mtl_main_impl* impl) {
  return &impl->stat_mgr;
}

static inline void stat_lock(struct mt_stat_mgr* mgr) {
  rte_spinlock_lock(&mgr->lock);
}

/* return true if try lock succ */
static inline bool stat_try_lock(struct mt_stat_mgr* mgr) {
  int ret = rte_spinlock_trylock(&mgr->lock);
  return ret ? true : false;
}

static inline void stat_unlock(struct mt_stat_mgr* mgr) {
  rte_spinlock_unlock(&mgr->lock);
}

static int _stat_dump(struct mt_stat_mgr* mgr) {
  struct mt_stat_item* item;

  if (!stat_try_lock(mgr)) {
    notice("STAT: failed to get lock\n");
    return -EIO;
  }
  MT_TAILQ_FOREACH(item, &mgr->head, next) {
    item->cb_func(item->cb_priv);
  }
  stat_unlock(mgr);

  return 0;
}

static void stat_dump(struct mt_stat_mgr* mgr) {
  struct mtl_main_impl* impl = mgr->parent;
  struct mtl_init_params* p = mt_get_user_params(impl);

  if (mt_in_reset(impl)) {
    notice("* *    M T    D E V   I N   R E S E T   * * \n");
    return;
  }

  notice("* *    M T    D E V   S T A T E   * * \n");
  _stat_dump(mgr);
  if (p->stat_dump_cb_fn) {
    dbg("%s, start stat_dump_cb_fn\n", __func__);
    p->stat_dump_cb_fn(p->priv);
  }
  notice("* *    E N D    S T A T E   * * \n\n");
}

static void* stat_thread(void* arg) {
  struct mt_stat_mgr* mgr = arg;

  info("%s, start\n", __func__);
  while (rte_atomic32_read(&mgr->stat_stop) == 0) {
    mt_pthread_mutex_lock(&mgr->stat_wake_mutex);
    if (!rte_atomic32_read(&mgr->stat_stop))
      mt_pthread_cond_wait(&mgr->stat_wake_cond, &mgr->stat_wake_mutex);
    mt_pthread_mutex_unlock(&mgr->stat_wake_mutex);

    if (!rte_atomic32_read(&mgr->stat_stop)) {
      dbg("%s, stat_dump\n", __func__);
      stat_dump(mgr);
    }
  }
  info("%s, stop\n", __func__);

  return NULL;
}

static void stat_wakeup_thread(struct mt_stat_mgr* mgr) {
  mt_pthread_mutex_lock(&mgr->stat_wake_mutex);
  mt_pthread_cond_signal(&mgr->stat_wake_cond);
  mt_pthread_mutex_unlock(&mgr->stat_wake_mutex);
}

static void stat_alarm_handler(void* param) {
  struct mt_stat_mgr* mgr = param;

  if (mgr->stat_tid)
    stat_wakeup_thread(mgr);
  else
    stat_dump(mgr);

  rte_eal_alarm_set(mgr->dump_period_us, stat_alarm_handler, mgr);
}

int mt_stat_register(struct mtl_main_impl* impl, mt_stat_cb_t cb, void* priv,
                     char* name) {
  struct mt_stat_mgr* mgr = get_stat_mgr(impl);
  struct mt_stat_item* item =
      mt_rte_zmalloc_socket(sizeof(*item), mt_socket_id(impl, MTL_PORT_P));
  if (!item) {
    err("%s, malloc fail\n", __func__);
    return -ENOMEM;
  }
  item->cb_func = cb;
  item->cb_priv = priv;
  if (name) snprintf(item->name, ST_MAX_NAME_LEN - 1, "%s", name);

  stat_lock(mgr);
  MT_TAILQ_INSERT_TAIL(&mgr->head, item, next);
  stat_unlock(mgr);

  dbg("%s, succ, priv %p\n", __func__, priv);
  return 0;
}

int mt_stat_unregister(struct mtl_main_impl* impl, mt_stat_cb_t cb, void* priv) {
  struct mt_stat_mgr* mgr = get_stat_mgr(impl);
  struct mt_stat_item *item, *tmp_item;

  stat_lock(mgr);
  for (item = MT_TAILQ_FIRST(&mgr->head); item != NULL; item = tmp_item) {
    tmp_item = MT_TAILQ_NEXT(item, next);
    if ((item->cb_func == cb && item->cb_priv == priv)) {
      /* found the matched item, remove it */
      MT_TAILQ_REMOVE(&mgr->head, item, next);
      stat_unlock(mgr);
      mt_rte_free(item);
      dbg("%s, succ, priv %p\n", __func__, priv);
      return 0;
    }
  }
  stat_unlock(mgr);

  warn("%s, cb %p priv %p not found\n", __func__, cb, priv);
  return -EIO;
}

int mt_stat_init(struct mtl_main_impl* impl) {
  struct mt_stat_mgr* mgr = get_stat_mgr(impl);
  struct mtl_init_params* p = mt_get_user_params(impl);
  int ret;

  mgr->parent = impl;
  rte_spinlock_init(&mgr->lock);
  MT_TAILQ_INIT(&mgr->head);

  /* rte_eth_stats_get fail in alarm context for VF, move it to thread */
  mt_pthread_mutex_init(&mgr->stat_wake_mutex, NULL);
  mt_pthread_cond_init(&mgr->stat_wake_cond, NULL);
  rte_atomic32_set(&mgr->stat_stop, 0);
  ret = pthread_create(&mgr->stat_tid, NULL, stat_thread, mgr);
  if (ret < 0) {
    err("%s, pthread_create fail %d\n", __func__, ret);
    return ret;
  }
  mtl_thread_setname(mgr->stat_tid, "mtl_stat");

  if (!p->dump_period_s) p->dump_period_s = MT_STAT_INTERVAL_S_DEFAULT;
  mgr->dump_period_us = (uint64_t)p->dump_period_s * US_PER_S;
  rte_eal_alarm_set(mgr->dump_period_us, stat_alarm_handler, mgr);

  info("%s, stat period %us\n", __func__, p->dump_period_s);
  return 0;
}

int mt_stat_uinit(struct mtl_main_impl* impl) {
  struct mt_stat_mgr* mgr = get_stat_mgr(impl);
  struct mt_stat_item* item;
  int ret;

  /* check if any not unregister */
  while ((item = MT_TAILQ_FIRST(&mgr->head))) {
    warn("%s, %p(%s) not unregister\n", __func__, item->cb_priv, item->name);
    MT_TAILQ_REMOVE(&mgr->head, item, next);
    mt_rte_free(item);
  }

  ret = rte_eal_alarm_cancel(stat_alarm_handler, (void*)-1);
  if (ret < 0) err("%s, alarm cancel fail %d\n", __func__, ret);
  if (mgr->stat_tid) {
    rte_atomic32_set(&mgr->stat_stop, 1);
    stat_wakeup_thread(mgr);
    pthread_join(mgr->stat_tid, NULL);
    mgr->stat_tid = 0;
  }
  mt_pthread_mutex_destroy(&mgr->stat_wake_mutex);
  mt_pthread_cond_destroy(&mgr->stat_wake_cond);

  return 0;
}
