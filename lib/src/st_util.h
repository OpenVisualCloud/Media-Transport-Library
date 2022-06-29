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

#ifndef _ST_LIB_UTIL_HEAD_H_
#define _ST_LIB_UTIL_HEAD_H_

#include "st_main.h"

bool st_bitmap_test_and_set(uint8_t* bitmap, int idx);

int st_ring_dequeue_clean(struct rte_ring* ring);

void st_mbuf_sanity_check(struct rte_mbuf** mbufs, uint16_t nb, char* tag);

int st_pacing_train_result_add(struct st_main_impl* impl, enum st_port port,
                               uint64_t rl_bps, float pad_interval);

int st_pacing_train_result_search(struct st_main_impl* impl, enum st_port port,
                                  uint64_t rl_bps, float* pad_interval);

int st_build_port_map(struct st_main_impl* impl, char** ports, enum st_port* maps,
                      int num_ports);

/* logical session port to main(physical) port */
static inline enum st_port st_port_logic2phy(enum st_port* maps,
                                             enum st_session_port logic) {
  return maps[logic];
}

void st_video_rtp_dump(enum st_port port, int idx, char* tag,
                       struct st20_rfc4175_rtp_hdr* rtp);

void st_mbuf_dump(enum st_port port, int idx, char* tag, struct rte_mbuf* m);

void st_lcore_dump();

void st_eth_link_dump(uint16_t port_id);

/* 7 bits payload type define in RFC3550 */
static inline bool st_is_valid_payload_type(int payload_type) {
  if (payload_type > 0 && payload_type < 0x7F)
    return true;
  else
    return false;
}

void st_eth_macaddr_dump(enum st_port port, char* tag, struct rte_ether_addr* mac_addr);

static inline bool st_rx_seq_drop(uint16_t new_id, uint16_t old_id, uint16_t delta) {
  if ((new_id <= old_id) && ((old_id - new_id) < delta))
    return true;
  else
    return false;
}

struct rte_mbuf* st_build_pad(struct st_main_impl* impl, enum st_port port,
                              uint16_t port_id, uint16_t ether_type, uint16_t len);

struct rte_mempool* st_mempool_create(struct st_main_impl* impl, enum st_port port,
                                      const char* name, unsigned int n,
                                      unsigned int cache_size, uint16_t priv_size,
                                      uint16_t element_size);

static inline struct rte_mempool* st_mempool_create_common(struct st_main_impl* impl,
                                                           enum st_port port,
                                                           const char* name,
                                                           unsigned int n) {
  return st_mempool_create(impl, port, name, n, ST_MBUF_CACHE_SIZE,
                           sizeof(struct st_muf_priv_data), ST_MBUF_DEFAULT_DATA_SIZE);
}

int st_mempool_free(struct rte_mempool* mp);

uint16_t st_rf1071_check_sum(uint8_t* p, size_t len, bool convert);

struct st_u64_fifo {
  uint64_t* data;
  int write_idx;
  int read_idx;
  int size;
  int used;
};

struct st_u64_fifo* st_u64_fifo_init(int size, int soc_id);
int st_u64_fifo_uinit(struct st_u64_fifo* fifo);
int st_u64_fifo_put(struct st_u64_fifo* fifo, uint64_t item);
int st_u64_fifo_get(struct st_u64_fifo* fifo, uint64_t* item);

struct st_cvt_dma_ctx {
  struct st_u64_fifo* fifo;
  int* tran;
  int* done;
};

struct st_cvt_dma_ctx* st_cvt_dma_ctx_init(int fifo_size, int soc_id, int type_num);
int st_cvt_dma_ctx_uinit(struct st_cvt_dma_ctx* ctx);
int st_cvt_dma_ctx_push(struct st_cvt_dma_ctx* ctx, int type);
int st_cvt_dma_ctx_pop(struct st_cvt_dma_ctx* ctx);
static inline int st_cvt_dma_ctx_get_done(struct st_cvt_dma_ctx* ctx, int type) {
  return ctx->done[type];
}

static inline int st_cvt_dma_ctx_get_tran(struct st_cvt_dma_ctx* ctx, int type) {
  return ctx->tran[type];
}

#endif
