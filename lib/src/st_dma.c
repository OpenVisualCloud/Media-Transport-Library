/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_dma.h"

#include "st_log.h"

static inline struct st_map_mgr* st_get_map_mgr(struct mtl_main_impl* impl) {
  return &impl->map_mgr;
}

int st_map_add(struct mtl_main_impl* impl, struct st_map_item* item) {
  struct st_map_mgr* mgr = st_get_map_mgr(impl);
  void* start = item->vaddr;
  void* end = start + item->size;
  struct st_map_item* i_item;
  void* i_start;
  void* i_end;
  mtl_iova_t iova_base = 0x10000; /* assume user IOVA start from 1M */
  mtl_iova_t iova_end;

  st_pthread_mutex_lock(&mgr->mutex);

  /* first check if any conflict with exist mapping */
  for (int i = 0; i < ST_MAP_MAX_ITEMS; i++) {
    i_item = mgr->items[i];
    if (!i_item) continue;
    i_start = i_item->vaddr;
    i_end = i_start + i_item->size;
    if ((start >= i_start) && (start <= i_end)) {
      err("%s, invalid start %p i_start %p i_end %p\n", __func__, start, i_start, i_end);
      st_pthread_mutex_unlock(&mgr->mutex);
      return -EINVAL;
    }
    if ((end >= i_start) && (end <= i_end)) {
      err("%s, invalid end %p i_start %p i_end %p\n", __func__, start, i_start, i_end);
      st_pthread_mutex_unlock(&mgr->mutex);
      return -EINVAL;
    }
    /* simply set iova_base to max iova in previous item */
    iova_end = i_item->iova + i_item->size;
    if (iova_end > iova_base) iova_base = iova_end;
  }
  item->iova = iova_base;

  /* find empty slot and insert */
  for (int i = 0; i < ST_MAP_MAX_ITEMS; i++) {
    i_item = mgr->items[i];
    if (i_item) continue;
    i_item = st_rte_zmalloc_socket(sizeof(*i_item), st_socket_id(impl, MTL_PORT_P));
    if (!i_item) {
      err("%s, i_item malloc fail\n", __func__);
      st_pthread_mutex_unlock(&mgr->mutex);
      return -EINVAL;
    }
    *i_item = *item;
    mgr->items[i] = i_item;
    st_pthread_mutex_unlock(&mgr->mutex);
    info("%s(%d), start %p end %p iova 0x%" PRIx64 "\n", __func__, i, start, end,
         i_item->iova);
    return 0;
  }

  err("%s, no space, all items are used\n", __func__);
  st_pthread_mutex_unlock(&mgr->mutex);
  return -EIO;
}

int st_map_remove(struct mtl_main_impl* impl, struct st_map_item* item) {
  struct st_map_mgr* mgr = st_get_map_mgr(impl);
  void* start = item->vaddr;
  void* end = start + item->size;
  struct st_map_item* i_item;
  void* i_start;
  void* i_end;

  st_pthread_mutex_lock(&mgr->mutex);

  /* find slot and delete */
  for (int i = 0; i < ST_MAP_MAX_ITEMS; i++) {
    i_item = mgr->items[i];
    if (!i_item) continue;
    i_start = i_item->vaddr;
    i_end = i_start + i_item->size;
    if ((start == i_start) && (end == i_end) && (item->iova == i_item->iova)) {
      info("%s(%d), start %p end %p iova 0x%" PRIx64 "\n", __func__, i, start, end,
           i_item->iova);
      st_rte_free(i_item);
      mgr->items[i] = NULL;
      st_pthread_mutex_unlock(&mgr->mutex);
      return 0;
    }
  }

  err("%s, unknown items start %p end %p iova %" PRIx64 "\n", __func__, start, end,
      item->iova);
  st_pthread_mutex_unlock(&mgr->mutex);
  return -EIO;
}

int st_map_init(struct mtl_main_impl* impl) {
  struct st_map_mgr* mgr = st_get_map_mgr(impl);

  st_pthread_mutex_init(&mgr->mutex, NULL);

  return 0;
}

int st_map_uinit(struct mtl_main_impl* impl) {
  struct st_map_mgr* mgr = st_get_map_mgr(impl);
  struct st_map_item* item;

  for (int i = 0; i < ST_MAP_MAX_ITEMS; i++) {
    item = mgr->items[i];
    if (item) {
      warn("%s(%d), still active, vaddr %p\n", __func__, i, item->vaddr);
      st_rte_free(item);
      mgr->items[i] = NULL;
    }
  }

  st_pthread_mutex_destroy(&mgr->mutex);

  return 0;
}

/* dmadev only available from DPDK 21.11 */
#if RTE_VERSION >= RTE_VERSION_NUM(21, 11, 0, 0)
#include <rte_dmadev.h>

static void dma_copy_test(struct mtl_main_impl* impl, struct mtl_dma_lender_dev* dev,
                          uint32_t off, uint32_t len) {
  void *dst = NULL, *src = NULL;
  int idx = st_dma_dev_id(dev);

  dst = st_rte_zmalloc_socket(len, st_socket_id(impl, MTL_PORT_P));
  src = st_rte_zmalloc_socket(len, st_socket_id(impl, MTL_PORT_P));
  memset(src, 0x55, len);

  if (dst && src) {
    int ret = st_dma_copy(dev, rte_malloc_virt2iova(dst) + off,
                          rte_malloc_virt2iova(src) + off, len - off);
    dbg("%s(%d), copy ret %d off %u len %u\n", __func__, idx, ret, off, len);
    if (ret >= 0) {
      ret = st_dma_submit(dev);
      dbg("%s(%d), submit ret %d\n", __func__, idx, ret);
      if (ret >= 0) {
        uint16_t nb_dq = 0;
        while (nb_dq < 1) {
          nb_dq = st_dma_completed(dev, 32, NULL, NULL);
          dbg("%s(%d), nb_dq %d\n", __func__, idx, nb_dq);
        }
      }
    }
    info("%s(%d), result %d off %u len %u\n", __func__, idx,
         memcmp(src + off, dst + off, len - off), off, len);
  }

  if (dst) st_rte_free(dst);
  if (src) st_rte_free(src);
}

static void dma_fill_test(struct mtl_main_impl* impl, struct mtl_dma_lender_dev* dev,
                          uint32_t off, uint32_t len, uint8_t pattern) {
  void *dst = NULL, *src = NULL;
  int idx = st_dma_dev_id(dev);
  uint64_t pattern_u64 = 0;

  dst = st_rte_zmalloc_socket(len, st_socket_id(impl, MTL_PORT_P));
  src = st_rte_zmalloc_socket(len, st_socket_id(impl, MTL_PORT_P));
  memset(src, pattern, len);

  /* pattern to u64 */
  memset(&pattern_u64, pattern, sizeof(pattern_u64));

  if (dst && src) {
    int ret = st_dma_fill(dev, rte_malloc_virt2iova(dst) + off, pattern_u64, len - off);
    dbg("%s(%d), copy ret %d off %u len %u\n", __func__, idx, ret, off, len);
    if (ret >= 0) {
      ret = st_dma_submit(dev);
      dbg("%s(%d), submit ret %d\n", __func__, idx, ret);
      if (ret >= 0) {
        uint16_t nb_dq = 0;
        while (nb_dq < 1) {
          nb_dq = st_dma_completed(dev, 32, NULL, NULL);
          dbg("%s(%d), nb_dq %d\n", __func__, idx, nb_dq);
        }
      }
    }
    info("%s(%d), result %d off %u len %u\n", __func__, idx,
         memcmp(src + off, dst + off, len - off), off, len);
  }

  if (dst) st_rte_free(dst);
  if (src) st_rte_free(src);
}

static void dma_test(struct mtl_main_impl* impl) {
  struct st_dma_request_req req;
  req.nb_desc = 128;
  req.max_shared = 1;
  req.sch_idx = 0;
  req.socket_id = st_socket_id(impl, MTL_PORT_P);
  req.priv = NULL;
  req.drop_mbuf_cb = NULL;
  struct mtl_dma_lender_dev* dev = st_dma_request_dev(impl, &req);
  if (!dev) return;

  dma_copy_test(impl, dev, 0, 1024);
  dma_fill_test(impl, dev, 0, 1024, 0x5a);

  st_dma_free_dev(impl, dev);
}

static int dma_drop_mbuf(struct st_dma_dev* dma_dev, uint16_t nb_mbuf) {
  struct rte_mbuf* mbuf = NULL;
  struct mtl_dma_lender_dev* mbuf_dev;

  for (uint16_t i = 0; i < nb_mbuf; i++) {
#if ST_DMA_RTE_RING
    int ret = rte_ring_sc_dequeue(dma_dev->borrow_queue, (void**)&mbuf);
    if (ret < 0) {
      err("%s(%d), no item to dequeue\n", __func__, dma_dev->idx);
      break;
    }
#else
    /* dequeue the mbuf */
    mbuf = dma_dev->inflight_mbufs[dma_dev->inflight_dequeue_idx];
    dma_dev->inflight_dequeue_idx++;
    if (dma_dev->inflight_dequeue_idx >= dma_dev->nb_desc)
      dma_dev->inflight_dequeue_idx = 0;
#endif
    dma_dev->nb_inflight--;
    mbuf_dev = &dma_dev->lenders[st_rx_mbuf_get_lender(mbuf)];
    mbuf_dev->nb_borrowed--;
    if (mbuf_dev->cb) {
      mbuf_dev->cb(mbuf_dev->priv, mbuf);
    }
    rte_pktmbuf_free(mbuf);
  }
  return 0;
}

static int dma_hw_start(struct mtl_main_impl* impl, struct st_dma_dev* dev,
                        uint16_t nb_desc) {
  struct rte_dma_info info;
  struct rte_dma_conf dev_config = {.nb_vchans = 1};
  struct rte_dma_vchan_conf qconf = {
      .direction = RTE_DMA_DIR_MEM_TO_MEM,
  };
  uint16_t vchan = 0;
  int16_t dev_id = dev->dev_id;
  int ret, idx = dev->idx;

  dbg("%s(%d), start\n", __func__, idx);

  ret = rte_dma_configure(dev_id, &dev_config);
  if (ret < 0) {
    err("%s(%d), rte_dma_configure fail %d\n", __func__, idx, ret);
    return ret;
  }

  qconf.nb_desc = nb_desc;
  ret = rte_dma_vchan_setup(dev_id, vchan, &qconf);
  if (ret < 0) {
    err("%s(%d), rte_dma_vchan_setup fail %d\n", __func__, idx, ret);
    return ret;
  }

  rte_dma_info_get(dev_id, &info);
  if (info.nb_vchans != dev_config.nb_vchans) {
    err("%s(%d), %u:%u nb_vchans mismatch\n", __func__, idx, info.nb_vchans,
        dev_config.nb_vchans);
    return ret;
  }

  ret = rte_dma_start(dev_id);
  if (ret < 0) {
    err("%s(%d), rte_dma_start fail %d\n", __func__, idx, ret);
    return ret;
  }

  /* WA to fix stop fail if no any copy task submitted */
  dma_copy_test(impl, &dev->lenders[0], 0, 32);

  return 0;
}

static int dma_hw_stop(struct st_dma_dev* dev) {
  int16_t dev_id = dev->dev_id;
  int ret, idx = dev->idx;

  ret = rte_dma_stop(dev_id);
  if (ret < 0) err("%s(%d), rte_dma_stop fail %d\n", __func__, idx, ret);

  return 0;
}

static int dma_sw_init(struct mtl_main_impl* impl, struct st_dma_dev* dev) {
  int idx = dev->idx;
#if ST_DMA_RTE_RING
  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count;

  snprintf(ring_name, 32, "RX-DMA-BORROW-RING-D%d", idx);
  flags = RING_F_SP_ENQ | RING_F_SC_DEQ;
  count = dev->nb_desc;
  ring = rte_ring_create(ring_name, count, st_socket_id(impl, MTL_PORT_P), flags);
  if (!ring) {
    err("%s(%d), rte_ring_create fail\n", __func__, idx);
    return -ENOMEM;
  }
  dev->borrow_queue = ring;
#else
  dev->inflight_enqueue_idx = 0;
  dev->inflight_dequeue_idx = 0;
  dev->inflight_mbufs = st_rte_zmalloc_socket(sizeof(*dev->inflight_mbufs) * dev->nb_desc,
                                              st_socket_id(impl, MTL_PORT_P));
  if (!dev->inflight_mbufs) {
    err("%s(%d), inflight_mbufs alloc fail\n", __func__, idx);
    return -ENOMEM;
  }
#endif
  dev->nb_inflight = 0;

  return 0;
}

static int dma_sw_uinit(struct st_dma_dev* dev) {
  uint16_t nb_inflight = 0;

#if ST_DMA_RTE_RING
  if (dev->borrow_queue) {
    nb_inflight = rte_ring_count(dev->borrow_queue);
    if (nb_inflight) {
      warn("%s(%d), still has %u mbufs\n", __func__, dev->idx, nb_inflight);
      dma_drop_mbuf(dev, nb_inflight);
    }
    rte_ring_free(dev->borrow_queue);
    dev->borrow_queue = NULL;
  }
#else
  if (dev->inflight_mbufs) {
    nb_inflight = dev->nb_inflight;
    if (nb_inflight) {
      warn("%s(%d), still has %u mbufs\n", __func__, dev->idx, nb_inflight);
      dma_drop_mbuf(dev, nb_inflight);
    }
    st_rte_free(dev->inflight_mbufs);
    dev->inflight_mbufs = NULL;
  }
#endif

  return 0;
}

static int dma_stat(struct mtl_main_impl* impl, struct st_dma_dev* dev) {
  int16_t dev_id = dev->dev_id;
  int idx = dev->idx;
  struct rte_dma_stats stats;
  uint64_t avg_nb_inflight = 0;

  rte_dma_stats_get(dev_id, 0, &stats);
  rte_dma_stats_reset(dev_id, 0);
  if (dev->stat_commit_sum)
    avg_nb_inflight = dev->stat_inflight_sum / dev->stat_commit_sum;
  dev->stat_inflight_sum = 0;
  dev->stat_commit_sum = 0;
  notice("DMA(%d), s %" PRIu64 " c %" PRIu64 " e %" PRIu64 " avg q %" PRIu64 "\n", idx,
         stats.submitted, stats.completed, stats.errors, avg_nb_inflight);

  return 0;
}

static int dma_free(struct mtl_main_impl* impl, struct st_dma_dev* dev) {
  if (!dev->active) {
    err("%s(%d), not active\n", __func__, dev->idx);
    return -EIO;
  }

  dma_hw_stop(dev);
  dma_sw_uinit(dev);
  dev->active = false;

  return 0;
}

struct mtl_dma_lender_dev* st_dma_request_dev(struct mtl_main_impl* impl,
                                             struct st_dma_request_req* req) {
  struct st_dma_mgr* mgr = st_get_dma_mgr(impl);
  struct st_dma_dev* dev;
  struct mtl_dma_lender_dev* lender_dev;
  int idx, ret;

  if (!mgr->num_dma_dev) return NULL;

  uint16_t nb_desc = req->nb_desc;
  if (!nb_desc) nb_desc = 128;

  st_pthread_mutex_lock(&mgr->mutex);
  /* first try to find a shared dma */
  for (idx = 0; idx < MTL_DMA_DEV_MAX; idx++) {
    dev = &mgr->devs[idx];
    if (dev->active && (dev->sch_idx == req->sch_idx) &&
        (dev->soc_id == req->socket_id) && (dev->nb_session < dev->max_shared)) {
      for (int render = 0; render < dev->max_shared; render++) {
        lender_dev = &dev->lenders[render];
        if (!lender_dev->active) {
          lender_dev->active = true;
          lender_dev->nb_borrowed = 0;
          lender_dev->priv = req->priv;
          lender_dev->cb = req->drop_mbuf_cb;
          dev->nb_session++;
          st_pthread_mutex_unlock(&mgr->mutex);
          info("%s(%d), shared dma with id %u\n", __func__, idx, render);
          return lender_dev;
        }
      }
    }
  }
  /* now try to create a new dma */
  for (idx = 0; idx < MTL_DMA_DEV_MAX; idx++) {
    dev = &mgr->devs[idx];
    if (dev->usable && !dev->active && (dev->soc_id == req->socket_id)) {
      ret = dma_hw_start(impl, dev, nb_desc);
      if (ret < 0) continue;
      dev->nb_desc = nb_desc;
      dev->sch_idx = req->sch_idx;
      dev->max_shared = RTE_MIN(req->max_shared, ST_DMA_MAX_SESSIONS);
      ret = dma_sw_init(impl, dev);
      if (ret < 0) {
        dma_hw_stop(dev);
        continue;
      }
      lender_dev = &dev->lenders[0];
      lender_dev->active = true;
      lender_dev->nb_borrowed = 0;
      lender_dev->priv = req->priv;
      lender_dev->cb = req->drop_mbuf_cb;
      dev->nb_session++;
      dev->active = true;
      rte_atomic32_inc(&mgr->num_dma_dev_active);
      st_pthread_mutex_unlock(&mgr->mutex);
      info("%s(%d), dma created with max share %u nb_desc %u\n", __func__, idx,
           dev->max_shared, dev->nb_desc);
      return lender_dev;
    }
  }
  st_pthread_mutex_unlock(&mgr->mutex);

  err("%s, fail to find free dev\n", __func__);
  return NULL;
}

int st_dma_free_dev(struct mtl_main_impl* impl, struct mtl_dma_lender_dev* dev) {
  struct st_dma_dev* dma_dev = dev->parent;
  int idx = dev->lender_id;
  int dma_idx = dma_dev->idx;
  struct st_dma_mgr* mgr = st_get_dma_mgr(impl);

  if (!dev->active) {
    err("%s(%d,%d), not active\n", __func__, dma_idx, idx);
    return -EIO;
  }

  dev->active = false;
  dev->cb = NULL;
  dma_dev->nb_session--;

  if (!dma_dev->nb_session) {
    dma_free(impl, dma_dev);
    rte_atomic32_dec(&mgr->num_dma_dev_active);
  }

  info("%s(%d,%d), nb_session now %u\n", __func__, dma_idx, idx, dma_dev->nb_session);
  return 0;
}

int st_dma_copy(struct mtl_dma_lender_dev* dev, rte_iova_t dst, rte_iova_t src,
                uint32_t length) {
  struct st_dma_dev* dma_dev = dev->parent;
  return rte_dma_copy(dma_dev->dev_id, 0, src, dst, length, 0);
}

int st_dma_fill(struct mtl_dma_lender_dev* dev, rte_iova_t dst, uint64_t pattern,
                uint32_t length) {
  struct st_dma_dev* dma_dev = dev->parent;
  return rte_dma_fill(dma_dev->dev_id, 0, pattern, dst, length, 0);
}

int st_dma_submit(struct mtl_dma_lender_dev* dev) {
  struct st_dma_dev* dma_dev = dev->parent;
  dma_dev->stat_commit_sum++;
  dma_dev->stat_inflight_sum += dma_dev->nb_inflight;
  return rte_dma_submit(dma_dev->dev_id, 0);
}

uint16_t st_dma_completed(struct mtl_dma_lender_dev* dev, uint16_t nb_cpls,
                          uint16_t* last_idx, bool* has_error) {
  struct st_dma_dev* dma_dev = dev->parent;
  return rte_dma_completed(dma_dev->dev_id, 0, nb_cpls, NULL, NULL);
}

int st_dma_borrow_mbuf(struct mtl_dma_lender_dev* dev, struct rte_mbuf* mbuf) {
  struct st_dma_dev* dma_dev = dev->parent;

  st_rx_mbuf_set_lender(mbuf, dev->lender_id);
#if ST_DMA_RTE_RING
  int ret = rte_ring_sp_enqueue(dma_dev->borrow_queue, (void*)mbuf);
  if (ret) {
    err("%s, no space for queue\n", __func__);
    return ret;
  }
#else
  dma_dev->inflight_mbufs[dma_dev->inflight_enqueue_idx] = mbuf;
  dma_dev->inflight_enqueue_idx++;
  if (dma_dev->inflight_enqueue_idx >= dma_dev->nb_desc)
    dma_dev->inflight_enqueue_idx = 0;
#endif
  dma_dev->nb_inflight++;
  dev->nb_borrowed++;
  return 0;
}

int st_dma_drop_mbuf(struct mtl_dma_lender_dev* dev, uint16_t nb_mbuf) {
  return dma_drop_mbuf(dev->parent, nb_mbuf);
}

bool st_dma_full(struct mtl_dma_lender_dev* dev) {
  struct st_dma_dev* dma_dev = dev->parent;
#if ST_DMA_RTE_RING
  return rte_ring_full(dma_dev->borrow_queue) ? true : false;
#else
  return (dma_dev->nb_inflight < dma_dev->nb_desc) ? false : true;
#endif
}

int st_dma_init(struct mtl_main_impl* impl) {
  struct st_dma_mgr* mgr = st_get_dma_mgr(impl);
  int16_t dev_id;
  int idx;
  struct st_dma_dev* dev;
  struct rte_dma_info dev_info;
  bool test = false;
  struct mtl_dma_lender_dev* lender_dev;

  st_pthread_mutex_init(&mgr->mutex, NULL);

  /* init idx */
  for (idx = 0; idx < MTL_DMA_DEV_MAX; idx++) {
    dev = &mgr->devs[idx];
    dev->idx = idx;
  }

  idx = 0;
  RTE_DMA_FOREACH_DEV(dev_id) {
    rte_dma_info_get(dev_id, &dev_info);
    if (!st_is_valid_socket(impl, dev_info.numa_node)) continue;
    dev = &mgr->devs[idx];
    dev->dev_id = dev_id;
    dev->soc_id = dev_info.numa_node;
    dev->usable = true;
    dev->nb_session = 0;
    info("%s(%d), dma dev id %u name %s capa 0x%" PRIx64 " numa %d desc %u:%u\n",
         __func__, idx, dev_id, dev_info.dev_name, dev_info.dev_capa, dev_info.numa_node,
         dev_info.min_desc, dev_info.max_desc);
    for (int render = 0; render < ST_DMA_MAX_SESSIONS; render++) {
      lender_dev = &dev->lenders[render];
      lender_dev->parent = dev;
      lender_dev->lender_id = render;
      lender_dev->active = false;
    }
    idx++;
  }
  mgr->num_dma_dev = idx;

  if (test && mgr->num_dma_dev) dma_test(impl);

  return 0;
}

int st_dma_uinit(struct mtl_main_impl* impl) {
  struct st_dma_mgr* mgr = st_get_dma_mgr(impl);
  int idx;
  struct st_dma_dev* dev;

  for (idx = 0; idx < MTL_DMA_DEV_MAX; idx++) {
    dev = &mgr->devs[idx];
    if (dev->active) {
      warn("%s(%d), still active\n", __func__, idx);
      dma_free(impl, dev);
    }
  }

  return 0;
}

int st_dma_stat(struct mtl_main_impl* impl) {
  struct st_dma_mgr* mgr = st_get_dma_mgr(impl);
  int idx;
  struct st_dma_dev* dev;

  for (idx = 0; idx < MTL_DMA_DEV_MAX; idx++) {
    dev = &mgr->devs[idx];
    if (dev->active) dma_stat(impl, dev);
  }

  return 0;
}
#else
int st_dma_init(struct mtl_main_impl* impl) {
  struct mtl_init_params* p = st_get_user_params(impl);

  if (p->num_dma_dev_port) {
    err("%s, total dma dev %d requested, but the lib build without dma dev support\n",
        __func__, p->num_dma_dev_port);
  }

  return -EINVAL;
}

int st_dma_uinit(struct mtl_main_impl* impl) { return -EINVAL; }

int st_dma_stat(struct mtl_main_impl* impl) { return -EINVAL; }

struct mtl_dma_lender_dev* st_dma_request_dev(struct mtl_main_impl* impl,
                                             struct st_dma_request_req* req) {
  return NULL;
}
int st_dma_free_dev(struct mtl_main_impl* impl, struct mtl_dma_lender_dev* dev) {
  return -EINVAL;
}
int st_dma_borrow_mbuf(struct mtl_dma_lender_dev* dev, struct rte_mbuf* mbuf) {
  return -EINVAL;
}
int st_dma_drop_mbuf(struct mtl_dma_lender_dev* dev, uint16_t nb_mbuf) { return -EINVAL; }
int st_dma_copy(struct mtl_dma_lender_dev* dev, rte_iova_t dst, rte_iova_t src,
                uint32_t length) {
  return -EINVAL;
}
int st_dma_fill(struct mtl_dma_lender_dev* dev, rte_iova_t dst, uint64_t pattern,
                uint32_t length) {
  return -EINVAL;
}
int st_dma_submit(struct mtl_dma_lender_dev* dev) { return -EINVAL; }
uint16_t st_dma_completed(struct mtl_dma_lender_dev* dev, uint16_t nb_cpls,
                          uint16_t* last_idx, bool* has_error) {
  return 0;
}
bool st_dma_full(struct mtl_dma_lender_dev* dev) { return true; }

#endif
