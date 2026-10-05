/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <stdlib.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "mt_main.h"
#include "st2110/st_tx_ancillary_session.h"

struct ut_txa_ctx {
  struct mtl_main_impl impl;
  struct st_tx_ancillary_sessions_mgr mgr;
  struct st_tx_ancillary_session_impl session;
  struct st_frame_trans frame;
  struct st40_frame frame_data;
  uint64_t mock_ptp_ns;
  uint64_t mock_tsc_ns;
  enum st10_timestamp_fmt app_tfmt;
  uint64_t app_timestamp;
  int get_next_frame_calls;
  int frame_count;
  int notify_frame_done_calls;
  uint16_t notify_frame_done_idx;
  struct st40_tx_frame_meta notify_frame_done_meta;
  enum st40_tx_frame_status frame_status;
  int frame_refcnt;
  uint32_t packet_len;
  int notify_late_calls;
  uint64_t notify_late_last_delta;
};

#include "session/st40_tx_harness.h"

static uint64_t ut_txa_ptp_time_fn(struct mtl_main_impl* impl, enum mtl_port port) {
  (void)port;
  struct ut_txa_ctx* ctx = (struct ut_txa_ctx*)impl;
  return ctx->mock_ptp_ns;
}

static uint64_t ut_txa_tsc_time_fn(struct mtl_main_impl* impl) {
  struct ut_txa_ctx* ctx = (struct ut_txa_ctx*)impl;
  return ctx->mock_tsc_ns;
}

static int ut_txa_notify_frame_late(void* priv, uint64_t epoch_skipped) {
  struct ut_txa_ctx* ctx = priv;
  ctx->notify_late_calls++;
  ctx->notify_late_last_delta = epoch_skipped;
  return 0;
}

static int ut_txa_get_next_frame(void* priv, uint16_t* next_frame_idx,
                                 struct st40_tx_frame_meta* meta) {
  struct ut_txa_ctx* ctx = priv;
  if (ctx->get_next_frame_calls >= ctx->frame_count) return -EIO;
  ctx->get_next_frame_calls++;
  *next_frame_idx = 0;
  meta->tfmt = ctx->app_tfmt;
  meta->timestamp = ctx->app_timestamp;
  return 0;
}

static int ut_txa_notify_frame_done(void* priv, uint16_t frame_idx,
                                    struct st40_tx_frame_meta* meta) {
  struct ut_txa_ctx* ctx = priv;
  ctx->notify_frame_done_calls++;
  ctx->notify_frame_done_idx = frame_idx;
  ctx->notify_frame_done_meta = *meta;
  return 0;
}

static int ut_txa_notify_rtp_done(void* priv) {
  (void)priv;
  return 0;
}

#define mt_get_tsc ut_txa_tsc_time_fn
#include "st2110/st_tx_ancillary_session.c"
#undef mt_get_tsc

int ut_txa_init(void) {
  return ut_eal_init();
}

ut_txa_ctx* ut_txa_create(void) {
  ut_txa_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->impl.type = MT_HANDLE_MAIN;
  ctx->impl.tsc_hz = rte_get_tsc_hz();
  ctx->impl.inf[MTL_PORT_P].ptp_get_time_fn = ut_txa_ptp_time_fn;
  ctx->mgr.parent = &ctx->impl;
  ctx->mgr.max_idx = 1;
  rte_spinlock_init(&ctx->mgr.mutex[0]);
  ctx->session.mgr = &ctx->mgr;
  ctx->session.pacing.frame_time = NS_PER_MS;
  ctx->session.pacing.max_onward_epochs = 3;
  ctx->session.fps_tm.sampling_clock_rate = ST10_VIDEO_SAMPLING_RATE_90K;
  ctx->session.max_pkt_len = ST_PKT_MAX_ETHER_BYTES - sizeof(struct st_rfc8331_anc_hdr);
  ctx->session.ops.get_next_frame = ut_txa_get_next_frame;
  ctx->session.ops.notify_frame_done = ut_txa_notify_frame_done;
  ctx->session.ops.notify_frame_late = ut_txa_notify_frame_late;
  ctx->session.ops.priv = ctx;
  ctx->frame_count = 1;
  ctx->mgr.sessions[0] = &ctx->session;
  return ctx;
}

void ut_txa_destroy(ut_txa_ctx* ctx) {
  ut_txa_cleanup_frame_tasklet(ctx);
  free(ctx);
}

void ut_txa_set_cur_epochs(ut_txa_ctx* ctx, uint64_t cur_epochs) {
  ctx->session.pacing.cur_epochs = cur_epochs;
}

void ut_txa_set_frame_time(ut_txa_ctx* ctx, long double frame_time_ns) {
  ctx->session.pacing.frame_time = frame_time_ns;
}

void ut_txa_set_user_pacing(ut_txa_ctx* ctx, bool enable) {
  if (enable)
    ctx->session.ops.flags |= ST40_TX_FLAG_USER_PACING;
  else
    ctx->session.ops.flags &= ~ST40_TX_FLAG_USER_PACING;
}

void ut_txa_set_exact_user_pacing(ut_txa_ctx* ctx, bool enable) {
  if (enable)
    ctx->session.ops.flags |= ST40_TX_FLAG_EXACT_USER_PACING;
  else
    ctx->session.ops.flags &= ~ST40_TX_FLAG_EXACT_USER_PACING;
}

void ut_txa_set_user_timestamp(ut_txa_ctx* ctx, bool enable) {
  if (enable)
    ctx->session.ops.flags |= ST40_TX_FLAG_USER_TIMESTAMP;
  else
    ctx->session.ops.flags &= ~ST40_TX_FLAG_USER_TIMESTAMP;
}

void ut_txa_set_mock_ptp_time(ut_txa_ctx* ctx, uint64_t ptp_ns) {
  ctx->mock_ptp_ns = ptp_ns;
}

void ut_txa_set_mock_tsc_time(ut_txa_ctx* ctx, uint64_t tsc_ns) {
  ctx->mock_tsc_ns = tsc_ns;
}

uint64_t ut_txa_calc_epoch(ut_txa_ctx* ctx, uint64_t cur_tai, uint64_t required_tai) {
  return tx_ancillary_calc_epoch(&ctx->session, cur_tai, required_tai);
}

uint64_t ut_txa_pacing_required_tai(ut_txa_ctx* ctx, enum st10_timestamp_fmt tfmt,
                                    uint64_t timestamp) {
  return tx_ancillary_pacing_required_tai(&ctx->session, tfmt, timestamp);
}

int ut_txa_sync_pacing(ut_txa_ctx* ctx, uint64_t required_tai) {
  return tx_ancillary_session_sync_pacing(&ctx->impl, &ctx->session, required_tai);
}

void ut_txa_update_rtp_time_stamp(ut_txa_ctx* ctx, enum st10_timestamp_fmt tfmt,
                                  uint64_t timestamp) {
  tx_ancillary_update_rtp_time_stamp(&ctx->session, tfmt, timestamp);
}

int ut_txa_prepare_frame_tasklet(ut_txa_ctx* ctx, enum st10_timestamp_fmt tfmt,
                                 uint64_t timestamp, unsigned int packets) {
  static unsigned int test_idx;
  struct st_tx_ancillary_session_impl* s = &ctx->session;
  char pool_name[RTE_MEMPOOL_NAMESIZE];
  char ring_name[RTE_RING_NAMESIZE];

  if (!packets || packets > 2) return -EINVAL;
  ut_txa_cleanup_frame_tasklet(ctx);

  snprintf(pool_name, sizeof(pool_name), "ut_txa_pool_%u", test_idx);
  snprintf(ring_name, sizeof(ring_name), "ut_txa_ring_%u", test_idx++);
  s->mbuf_mempool_hdr[MTL_SESSION_PORT_P] =
      rte_pktmbuf_pool_create(pool_name, 32, 0, sizeof(struct mt_muf_priv_data),
                              RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
  ctx->mgr.ring[MTL_PORT_P] = ut_ring_create(ring_name, 32);
  if (!s->mbuf_mempool_hdr[MTL_SESSION_PORT_P] || !ctx->mgr.ring[MTL_PORT_P]) {
    ut_txa_cleanup_frame_tasklet(ctx);
    return -ENOMEM;
  }

  memset(&ctx->frame, 0, sizeof(ctx->frame));
  memset(&ctx->frame_data, 0, sizeof(ctx->frame_data));
  ctx->frame.addr = &ctx->frame_data;
  ctx->frame_data.meta_num = packets == 2 ? 2 : 0;
  s->st40_frames = &ctx->frame;
  s->st40_frames_cnt = 1;
  s->st40_frame_stat = ST40_TX_STAT_WAIT_FRAME;
  s->calculate_time_cursor = true;
  s->max_pkt_len = 1200;
  s->tx_no_chain = true;
  s->split_payload = packets == 2;
  s->ops.num_port = 1;
  s->ops.type = ST40_TYPE_FRAME_LEVEL;
  ctx->app_tfmt = tfmt;
  ctx->app_timestamp = timestamp;
  ctx->get_next_frame_calls = 0;
  ctx->notify_frame_done_calls = 0;
  memset(&ctx->notify_frame_done_meta, 0, sizeof(ctx->notify_frame_done_meta));
  return 0;
}

int ut_txa_prepare_rtp_tasklet(ut_txa_ctx* ctx) {
  static unsigned int test_idx;
  char ring_name[RTE_RING_NAMESIZE];
  int ret = ut_txa_prepare_frame_tasklet(ctx, ST10_TIMESTAMP_FMT_TAI, 0, 1);

  if (ret < 0) return ret;
  (void)snprintf(ring_name, sizeof(ring_name), "ut_txa_rtp_%u", test_idx++);
  ctx->session.packet_ring = ut_ring_create(ring_name, 32);
  if (!ctx->session.packet_ring) {
    ut_txa_cleanup_frame_tasklet(ctx);
    return -ENOMEM;
  }
  ctx->session.ops.type = ST40_TYPE_RTP_LEVEL;
  ctx->session.ops.notify_rtp_done = ut_txa_notify_rtp_done;
  return 0;
}

int ut_txa_put_app_rtp(ut_txa_ctx* ctx, const uint8_t* rtp, uint16_t len) {
  struct rte_mbuf* m = rte_pktmbuf_alloc(ut_pool());

  if (!m) return -ENOMEM;
  if (rte_pktmbuf_tailroom(m) < sizeof(struct mt_udp_hdr) + len) {
    rte_pktmbuf_free(m);
    return -ENOSPC;
  }
  memcpy(rte_pktmbuf_mtod_offset(m, uint8_t*, sizeof(struct mt_udp_hdr)), rtp, len);
  m->data_len = sizeof(struct mt_udp_hdr) + len;
  m->pkt_len = m->data_len;
  if (rte_ring_sp_enqueue(ctx->session.packet_ring, m) < 0) {
    rte_pktmbuf_free(m);
    return -ENOSPC;
  }
  return 0;
}

int ut_txa_step_frame_tasklet(ut_txa_ctx* ctx) {
  return tx_ancillary_sessions_tasklet_handler(&ctx->mgr);
}

unsigned int ut_txa_queued_packets(const ut_txa_ctx* ctx) {
  if (!ctx->mgr.ring[MTL_PORT_P]) return 0;
  return rte_ring_count(ctx->mgr.ring[MTL_PORT_P]);
}

void ut_txa_set_frame_count(ut_txa_ctx* ctx, int frames) {
  ctx->frame_count = frames;
}

int ut_txa_pop_packet(ut_txa_ctx* ctx, uint64_t* packet_tsc, uint8_t* out, uint32_t cap) {
  struct rte_mbuf* packet = NULL;

  if (!ctx->mgr.ring[MTL_PORT_P] ||
      rte_ring_sc_dequeue(ctx->mgr.ring[MTL_PORT_P], (void**)&packet) < 0)
    return -ENOENT;
  int len = RTE_MIN(packet->pkt_len, cap);
  const void* data = rte_pktmbuf_read(packet, 0, len, out);
  if (data != out) memcpy(out, data, len);
  *packet_tsc = st_tx_mbuf_get_tsc(packet);
  rte_pktmbuf_free(packet);
  return len;
}

int ut_txa_pop_packet_tsc(ut_txa_ctx* ctx, uint64_t* packet_tsc) {
  struct rte_mbuf* packet = NULL;

  if (!ctx->mgr.ring[MTL_PORT_P] ||
      rte_ring_sc_dequeue(ctx->mgr.ring[MTL_PORT_P], (void**)&packet) < 0)
    return -ENOENT;
  *packet_tsc = st_tx_mbuf_get_tsc(packet);
  rte_pktmbuf_free(packet);
  return 0;
}

void ut_txa_cleanup_frame_tasklet(ut_txa_ctx* ctx) {
  struct st_tx_ancillary_session_impl* s;

  if (!ctx) return;
  s = &ctx->session;
  if (ctx->mgr.ring[MTL_PORT_P]) {
    ut_ring_drain(ctx->mgr.ring[MTL_PORT_P]);
    rte_ring_free(ctx->mgr.ring[MTL_PORT_P]);
    ctx->mgr.ring[MTL_PORT_P] = NULL;
  }
  if (s->packet_ring) {
    ut_ring_drain(s->packet_ring);
    rte_ring_free(s->packet_ring);
    s->packet_ring = NULL;
  }
  if (s->mbuf_mempool_hdr[MTL_SESSION_PORT_P]) {
    rte_mempool_free(s->mbuf_mempool_hdr[MTL_SESSION_PORT_P]);
    s->mbuf_mempool_hdr[MTL_SESSION_PORT_P] = NULL;
  }
  s->st40_frames = NULL;
}

int ut_txa_run_frame_tasklet(ut_txa_ctx* ctx, enum st10_timestamp_fmt tfmt,
                             uint64_t timestamp, uint64_t* packet_tsc) {
  int ret = ut_txa_prepare_frame_tasklet(ctx, tfmt, timestamp, 1);
  if (ret < 0) return ret;

  ut_txa_step_frame_tasklet(ctx);
  ctx->mock_tsc_ns = (uint64_t)ctx->session.pacing.tsc_time_cursor;
  ut_txa_step_frame_tasklet(ctx);
  ctx->frame_status = ctx->session.st40_frame_stat;
  ctx->frame_refcnt = rte_atomic32_read(&ctx->frame.refcnt);
  ret = ut_txa_pop_packet_tsc(ctx, packet_tsc);
  ctx->packet_len = ctx->session.port_user_stats.common.port[MTL_SESSION_PORT_P].bytes;
  ut_txa_cleanup_frame_tasklet(ctx);
  return ret;
}

uint64_t ut_txa_cur_epochs(const ut_txa_ctx* ctx) {
  return ctx->session.pacing.cur_epochs;
}

uint64_t ut_txa_ptp_time_cursor(const ut_txa_ctx* ctx) {
  return (uint64_t)ctx->session.pacing.ptp_time_cursor;
}

uint64_t ut_txa_tsc_time_cursor(const ut_txa_ctx* ctx) {
  return (uint64_t)ctx->session.pacing.tsc_time_cursor;
}

uint64_t ut_txa_stat_epoch_onward(const ut_txa_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_epoch_onward;
}

uint64_t ut_txa_stat_epoch_drop(const ut_txa_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_epoch_drop;
}

uint64_t ut_txa_stat_error_user_timestamp(const ut_txa_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_error_user_timestamp;
}

uint64_t ut_txa_stat_epoch_mismatch(const ut_txa_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_epoch_mismatch;
}

int ut_txa_notify_late_calls(const ut_txa_ctx* ctx) {
  return ctx->notify_late_calls;
}

uint64_t ut_txa_notify_late_last_delta(const ut_txa_ctx* ctx) {
  return ctx->notify_late_last_delta;
}

int ut_txa_get_next_frame_calls(const ut_txa_ctx* ctx) {
  return ctx->get_next_frame_calls;
}

int ut_txa_notify_frame_done_calls(const ut_txa_ctx* ctx) {
  return ctx->notify_frame_done_calls;
}

uint16_t ut_txa_notify_frame_done_idx(const ut_txa_ctx* ctx) {
  return ctx->notify_frame_done_idx;
}

uint64_t ut_txa_notify_frame_done_epoch(const ut_txa_ctx* ctx) {
  return ctx->notify_frame_done_meta.epoch;
}

uint64_t ut_txa_notify_frame_done_timestamp(const ut_txa_ctx* ctx) {
  return ctx->notify_frame_done_meta.timestamp;
}

enum st10_timestamp_fmt ut_txa_notify_frame_done_tfmt(const ut_txa_ctx* ctx) {
  return ctx->notify_frame_done_meta.tfmt;
}

uint32_t ut_txa_notify_frame_done_rtp_timestamp(const ut_txa_ctx* ctx) {
  return ctx->notify_frame_done_meta.rtp_timestamp;
}

bool ut_txa_frame_is_waiting(const ut_txa_ctx* ctx) {
  return ctx->frame_status == ST40_TX_STAT_WAIT_FRAME;
}

int ut_txa_frame_refcnt(const ut_txa_ctx* ctx) {
  return ctx->frame_refcnt;
}

uint64_t ut_txa_stat_port_build(const ut_txa_ctx* ctx) {
  return ctx->session.port_user_stats.common.port[MTL_SESSION_PORT_P].build;
}

uint64_t ut_txa_stat_port_packets(const ut_txa_ctx* ctx) {
  return ctx->session.port_user_stats.common.port[MTL_SESSION_PORT_P].packets;
}

uint64_t ut_txa_stat_port_bytes(const ut_txa_ctx* ctx) {
  return ctx->session.port_user_stats.common.port[MTL_SESSION_PORT_P].bytes;
}

uint32_t ut_txa_packet_len(const ut_txa_ctx* ctx) {
  return ctx->packet_len;
}

uint64_t ut_txa_stat_port_frames(const ut_txa_ctx* ctx) {
  return ctx->session.port_user_stats.common.port[MTL_SESSION_PORT_P].frames;
}

uint64_t ut_txa_stat_recoverable_error(const ut_txa_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_recoverable_error;
}

uint64_t ut_txa_stat_unrecoverable_error(const ut_txa_ctx* ctx) {
  return ctx->session.port_user_stats.common.stat_unrecoverable_error;
}

uint32_t ut_txa_rtp_time_stamp(const ut_txa_ctx* ctx) {
  return ctx->session.pacing.rtp_time_stamp;
}

void ut_txa_set_fmd(ut_txa_ctx* ctx, uint32_t fmd_dit, uint8_t fmd_k_bit) {
  ctx->session.ops.flags |= ST40_TX_FLAG_FAST_METADATA;
  ctx->session.ops.fmd_dit = fmd_dit;
  ctx->session.ops.fmd_k_bit = fmd_k_bit;
  ctx->session.max_pkt_len = ST_PKT_MAX_ETHER_BYTES - sizeof(struct st_fmd_hdr);
}

int ut_txa_init_hdr(ut_txa_ctx* ctx, uint8_t payload_type) {
  struct st_tx_ancillary_session_impl* s = &ctx->session;
  static const uint8_t dip[MTL_IP_ADDR_LEN] = {239, 1, 1, 1};

  s->ops.payload_type = payload_type;
  s->ops.flags |= ST40_TX_FLAG_USER_P_MAC;
  memcpy(s->ops.dip_addr[MTL_SESSION_PORT_P], dip, sizeof(dip));
  ctx->impl.inf[MTL_PORT_P].drv_info.flags |= MT_DRV_F_NOT_DPDK_PMD;
  return tx_ancillary_session_init_hdr(&ctx->impl, &ctx->mgr, s, MTL_SESSION_PORT_P);
}

void ut_txa_set_frame_payload(ut_txa_ctx* ctx, uint8_t* data, uint32_t size) {
  struct st_tx_ancillary_session_impl* s = &ctx->session;

  ctx->frame.addr = &ctx->frame_data;
  ctx->frame_data.data = data;
  ctx->frame_data.data_size = size;
  s->st40_frames = &ctx->frame;
  s->st40_frames_cnt = 1;
  s->st40_frame_idx = 0;
  s->st40_total_pkts = 1;
  s->st40_pkt_idx = 0;
}

void ut_txa_set_anc_meta(ut_txa_ctx* ctx, uint16_t udw_size, uint32_t meta_num) {
  for (uint32_t i = 0; i < meta_num; i++) {
    memset(&ctx->frame_data.meta[i], 0, sizeof(ctx->frame_data.meta[i]));
    ctx->frame_data.meta[i].did = 0x41;
    ctx->frame_data.meta[i].sdid = 0x05;
    ctx->frame_data.meta[i].udw_size = udw_size;
  }
  ctx->frame_data.meta_num = meta_num;
}

void ut_txa_set_ssrc(ut_txa_ctx* ctx, uint32_t ssrc) {
  ctx->session.ops.ssrc = ssrc;
}

void ut_txa_set_seq(ut_txa_ctx* ctx, uint16_t seq) {
  ctx->session.st40_seq_id = seq;
}

struct rte_mbuf* ut_txa_alloc_mbuf(size_t room) {
  struct rte_mbuf* m = rte_pktmbuf_alloc(ut_pool());
  if (!m) return NULL;
  if (room > m->buf_len) {
    rte_pktmbuf_free(m);
    return NULL;
  }
  m->data_off = (uint16_t)(m->buf_len - room);
  return m;
}

void ut_txa_free_mbuf(struct rte_mbuf* m) {
  rte_pktmbuf_free(m);
}

int ut_txa_build_packet(ut_txa_ctx* ctx, struct rte_mbuf* pkt) {
  return tx_ancillary_session_build_packet(&ctx->session, pkt);
}

int ut_txa_build_rtp_packet(ut_txa_ctx* ctx, struct rte_mbuf* pkt) {
  return tx_ancillary_session_build_rtp_packet(&ctx->session, pkt,
                                               ctx->session.st40_anc_idx);
}

uint8_t* ut_txa_pkt_data(struct rte_mbuf* pkt) {
  return rte_pktmbuf_mtod(pkt, uint8_t*);
}

uint32_t ut_txa_pkt_data_len(const struct rte_mbuf* pkt) {
  return pkt->data_len;
}

uint32_t ut_txa_pkt_pkt_len(const struct rte_mbuf* pkt) {
  return pkt->pkt_len;
}

void ut_txa_set_pkt_len(struct rte_mbuf* pkt, uint32_t len) {
  pkt->data_len = len;
  pkt->pkt_len = len;
}

size_t ut_txa_udp_hdr_len(void) {
  return sizeof(struct mt_udp_hdr);
}

size_t ut_txa_fmd_hdr_len(void) {
  return sizeof(struct st_fmd_hdr);
}

size_t ut_txa_chain_room(void) {
  return ST_PKT_MAX_ETHER_BYTES - sizeof(struct mt_udp_hdr);
}

int ut_txa_stat_build_ret_code(const ut_txa_ctx* ctx) {
  return ctx->session.stat_build_ret_code;
}

int ut_txa_err_anc_too_large(void) {
  return -STI_FRAME_ANC_TOO_LARGE;
}

bool ut_txa_session_waiting_frame(const ut_txa_ctx* ctx) {
  return ctx->session.st40_frame_stat == ST40_TX_STAT_WAIT_FRAME;
}

void ut_txa_set_rtp_level(ut_txa_ctx* ctx, bool interlaced) {
  ctx->session.ops.type = ST40_TYPE_RTP_LEVEL;
  ctx->session.ops.interlaced = interlaced;
  ctx->session.ops.num_port = 1;
}

int ut_txa_rtp_update_packet(ut_txa_ctx* ctx, struct rte_mbuf* pkt) {
  return tx_ancillary_session_rtp_update_packet(&ctx->impl, &ctx->session, pkt);
}

int ut_txa_build_packet_chain(ut_txa_ctx* ctx, struct rte_mbuf* pkt,
                              struct rte_mbuf* rtp) {
  return tx_ancillary_session_build_packet_chain(&ctx->impl, &ctx->session, pkt, rtp,
                                                 MTL_SESSION_PORT_P);
}

uint64_t ut_txa_stat_interlace_first(const ut_txa_ctx* ctx) {
  return ctx->session.port_user_stats.stat_interlace_first_field;
}

uint64_t ut_txa_stat_interlace_second(const ut_txa_ctx* ctx) {
  return ctx->session.port_user_stats.stat_interlace_second_field;
}

int ut_txa_ops_check(uint32_t flags, uint32_t fmd_dit, uint8_t fmd_k_bit,
                     uint8_t payload_type) {
  struct st40_tx_ops ops;
  static const uint8_t dip[MTL_IP_ADDR_LEN] = {239, 1, 1, 1};

  memset(&ops, 0, sizeof(ops));
  ops.num_port = 1;
  memcpy(ops.dip_addr[MTL_SESSION_PORT_P], dip, sizeof(dip));
  ops.type = ST40_TYPE_FRAME_LEVEL;
  ops.framebuff_cnt = 1;
  ops.get_next_frame = ut_txa_get_next_frame;
  ops.flags = flags;
  ops.fmd_dit = fmd_dit;
  ops.fmd_k_bit = fmd_k_bit;
  ops.payload_type = payload_type;
  return tx_ancillary_ops_check(&ops);
}
