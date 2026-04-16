/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * C harness for ST20 (video) RX redundancy unit tests.
 * Wraps internal MTL functions via the fuzz entry point so that
 * C++ gtest code never includes internal MTL headers.
 *
 * Test geometry: 16x2 progressive YUV422-10bit
 *   pgroup   = { size=5, coverage=2 }
 *   linesize = 16 / 2 * 5 = 40 bytes
 *   frame_size = fb_size = 40 * 2 = 80 bytes
 *   payload_per_pkt = 40 bytes (exactly one line)
 *   total_pkts_per_frame = 2
 *   bitmap_size = 1 byte (manually set; integer division would give 0)
 */

#include <rte_atomic.h>
#include <rte_eal.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <stdlib.h>
#include <string.h>

#include "st2110/st_rx_video_session.h"
#include "st2110/st_pkt.h"
#include "st20_api.h"
#include "st_api.h"

/* ── geometry constants ───────────────────────────────────────────────── */

#define UT20_WIDTH 16
#define UT20_HEIGHT 2
#define UT20_LINESIZE 40   /* 16/2 * 5 */
#define UT20_FRAME_SIZE 80 /* UT20_LINESIZE * UT20_HEIGHT */
#define UT20_PAYLOAD_PER_PKT UT20_LINESIZE
#define UT20_PKTS_PER_FRAME 2
#define UT20_BITMAP_SIZE 1 /* 8 bits >= UT20_PKTS_PER_FRAME */

#define UT20_POOL_SIZE 2048
#define UT20_FRAME_COUNT 2

/* ── opaque context ───────────────────────────────────────────────────── */

struct ut20_test_ctx {
  struct mtl_main_impl impl;
  struct st_rx_video_sessions_mgr mgr;
  struct st_rx_video_session_impl session;

  struct st_frame_trans frames[UT20_FRAME_COUNT];
  uint8_t frame_storage[UT20_FRAME_COUNT][UT20_FRAME_SIZE];
  uint8_t bitmaps[ST_VIDEO_RX_REC_NUM_OFO][UT20_BITMAP_SIZE];
};

#include "st20_rx_redundancy_harness.h"

/* ── globals ──────────────────────────────────────────────────────────── */

static struct rte_mempool* g20_pool;
static bool g20_eal_ready;

/* ── PTP time stub ────────────────────────────────────────────────────── */

static uint64_t ut20_ptp_time(struct mtl_main_impl* impl, enum mtl_port port) {
  (void)port;
  impl->ptp_usync += 1000; /* advance 1 µs each call */
  return impl->ptp_usync;
}

/* ── frame-ready callback ─────────────────────────────────────────────── */

static int ut20_notify_frame_ready(void* priv, void* frame,
                                   struct st20_rx_frame_meta* meta) {
  (void)meta;
  struct st_rx_video_session_impl* s = priv;
  if (!s || !frame) return 0;

  /* return frame to pool by decrementing refcnt */
  for (int i = 0; i < s->st20_frames_cnt; i++) {
    if (!s->st20_frames) break;
    if (s->st20_frames[i].addr == frame) {
      rte_atomic32_dec(&s->st20_frames[i].refcnt);
      break;
    }
  }
  return 0;
}

/* ── EAL init (shared — idempotent) ───────────────────────────────────── */

int ut20_eal_init(void) {
  if (g20_eal_ready && g20_pool) return 0;

  if (!g20_eal_ready) {
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
      /* EAL already initialised by another harness — that's fine */
    } else if (rc < 0) {
      return -1;
    }
    g20_eal_ready = true;
  }

  if (!g20_pool) {
    g20_pool = rte_pktmbuf_pool_create("ut20_pool", UT20_POOL_SIZE, 0, 0,
                                       RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (!g20_pool) return -1;
  }

  return 0;
}

/* ── context create / destroy ─────────────────────────────────────────── */

ut20_test_ctx* ut20_ctx_create(int num_port) {
  ut20_test_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  /* impl — need a valid type so mtl_ptp_read_time() works */
  ctx->impl.type = MT_HANDLE_MAIN;
  ctx->impl.tsc_hz = rte_get_tsc_hz();
  ctx->impl.ptp_usync = 1000000; /* 1 ms */
  ctx->impl.ptp_usync_tsc = rte_get_tsc_cycles();
  for (int i = 0; i < MTL_PORT_MAX; i++) {
    ctx->impl.inf[i].parent = &ctx->impl;
    ctx->impl.inf[i].port = i;
    ctx->impl.inf[i].ptp_get_time_fn = ut20_ptp_time;
  }

  /* mgr — rv_get_impl() traverses s->parent->parent */
  ctx->mgr.parent = &ctx->impl;
  ctx->mgr.idx = 0;

  /* prepare frame buffers */
  for (int i = 0; i < UT20_FRAME_COUNT; i++) {
    memset(&ctx->frames[i], 0, sizeof(ctx->frames[i]));
    ctx->frames[i].idx = i;
    ctx->frames[i].addr = ctx->frame_storage[i];
    rte_atomic32_set(&ctx->frames[i].refcnt, 0);
  }

  /* session */
  struct st_rx_video_session_impl* s = &ctx->session;
  s->impl = &ctx->impl;
  s->parent = &ctx->mgr;
  s->idx = 0;
  s->socket_id = rte_socket_id();
  s->attached = true;

  /* video ops */
  s->ops.type = ST20_TYPE_FRAME_LEVEL;
  s->ops.num_port = num_port;
  s->ops.width = UT20_WIDTH;
  s->ops.height = UT20_HEIGHT;
  s->ops.fps = ST_FPS_P30;
  s->ops.fmt = ST20_FMT_YUV_422_10BIT;
  s->ops.interlaced = false;
  s->ops.payload_type = 0; /* disable PT check */
  s->ops.ssrc = 0;         /* disable SSRC check */
  s->ops.framebuff_cnt = UT20_FRAME_COUNT;
  s->ops.notify_frame_ready = ut20_notify_frame_ready;
  s->ops.priv = s;
  s->ops.name = "ut20_test";

  /* pixel group */
  s->st20_pg.fmt = ST20_FMT_YUV_422_10BIT;
  s->st20_pg.size = 5;
  s->st20_pg.coverage = 2;
  s->st20_pg.name = "YUV422P10LE";

  /* frame geometry */
  s->st20_linesize = UT20_LINESIZE;
  s->st20_bytes_in_line = UT20_LINESIZE;
  s->st20_frame_size = UT20_FRAME_SIZE;
  s->st20_fb_size = UT20_FRAME_SIZE;
  s->st20_frame_bitmap_size = UT20_BITMAP_SIZE;
  s->st20_uframe_size = 0;

  /* frame info */
  s->st20_frames = ctx->frames;
  s->st20_frames_cnt = UT20_FRAME_COUNT;

  /* frame time — approximate, not critical for filter tests */
  s->frame_time = 33333333.0; /* ~30fps in ns */
  s->frame_time_sampling = 3003.0;

  /* slots — pre-allocate bitmaps */
  s->slot_max = (num_port > 1) ? 2 : 1;
  for (int i = 0; i < ST_VIDEO_RX_REC_NUM_OFO; i++) {
    s->slots[i].frame_bitmap = ctx->bitmaps[i];
    s->slots[i].idx = i;
  }

  /* no DMA, no timing parser, no st22, no pkt_handler */
  s->dma_dev = NULL;
  s->enable_timing_parser = false;
  s->st22_info = NULL;
  s->pkt_handler = NULL;

  s->port_maps[MTL_SESSION_PORT_P] = MTL_PORT_P;
  s->port_maps[MTL_SESSION_PORT_R] = MTL_PORT_R;

  st_rx_video_session_fuzz_reset(s);
  return ctx;
}

void ut20_ctx_destroy(ut20_test_ctx* ctx) {
  free(ctx);
}

/* ── mbuf builder ─────────────────────────────────────────────────────── */

static struct rte_mbuf* make_video_mbuf_full(uint32_t seq, uint32_t ts,
                                             uint16_t line_num, uint16_t line_offset,
                                             uint16_t line_length, uint8_t pt,
                                             uint32_t ssrc) {
  struct rte_mbuf* m = rte_pktmbuf_alloc(g20_pool);
  if (!m) return NULL;

  /* total mbuf data: full video header + payload */
  size_t total = sizeof(struct st_rfc4175_video_hdr) + line_length;

  if (rte_pktmbuf_tailroom(m) < total) {
    rte_pktmbuf_free(m);
    return NULL;
  }

  uint8_t* buf = rte_pktmbuf_mtod(m, uint8_t*);
  memset(buf, 0, total);

  /* RTP header sits at hdr_offset into the mbuf */
  size_t hdr_offset =
      sizeof(struct st_rfc4175_video_hdr) - sizeof(struct st20_rfc4175_rtp_hdr);
  struct st20_rfc4175_rtp_hdr* rtp =
      (struct st20_rfc4175_rtp_hdr*)(buf + hdr_offset);

  rtp->base.version = 2;
  rtp->base.payload_type = pt;
  rtp->base.ssrc = htonl(ssrc);
  rtp->base.marker = 0;
  rtp->base.seq_number = htons((uint16_t)(seq & 0xFFFF));
  rtp->base.tmstamp = htonl(ts);
  rtp->seq_number_ext = htons((uint16_t)(seq >> 16));
  rtp->row_number = htons(line_num);
  rtp->row_offset = htons(line_offset);
  rtp->row_length = htons(line_length);

  m->data_len = total;
  m->pkt_len = total;
  m->next = NULL;
  return m;
}

static struct rte_mbuf* make_video_mbuf(uint32_t seq, uint32_t ts,
                                        uint16_t line_num, uint16_t line_offset,
                                        uint16_t line_length) {
  return make_video_mbuf_full(seq, ts, line_num, line_offset, line_length, 0, 0);
}

/* ── pkt_idx → line/offset mapping ────────────────────────────────────── */

static void pkt_idx_to_line(int pkt_idx, uint16_t* line_num, uint16_t* line_offset,
                            uint16_t* line_length) {
  /* For 16x2 BPM: each pkt covers exactly one line (40 bytes).
   * pkt_idx 0 → line 0, pkt_idx 1 → line 1. */
  *line_num = (uint16_t)pkt_idx;
  *line_offset = 0;
  *line_length = UT20_PAYLOAD_PER_PKT;
}

/* ── feed one packet (raw) ────────────────────────────────────────────── */

int ut20_feed_pkt(ut20_test_ctx* ctx, uint32_t seq, uint32_t ts,
                  uint16_t line_num, uint16_t line_offset, uint16_t line_length,
                  enum mtl_session_port port) {
  struct rte_mbuf* m = make_video_mbuf(seq, ts, line_num, line_offset, line_length);
  if (!m) return -1;
  int rc = st_rx_video_session_fuzz_handle_pkt(&ctx->session, m, port);
  rte_pktmbuf_free(m);
  return rc;
}

/* ── feed one packet by pkt index (auto seq) ──────────────────────────── */

int ut20_feed_frame_pkt(ut20_test_ctx* ctx, int pkt_idx, uint32_t ts,
                        enum mtl_session_port port) {
  uint32_t seq = ts * UT20_PKTS_PER_FRAME + (uint32_t)pkt_idx;
  uint16_t ln, lo, ll;
  pkt_idx_to_line(pkt_idx, &ln, &lo, &ll);
  return ut20_feed_pkt(ctx, seq, ts, ln, lo, ll, port);
}

/* ── feed one packet by pkt index (explicit seq) ──────────────────────── */

int ut20_feed_frame_pkt_seq(ut20_test_ctx* ctx, int pkt_idx, uint32_t seq,
                            uint32_t ts, enum mtl_session_port port) {
  uint16_t ln, lo, ll;
  pkt_idx_to_line(pkt_idx, &ln, &lo, &ll);
  return ut20_feed_pkt(ctx, seq, ts, ln, lo, ll, port);
}

/* ── feed full frame ──────────────────────────────────────────────────── */

void ut20_feed_full_frame(ut20_test_ctx* ctx, uint32_t ts,
                          enum mtl_session_port port) {
  for (int i = 0; i < UT20_PKTS_PER_FRAME; i++) {
    ut20_feed_frame_pkt(ctx, i, ts, port);
  }
}

/* ── stat accessors ───────────────────────────────────────────────────── */

uint64_t ut20_stat_received(const ut20_test_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_pkts_received;
}

uint64_t ut20_stat_redundant(const ut20_test_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_pkts_redundant;
}

uint64_t ut20_stat_out_of_order(const ut20_test_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_pkts_out_of_order;
}

uint64_t ut20_stat_no_slot(const ut20_test_ctx* ctx) {
  return ctx->session.port_user_stats.stat_pkts_no_slot;
}

uint64_t ut20_stat_idx_oo_bitmap(const ut20_test_ctx* ctx) {
  return ctx->session.port_user_stats.stat_pkts_idx_oo_bitmap;
}

uint64_t ut20_stat_frames_dropped(const ut20_test_ctx* ctx) {
  return ctx->session.port_user_stats.stat_frames_dropped;
}

int ut20_frames_received(const ut20_test_ctx* ctx) {
  return rte_atomic32_read(&ctx->session.stat_frames_received);
}

int ut20_total_frame_pkts(void) {
  return UT20_PKTS_PER_FRAME;
}

/* ── feed with custom PT ──────────────────────────────────────────────── */

int ut20_feed_pkt_pt(ut20_test_ctx* ctx, uint32_t seq, uint32_t ts,
                     uint16_t line_num, uint16_t line_offset, uint16_t line_length,
                     enum mtl_session_port port, uint8_t pt) {
  struct rte_mbuf* m =
      make_video_mbuf_full(seq, ts, line_num, line_offset, line_length, pt, 0);
  if (!m) return -1;
  int rc = st_rx_video_session_fuzz_handle_pkt(&ctx->session, m, port);
  rte_pktmbuf_free(m);
  return rc;
}

/* ── feed with custom SSRC ────────────────────────────────────────────── */

int ut20_feed_pkt_ssrc(ut20_test_ctx* ctx, uint32_t seq, uint32_t ts,
                       uint16_t line_num, uint16_t line_offset, uint16_t line_length,
                       enum mtl_session_port port, uint32_t ssrc) {
  struct rte_mbuf* m =
      make_video_mbuf_full(seq, ts, line_num, line_offset, line_length, 0, ssrc);
  if (!m) return -1;
  int rc = st_rx_video_session_fuzz_handle_pkt(&ctx->session, m, port);
  rte_pktmbuf_free(m);
  return rc;
}

/* ── context config setters ───────────────────────────────────────────── */

void ut20_ctx_set_pt(ut20_test_ctx* ctx, uint8_t pt) {
  ctx->session.ops.payload_type = pt;
}

void ut20_ctx_set_ssrc(ut20_test_ctx* ctx, uint32_t ssrc) {
  ctx->session.ops.ssrc = ssrc;
}

/* ── validation stat accessors ────────────────────────────────────────── */

uint64_t ut20_stat_wrong_pt(const ut20_test_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_pkts_wrong_pt_dropped;
}

uint64_t ut20_stat_wrong_ssrc(const ut20_test_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_pkts_wrong_ssrc_dropped;
}

uint64_t ut20_stat_wrong_interlace(const ut20_test_ctx* ctx) {
  return ctx->session.port_user_stats.stat_pkts_wrong_interlace_dropped;
}

uint64_t ut20_stat_offset_dropped(const ut20_test_ctx* ctx) {
  return ctx->session.port_user_stats.stat_pkts_offset_dropped;
}

uint64_t ut20_stat_wrong_len(const ut20_test_ctx* ctx) {
  return ctx->session.port_user_stats.stat_pkts_wrong_len_dropped;
}
