/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_stat.h"

// #define DEBUG
#include "mt_log.h"

static inline struct mt_stat_mgr* get_stat_mgr(struct mtl_main_impl* impl) {
  return &impl->stat_mgr;
}

static inline void stat_lock(struct mt_stat_mgr* mgr) { rte_spinlock_lock(&mgr->lock); }

/* return true if try lock succ */
static inline bool stat_try_lock(struct mt_stat_mgr* mgr) {
  int ret = rte_spinlock_trylock(&mgr->lock);
  return ret ? true : false;
}

static inline void stat_unlock(struct mt_stat_mgr* mgr) {
  rte_spinlock_unlock(&mgr->lock);
}

int mt_stat_dump(struct mtl_main_impl* impl) {
  struct mt_stat_mgr* mgr = get_stat_mgr(impl);
  struct mt_stat_item* item;

  if (!stat_try_lock(mgr)) {
    notice("STAT: failed to get lock\n");
    return -EIO;
  }
  MT_TAILQ_FOREACH(item, &mgr->head, next) { item->cb_func(item->cb_priv); }
  stat_unlock(mgr);

  return 0;
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

  rte_spinlock_init(&mgr->lock);
  MT_TAILQ_INIT(&mgr->head);

  return 0;
}

int mt_stat_uinit(struct mtl_main_impl* impl) {
  struct mt_stat_mgr* mgr = get_stat_mgr(impl);
  struct mt_stat_item* item;

  /* check if any not unregister */
  while ((item = MT_TAILQ_FIRST(&mgr->head))) {
    warn("%s, %p(%s) not unregister\n", __func__, item->cb_priv, item->name);
    MT_TAILQ_REMOVE(&mgr->head, item, next);
    mt_free(item);
  }

  return 0;
}
