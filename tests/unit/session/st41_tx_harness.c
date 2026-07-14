/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for ST41 (fast metadata) TX build-packet unit tests (session layer).
 *
 * Includes the production .c directly so the static
 * tx_fastmetadata_session_build_packet() becomes visible in this translation
 * unit. Non-static symbols will duplicate those in libmtl; the linker flag
 * --allow-multiple-definition (already set for UnitTest) resolves this safely
 * (identical code). Disable USDT to avoid linker references to probe semaphores.
 */

#include <stdlib.h>
#include <string.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "st2110/st_tx_fastmetadata_session.c"

struct ut_test_ctx {
  struct st_tx_fastmetadata_session_impl session;
  struct st_frame_trans frame;
  struct st41_frame fmd_frame;
  uint8_t payload_buf[256];
};

#include "session/st41_tx_harness.h"

int ut41tx_init(void) {
  return ut_eal_init();
}

ut_test_ctx* ut41tx_ctx_create(void) {
  ut_test_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->session.idx = 0;
  ctx->session.max_pkt_len = UINT16_MAX;
  ctx->session.st41_frames_cnt = 1;
  ctx->session.st41_frames = &ctx->frame;
  ctx->session.st41_frame_idx = 0;
  ctx->session.eth_ipv4_cksum_offload[MTL_SESSION_PORT_P] = true;

  ctx->frame.addr = &ctx->fmd_frame;

  return ctx;
}

void ut41tx_ctx_destroy(ut_test_ctx* ctx) {
  free(ctx);
}

void ut41tx_ctx_set_payload(ut_test_ctx* ctx, const uint8_t* data, uint16_t len) {
  memcpy(ctx->payload_buf, data, len);
  ctx->fmd_frame.data_item_length_bytes = len;
  ctx->fmd_frame.data = ctx->payload_buf;
}

struct rte_mbuf* ut41tx_alloc_mbuf(size_t room) {
  struct rte_mbuf* m = rte_pktmbuf_alloc(ut_pool());
  if (!m) return NULL;
  if (room > m->buf_len) room = m->buf_len;
  m->data_off = (uint16_t)(m->buf_len - room);
  return m;
}

struct rte_mbuf* ut41tx_alloc_mbuf_stale_data_len(size_t room, size_t stale_data_len) {
  size_t total = room + stale_data_len;
  struct rte_mbuf* m = rte_pktmbuf_alloc(ut_pool());
  if (!m) return NULL;
  if (total > m->buf_len) total = m->buf_len;
  m->data_off = (uint16_t)(m->buf_len - total);
  m->data_len = (uint16_t)stale_data_len;
  return m;
}

void ut41tx_free_mbuf(struct rte_mbuf* m) {
  rte_pktmbuf_free(m);
}

void ut41tx_build_packet(ut_test_ctx* ctx, struct rte_mbuf* pkt) {
  tx_fastmetadata_session_build_packet(&ctx->session, pkt);
}

uint32_t ut41tx_pkt_data_len(const struct rte_mbuf* pkt) {
  return pkt->data_len;
}

uint32_t ut41tx_pkt_pkt_len(const struct rte_mbuf* pkt) {
  return pkt->pkt_len;
}

size_t ut41tx_l234_hdr_len(void) {
  return sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
         sizeof(struct rte_udp_hdr);
}

size_t ut41tx_fmd_hdr_len(void) {
  return sizeof(struct st41_fmd_hdr);
}
