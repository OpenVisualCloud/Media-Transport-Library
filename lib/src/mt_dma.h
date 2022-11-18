/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_DMA_HEAD_H_
#define _MT_LIB_DMA_HEAD_H_

#include "mt_main.h"

int st_dma_init(struct mtl_main_impl* impl);
int st_dma_uinit(struct mtl_main_impl* impl);
int st_dma_stat(struct mtl_main_impl* impl);

struct st_dma_request_req {
  uint16_t nb_desc;
  uint16_t max_shared;
  int sch_idx;
  int socket_id;
  void* priv;
  mt_dma_drop_mbuf_cb drop_mbuf_cb;
};

struct mtl_dma_lender_dev* st_dma_request_dev(struct mtl_main_impl* impl,
                                              struct st_dma_request_req* req);
int st_dma_free_dev(struct mtl_main_impl* impl, struct mtl_dma_lender_dev* dev);

/* enqueue mbuf for later free, also mark the lender session */
int st_dma_borrow_mbuf(struct mtl_dma_lender_dev* dev, struct rte_mbuf* mbuf);
/* dequeue and free mbufs */
int st_dma_drop_mbuf(struct mtl_dma_lender_dev* dev, uint16_t nb_mbuf);

bool st_dma_full(struct mtl_dma_lender_dev* dev);

int st_dma_copy(struct mtl_dma_lender_dev* dev, rte_iova_t dst, rte_iova_t src,
                uint32_t length);
int st_dma_fill(struct mtl_dma_lender_dev* dev, rte_iova_t dst, uint64_t pattern,
                uint32_t length);
int st_dma_submit(struct mtl_dma_lender_dev* dev);
uint16_t st_dma_completed(struct mtl_dma_lender_dev* dev, uint16_t nb_cpls,
                          uint16_t* last_idx, bool* has_error);

static inline void st_dma_copy_busy(struct mtl_dma_lender_dev* dev, rte_iova_t dst,
                                    rte_iova_t src, uint32_t length) {
  int ret;
  do {
    ret = st_dma_copy(dev, dst, src, length);
  } while (ret < 0);
}

static inline void st_dma_submit_busy(struct mtl_dma_lender_dev* dev) {
  int ret;
  do {
    ret = st_dma_submit(dev);
  } while (ret < 0);
}

static inline bool st_dma_empty(struct mtl_dma_lender_dev* dev) {
  if (dev->nb_borrowed)
    return false;
  else
    return true;
}

static inline int st_dma_lender_id(struct mtl_dma_lender_dev* dev) {
  return dev->lender_id;
}

static inline int st_dma_dev_id(struct mtl_dma_lender_dev* dev) {
  return dev->parent->idx;
}

int st_map_init(struct mtl_main_impl* impl);
int st_map_uinit(struct mtl_main_impl* impl);
int st_map_add(struct mtl_main_impl* impl, struct st_map_item* item);
int st_map_remove(struct mtl_main_impl* impl, struct st_map_item* item);

#endif
