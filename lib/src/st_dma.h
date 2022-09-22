/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_DMA_HEAD_H_
#define _ST_LIB_DMA_HEAD_H_

#include "st_main.h"

int st_dma_init(struct st_main_impl* impl);
int st_dma_uinit(struct st_main_impl* impl);
int st_dma_stat(struct st_main_impl* impl);

struct st_dma_request_req {
  uint16_t nb_desc;
  uint16_t max_shared;
  int sch_idx;
  int socket_id;
  void* priv;
  st_dma_drop_mbuf_cb drop_mbuf_cb;
};

struct st_dma_lender_dev* st_dma_request_dev(struct st_main_impl* impl,
                                             struct st_dma_request_req* req);
int st_dma_free_dev(struct st_main_impl* impl, struct st_dma_lender_dev* dev);

/* enqueue mbuf for later free, also mark the lender session */
int st_dma_borrow_mbuf(struct st_dma_lender_dev* dev, struct rte_mbuf* mbuf);
/* dequeue and free mbufs */
int st_dma_drop_mbuf(struct st_dma_lender_dev* dev, uint16_t nb_mbuf);

bool st_dma_full(struct st_dma_lender_dev* dev);

int st_dma_copy(struct st_dma_lender_dev* dev, rte_iova_t dst, rte_iova_t src,
                uint32_t length);
int st_dma_fill(struct st_dma_lender_dev* dev, rte_iova_t dst, uint64_t pattern,
                uint32_t length);
int st_dma_submit(struct st_dma_lender_dev* dev);
uint16_t st_dma_completed(struct st_dma_lender_dev* dev, uint16_t nb_cpls,
                          uint16_t* last_idx, bool* has_error);

static inline void st_dma_copy_busy(struct st_dma_lender_dev* dev, rte_iova_t dst,
                                    rte_iova_t src, uint32_t length) {
  int ret;
  do {
    ret = st_dma_copy(dev, dst, src, length);
  } while (ret < 0);
}

static inline void st_dma_submit_busy(struct st_dma_lender_dev* dev) {
  int ret;
  do {
    ret = st_dma_submit(dev);
  } while (ret < 0);
}

static inline bool st_dma_empty(struct st_dma_lender_dev* dev) {
  if (dev->nb_borrowed)
    return false;
  else
    return true;
}

static inline int st_dma_lender_id(struct st_dma_lender_dev* dev) {
  return dev->lender_id;
}

static inline int st_dma_dev_id(struct st_dma_lender_dev* dev) {
  return dev->parent->idx;
}

int st_map_init(struct st_main_impl* impl);
int st_map_uinit(struct st_main_impl* impl);
int st_map_add(struct st_main_impl* impl, struct st_map_item* item);
int st_map_remove(struct st_main_impl* impl, struct st_map_item* item);

#endif
