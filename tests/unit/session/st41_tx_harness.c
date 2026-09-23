/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for ST41 (fast metadata) TX build-packet unit tests (session layer).
 *
 * Includes the production .c directly so the static
 * tx_fastmetadata_session_build_packet() becomes visible in this translation
 * unit. Non-static symbols duplicate those in libmtl; this object's
 * definition preempts the shared library's. USDT is disabled to avoid
 * probe-semaphore link references.
 */

#include <stdlib.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "st2110/st_tx_fastmetadata_session.c"

struct ut41tx_ctx {
  struct st_tx_fastmetadata_session_impl session;
  struct st_frame_trans frame;
  struct st41_frame fmd_frame;
};

#include "session/st41_tx_harness.h"

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
