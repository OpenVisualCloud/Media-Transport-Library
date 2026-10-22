/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "mt_hdr_split.h"

#include "mt_log.h"
#include "mt_util.h"

#define MT_HDR_SPLIT_OPS_NAME "mtl_hdr_split"

/*
 * The ops run in one thread at a time: the RX tasklet of the bound session (PMD refill,
 * free after the handler), or the control thread at queue start, flush and stop under
 * the session lock the tasklet try-locks. An ice rte_tm_hierarchy_commit() stops and
 * starts the whole port without that lock, so mt_dev refuses it while a pool is bound.
 */

static inline uint32_t hs_mbuf_idx(struct rte_mbuf* m) {
  return *(uint32_t*)rte_mbuf_to_priv(m);
}

static int hs_alloc(struct rte_mempool* mp) {
  mp->pool_data = mp->pool_config;
  return 0;
}

static unsigned int hs_get_count(const struct rte_mempool* mp) {
  const struct mt_hdr_split_pool* hp = mp->pool_data;
  unsigned int count = 0;

  if (!hp->mp) return 0;
  for (uint32_t i = 0; i < hp->nb_mbufs; i++)
    if (hp->mbufs[i]) count++;
  return count;
}

/* Frames start every mbufs_per_frame mbufs from the last frame start the handler saw, or
 * from mbuf 0 once the dequeue wrapped past it. */
static uint32_t hs_next_idx(const struct mt_hdr_split_pool* hp) {
  uint32_t idx = hp->alloc_idx;
  uint32_t mpf = hp->mbufs_per_frame;
  uint32_t cur = hp->slots ? hp->slots->cur_frame_mbuf_idx : 0;
  uint32_t grid = (idx >= cur) ? cur : 0;

  if (((idx - grid) % mpf == 0) && (idx + mpf > hp->nb_active)) return 0;
  return idx;
}

static int hs_dequeue(struct rte_mempool* mp, void** obj_table, unsigned int n) {
  struct mt_hdr_split_pool* hp = mp->pool_data;
  uint32_t alloc_idx = hp->alloc_idx;

  for (unsigned int i = 0; i < n; i++) {
    uint32_t idx = hs_next_idx(hp);
    struct rte_mbuf* m = hp->mbufs[idx];

    if (!m) {
      for (unsigned int j = 0; j < i; j++)
        hp->mbufs[hs_mbuf_idx(obj_table[j])] = obj_table[j];
      hp->alloc_idx = alloc_idx;
      return -ENOBUFS;
    }
    hp->mbufs[idx] = NULL;
    obj_table[i] = m;
    hp->alloc_idx = (idx + 1 < hp->nb_active) ? (idx + 1) : 0;
  }
  return 0;
}

static int hs_enqueue(struct rte_mempool* mp, void* const* obj_table, unsigned int n) {
  struct mt_hdr_split_pool* hp = mp->pool_data;

  /* rte_mempool_populate_default() enqueues each object before it is a mbuf */
  if (!hp->mp) return 0;
  for (unsigned int i = 0; i < n; i++) {
    struct rte_mbuf* m = obj_table[i];
    hp->mbufs[hs_mbuf_idx(m)] = m;
  }
  return 0;
}

static const struct rte_mempool_ops hs_ops = {
    .name = MT_HDR_SPLIT_OPS_NAME,
    .alloc = hs_alloc,
    .enqueue = hs_enqueue,
    .dequeue = hs_dequeue,
    .get_count = hs_get_count,
};

int mt_hdr_split_ops_register(void) {
  static int ops_index = -1;

  if (ops_index < 0) ops_index = rte_mempool_register_ops(&hs_ops);
  return (ops_index < 0) ? ops_index : 0;
}

int mt_hdr_split_ops_check(void) {
  for (uint32_t i = 0; i < rte_mempool_ops_table.num_ops; i++)
    if (!strcmp(rte_mempool_ops_table.ops[i].name, MT_MEMPOOL_OPS_DEFAULT)) return 0;
  err("%s, no %s mempool ops: the %d entries of the DPDK table are full, and the hdr "
      "split ops took the entry of the driver that registered last\n",
      __func__, MT_MEMPOOL_OPS_DEFAULT, RTE_MEMPOOL_MAX_OPS_IDX);
  return -ENOENT;
}

static void hs_mbuf_point(struct mt_hdr_split_pool* hp, struct rte_mbuf* m,
                          uint32_t idx) {
  struct mt_hdr_split_slots* slots = hp->slots;
  size_t offset;

  if (slots && (idx < slots->nb_slots)) {
    offset = (size_t)idx * slots->stride;
    m->buf_addr = (uint8_t*)slots->frames + offset;
    rte_mbuf_iova_set(m, slots->frames_iova + offset);
    m->buf_len = slots->stride;
  } else {
    offset = (size_t)(idx % hp->nb_scratch) * hp->scratch_size;
    m->buf_addr = (uint8_t*)hp->scratch + offset;
    rte_mbuf_iova_set(m, hp->scratch_iova + offset);
    m->buf_len = hp->scratch_size;
  }
}

/* rte_pktmbuf_free_pinned_extmem() of DPDK lib/mbuf/rte_mbuf.c, which is static */
static void hs_free_cb(void* addr, void* opaque) {
  struct rte_mbuf* m = opaque;

  MTL_MAY_UNUSED(addr);
  rte_mbuf_ext_refcnt_set(m->shinfo, 1);
  m->ol_flags = RTE_MBUF_F_EXTERNAL;
  m->next = NULL;
  m->nb_segs = 1;
  rte_mbuf_raw_free(m);
}

/* __rte_pktmbuf_init_extmem() of DPDK lib/mbuf/rte_mbuf.c, with the idx in the priv */
static void hs_mbuf_init(struct rte_mempool* mp, void* opaque, void* obj,
                         unsigned int idx) {
  struct mt_hdr_split_pool* hp = opaque;
  struct rte_mbuf* m = obj;
  uint16_t priv_size = rte_pktmbuf_priv_size(mp);
  uint32_t mbuf_size = sizeof(*m) + priv_size;
  struct rte_mbuf_ext_shared_info* shinfo = RTE_PTR_ADD(m, mbuf_size);

  memset(m, 0, mbuf_size);
  m->priv_size = priv_size;
  *(uint32_t*)rte_mbuf_to_priv(m) = idx;
  hs_mbuf_point(hp, m, idx);
  m->data_off = RTE_PKTMBUF_HEADROOM;
  m->pool = mp;
  m->nb_segs = 1;
  m->port = RTE_MBUF_PORT_INVALID;
  m->ol_flags = RTE_MBUF_F_EXTERNAL;
  rte_mbuf_refcnt_set(m, 1);

  m->shinfo = shinfo;
  shinfo->free_cb = hs_free_cb;
  shinfo->fcb_opaque = m;
  rte_mbuf_ext_refcnt_set(shinfo, 1);

  hp->mbufs[idx] = m;
}

static void hs_free(struct mt_hdr_split_pool* hp) {
  if (hp->mp) rte_mempool_free(hp->mp);
  if (hp->mbufs) mt_rte_free(hp->mbufs);
  if (hp->scratch) mt_rte_free(hp->scratch);
  mt_rte_free(hp);
}

struct mt_hdr_split_pool* mt_hdr_split_pool_create(const char* name, uint32_t nb_mbufs,
                                                   uint32_t nb_scratch,
                                                   uint32_t scratch_size, int socket_id) {
  struct rte_pktmbuf_pool_private mbp_priv = {
      .mbuf_data_room_size = scratch_size,
      .mbuf_priv_size = RTE_ALIGN(sizeof(uint32_t), RTE_MBUF_PRIV_ALIGN),
      .flags = RTE_PKTMBUF_POOL_F_PINNED_EXT_BUF,
  };
  unsigned int elt_size = sizeof(struct rte_mbuf) + mbp_priv.mbuf_priv_size +
                          sizeof(struct rte_mbuf_ext_shared_info);
  struct rte_mempool* mp;
  int ret;

  struct mt_hdr_split_pool* hp = mt_rte_zmalloc_socket(sizeof(*hp), socket_id);
  if (!hp) return NULL;
  hp->nb_mbufs = nb_mbufs;
  hp->nb_active = nb_mbufs;
  hp->mbufs_per_frame = 1;
  hp->nb_scratch = nb_scratch;
  hp->scratch_size = scratch_size;
  hp->mbufs = mt_rte_zmalloc_socket(sizeof(*hp->mbufs) * nb_mbufs, socket_id);
  hp->scratch = mt_rte_zmalloc_socket((size_t)nb_scratch * scratch_size, socket_id);
  if (!hp->mbufs || !hp->scratch) {
    err("%s(%s), malloc fail\n", __func__, name);
    hs_free(hp);
    return NULL;
  }
  hp->scratch_iova = rte_malloc_virt2iova(hp->scratch);

  /* rte_pktmbuf_pool_create_extbuf() always takes rte_mbuf_best_mempool_ops() */
  mp = rte_mempool_create_empty(name, nb_mbufs, elt_size, 0, sizeof(mbp_priv), socket_id,
                                0);
  if (!mp) {
    err("%s(%s), create fail %s\n", __func__, name, rte_strerror(rte_errno));
    hs_free(hp);
    return NULL;
  }
  ret = rte_mempool_set_ops_byname(mp, MT_HDR_SPLIT_OPS_NAME, hp);
  if (ret < 0) {
    err("%s(%s), no %s mempool ops %d, the ops table of DPDK may be full\n", __func__,
        name, MT_HDR_SPLIT_OPS_NAME, ret);
    rte_mempool_free(mp);
    hs_free(hp);
    return NULL;
  }
  rte_pktmbuf_pool_init(mp, &mbp_priv);
  ret = rte_mempool_populate_default(mp);
  if (ret < 0) {
    err("%s(%s), populate fail %d\n", __func__, name, ret);
    rte_mempool_free(mp);
    hs_free(hp);
    return NULL;
  }

  rte_mempool_obj_iter(mp, hs_mbuf_init, hp);
  hp->mp = mp;
  info("%s(%s), %u mbufs, %u scratch buffers of %u bytes\n", __func__, name, nb_mbufs,
       nb_scratch, scratch_size);
  return hp;
}

int mt_hdr_split_pool_free(struct mt_hdr_split_pool* hp) {
  unsigned int in_use = rte_mempool_in_use_count(hp->mp);

  if (in_use) {
    err("%s(%s), %u mbufs still in use, keep the pool\n", __func__, hp->mp->name, in_use);
    return -EBUSY;
  }
  hs_free(hp);
  return 0;
}

static int hs_rebind(struct mt_hdr_split_pool* hp, struct mt_hdr_split_slots* slots) {
  for (uint32_t i = 0; i < hp->nb_mbufs; i++) {
    if (!hp->mbufs[i]) {
      err("%s(%s), mbuf %u still in use\n", __func__, hp->mp->name, i);
      return -EBUSY;
    }
  }

  hp->slots = slots;
  hp->nb_active = slots ? slots->nb_slots : hp->nb_mbufs;
  hp->mbufs_per_frame = slots ? slots->mbufs_per_frame : 1;
  hp->alloc_idx = 0;
  if (slots) slots->cur_frame_mbuf_idx = 0;
  for (uint32_t i = 0; i < hp->nb_mbufs; i++) hs_mbuf_point(hp, hp->mbufs[i], i);
  return 0;
}

int mt_hdr_split_bind(struct mt_hdr_split_pool* hp, struct mt_hdr_split_slots* slots) {
  if (slots->nb_slots > hp->nb_mbufs) {
    err("%s(%s), the session needs %u slots but the pool has %u, raise "
        "mtl_init_params.hdr_split_slots\n",
        __func__, hp->mp->name, slots->nb_slots, hp->nb_mbufs);
    return -ENOSPC;
  }
  return hs_rebind(hp, slots);
}

int mt_hdr_split_unbind(struct mt_hdr_split_pool* hp) {
  return hs_rebind(hp, NULL);
}
