/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * C harness for ST30 (audio) RX redundancy unit tests.
 * Follows the same pattern as st30_rx_frame_fuzz.c but exposes an
 * opaque API for gtest.
 */

#include <rte_atomic.h>
#include <rte_eal.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <stdlib.h>
#include <string.h>

#include "st2110/st_rx_audio_session.h"
#include "st2110/st_pkt.h"
#include "st30_api.h"
#include "st_api.h"

/* ── tuning constants ─────────────────────────────────────────────────── */

#define UT30_POOL_SIZE 2048
#define UT30_FRAME_COUNT 2
#define UT30_FRAME_CAPACITY 8192

/*
 * Audio config: 2-channel 48 kHz PCM16 @ 1 ms ptime.
 * Payload per packet = 48000 * 0.001 * 2ch * 2bytes = 192 bytes.
 */
#define UT30_CHANNELS 2
#define UT30_PKT_PAYLOAD 192

/* ── opaque context ───────────────────────────────────────────────────── */

struct ut30_test_ctx {
  struct mtl_main_impl impl;
  struct st_rx_audio_sessions_mgr mgr;
  struct st_rx_audio_session_impl session;

  struct st_frame_trans frames[UT30_FRAME_COUNT];
  uint8_t frame_storage[UT30_FRAME_COUNT][UT30_FRAME_CAPACITY];
};

#include "st30_rx_redundancy_harness.h"

/* ── globals ──────────────────────────────────────────────────────────── */

static struct rte_mempool* g30_pool;
static bool g30_eal_ready;

/* ── PTP time stub ────────────────────────────────────────────────────── */

static uint64_t ut30_ptp_time(struct mtl_main_impl* impl, enum mtl_port port) {
  (void)port;
  impl->ptp_usync += 1000; /* advance 1 µs each call */
  return impl->ptp_usync;
}

/* ── frame-ready callback ─────────────────────────────────────────────── */

static int ut30_notify_frame_ready(void* priv, void* frame,
                                   struct st30_rx_frame_meta* meta) {
  (void)meta;
  struct st_rx_audio_session_impl* s = priv;
  if (!s || !frame) return 0;

  for (int i = 0; i < s->st30_frames_cnt; i++) {
    if (!s->st30_frames) break;
    if (s->st30_frames[i].addr == frame) {
      rte_atomic32_dec(&s->st30_frames[i].refcnt);
      break;
    }
  }
  return 0;
}

/* ── EAL init (shared — idempotent) ───────────────────────────────────── */

int ut30_eal_init(void) {
  if (g30_eal_ready && g30_pool) return 0;

  if (!g30_eal_ready) {
    static char a0[] = "unit_test";
    static char a1[] = "--no-huge";
    static char a2[] = "--no-shconf";
    static char a3[] = "-c1";
    static char a4[] = "-n1";
    static char a5[] = "--no-pci";
    static char a6[] = "--vdev=net_null0";
    static char* args[] = {a0, a1, a2, a3, a4, a5, a6};
    int rc = rte_eal_init(7, args);
    if (rc < 0 && rte_eal_has_hugepages() == 0) {
      /* EAL already initialised by the ST40 harness — that's fine */
    } else if (rc < 0) {
      return -1;
    }
    g30_eal_ready = true;
  }

  if (!g30_pool) {
    g30_pool = rte_pktmbuf_pool_create("ut30_pool", UT30_POOL_SIZE, 0, 0,
                                       RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (!g30_pool) return -1;
  }

  return 0;
}

/* ── context create / destroy ─────────────────────────────────────────── */

ut30_test_ctx* ut30_ctx_create(int num_port) {
  ut30_test_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  /* impl */
  ctx->impl.type = MT_HANDLE_MAIN;
  ctx->impl.tsc_hz = rte_get_tsc_hz();
  ctx->impl.ptp_usync = 0;
  for (int i = 0; i < MTL_PORT_MAX; i++) {
    ctx->impl.inf[i].parent = &ctx->impl;
    ctx->impl.inf[i].port = i;
    ctx->impl.inf[i].ptp_get_time_fn = ut30_ptp_time;
  }

  /* mgr */
  ctx->mgr.parent = &ctx->impl;
  ctx->mgr.idx = 0;

  /* prepare frame buffers */
  for (int i = 0; i < UT30_FRAME_COUNT; i++) {
    memset(&ctx->frames[i], 0, sizeof(ctx->frames[i]));
    ctx->frames[i].idx = i;
    ctx->frames[i].addr = ctx->frame_storage[i];
    rte_atomic32_set(&ctx->frames[i].refcnt, 0);
  }

  /* session */
  struct st_rx_audio_session_impl* s = &ctx->session;
  s->idx = 0;
  s->socket_id = rte_socket_id();
  s->mgr = &ctx->mgr;
  s->attached = true;
  s->usdt_dump_fd = -1;

  s->ops.type = ST30_TYPE_FRAME_LEVEL;
  s->ops.num_port = num_port;
  s->ops.channel = UT30_CHANNELS;
  s->ops.sampling = ST30_SAMPLING_48K;
  s->ops.fmt = ST30_FMT_PCM16;
  s->ops.ptime = ST30_PTIME_1MS;
  s->ops.framebuff_cnt = UT30_FRAME_COUNT;
  s->ops.notify_frame_ready = ut30_notify_frame_ready;
  s->ops.priv = s;
  s->ops.name = "ut30_test";
  s->ops.payload_type = 0;
  s->ops.ssrc = 0;

  s->st30_frames = ctx->frames;
  s->st30_frames_cnt = UT30_FRAME_COUNT;

  /* calculate packet and frame geometry */
  s->pkt_len = UT30_PKT_PAYLOAD;
  size_t pkt_multiple = UT30_FRAME_CAPACITY / UT30_PKT_PAYLOAD;
  if (!pkt_multiple) pkt_multiple = 1;
  s->st30_total_pkts = (int)pkt_multiple;
  s->st30_frame_size = pkt_multiple * UT30_PKT_PAYLOAD;
  s->ops.framebuff_size = (uint32_t)s->st30_frame_size;
  s->st30_pkt_size = UT30_PKT_PAYLOAD + sizeof(struct st_rfc3550_audio_hdr);

  s->port_maps[MTL_SESSION_PORT_P] = MTL_PORT_P;
  s->port_maps[MTL_SESSION_PORT_R] = MTL_PORT_R;

  st_rx_audio_session_fuzz_reset(s);
  return ctx;
}

void ut30_ctx_destroy(ut30_test_ctx* ctx) {
  free(ctx);
}

/* ── mbuf builder ─────────────────────────────────────────────────────── */

static struct rte_mbuf* make_audio_mbuf_full(uint16_t seq, uint32_t ts, uint8_t pt,
                                             uint32_t ssrc, uint32_t payload_len) {
  struct rte_mbuf* m = rte_pktmbuf_alloc(g30_pool);
  if (!m) return NULL;

  size_t total = sizeof(struct st_rfc3550_audio_hdr) + payload_len;

  if (rte_pktmbuf_tailroom(m) < total) {
    rte_pktmbuf_free(m);
    return NULL;
  }

  uint8_t* buf = rte_pktmbuf_mtod(m, uint8_t*);
  memset(buf, 0, total);

  /* RTP header is at offset = sizeof(audio_hdr) - sizeof(rtp_hdr) = 42 */
  size_t hdr_offset =
      sizeof(struct st_rfc3550_audio_hdr) - sizeof(struct st_rfc3550_rtp_hdr);
  struct st_rfc3550_rtp_hdr* rtp = (struct st_rfc3550_rtp_hdr*)(buf + hdr_offset);
  rtp->version = 2;
  rtp->seq_number = htons(seq);
  rtp->tmstamp = htonl(ts);
  rtp->ssrc = htonl(ssrc);
  rtp->payload_type = pt;
  rtp->marker = 0;

  m->data_len = total;
  m->pkt_len = total;
  return m;
}

static struct rte_mbuf* make_audio_mbuf(uint16_t seq, uint32_t ts) {
  return make_audio_mbuf_full(seq, ts, 0, 0, UT30_PKT_PAYLOAD);
}

/* ── feed one packet ──────────────────────────────────────────────────── */

int ut30_feed_pkt(ut30_test_ctx* ctx, uint16_t seq, uint32_t ts,
                  enum mtl_session_port port) {
  struct rte_mbuf* m = make_audio_mbuf(seq, ts);
  if (!m) return -1;
  int rc = st_rx_audio_session_fuzz_handle_pkt(&ctx->impl, &ctx->session, m, port);
  rte_pktmbuf_free(m);
  return rc;
}

/* ── feed a burst — each pkt gets ts, ts+1, ts+2, … (real ST30 behavior) ─── */

void ut30_feed_burst(ut30_test_ctx* ctx, uint16_t seq_start, int count,
                     uint32_t ts, enum mtl_session_port port) {
  for (int i = 0; i < count; i++) {
    ut30_feed_pkt(ctx, seq_start + i, ts + i, port);
  }
}

/* ── stat accessors ───────────────────────────────────────────────────── */

uint64_t ut30_stat_unrecovered(const ut30_test_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_pkts_unrecovered;
}

uint64_t ut30_stat_redundant(const ut30_test_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_pkts_redundant;
}

uint64_t ut30_stat_received(const ut30_test_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_pkts_received;
}

uint64_t ut30_stat_out_of_order(const ut30_test_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_pkts_out_of_order;
}

int ut30_session_seq_id(const ut30_test_ctx* ctx) {
  return ctx->session.session_seq_id;
}

int ut30_frames_received(const ut30_test_ctx* ctx) {
  return rte_atomic32_read(&ctx->session.stat_frames_received);
}

int ut30_pkts_per_frame(const ut30_test_ctx* ctx) {
  return ctx->session.st30_total_pkts;
}

/* ── feed with custom PT ──────────────────────────────────────────────── */

int ut30_feed_pkt_pt(ut30_test_ctx* ctx, uint16_t seq, uint32_t ts,
                     enum mtl_session_port port, uint8_t payload_type) {
  struct rte_mbuf* m = make_audio_mbuf_full(seq, ts, payload_type, 0, UT30_PKT_PAYLOAD);
  if (!m) return -1;
  int rc = st_rx_audio_session_fuzz_handle_pkt(&ctx->impl, &ctx->session, m, port);
  rte_pktmbuf_free(m);
  return rc;
}

/* ── feed with custom SSRC ────────────────────────────────────────────── */

int ut30_feed_pkt_ssrc(ut30_test_ctx* ctx, uint16_t seq, uint32_t ts,
                       enum mtl_session_port port, uint32_t ssrc) {
  struct rte_mbuf* m = make_audio_mbuf_full(seq, ts, 0, ssrc, UT30_PKT_PAYLOAD);
  if (!m) return -1;
  int rc = st_rx_audio_session_fuzz_handle_pkt(&ctx->impl, &ctx->session, m, port);
  rte_pktmbuf_free(m);
  return rc;
}

/* ── feed with custom payload length ──────────────────────────────────── */

int ut30_feed_pkt_len(ut30_test_ctx* ctx, uint16_t seq, uint32_t ts,
                      enum mtl_session_port port, uint32_t payload_len) {
  struct rte_mbuf* m = make_audio_mbuf_full(seq, ts, 0, 0, payload_len);
  if (!m) return -1;
  int rc = st_rx_audio_session_fuzz_handle_pkt(&ctx->impl, &ctx->session, m, port);
  rte_pktmbuf_free(m);
  return rc;
}

/* ── context config setters ───────────────────────────────────────────── */

void ut30_ctx_set_pt(ut30_test_ctx* ctx, uint8_t pt) {
  ctx->session.ops.payload_type = pt;
}

void ut30_ctx_set_ssrc(ut30_test_ctx* ctx, uint32_t ssrc) {
  ctx->session.ops.ssrc = ssrc;
}

/* ── per-port stat accessors ──────────────────────────────────────────── */

uint64_t ut30_stat_port_pkts(const ut30_test_ctx* ctx, enum mtl_session_port port) {
  return ctx->session.port_user_stats.common.port[port].packets;
}

uint64_t ut30_stat_port_bytes(const ut30_test_ctx* ctx, enum mtl_session_port port) {
  return ctx->session.port_user_stats.common.port[port].bytes;
}

uint64_t ut30_stat_port_ooo(const ut30_test_ctx* ctx, enum mtl_session_port port) {
  return ctx->session.port_user_stats.common.port[port].out_of_order_packets;
}

/* ── validation stat accessors ────────────────────────────────────────── */

uint64_t ut30_stat_wrong_pt(const ut30_test_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_pkts_wrong_pt_dropped;
}

uint64_t ut30_stat_wrong_ssrc(const ut30_test_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_pkts_wrong_ssrc_dropped;
}

uint64_t ut30_stat_len_mismatch(const ut30_test_ctx* ctx) {
  return ctx->session.port_user_stats.stat_pkts_len_mismatch_dropped;
}
