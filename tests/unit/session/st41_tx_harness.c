/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for ST41 (fast metadata) TX session unit tests.
 *
 * Includes the production .c directly so the static
 * tx_fastmetadata_session_build_packet() and the tasklet become visible in
 * this translation unit. Non-static symbols duplicate those in libmtl; this
 * object's definition preempts the shared library's. USDT is disabled to
 * avoid probe-semaphore link references. mt_get_tsc() is mocked with the same
 * preprocessor seam as st40_tx_harness.c; mt_get_ptp_time() dispatches
 * through ptp_get_time_fn.
 */

#include <stdlib.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "mt_main.h"

struct ut41tx_ctx {
  struct mtl_main_impl impl;
  struct st_tx_fastmetadata_sessions_mgr mgr;
  struct st_tx_fastmetadata_session_impl session;
  struct st_frame_trans frame;
  struct st41_frame fmd_frame;
  uint64_t mock_ptp_ns;
  uint64_t mock_tsc_ns;
  enum st10_timestamp_fmt app_tfmt;
  uint64_t app_timestamp;
  bool frame_handed_over;
  struct st41_tx_frame_meta done_meta;
  uint32_t wire_rtp_timestamp;
};

static uint64_t ut41tx_tsc_time_fn(struct mtl_main_impl* impl) {
  return ((struct ut41tx_ctx*)impl)->mock_tsc_ns;
}

#define mt_get_tsc ut41tx_tsc_time_fn
#include "st2110/st_tx_fastmetadata_session.c"
#undef mt_get_tsc

#include "session/st41_tx_harness.h"

static uint64_t ut41tx_ptp_time_fn(struct mtl_main_impl* impl, enum mtl_port port) {
  (void)port;
  return ((struct ut41tx_ctx*)impl)->mock_ptp_ns;
}

static int ut41tx_get_next_frame(void* priv, uint16_t* next_frame_idx,
                                 struct st41_tx_frame_meta* meta) {
  struct ut41tx_ctx* ctx = priv;
  if (ctx->frame_handed_over) return -EBUSY;
  ctx->frame_handed_over = true;
  *next_frame_idx = 0;
  meta->tfmt = ctx->app_tfmt;
  meta->timestamp = ctx->app_timestamp;
  return 0;
}

static int ut41tx_notify_frame_done(void* priv, uint16_t frame_idx,
                                    struct st41_tx_frame_meta* meta) {
  struct ut41tx_ctx* ctx = priv;
  (void)frame_idx;
  ctx->done_meta = *meta;
  return 0;
}

int ut41tx_init(void) {
  return ut_eal_init();
}

ut41tx_ctx* ut41tx_ctx_create(void) {
  ut41tx_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  /* as tx_fastmetadata_session_attach() */
  ctx->session.max_pkt_len = ST_PKT_MAX_ETHER_BYTES - sizeof(struct st41_fmd_hdr);
  ctx->session.st41_frames_cnt = 1;
  ctx->session.st41_frames = &ctx->frame;
  ctx->session.eth_ipv4_cksum_offload[MTL_SESSION_PORT_P] = true;

  ctx->frame.addr = &ctx->fmd_frame;

  ctx->impl.type = MT_HANDLE_MAIN;
  ctx->impl.inf[MTL_PORT_P].ptp_get_time_fn = ut41tx_ptp_time_fn;
  ctx->mgr.parent = &ctx->impl;
  ctx->mgr.max_idx = 1;
  rte_spinlock_init(&ctx->mgr.mutex[0]);
  ctx->mgr.sessions[0] = &ctx->session;
  ctx->session.mgr = &ctx->mgr;
  ctx->session.ops.type = ST41_TYPE_FRAME_LEVEL;
  ctx->session.ops.num_port = 1;
  ctx->session.ops.get_next_frame = ut41tx_get_next_frame;
  ctx->session.ops.notify_frame_done = ut41tx_notify_frame_done;
  ctx->session.ops.priv = ctx;
  ctx->session.tx_no_chain = true;
  ctx->session.pacing.frame_time = NS_PER_MS;
  ctx->session.pacing.frame_time_sampling = 90;
  ctx->session.pacing.max_onward_epochs = 3;

  return ctx;
}

void ut41tx_ctx_destroy(ut41tx_ctx* ctx) {
  free(ctx);
}

void ut41tx_ctx_set_payload(ut41tx_ctx* ctx, uint8_t* data, uint16_t len) {
  ctx->fmd_frame.data = data;
  ctx->fmd_frame.data_item_length_bytes = len;
}

struct rte_mbuf* ut41tx_alloc_mbuf(size_t room) {
  struct rte_mbuf* m = rte_pktmbuf_alloc(ut_pool());
  if (!m) return NULL;
  if (room > m->buf_len) {
    rte_pktmbuf_free(m);
    return NULL;
  }
  m->data_off = (uint16_t)(m->buf_len - room);
  return m;
}

void ut41tx_free_mbuf(struct rte_mbuf* m) {
  rte_pktmbuf_free(m);
}

void ut41tx_build_packet(ut41tx_ctx* ctx, struct rte_mbuf* pkt) {
  tx_fastmetadata_session_build_packet(&ctx->session, pkt);
}

uint32_t ut41tx_pkt_data_len(const struct rte_mbuf* pkt) {
  return pkt->data_len;
}

uint32_t ut41tx_pkt_pkt_len(const struct rte_mbuf* pkt) {
  return pkt->pkt_len;
}

size_t ut41tx_fmd_hdr_len(void) {
  return sizeof(struct st41_fmd_hdr);
}

void ut41tx_set_ops_flags(ut41tx_ctx* ctx, uint32_t flags) {
  ctx->session.ops.flags = flags;
}

void ut41tx_set_mock_ptp_time(ut41tx_ctx* ctx, uint64_t ptp_ns) {
  ctx->mock_ptp_ns = ptp_ns;
}

int ut41tx_run_frame(ut41tx_ctx* ctx, enum st10_timestamp_fmt tfmt, uint64_t timestamp) {
  static unsigned int run_idx;
  struct st_tx_fastmetadata_session_impl* s = &ctx->session;
  char pool_name[RTE_MEMPOOL_NAMESIZE];
  char ring_name[RTE_RING_NAMESIZE];
  struct rte_mbuf* pkt = NULL;
  int ret = -EIO;

  snprintf(pool_name, sizeof(pool_name), "ut41tx_pool_%u", run_idx);
  snprintf(ring_name, sizeof(ring_name), "ut41tx_ring_%u", run_idx++);
  s->mbuf_mempool_hdr[MTL_SESSION_PORT_P] =
      rte_pktmbuf_pool_create(pool_name, 32, 0, sizeof(struct mt_muf_priv_data),
                              RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
  ctx->mgr.ring[MTL_PORT_P] = ut_ring_create(ring_name, 32);
  if (!s->mbuf_mempool_hdr[MTL_SESSION_PORT_P] || !ctx->mgr.ring[MTL_PORT_P]) goto out;

  ctx->app_tfmt = tfmt;
  ctx->app_timestamp = timestamp;
  ctx->frame_handed_over = false;
  s->st41_frame_stat = ST41_TX_STAT_WAIT_FRAME;
  s->calculate_time_cursor = true;
  tx_fastmetadata_sessions_tasklet_start(&ctx->mgr);

  tx_fastmetadata_sessions_tasklet_handler(&ctx->mgr);
  ctx->mock_tsc_ns = (uint64_t)s->pacing.tsc_time_cursor;
  tx_fastmetadata_sessions_tasklet_handler(&ctx->mgr);

  if (rte_ring_sc_dequeue(ctx->mgr.ring[MTL_PORT_P], (void**)&pkt) == 0) {
    struct st41_fmd_hdr* hdr = rte_pktmbuf_mtod(pkt, struct st41_fmd_hdr*);
    ctx->wire_rtp_timestamp = ntohl(hdr->rtp.base.tmstamp);
    rte_pktmbuf_free(pkt);
    ret = 0;
  }

out:
  if (ctx->mgr.ring[MTL_PORT_P]) {
    ut_ring_drain(ctx->mgr.ring[MTL_PORT_P]);
    rte_ring_free(ctx->mgr.ring[MTL_PORT_P]);
    ctx->mgr.ring[MTL_PORT_P] = NULL;
  }
  rte_mempool_free(s->mbuf_mempool_hdr[MTL_SESSION_PORT_P]);
  s->mbuf_mempool_hdr[MTL_SESSION_PORT_P] = NULL;
  return ret;
}

const struct st41_tx_frame_meta* ut41tx_done_meta(const ut41tx_ctx* ctx) {
  return &ctx->done_meta;
}

uint32_t ut41tx_wire_rtp_timestamp(const ut41tx_ctx* ctx) {
  return ctx->wire_rtp_timestamp;
}

uint64_t ut41tx_stat_error_user_timestamp(const ut41tx_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_error_user_timestamp;
}

uint64_t ut41tx_stat_epoch_mismatch(const ut41tx_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_epoch_mismatch;
}
