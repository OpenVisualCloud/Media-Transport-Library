/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * C harness for ST40 pipeline-layer unit tests.
 * Tests rx_st40p_rtp_ready() — the frame-assembly callback that runs
 * above the session-layer redundancy filter.
 */

#include <rte_eal.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_ring.h>
#include <rte_udp.h>
#include <stdlib.h>
#include <string.h>

/*
 * Include the production pipeline .c so that all static functions
 * (rx_st40p_rtp_ready, rx_st40p_next_available, etc.) become visible.
 * Disable USDT to avoid linker references to probe semaphores.
 * Suppress -Wunused-variable for variables only used in USDT macros.
 */
#undef MTL_HAS_USDT
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "st2110/pipeline/st40_pipeline_rx.c"
#pragma GCC diagnostic pop

/*
 * Provide stubs for symbols called by rx_st40p_rtp_ready() that would
 * otherwise resolve to libmtl.so (which requires real HW / hugepages).
 * With --allow-multiple-definition these override the libmtl versions.
 */

uint64_t mt_mbuf_time_stamp(struct mtl_main_impl* impl, struct rte_mbuf* mbuf,
                            enum mtl_port port) {
  (void)impl;
  (void)mbuf;
  (void)port;
  return 0; /* HW timestamps are irrelevant for frame-assembly tests */
}

#include "common/ut_common.h"

/* ── constants ────────────────────────────────────────────────────────── */

#define UT40P_RING_SIZE 512
#define UT40P_UDW_BUFF_SIZE 4096
/* L2+L3+L4 header sizes — must match st40_rx_get_mbuf skip logic */
#define UT40P_L234_HDR_LEN                                      \
  (sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + \
   sizeof(struct rte_udp_hdr))

/* ── opaque context ───────────────────────────────────────────────────── */

struct ut40p_ctx {
  /* mock impl — just needs type for sanity checks */
  struct mtl_main_impl impl;

  /* mock transport: handle → session → packet_ring */
  struct st_rx_ancillary_session_handle_impl handle;
  struct st_rx_ancillary_session_impl session;

  /* the pipeline context under test */
  struct st40p_rx_ctx pipeline;

  /* frame buffers allocated by us (not via mt_rte_zmalloc) */
  struct st40p_rx_frame* framebuffs;
  uint8_t** udw_buffers; /* per-frame UDW buffers */
  int framebuff_cnt;
};

#include "pipeline/st40p_harness.h"

/* ── globals ──────────────────────────────────────────────────────────── */

static struct rte_ring* g_mock_ring;

/* ── init ─────────────────────────────────────────────────────────────── */

int ut40p_init(void) {
  if (ut_eal_init() < 0) return -1;

  if (!g_mock_ring) {
    g_mock_ring = ut_ring_create("st40p_ring", UT40P_RING_SIZE);
    if (!g_mock_ring) return -1;
  }
  return 0;
}

/* ── context create / destroy ─────────────────────────────────────────── */

ut40p_ctx* ut40p_ctx_create(int num_port, int framebuff_cnt) {
  ut40p_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->framebuff_cnt = framebuff_cnt;

  /* minimal impl */
  ctx->impl.type = MT_HANDLE_MAIN;

  /* mock session — only needs idx, packet_ring, mgr */
  ctx->session.idx = 0;
  ctx->session.packet_ring = g_mock_ring;
  ctx->session.mgr = NULL; /* USDT disabled — mgr not accessed */

  /* mock transport handle */
  ctx->handle.type = MT_HANDLE_RX_ANC;
  ctx->handle.impl = &ctx->session;

  /* allocate frame buffers */
  ctx->framebuffs = calloc(framebuff_cnt, sizeof(struct st40p_rx_frame));
  ctx->udw_buffers = calloc(framebuff_cnt, sizeof(uint8_t*));
  if (!ctx->framebuffs || !ctx->udw_buffers) {
    ut40p_ctx_destroy(ctx);
    return NULL;
  }

  for (int i = 0; i < framebuff_cnt; i++) {
    struct st40p_rx_frame* fb = &ctx->framebuffs[i];
    fb->stat = ST40P_RX_FRAME_FREE;
    fb->idx = i;
    fb->frame_info.meta = fb->meta;
    fb->frame_info.priv = fb;

    ctx->udw_buffers[i] = calloc(1, UT40P_UDW_BUFF_SIZE);
    if (!ctx->udw_buffers[i]) {
      ut40p_ctx_destroy(ctx);
      return NULL;
    }
    fb->frame_info.udw_buff_addr = ctx->udw_buffers[i];
    fb->frame_info.udw_buffer_size = UT40P_UDW_BUFF_SIZE;
  }

  /* pipeline context */
  struct st40p_rx_ctx* p = &ctx->pipeline;
  p->impl = &ctx->impl;
  p->idx = 0;
  p->socket_id = rte_socket_id();
  p->type = MT_ST40_HANDLE_PIPELINE_RX;

  p->transport = (st40_rx_handle)&ctx->handle;
  p->framebuff_cnt = framebuff_cnt;
  p->framebuff_producer_idx = 0;
  p->framebuff_consumer_idx = 0;
  p->framebuffs = ctx->framebuffs;
  p->inflight_frame = NULL;

  /* port mapping: session port 0 → DPDK port id 0, session port 1 → 1 */
  p->port_map[MTL_SESSION_PORT_P] = MTL_PORT_P;
  p->port_map[MTL_SESSION_PORT_R] = MTL_PORT_R;
  p->port_id[MTL_SESSION_PORT_P] = 0;
  p->port_id[MTL_SESSION_PORT_R] = 1;

  p->ready = true;
  if (pthread_mutex_init(&p->lock, NULL) != 0) {
    ut40p_ctx_destroy(ctx);
    return NULL;
  }

  /* drain any stale mbufs from a previous test */
  ut_ring_drain(g_mock_ring);

  return ctx;
}

void ut40p_ctx_destroy(ut40p_ctx* ctx) {
  if (!ctx) return;

  ut_ring_drain(g_mock_ring);
  pthread_mutex_destroy(&ctx->pipeline.lock);

  if (ctx->udw_buffers) {
    for (int i = 0; i < ctx->framebuff_cnt; i++) free(ctx->udw_buffers[i]);
    free(ctx->udw_buffers);
  }
  free(ctx->framebuffs);
  free(ctx);
}

/* ── mbuf builder ─────────────────────────────────────────────────────── */

/*
 * Build an mbuf with L2+L3+L4 prefix followed by an RFC 8331 RTP header.
 * The payload contains anc_count=1 with zero UDW words (empty ANC packet).
 * This matches the simplest valid ANC RTP packet for testing frame assembly.
 *
 * Layout:
 *   [rte_ether_hdr][rte_ipv4_hdr][rte_udp_hdr][st40_rfc8331_rtp_hdr][payload_hdr]
 */
static struct rte_mbuf* make_pipeline_mbuf(uint16_t seq, uint32_t ts, int marker,
                                           uint16_t dpdk_port_id) {
  struct rte_mbuf* m = rte_pktmbuf_alloc(ut_pool());
  if (!m) return NULL;

  /* ANC payload: one empty ANC packet with 0 UDW words.
   * Payload header = st40_rfc8331_payload_hdr (8 bytes).
   * With 0 UDW words: total_bits = (3+0+1)*10 = 40, total_size = 5 bytes,
   * aligned to 4 bytes = 8, anc_packet_bytes = sizeof(payload_hdr)-4+8 = 12.
   * But the simplest valid option is a header with anc_count=0 for tests
   * that only care about frame assembly (not ANC parsing). */
  size_t rtp_len = sizeof(struct st40_rfc8331_rtp_hdr);
  size_t total = UT40P_L234_HDR_LEN + rtp_len;

  if (rte_pktmbuf_tailroom(m) < total) {
    rte_pktmbuf_free(m);
    return NULL;
  }

  uint8_t* buf = rte_pktmbuf_mtod(m, uint8_t*);
  memset(buf, 0, total);

  /* RTP header starts after L2+L3+L4 */
  struct st40_rfc8331_rtp_hdr* rtp =
      (struct st40_rfc8331_rtp_hdr*)(buf + UT40P_L234_HDR_LEN);
  rtp->base.version = 2;
  rtp->base.seq_number = htons(seq);
  rtp->base.tmstamp = htonl(ts);
  rtp->base.marker = marker ? 1 : 0;

  /* anc_count = 0: no ANC payload to parse, simplifies frame-assembly tests */
  uint32_t chunk = 0; /* anc_count=0, f=0b00 (progressive) */
  rtp->swapped_first_hdr_chunk = htonl(chunk);

  m->data_len = total;
  m->pkt_len = total;
  m->port = dpdk_port_id;
  return m;
}

/* ── enqueue functions ────────────────────────────────────────────────── */

int ut40p_enqueue_pkt(ut40p_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                      enum mtl_session_port port) {
  uint16_t dpdk_port_id = (port == MTL_SESSION_PORT_R) ? 1 : 0;
  return ut40p_enqueue_pkt_port_id(ctx, seq, ts, marker, dpdk_port_id);
}

int ut40p_enqueue_pkt_port_id(ut40p_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                              uint16_t dpdk_port_id) {
  (void)ctx;
  struct rte_mbuf* m = make_pipeline_mbuf(seq, ts, marker, dpdk_port_id);
  if (!m) return -1;

  if (rte_ring_sp_enqueue(g_mock_ring, m) != 0) {
    rte_pktmbuf_free(m);
    return -1;
  }
  return 0;
}

void ut40p_enqueue_burst(ut40p_ctx* ctx, uint16_t seq_start, int count, uint32_t ts,
                         int last_marker, enum mtl_session_port port) {
  for (int i = 0; i < count; i++) {
    int marker = last_marker && (i == count - 1);
    ut40p_enqueue_pkt(ctx, seq_start + i, ts, marker, port);
  }
}

/* ── process functions ────────────────────────────────────────────────── */

int ut40p_process(ut40p_ctx* ctx) {
  return rx_st40p_rtp_ready(&ctx->pipeline);
}

void ut40p_process_all(ut40p_ctx* ctx) {
  while (rx_st40p_rtp_ready(&ctx->pipeline) == 0) {
    /* keep processing until ring is empty or error */
  }
}

/* ── frame get/put ────────────────────────────────────────────────────── */

struct st40_frame_info* ut40p_get_frame(ut40p_ctx* ctx) {
  return st40p_rx_get_frame(&ctx->pipeline);
}

int ut40p_put_frame(ut40p_ctx* ctx, struct st40_frame_info* frame) {
  return st40p_rx_put_frame(&ctx->pipeline, frame);
}

/* ── stat accessors ───────────────────────────────────────────────────── */

uint32_t ut40p_stat_busy(const ut40p_ctx* ctx) {
  return ctx->pipeline.stat_busy;
}

uint32_t ut40p_stat_drop_frame(const ut40p_ctx* ctx) {
  return ctx->pipeline.stat_drop_frame;
}
