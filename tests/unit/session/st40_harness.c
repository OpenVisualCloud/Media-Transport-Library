/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * C harness for ST40 RX redundancy unit tests (session layer).
 * Wraps internal MTL functions that cannot be called directly from C++
 * (internal headers use C keywords like "new" as identifiers).
 */

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
#include "common/ut_common.h"
#include "st2110/st_rx_ancillary_session.c"

/* ── define the opaque context ────────────────────────────────────────── */

struct ut_test_ctx {
  struct mtl_main_impl impl;
  struct st_rx_ancillary_sessions_mgr mgr;
  struct st_rx_ancillary_session_impl session;
};

/* pull in the header after the struct definition so types resolve */
#include "session/st40_harness.h"

/* ── globals ──────────────────────────────────────────────────────────── */

static struct rte_ring* g_ring;
static bool g_skip_drain;      /* when true, notify_rtp_ready does not drain the ring */
static int g_notify_rtp_calls; /* T3: count to verify dispatch suppression */

/* ── RTP ready callback ───────────────────────────────────── */

static int ut_notify_rtp_ready(void* priv) {
  (void)priv;
  g_notify_rtp_calls++;
  if (!g_skip_drain) ut_ring_drain(g_ring);
  return 0;
}

/* ── init (delegates to common) ───────────────────────────────────────── */

int ut40_init(void) {
  if (ut_eal_init() < 0) return -1;

  if (!g_ring) {
    g_ring = ut_ring_create("st40_ring", UT_RING_SIZE);
    if (!g_ring) return -1;
  }
  return 0;
}

/* ── context create / destroy ─────────────────────────────────────────── */

ut_test_ctx* ut40_ctx_create(int num_port) {
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
  ctx->session.ops.type = ST40_TYPE_RTP_LEVEL; /* legacy default for harness */
  ctx->session.ops.rtp_ring_size = UT_RING_SIZE;
  ctx->session.ops.notify_rtp_ready = ut_notify_rtp_ready;
  ctx->session.ops.priv = &ctx->session;
  ctx->session.ops.name = "unit_test";
  ctx->session.interlace_auto = false;

  rx_ancillary_session_reset(&ctx->session, false);
  return ctx;
}

void ut40_ctx_destroy(ut_test_ctx* ctx) {
  free(ctx);
}

void ut40_drain_ring(void) {
  ut_ring_drain(g_ring);
}

/* ── mbuf builder (internal) ──────────────────────────────────────────── */

static struct rte_mbuf* make_anc_mbuf_full(uint16_t seq, uint32_t ts, int marker,
                                           uint8_t pt, uint32_t ssrc, uint8_t f_bits) {
  struct rte_mbuf* m = rte_pktmbuf_alloc(ut_pool());
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

/* ── feed functions ───────────────────────────────────────────────────── */

int ut40_feed_pkt(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                  enum mtl_session_port port) {
  struct rte_mbuf* m = make_anc_mbuf(seq, ts, marker);
  if (!m) return -1;
  int rc = rx_ancillary_session_handle_pkt(&ctx->impl, &ctx->session, m, port);
  rte_pktmbuf_free(m);
  return rc;
}

void ut40_feed_burst(ut_test_ctx* ctx, uint16_t seq_start, int count, uint32_t ts,
                     int last_marker, enum mtl_session_port port) {
  for (int i = 0; i < count; i++) {
    int marker = last_marker && (i == count - 1);
    ut40_feed_pkt(ctx, seq_start + i, ts, marker, port);
  }
}

int ut40_feed_spec(ut_test_ctx* ctx, struct ut40_pkt_spec spec) {
  struct rte_mbuf* m = make_anc_mbuf_full(spec.seq, spec.ts, spec.marker,
                                          spec.payload_type, spec.ssrc, spec.f_bits);
  if (!m) return -1;
  int rc = rx_ancillary_session_handle_pkt(&ctx->impl, &ctx->session, m, spec.port);
  rte_pktmbuf_free(m);
  return rc;
}

int ut40_feed_pkt_pt(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                     enum mtl_session_port port, uint8_t payload_type) {
  struct ut40_pkt_spec spec = {
      .seq = seq, .ts = ts, .marker = marker, .port = port, .payload_type = payload_type};
  return ut40_feed_spec(ctx, spec);
}

int ut40_feed_pkt_ssrc(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                       enum mtl_session_port port, uint32_t ssrc) {
  struct ut40_pkt_spec spec = {
      .seq = seq, .ts = ts, .marker = marker, .port = port, .ssrc = ssrc};
  return ut40_feed_spec(ctx, spec);
}

int ut40_feed_pkt_fbits(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                        enum mtl_session_port port, uint8_t f_bits) {
  struct ut40_pkt_spec spec = {
      .seq = seq, .ts = ts, .marker = marker, .port = port, .f_bits = f_bits};
  return ut40_feed_spec(ctx, spec);
}

/* ── config setters ───────────────────────────────────────────────────── */

void ut40_ctx_set_pt(ut_test_ctx* ctx, uint8_t pt) {
  ctx->session.ops.payload_type = pt;
}

void ut40_ctx_set_ssrc(ut_test_ctx* ctx, uint32_t ssrc) {
  ctx->session.ops.ssrc = ssrc;
}

void ut40_ctx_set_interlace_auto(ut_test_ctx* ctx, bool enable) {
  ctx->session.interlace_auto = enable;
}

/* ── stat accessors ───────────────────────────────────────────────────── */

uint64_t ut40_stat_unrecovered(const ut_test_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_pkts_unrecovered;
}

uint64_t ut40_stat_redundant(const ut_test_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_pkts_redundant;
}

uint64_t ut40_stat_received(const ut_test_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_pkts_received;
}

uint64_t ut40_stat_lost_pkts(const ut_test_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_lost_packets;
}

int ut40_session_seq_id(const ut_test_ctx* ctx) {
  return ctx->session.session_seq_id;
}

uint64_t ut40_stat_port_pkts(const ut_test_ctx* ctx, enum mtl_session_port port) {
  return ctx->session.port_user_stats.common.port[port].packets;
}

uint64_t ut40_stat_port_bytes(const ut_test_ctx* ctx, enum mtl_session_port port) {
  return ctx->session.port_user_stats.common.port[port].bytes;
}

uint64_t ut40_stat_port_lost(const ut_test_ctx* ctx, enum mtl_session_port port) {
  return ctx->session.port_user_stats.common.port[port].lost_packets;
}

uint64_t ut40_stat_port_frames(const ut_test_ctx* ctx, enum mtl_session_port port) {
  return ctx->session.port_user_stats.common.port[port].frames;
}

uint64_t ut40_stat_port_reordered(const ut_test_ctx* ctx, enum mtl_session_port port) {
  return ctx->session.port_user_stats.common.port[port].reordered_packets;
}

uint64_t ut40_stat_port_duplicates(const ut_test_ctx* ctx, enum mtl_session_port port) {
  return ctx->session.port_user_stats.common.port[port].duplicates_same_port;
}

uint64_t ut40_stat_field_bit_mismatch(const ut_test_ctx* ctx) {
  return ctx->session.stat_internal_field_bit_mismatch;
}

uint64_t ut40_stat_wrong_pt(const ut_test_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_pkts_wrong_pt_dropped;
}

uint64_t ut40_stat_wrong_ssrc(const ut_test_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_pkts_wrong_ssrc_dropped;
}

uint64_t ut40_stat_wrong_interlace(const ut_test_ctx* ctx) {
  return ctx->session.port_user_stats.stat_pkts_wrong_interlace_dropped;
}

uint64_t ut40_stat_interlace_first(const ut_test_ctx* ctx) {
  return ctx->session.port_user_stats.stat_interlace_first_field;
}

uint64_t ut40_stat_interlace_second(const ut_test_ctx* ctx) {
  return ctx->session.port_user_stats.stat_interlace_second_field;
}

uint64_t ut40_stat_enqueue_fail(const ut_test_ctx* ctx) {
  return ctx->session.port_user_stats.stat_pkts_enqueue_fail;
}

int ut40_frames_received(const ut_test_ctx* ctx) {
  return rte_atomic32_read(&ctx->session.stat_frames_received);
}

/* ── ring marker inspection ───────────────────────────────────────────── */

void ut40_set_skip_drain(bool skip) {
  g_skip_drain = skip;
}

void ut40_set_frame_level(ut_test_ctx* ctx) {
  ctx->session.ops.type = ST40_TYPE_FRAME_LEVEL;
  /* No frame_slots needed for the T3 dispatch test — the stub does not
   * touch them. T4+ will require the slot pool. */
}

uint64_t ut40_stat_assemble_dispatched(const ut_test_ctx* ctx) {
  return ctx->session.stat_assemble_dispatched;
}

int ut40_notify_rtp_calls(void) {
  return g_notify_rtp_calls;
}
void ut40_notify_rtp_calls_reset(void) {
  g_notify_rtp_calls = 0;
}

/* ── T4 FRAME_LEVEL test helpers ─────────────────────────────────────── */

#define UT40_CAPTURE_MAX 32
#define UT40_CAPTURE_META_PER_FRAME ST40_MAX_META

struct ut40_captured_frame {
  void* addr;
  uint16_t meta_num;
  uint32_t udw_buffer_fill;
  uint32_t rtp_timestamp;
  bool rtp_marker;
  bool interlaced;
  bool second_field;
  /* T5 seq stats */
  uint32_t seq_lost;
  bool seq_discont;
  uint32_t port_seq_lost[MTL_SESSION_PORT_MAX];
  bool port_seq_discont[MTL_SESSION_PORT_MAX];
  uint32_t pkts_recv[MTL_SESSION_PORT_MAX];
  uint32_t pkts_total;
  int status;
  /* deep copy of meta[] so caller can release the slot before inspecting */
  struct st40_meta meta[UT40_CAPTURE_META_PER_FRAME];
  /* deep copy of udw payload bytes (capped) */
  uint8_t udw_copy[1024];
  uint32_t udw_copy_len;
};

static struct ut40_captured_frame g_captured[UT40_CAPTURE_MAX];
static int g_captured_count;
static int g_notify_frame_fail_after = -1; /* -1 = never fail */

static int ut_notify_frame_ready(void* priv, void* addr,
                                 struct st40_rx_frame_meta* meta) {
  (void)priv;
  if (g_notify_frame_fail_after >= 0 && g_captured_count >= g_notify_frame_fail_after)
    return -1;
  if (g_captured_count >= UT40_CAPTURE_MAX) return 0; /* silently drop overflow */

  struct ut40_captured_frame* c = &g_captured[g_captured_count++];
  c->addr = addr;
  c->meta_num = meta->meta_num;
  c->udw_buffer_fill = meta->udw_buffer_fill;
  c->rtp_timestamp = meta->rtp_timestamp;
  c->rtp_marker = meta->rtp_marker;
  c->interlaced = meta->interlaced;
  c->second_field = meta->second_field;
  if (meta->meta && meta->meta_num <= UT40_CAPTURE_META_PER_FRAME)
    memcpy(c->meta, meta->meta, sizeof(struct st40_meta) * meta->meta_num);
  c->udw_copy_len = meta->udw_buffer_fill < sizeof(c->udw_copy) ? meta->udw_buffer_fill
                                                                : sizeof(c->udw_copy);
  if (addr) memcpy(c->udw_copy, addr, c->udw_copy_len);
  c->seq_lost = meta->seq_lost;
  c->seq_discont = meta->seq_discont;
  c->pkts_total = meta->pkts_total;
  c->status = meta->status;
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    c->port_seq_lost[i] = meta->port_seq_lost[i];
    c->port_seq_discont[i] = meta->port_seq_discont[i];
    c->pkts_recv[i] = meta->pkts_recv[i];
  }
  return 0;
}

void ut40_setup_frame_pool(ut_test_ctx* ctx, uint16_t framebuff_cnt,
                           uint32_t framebuff_size) {
  ctx->session.ops.type = ST40_TYPE_FRAME_LEVEL;
  ctx->session.ops.framebuff_cnt = framebuff_cnt;
  ctx->session.ops.framebuff_size = framebuff_size;
  ctx->session.ops.notify_frame_ready = ut_notify_frame_ready;
  rx_ancillary_session_init_frames(&ctx->session);
}

void ut40_teardown_frame_pool(ut_test_ctx* ctx) {
  rx_ancillary_session_uinit_frames(&ctx->session);
  g_captured_count = 0;
  g_notify_frame_fail_after = -1;
}

int ut40_captured_count(void) {
  return g_captured_count;
}
void ut40_captured_reset(void) {
  g_captured_count = 0;
}
void* ut40_captured_addr(int i) {
  return (i >= 0 && i < g_captured_count) ? g_captured[i].addr : NULL;
}
uint16_t ut40_captured_meta_num(int i) {
  return (i >= 0 && i < g_captured_count) ? g_captured[i].meta_num : 0;
}
uint32_t ut40_captured_udw_fill(int i) {
  return (i >= 0 && i < g_captured_count) ? g_captured[i].udw_buffer_fill : 0;
}
uint32_t ut40_captured_rtp_ts(int i) {
  return (i >= 0 && i < g_captured_count) ? g_captured[i].rtp_timestamp : 0;
}
bool ut40_captured_marker(int i) {
  return (i >= 0 && i < g_captured_count) ? g_captured[i].rtp_marker : false;
}
bool ut40_captured_interlaced(int i) {
  return (i >= 0 && i < g_captured_count) ? g_captured[i].interlaced : false;
}
int ut40_captured_meta_did(int frame_i, int meta_i) {
  if (frame_i < 0 || frame_i >= g_captured_count) return -1;
  if (meta_i < 0 || meta_i >= UT40_CAPTURE_META_PER_FRAME) return -1;
  return g_captured[frame_i].meta[meta_i].did;
}
int ut40_captured_meta_sdid(int frame_i, int meta_i) {
  if (frame_i < 0 || frame_i >= g_captured_count) return -1;
  if (meta_i < 0 || meta_i >= UT40_CAPTURE_META_PER_FRAME) return -1;
  return g_captured[frame_i].meta[meta_i].sdid;
}
int ut40_captured_meta_udw_size(int frame_i, int meta_i) {
  if (frame_i < 0 || frame_i >= g_captured_count) return -1;
  if (meta_i < 0 || meta_i >= UT40_CAPTURE_META_PER_FRAME) return -1;
  return g_captured[frame_i].meta[meta_i].udw_size;
}
uint32_t ut40_captured_meta_udw_offset(int frame_i, int meta_i) {
  if (frame_i < 0 || frame_i >= g_captured_count) return 0;
  if (meta_i < 0 || meta_i >= UT40_CAPTURE_META_PER_FRAME) return 0;
  return g_captured[frame_i].meta[meta_i].udw_offset;
}
uint8_t ut40_captured_udw_byte(int frame_i, uint32_t off) {
  if (frame_i < 0 || frame_i >= g_captured_count) return 0;
  if (off >= g_captured[frame_i].udw_copy_len) return 0;
  return g_captured[frame_i].udw_copy[off];
}

uint32_t ut40_captured_seq_lost(int i) {
  return (i >= 0 && i < g_captured_count) ? g_captured[i].seq_lost : 0;
}
bool ut40_captured_seq_discont(int i) {
  return (i >= 0 && i < g_captured_count) ? g_captured[i].seq_discont : false;
}
uint32_t ut40_captured_port_seq_lost(int i, enum mtl_session_port p) {
  if (i < 0 || i >= g_captured_count) return 0;
  if ((int)p < 0 || (int)p >= MTL_SESSION_PORT_MAX) return 0;
  return g_captured[i].port_seq_lost[p];
}
bool ut40_captured_port_seq_discont(int i, enum mtl_session_port p) {
  if (i < 0 || i >= g_captured_count) return false;
  if ((int)p < 0 || (int)p >= MTL_SESSION_PORT_MAX) return false;
  return g_captured[i].port_seq_discont[p];
}
uint32_t ut40_captured_port_pkts_recv(int i, enum mtl_session_port p) {
  if (i < 0 || i >= g_captured_count) return 0;
  if ((int)p < 0 || (int)p >= MTL_SESSION_PORT_MAX) return 0;
  return g_captured[i].pkts_recv[p];
}
uint32_t ut40_captured_pkts_total(int i) {
  return (i >= 0 && i < g_captured_count) ? g_captured[i].pkts_total : 0;
}
int ut40_captured_status(int i) {
  return (i >= 0 && i < g_captured_count) ? g_captured[i].status : -1;
}

void ut40_set_notify_frame_fail_after(int n) {
  g_notify_frame_fail_after = n;
}

void ut40_release_frame(ut_test_ctx* ctx, void* addr) {
  for (uint16_t i = 0; i < ctx->session.frame_slots_cnt; i++) {
    if (ctx->session.frame_slots[i].udw_buf == addr) {
      ctx->session.frame_slots[i].state = ST_RX_ANC_SLOT_FREE;
      return;
    }
  }
}

uint64_t ut40_stat_anc_frames_ready(const ut_test_ctx* ctx) {
  return ctx->session.stat_anc_frames_ready;
}
uint64_t ut40_stat_anc_frames_dropped(const ut_test_ctx* ctx) {
  return ctx->session.stat_anc_frames_dropped;
}
uint64_t ut40_stat_anc_pkt_parse_err(const ut_test_ctx* ctx) {
  return ctx->session.stat_anc_pkt_parse_err;
}

/* Build + feed an mbuf carrying one real ANC packet (parity + checksum
 * applied by harness). Use corrupt_parity_word>=0 to flip parity bits on
 * that UDW word; corrupt_checksum=true to bit-flip the checksum word. */
int ut40_feed_anc_pkt(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                      enum mtl_session_port port, uint8_t did, uint8_t sdid,
                      const uint8_t* udw_bytes, uint16_t udw_size,
                      int corrupt_parity_word, bool corrupt_checksum) {
  struct rte_mbuf* m = rte_pktmbuf_alloc(ut_pool());
  if (!m) return -1;

  size_t hdr_offset = sizeof(struct st_rfc3550_hdr) - sizeof(struct st_rfc3550_rtp_hdr);
  uint32_t total_bits = (uint32_t)(3 + udw_size + 1) * 10;
  uint32_t udw_payload_size = (total_bits + 7) / 8;
  udw_payload_size = (udw_payload_size + 3) & ~0x3U;
  size_t anc_pkt_bytes = sizeof(struct st40_rfc8331_payload_hdr) - 4 + udw_payload_size;
  size_t total = hdr_offset + sizeof(struct st40_rfc8331_rtp_hdr) + anc_pkt_bytes;

  if (rte_pktmbuf_tailroom(m) < total) {
    rte_pktmbuf_free(m);
    return -1;
  }

  uint8_t* buf = rte_pktmbuf_mtod(m, uint8_t*);
  memset(buf, 0, total);

  /* RTP base + first chunk (anc_count = 1, f=0) */
  struct st40_rfc8331_rtp_hdr* rtp = (struct st40_rfc8331_rtp_hdr*)(buf + hdr_offset);
  rtp->base.version = 2;
  rtp->base.seq_number = htons(seq);
  rtp->base.tmstamp = htonl(ts);
  rtp->base.payload_type = 0;
  rtp->base.marker = marker ? 1 : 0;
  uint32_t chunk = ((uint32_t)1) << 24; /* anc_count = 1, f=0 */
  rtp->swapped_first_hdr_chunk = htonl(chunk);

  /* ANC payload */
  struct st40_rfc8331_payload_hdr* pkt =
      (struct st40_rfc8331_payload_hdr*)((uint8_t*)rtp + sizeof(*rtp));
  pkt->first_hdr_chunk.c = 0;
  pkt->first_hdr_chunk.line_number = 9;
  pkt->first_hdr_chunk.horizontal_offset = 0;
  pkt->first_hdr_chunk.s = 0;
  pkt->first_hdr_chunk.stream_num = 0;
  pkt->second_hdr_chunk.did = st40_add_parity_bits(did);
  pkt->second_hdr_chunk.sdid = st40_add_parity_bits(sdid);
  pkt->second_hdr_chunk.data_count = st40_add_parity_bits((uint8_t)udw_size);
  pkt->swapped_first_hdr_chunk = htonl(pkt->swapped_first_hdr_chunk);
  pkt->swapped_second_hdr_chunk = htonl(pkt->swapped_second_hdr_chunk);

  uint8_t* udw_dst = (uint8_t*)&pkt->second_hdr_chunk;
  for (uint16_t i = 0; i < udw_size; i++) {
    uint16_t v = st40_add_parity_bits(udw_bytes[i]);
    if (corrupt_parity_word == (int)i) v ^= 0x300; /* flip parity bits */
    st40_set_udw(i + 3, v, udw_dst);
  }
  uint16_t cs = st40_calc_checksum(3 + udw_size, udw_dst);
  if (corrupt_checksum) cs ^= 0x1;
  st40_set_udw(udw_size + 3, cs, udw_dst);

  m->data_len = total;
  m->pkt_len = total;

  int rc = rx_ancillary_session_handle_pkt(&ctx->impl, &ctx->session, m, port);
  rte_pktmbuf_free(m);
  return rc;
}

/* Framing-only feed: one well-formed empty (udw_size=0) ANC packet. Use for
 * slot-lifecycle tests where ANC content doesn't matter but the parser must
 * still accept the packet. */
int ut40_feed_pkt_anc0(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                       enum mtl_session_port port) {
  return ut40_feed_anc_pkt(ctx, seq, ts, marker, port, 0x41, 0x05, NULL, 0, -1, false);
}

void ut40_session_reset(ut_test_ctx* ctx) {
  rx_ancillary_session_reset(&ctx->session, false);
}

int ut40_ring_dequeue_markers(int* out_count, bool* out_has_marker) {
  *out_count = 0;
  *out_has_marker = false;
  if (!g_ring) return -1;

  struct rte_mbuf* pkt = NULL;
  while (rte_ring_sc_dequeue(g_ring, (void**)&pkt) == 0) {
    size_t hdr_offset = sizeof(struct st_rfc3550_hdr) - sizeof(struct st_rfc3550_rtp_hdr);
    struct st_rfc3550_rtp_hdr* rtp =
        rte_pktmbuf_mtod_offset(pkt, struct st_rfc3550_rtp_hdr*, hdr_offset);
    if (rtp->marker) *out_has_marker = true;
    (*out_count)++;
    rte_pktmbuf_free(pkt);
  }
  return 0;
}
