/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "session/st20_hdr_split_pool_harness.h"

#include <rte_malloc.h>
#include <stdio.h>
#include <stdlib.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
/* Compiled in, so the pool takes the plain rte_zmalloc_socket() of mt_mem.h; the st20
 * session harness also uses this copy. */
#include "mt_hdr_split.c"

#define UT_HS_MAX_MBUFS (256)
#define UT_HS_FRAMES_IOVA (0x100000)
#define UT_HS_SCRATCH (4)
#define UT_HS_SCRATCH_SIZE (2048)

struct ut_hs_ctx {
  struct mt_hdr_split_pool* hp;
  struct mt_hdr_split_slots slots;
  struct mt_hdr_split_slots big_slots;
  struct rte_mbuf* held[UT_HS_MAX_MBUFS];
};

/* rte_eal_init() fills the mempool ops table with the DPDK drivers */
__attribute__((constructor)) static void ut_hs_register_ops(void) {
  mt_hdr_split_ops_register();
}

ut_hs_ctx* ut_hs_create(uint32_t mbufs_per_frame, uint32_t frames_cnt, uint32_t slot_size,
                        uint32_t extra_mbufs) {
  static int pool_idx;
  char name[32];

  if (ut_eal_init() < 0) return NULL;
  uint32_t nb_slots = mbufs_per_frame * frames_cnt + mbufs_per_frame - 1;
  if (nb_slots + extra_mbufs > UT_HS_MAX_MBUFS) return NULL;

  ut_hs_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;
  struct mt_hdr_split_slots* slots = &ctx->slots;
  slots->stride = mt_hdr_split_stride(slot_size, 0);
  slots->mbufs_per_frame = mbufs_per_frame;
  slots->nb_slots = nb_slots;
  slots->frames =
      rte_zmalloc_socket("ut_hs", (size_t)nb_slots * slots->stride, 0, rte_socket_id());
  slots->frames_iova = UT_HS_FRAMES_IOVA;
  snprintf(name, sizeof(name), "ut_hs_%d", pool_idx++);
  if (slots->frames)
    ctx->hp = mt_hdr_split_pool_create(name, nb_slots + extra_mbufs, UT_HS_SCRATCH,
                                       UT_HS_SCRATCH_SIZE, rte_socket_id());
  if (!ctx->hp || (mt_hdr_split_bind(ctx->hp, slots) < 0)) {
    if (ctx->hp) mt_hdr_split_pool_free(ctx->hp);
    rte_free(slots->frames);
    free(ctx);
    return NULL;
  }
  return ctx;
}

void ut_hs_destroy(ut_hs_ctx* ctx) {
  if (!ctx) return;
  for (int i = 0; i < UT_HS_MAX_MBUFS; i++) ut_hs_put(ctx, i);
  mt_hdr_split_pool_free(ctx->hp);
  rte_free(ctx->slots.frames);
  free(ctx);
}

uint32_t ut_hs_slots(const ut_hs_ctx* ctx) {
  return ctx->slots.nb_slots;
}

uint32_t ut_hs_stride(const ut_hs_ctx* ctx) {
  return ctx->slots.stride;
}

unsigned int ut_hs_avail(const ut_hs_ctx* ctx) {
  return rte_mempool_avail_count(ctx->hp->mp);
}

int ut_hs_bind(ut_hs_ctx* ctx, uint32_t extra_slots) {
  if (!extra_slots) return mt_hdr_split_bind(ctx->hp, &ctx->slots);
  ctx->big_slots = ctx->slots;
  ctx->big_slots.nb_slots += extra_slots;
  return mt_hdr_split_bind(ctx->hp, &ctx->big_slots);
}

int ut_hs_unbind(ut_hs_ctx* ctx) {
  return mt_hdr_split_unbind(ctx->hp);
}

int ut_hs_get(ut_hs_ctx* ctx) {
  struct rte_mbuf* m = rte_mbuf_raw_alloc(ctx->hp->mp);
  if (!m) return -1;
  int idx = hs_mbuf_idx(m);
  ctx->held[idx] = m;
  return idx;
}

int ut_hs_get_bulk(ut_hs_ctx* ctx, unsigned int n) {
  struct rte_mbuf* m[UT_HS_MAX_MBUFS];

  if (rte_mbuf_raw_alloc_bulk(ctx->hp->mp, m, n) < 0) return -1;
  for (unsigned int i = 0; i < n; i++) ctx->held[hs_mbuf_idx(m[i])] = m[i];
  return 0;
}

void ut_hs_put(ut_hs_ctx* ctx, int idx) {
  if (idx < 0 || idx >= UT_HS_MAX_MBUFS || !ctx->held[idx]) return;
  rte_pktmbuf_free(ctx->held[idx]);
  ctx->held[idx] = NULL;
}

void ut_hs_set_frame_start(ut_hs_ctx* ctx, uint32_t slot) {
  ctx->slots.cur_frame_mbuf_idx = slot;
}

int64_t ut_hs_data_offset(const ut_hs_ctx* ctx, int idx) {
  return rte_pktmbuf_mtod(ctx->held[idx], uint8_t*) - (uint8_t*)ctx->slots.frames;
}

int64_t ut_hs_data_iova_offset(const ut_hs_ctx* ctx, int idx) {
  return (int64_t)(rte_mbuf_data_iova_default(ctx->held[idx]) - ctx->slots.frames_iova);
}

bool ut_hs_in_scratch(const ut_hs_ctx* ctx, int idx) {
  uint8_t* addr = ctx->held[idx]->buf_addr;
  uint8_t* scratch = ctx->hp->scratch;
  return (addr >= scratch) && (addr < scratch + UT_HS_SCRATCH * UT_HS_SCRATCH_SIZE);
}
