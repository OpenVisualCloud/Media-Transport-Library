/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_stat.h"

// #define DEBUG
#include "mt_log.h"

static inline struct mt_stat_mgr* get_stat_mgr(struct mtl_main_impl* impl) {
  return &impl->stat_mgr;
}

int mt_stat_dump(struct mtl_main_impl* impl) {
  struct mt_stat_mgr* mgr = get_stat_mgr(impl);
  struct mt_stat_item* item;

  mt_pthread_mutex_lock(&mgr->mutex);
  MT_TAILQ_FOREACH(item, &mgr->head, next) { item->cb_func(item->cb_priv); }
  mt_pthread_mutex_unlock(&mgr->mutex);

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
  if (name) strncpy(item->name, name, ST_MAX_NAME_LEN - 1);

  mt_pthread_mutex_lock(&mgr->mutex);
  MT_TAILQ_INSERT_TAIL(&mgr->head, item, next);
  mt_pthread_mutex_unlock(&mgr->mutex);

  dbg("%s, succ, priv %p\n", __func__, priv);
  return 0;
}

int mt_stat_unregister(struct mtl_main_impl* impl, mt_stat_cb_t cb, void* priv) {
  struct mt_stat_mgr* mgr = get_stat_mgr(impl);
  struct mt_stat_item *item, *tmp_item;

  mt_pthread_mutex_lock(&mgr->mutex);
  for (item = MT_TAILQ_FIRST(&mgr->head); item != NULL; item = tmp_item) {
    tmp_item = MT_TAILQ_NEXT(item, next);
    if ((item->cb_func == cb && item->cb_priv == priv)) {
      /* found the matched item, remove it */
      MT_TAILQ_REMOVE(&mgr->head, item, next);
      mt_pthread_mutex_unlock(&mgr->mutex);
      mt_rte_free(item);
      dbg("%s, succ, priv %p\n", __func__, priv);
      return 0;
    }
  }
  mt_pthread_mutex_unlock(&mgr->mutex);

  warn("%s, cb %p priv %p not found\n", __func__, cb, priv);
  return -EIO;
}

int mt_stat_init(struct mtl_main_impl* impl) {
  struct mt_stat_mgr* mgr = get_stat_mgr(impl);

  mt_pthread_mutex_init(&mgr->mutex, NULL);
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

  mt_pthread_mutex_destroy(&mgr->mutex);

  return 0;
}
