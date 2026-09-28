/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Attack harness for the RTCP TX NACK path, see rtcp_attack_harness.h.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "common/ut_common.h"

/* rtcp_tx_harness.c compiles mt_rtcp.c too; give this copy private names so
 * the link cannot pick the other one. Mock the burst and read_front. */
#undef MTL_HAS_USDT
#define mt_rtcp_tx_buffer_rtp_packets ut_rtk_tx_buffer_rtp_packets
#define mt_rtcp_tx_parse_rtcp_packet ut_rtk_tx_parse_rtcp_packet
#define mt_rtcp_rx_parse_rtp_packet ut_rtk_rx_parse_rtp_packet
#define mt_rtcp_rx_send_nack_packet ut_rtk_rx_send_nack_packet
#define mt_rtcp_tx_create ut_rtk_tx_create
#define mt_rtcp_tx_free ut_rtk_tx_free
#define mt_rtcp_rx_create ut_rtk_rx_create
#define mt_rtcp_rx_free ut_rtk_rx_free
#define mt_txq_burst ut_rtk_txq_burst
#define mt_u64_fifo_read_front ut_rtk_fifo_read_front
#include "mt_rtcp.c"
#undef mt_txq_burst
#undef mt_u64_fifo_read_front

#include "session/rtcp_attack_harness.h"

int mt_u64_fifo_read_front(struct mt_u64_fifo* fifo, uint64_t* item);

#define UT_RTK_GUARD_SLOTS 16
#define UT_RTK_GUARD_VAL 0xdeadbeefcafef00dULL
#define UT_RTK_SENT_CAP (1u << 18)
#define UT_RTK_PKT_LEN (sizeof(struct mt_udp_hdr) + sizeof(struct st20_rfc4175_rtp_hdr))

struct ut_rtk_ctx {
  struct mt_rtcp_tx tx;
  struct mt_u64_fifo fifo;
  struct rte_mempool* src_pool;
  struct rte_mempool* copy_pool;
  unsigned int copy_avail_at_create;
  int burst_limit;
  uint32_t sent;
  uint16_t* sent_seq;
  uint16_t* sent_row_length;
};

static ut_rtk_ctx* ut_rtk_active;
static uintptr_t ut_rtk_stack_top;
static size_t ut_rtk_stack_depth;

uint16_t ut_rtk_txq_burst(struct mt_txq_entry* entry, struct rte_mbuf** pkts,
                          uint16_t nb) {
  (void)entry;
  ut_rtk_ctx* ctx = ut_rtk_active;
  uint16_t take = nb;
  if (ctx->burst_limit >= 0 && take > ctx->burst_limit) take = ctx->burst_limit;
  for (uint16_t i = 0; i < take; i++) {
    struct st20_rfc4175_rtp_hdr* rtp = rte_pktmbuf_mtod_offset(
        pkts[i], struct st20_rfc4175_rtp_hdr*, sizeof(struct mt_udp_hdr));
    if (ctx->sent < UT_RTK_SENT_CAP) {
      ctx->sent_seq[ctx->sent] = ntohs(rtp->base.seq_number);
      ctx->sent_row_length[ctx->sent] = ntohs(rtp->row_length);
    }
    ctx->sent++;
  }
  rte_pktmbuf_free_bulk(pkts, take);
  return take;
}

__attribute__((noinline, noclone)) int ut_rtk_fifo_read_front(struct mt_u64_fifo* fifo,
                                                              uint64_t* item) {
  uintptr_t here = (uintptr_t)__builtin_frame_address(0);
  size_t depth = ut_rtk_stack_top > here ? ut_rtk_stack_top - here : 0;
  if (depth > ut_rtk_stack_depth) ut_rtk_stack_depth = depth;
  return mt_u64_fifo_read_front(fifo, item);
}

int ut_rtk_init(void) {
  return ut_eal_init();
}

static struct rte_mempool* ut_rtk_pool(unsigned int n) {
  static int seq;
  char name[32];
  snprintf(name, sizeof(name), "ut_rtk_%d", seq++);
  return rte_pktmbuf_pool_create(name, n, 0, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
                                 rte_socket_id());
}

ut_rtk_ctx* ut_rtk_create(int ring_size, int rfc4175, unsigned int pool_n) {
  ut_rtk_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;
  ctx->fifo.data = calloc(ring_size + UT_RTK_GUARD_SLOTS, sizeof(uint64_t));
  ctx->sent_seq = calloc(UT_RTK_SENT_CAP, sizeof(uint16_t));
  ctx->sent_row_length = calloc(UT_RTK_SENT_CAP, sizeof(uint16_t));
  for (int i = 0; i < UT_RTK_GUARD_SLOTS; i++)
    ctx->fifo.data[ring_size + i] = UT_RTK_GUARD_VAL;
  ctx->fifo.size = ring_size;

  /* A ring of 65535 only probes the stack; it never gets packets. */
  unsigned int src_n = ring_size <= 4096 ? ring_size + 64 : 64;
  ctx->src_pool = ut_rtk_pool(src_n);
  ctx->copy_pool = ut_rtk_pool(pool_n ? pool_n : 4096);
  ctx->copy_avail_at_create = rte_mempool_avail_count(ctx->copy_pool);
  ctx->burst_limit = -1;

  struct mt_rtcp_tx* tx = &ctx->tx;
  tx->active = true;
  tx->mbuf_ring = &ctx->fifo;
  tx->mbuf_pool = ctx->copy_pool;
  tx->payload_format =
      rfc4175 ? MT_RTP_PAYLOAD_FORMAT_RFC4175 : MT_RTP_PAYLOAD_FORMAT_RAW;
  snprintf(tx->name, sizeof(tx->name), "ut_rtk");
  mt_set_log_global_level(MTL_LOG_LEVEL_CRIT);
  return ctx;
}

void ut_rtk_destroy(ut_rtk_ctx* ctx) {
  uint64_t item;
  while (mt_u64_fifo_get(&ctx->fifo, &item) >= 0)
    rte_pktmbuf_free((struct rte_mbuf*)item);
  rte_mempool_free(ctx->src_pool);
  rte_mempool_free(ctx->copy_pool);
  free(ctx->fifo.data);
  free(ctx->sent_seq);
  free(ctx->sent_row_length);
  free(ctx);
}

int ut_rtk_buffer(ut_rtk_ctx* ctx, uint16_t first, unsigned int n) {
  for (unsigned int i = 0; i < n; i++) {
    struct rte_mbuf* m = rte_pktmbuf_alloc(ctx->src_pool);
    if (!m) return -ENOMEM;
    uint8_t* p = (uint8_t*)rte_pktmbuf_append(m, UT_RTK_PKT_LEN);
    memset(p, 0, UT_RTK_PKT_LEN);
    struct st20_rfc4175_rtp_hdr* rtp =
        (struct st20_rfc4175_rtp_hdr*)(p + sizeof(struct mt_udp_hdr));
    rtp->base.seq_number = htons((uint16_t)(first + i));
    rtp->row_length = htons(1200);
    int ret = ut_rtk_tx_buffer_rtp_packets(&ctx->tx, &m, 1);
    rte_pktmbuf_free(m); /* the ring holds its own reference */
    if (ret < 0) return ret;
  }
  return 0;
}

void ut_rtk_set_burst_limit(ut_rtk_ctx* ctx, int limit) {
  ctx->burst_limit = limit;
}

static __attribute__((noinline)) int ut_rtk_call_parse(ut_rtk_ctx* ctx, void* pkt,
                                                       size_t len) {
  ut_rtk_stack_top = (uintptr_t)__builtin_frame_address(0);
  ut_rtk_stack_depth = 0;
  return ut_rtk_tx_parse_rtcp_packet(&ctx->tx, (struct mt_rtcp_hdr*)pkt, len);
}

static struct ut_rtk_stats ut_rtk_parse_guarded(ut_rtk_ctx* ctx, const uint8_t* bytes,
                                                size_t len) {
  struct ut_rtk_stats s;
  memset(&s, 0, sizeof(s));
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  size_t data_pages = (len + page - 1) / page;
  if (!data_pages) data_pages = 1;
  size_t map_len = (data_pages + 1) * page;
  uint8_t* map =
      mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (map == MAP_FAILED) {
    s.ret = -ENOMEM;
    return s;
  }
  uint8_t* guard = map + data_pages * page;
  mprotect(guard, page, PROT_NONE);
  uint8_t* pkt = guard - len; /* last received byte touches the guard page */
  memcpy(pkt, bytes, len);

  struct mt_rtcp_tx* tx = &ctx->tx;
  uint32_t sent0 = ctx->sent;
  uint32_t nack0 = tx->stat_nack_received, succ0 = tx->stat_rtp_retransmit_succ;
  uint32_t fail0 = tx->stat_rtp_retransmit_fail;
  uint32_t obs0 = tx->stat_rtp_retransmit_fail_obsolete;
  uint32_t read0 = tx->stat_rtp_retransmit_fail_read;
  uint32_t nobuf0 = tx->stat_rtp_retransmit_fail_nobuf;
  uint32_t burst0 = tx->stat_rtp_retransmit_fail_burst;

  ut_rtk_active = ctx;
  s.ret = ut_rtk_call_parse(ctx, pkt, len);
  s.max_stack_depth = ut_rtk_stack_depth;

  s.nack_received = tx->stat_nack_received - nack0;
  s.succ = tx->stat_rtp_retransmit_succ - succ0;
  s.fail = tx->stat_rtp_retransmit_fail - fail0;
  s.fail_obsolete = tx->stat_rtp_retransmit_fail_obsolete - obs0;
  s.fail_read = tx->stat_rtp_retransmit_fail_read - read0;
  s.fail_nobuf = tx->stat_rtp_retransmit_fail_nobuf - nobuf0;
  s.fail_burst = tx->stat_rtp_retransmit_fail_burst - burst0;
  s.sent = ctx->sent - sent0;
  munmap(map, map_len);
  return s;
}

struct ut_rtk_stats ut_rtk_nack(ut_rtk_ctx* ctx, uint16_t len_field,
                                const struct ut_rtk_fci* fcis, uint32_t nb_fci,
                                size_t recv_len) {
  size_t build = sizeof(struct mt_rtcp_hdr) + nb_fci * sizeof(struct mt_rtcp_fci);
  if (build < recv_len) build = recv_len;
  uint8_t* buf = calloc(1, build);
  struct mt_rtcp_hdr* rtcp = (struct mt_rtcp_hdr*)buf;
  rtcp->flags = 0x80;
  rtcp->ptype = MT_RTCP_PTYPE_NACK;
  rtcp->len = htons(len_field);
  memcpy(rtcp->name, "IMTL", 4);
  for (uint32_t i = 0; i < nb_fci; i++) {
    rtcp->fci[i].start = htons(fcis[i].start);
    rtcp->fci[i].follow = htons(fcis[i].follow);
  }
  struct ut_rtk_stats s = ut_rtk_parse_guarded(ctx, buf, recv_len);
  free(buf);
  return s;
}

struct ut_rtk_stats ut_rtk_raw(ut_rtk_ctx* ctx, const uint8_t* bytes, size_t recv_len) {
  return ut_rtk_parse_guarded(ctx, bytes, recv_len);
}

uint16_t ut_rtk_sent_seq(ut_rtk_ctx* ctx, unsigned int idx) {
  return ctx->sent_seq[idx];
}

uint16_t ut_rtk_sent_row_length(ut_rtk_ctx* ctx, unsigned int idx) {
  return ctx->sent_row_length[idx];
}

int ut_rtk_fifo_guard_intact(ut_rtk_ctx* ctx) {
  for (int i = 0; i < UT_RTK_GUARD_SLOTS; i++)
    if (ctx->fifo.data[ctx->fifo.size + i] != UT_RTK_GUARD_VAL) return 0;
  return 1;
}

unsigned int ut_rtk_pool_avail(ut_rtk_ctx* ctx) {
  return rte_mempool_avail_count(ctx->copy_pool);
}

unsigned int ut_rtk_pool_avail_at_create(ut_rtk_ctx* ctx) {
  return ctx->copy_avail_at_create;
}
