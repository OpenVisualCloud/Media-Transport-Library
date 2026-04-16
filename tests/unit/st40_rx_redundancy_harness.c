/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * C harness for ST40 RX redundancy unit tests.
 * Wraps internal MTL functions that cannot be called directly from C++
 * (internal headers use C keywords like "new" as identifiers).
 */

#include <rte_eal.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <stdlib.h>
#include <string.h>

/*
 * Include the production .c directly so that all static functions
 * (rx_ancillary_session_handle_pkt, rx_ancillary_session_reset, etc.)
 * become visible in this translation unit.  Non-static symbols will
 * duplicate those in libmtl; the linker flag --allow-multiple-definition
 * is used to resolve this safely (identical code).
 * Disable USDT to avoid linker references to probe semaphores.
 */
#undef MTL_HAS_USDT
#include "st2110/st_rx_ancillary_session.c"

/* ── define the opaque context ────────────────────────────────────────── */

#define UT_RING_SIZE 512
#define UT_POOL_SIZE 2048

struct ut_test_ctx {
  struct mtl_main_impl impl;
  struct st_rx_ancillary_sessions_mgr mgr;
  struct st_rx_ancillary_session_impl session;
};

/* pull in the header after the struct definition so types resolve */
#include "st40_rx_redundancy_harness.h"

/* ── globals ──────────────────────────────────────────────────────────── */

static struct rte_mempool* g_pool;
static struct rte_ring* g_ring;
static bool g_eal_ready;

/* ── drain helper ─────────────────────────────────────────────────────── */

void ut_drain_ring(void) {
  if (!g_ring) return;
  struct rte_mbuf* pkt = NULL;
  while (rte_ring_sc_dequeue(g_ring, (void**)&pkt) == 0) rte_pktmbuf_free(pkt);
}

/* ── RTP ready callback ──────────────────────────────────────────────── */

static int ut_notify_rtp_ready(void* priv) {
  (void)priv;
  ut_drain_ring();
  return 0;
}

/* ── EAL + pool + ring init ──────────────────────────────────────────── */

int ut_eal_init(void) {
  if (g_eal_ready && g_pool && g_ring) return 0;

  if (!g_eal_ready) {
    static char a0[] = "unit_test";
    static char a1[] = "--no-huge";
    static char a2[] = "--no-shconf";
    static char a3[] = "-c1";
    static char a4[] = "-n1";
    static char a5[] = "--no-pci";
    static char a6[] = "--vdev=net_null0";
    static char* args[] = {a0, a1, a2, a3, a4, a5, a6};
    if (rte_eal_init(7, args) < 0) return -1;
    g_eal_ready = true;
  }

  if (!g_pool) {
    g_pool = rte_pktmbuf_pool_create("unit_pool", UT_POOL_SIZE, 0, 0,
                                     RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (!g_pool) return -1;
  }

  if (!g_ring) {
    g_ring = rte_ring_create("unit_ring", UT_RING_SIZE, rte_socket_id(),
                             RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (!g_ring) return -1;
  }

  return 0;
}

/* ── context create / destroy ─────────────────────────────────────────── */

ut_test_ctx* ut_ctx_create(int num_port) {
  ut_test_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  mt_stat_u64_init(&ctx->session.stat_time);

  ctx->impl.type = MT_HANDLE_MAIN;
  ctx->impl.tsc_hz = rte_get_tsc_hz();

  ctx->mgr.parent = &ctx->impl;
  ctx->mgr.idx = 0;

  ctx->session.idx = 0;
  ctx->session.socket_id = rte_socket_id();
  ctx->session.mgr = &ctx->mgr;
  ctx->session.packet_ring = g_ring;
  ctx->session.attached = true;
  ctx->session.ops.num_port = num_port;
  ctx->session.ops.payload_type = 0;
  ctx->session.ops.interlaced = false;
  ctx->session.ops.rtp_ring_size = UT_RING_SIZE;
  ctx->session.ops.notify_rtp_ready = ut_notify_rtp_ready;
  ctx->session.ops.priv = &ctx->session;
  ctx->session.ops.name = "unit_test";
  ctx->session.interlace_auto = false;

  rx_ancillary_session_reset(&ctx->session, false);
  return ctx;
}

void ut_ctx_destroy(ut_test_ctx* ctx) {
  free(ctx);
}

/* ── mbuf builder (internal) ──────────────────────────────────────────── */

static struct rte_mbuf* make_anc_mbuf_full(uint16_t seq, uint32_t ts, int marker,
                                           uint8_t pt, uint32_t ssrc, uint8_t f_bits) {
  struct rte_mbuf* m = rte_pktmbuf_alloc(g_pool);
  if (!m) return NULL;

  size_t hdr_offset = sizeof(struct st_rfc3550_hdr) - sizeof(struct st_rfc3550_rtp_hdr);
  size_t total = hdr_offset + sizeof(struct st40_rfc8331_rtp_hdr);

  if (rte_pktmbuf_tailroom(m) < total) {
    rte_pktmbuf_free(m);
    return NULL;
  }

  uint8_t* buf = rte_pktmbuf_mtod(m, uint8_t*);
  memset(buf, 0, total);

  struct st40_rfc8331_rtp_hdr* rtp = (struct st40_rfc8331_rtp_hdr*)(buf + hdr_offset);
  rtp->base.version = 2;
  rtp->base.seq_number = htons(seq);
  rtp->base.tmstamp = htonl(ts);
  rtp->base.ssrc = htonl(ssrc);
  rtp->base.payload_type = pt;
  rtp->base.marker = marker ? 1 : 0;

  /* pack anc_count=1 and f_bits into the chunk that gets ntohl'd */
  uint32_t chunk = ((uint32_t)1) << 24; /* anc_count = 1 */
  chunk |= ((uint32_t)(f_bits & 0x3)) << 22;
  rtp->swapped_first_hdr_chunk = htonl(chunk);

  m->data_len = total;
  m->pkt_len = total;
  return m;
}

static struct rte_mbuf* make_anc_mbuf(uint16_t seq, uint32_t ts, int marker) {
  return make_anc_mbuf_full(seq, ts, marker, 0, 0, 0);
}

/* ── feed one packet ──────────────────────────────────────────────────── */

int ut_feed_pkt(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                enum mtl_session_port port) {
  struct rte_mbuf* m = make_anc_mbuf(seq, ts, marker);
  if (!m) return -1;
  int rc = rx_ancillary_session_handle_pkt(&ctx->impl, &ctx->session, m, port);
  rte_pktmbuf_free(m);
  return rc;
}

/* ── feed a burst ─────────────────────────────────────────────────────── */

void ut_feed_burst(ut_test_ctx* ctx, uint16_t seq_start, int count, uint32_t ts,
                   int last_marker, enum mtl_session_port port) {
  for (int i = 0; i < count; i++) {
    int marker = last_marker && (i == count - 1);
    ut_feed_pkt(ctx, seq_start + i, ts, marker, port);
  }
}

/* ── stat accessors ───────────────────────────────────────────────────── */

uint64_t ut_stat_unrecovered(const ut_test_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_pkts_unrecovered;
}

uint64_t ut_stat_redundant(const ut_test_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_pkts_redundant;
}

uint64_t ut_stat_received(const ut_test_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_pkts_received;
}

uint64_t ut_stat_out_of_order(const ut_test_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_pkts_out_of_order;
}

int ut_session_seq_id(const ut_test_ctx* ctx) {
  return ctx->session.session_seq_id;
}

/* ── feed with custom PT ──────────────────────────────────────────────── */

int ut_feed_pkt_pt(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                   enum mtl_session_port port, uint8_t payload_type) {
  struct rte_mbuf* m = make_anc_mbuf_full(seq, ts, marker, payload_type, 0, 0);
  if (!m) return -1;
  int rc = rx_ancillary_session_handle_pkt(&ctx->impl, &ctx->session, m, port);
  rte_pktmbuf_free(m);
  return rc;
}

/* ── feed with custom SSRC ────────────────────────────────────────────── */

int ut_feed_pkt_ssrc(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                     enum mtl_session_port port, uint32_t ssrc) {
  struct rte_mbuf* m = make_anc_mbuf_full(seq, ts, marker, 0, ssrc, 0);
  if (!m) return -1;
  int rc = rx_ancillary_session_handle_pkt(&ctx->impl, &ctx->session, m, port);
  rte_pktmbuf_free(m);
  return rc;
}

/* ── feed with custom F-bits ──────────────────────────────────────────── */

int ut_feed_pkt_fbits(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                      enum mtl_session_port port, uint8_t f_bits) {
  struct rte_mbuf* m = make_anc_mbuf_full(seq, ts, marker, 0, 0, f_bits);
  if (!m) return -1;
  int rc = rx_ancillary_session_handle_pkt(&ctx->impl, &ctx->session, m, port);
  rte_pktmbuf_free(m);
  return rc;
}

/* ── context config setters ───────────────────────────────────────────── */

void ut_ctx_set_pt(ut_test_ctx* ctx, uint8_t pt) {
  ctx->session.ops.payload_type = pt;
}

void ut_ctx_set_ssrc(ut_test_ctx* ctx, uint32_t ssrc) {
  ctx->session.ops.ssrc = ssrc;
}

void ut_ctx_set_interlace_auto(ut_test_ctx* ctx, bool enable) {
  ctx->session.interlace_auto = enable;
}

/* ── per-port stat accessors ──────────────────────────────────────────── */

uint64_t ut_stat_port_pkts(const ut_test_ctx* ctx, enum mtl_session_port port) {
  return ctx->session.port_user_stats.common.port[port].packets;
}

uint64_t ut_stat_port_bytes(const ut_test_ctx* ctx, enum mtl_session_port port) {
  return ctx->session.port_user_stats.common.port[port].bytes;
}

uint64_t ut_stat_port_ooo(const ut_test_ctx* ctx, enum mtl_session_port port) {
  return ctx->session.port_user_stats.common.port[port].out_of_order_packets;
}

uint64_t ut_stat_port_frames(const ut_test_ctx* ctx, enum mtl_session_port port) {
  return ctx->session.port_user_stats.common.port[port].frames;
}

/* ── validation stat accessors ────────────────────────────────────────── */

uint64_t ut_stat_wrong_pt(const ut_test_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_pkts_wrong_pt_dropped;
}

uint64_t ut_stat_wrong_ssrc(const ut_test_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_pkts_wrong_ssrc_dropped;
}

uint64_t ut_stat_wrong_interlace(const ut_test_ctx* ctx) {
  return ctx->session.port_user_stats.stat_pkts_wrong_interlace_dropped;
}

uint64_t ut_stat_interlace_first(const ut_test_ctx* ctx) {
  return ctx->session.port_user_stats.stat_interlace_first_field;
}

uint64_t ut_stat_interlace_second(const ut_test_ctx* ctx) {
  return ctx->session.port_user_stats.stat_interlace_second_field;
}

uint64_t ut_stat_enqueue_fail(const ut_test_ctx* ctx) {
  return ctx->session.port_user_stats.stat_pkts_enqueue_fail;
}

int ut_frames_received(const ut_test_ctx* ctx) {
  return rte_atomic32_read(&ctx->session.stat_frames_received);
}
