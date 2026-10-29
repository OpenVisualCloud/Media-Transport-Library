/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

#include <stdlib.h>
#include <string.h>

/* libmtl exports the real ipmx_sr_*(), give the copies compiled here private names */
#define ipmx_sr_tx_init ut_sr_tx_init
#define ipmx_sr_tx_uinit ut_sr_tx_uinit
#define ipmx_sr_tx_frame_start ut_sr_tx_frame_start
#define ipmx_sr_tx_send_pending ut_sr_tx_send_pending
#define ipmx_sr_tx_rl_lead_ns ut_sr_tx_rl_lead_ns
#define ipmx_sr_stat_log ut_sr_stat_log
#define ipmx_sr_tx_set_block_version ut_sr_tx_set_block_version
#define ipmx_sr_tx_get_info ut_sr_tx_get_info

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "ipmx/ipmx.h"
#include "ipmx/ipmx_info.h"
#include "ipmx/ipmx_sr.h"
#include "ipmx/ipmx_sr_harness.h"
#include "mt_main.h"
#include "st2110/st_tx_video_session.h"

#define UT_SR_WIRE_MAX (512)
#define UT_SR_HELD_MAX (128)
#define UT_SR_PAD_SIZE (1262)

struct ut_sr_ctx {
  struct mtl_main_impl impl;
  struct st_tx_video_sessions_mgr mgr;
  struct st_tx_video_session_impl session;
  int queue_tag[MTL_SESSION_PORT_MAX]; /* s->queue[] identity for the burst mock */
  struct rte_mempool* pkt_pool;
  uint64_t now;
  bool refuse[UT_SR_WIRE_PKT + 1];
  bool hold;
  struct rte_mbuf* held[UT_SR_HELD_MAX];
  int held_num;
  struct ut_sr_wire wire[UT_SR_WIRE_MAX];
  int wire_num;
  uint8_t last_sr[UT_SR_PKT_MAX];
  uint16_t last_sr_len;
  char pool_name[RTE_MEMPOOL_NAMESIZE];
};

/* mt_txq_burst() and mt_get_tsc() have no private context argument */
static struct ut_sr_ctx* ut_sr_active_ctx;

static uint16_t ut_sr_txq_burst(struct mt_txq_entry* entry, struct rte_mbuf** tx_pkts,
                                uint16_t nb_pkts) {
  struct ut_sr_ctx* ctx = ut_sr_active_ctx;
  struct st_tx_video_session_impl* s = &ctx->session;
  struct mt_interface* inf = mt_if(&ctx->impl, MTL_PORT_P);
  int port = entry == (struct mt_txq_entry*)&ctx->queue_tag[MTL_SESSION_PORT_R]
                 ? MTL_SESSION_PORT_R
                 : MTL_SESSION_PORT_P;

  for (uint16_t i = 0; i < nb_pkts; i++) {
    struct rte_mbuf* m = tx_pkts[i];
    struct ut_sr_wire* w = &ctx->wire[ctx->wire_num % UT_SR_WIRE_MAX];
    bool sr = s->ipmx_sr && m->pool == s->ipmx_sr->pool;
    enum ut_sr_wire_kind kind =
        sr ? UT_SR_WIRE_SR : (m->pool == ctx->pkt_pool ? UT_SR_WIRE_PKT : UT_SR_WIRE_PAD);

    if (ctx->refuse[kind]) return i;

    ctx->wire_num++;
    memset(w, 0, sizeof(*w));
    w->port = port;
    w->len = m->pkt_len;
    w->kind = kind;
    if (w->kind == UT_SR_WIRE_PKT) w->pkt_idx = st_tx_mbuf_get_idx(m);
    w->launch = inf->tx_launch_time_flag && (m->ol_flags & inf->tx_launch_time_flag);
    if (w->launch)
      w->launch_time = *RTE_MBUF_DYNFIELD(m, inf->tx_dynfield_offset, uint64_t*);
    if (sr) {
      ctx->last_sr_len = RTE_MIN(m->pkt_len, UT_SR_PKT_MAX);
      memcpy(ctx->last_sr, rte_pktmbuf_mtod(m, uint8_t*), ctx->last_sr_len);
      if (ctx->hold && ctx->held_num < UT_SR_HELD_MAX) {
        ctx->held[ctx->held_num++] = m;
        continue;
      }
    }
    rte_pktmbuf_free(m);
  }
  return nb_pkts;
}

static uint64_t ut_sr_tsc(struct mtl_main_impl* impl) {
  return ((struct ut_sr_ctx*)impl)->now; /* impl is ctx's first member */
}

static uint64_t ut_sr_ptp(struct mtl_main_impl* impl, enum mtl_port port) {
  MTL_MAY_UNUSED(port);
  return ((struct ut_sr_ctx*)impl)->now;
}

/* the transmitter externals too */
#define st_video_resolve_pacing_tasklet ut_sr_resolve_pacing_tasklet
#define st_video_transmitter_init ut_sr_transmitter_init
#define st_video_transmitter_uinit ut_sr_transmitter_uinit
#define mt_txq_burst ut_sr_txq_burst
#define mt_get_tsc ut_sr_tsc
#define mt_get_ptp_time ut_sr_ptp
#include "ipmx/ipmx_sr.c"
#include "st2110/st_video_transmitter.c"
#undef mt_get_ptp_time
#undef mt_get_tsc
#undef mt_txq_burst

int ut_sr_init(void) {
  return ut_eal_init();
}

static struct rte_mbuf* ut_sr_pad(uint16_t len) {
  struct rte_mbuf* pad = rte_pktmbuf_alloc(ut_pool());
  if (pad && !rte_pktmbuf_append(pad, len)) {
    rte_pktmbuf_free(pad);
    return NULL;
  }
  return pad;
}

static void ut_sr_free_pads(struct st_tx_video_session_impl* s) {
  rte_pktmbuf_free(s->pad[MTL_SESSION_PORT_P][ST20_PKT_TYPE_NORMAL]);
  s->pad[MTL_SESSION_PORT_P][ST20_PKT_TYPE_NORMAL] = NULL;
  for (int j = 0; j < ST_TX_VIDEO_RL_TRIM_PAD_MAX; j++) {
    rte_pktmbuf_free(s->trim_pad[MTL_SESSION_PORT_P][j]);
    s->trim_pad[MTL_SESSION_PORT_P][j] = NULL;
  }
}

ut_sr_ctx* ut_sr_create(const uint8_t sip[4], uint32_t ssrc, uint16_t media_dst_port) {
  static const uint8_t dip[MTL_IP_ADDR_LEN] = {239, 0, 0, 1};
  static const uint8_t dmac[RTE_ETHER_ADDR_LEN] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0x01};
  static const uint8_t smac[RTE_ETHER_ADDR_LEN] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
  static int create_idx;
  char name[32];
  ut_sr_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  struct st_tx_video_session_impl* s = &ctx->session;
  snprintf(name, sizeof(name), "ut_sr_pkt_%d", create_idx);
  ctx->pkt_pool = rte_pktmbuf_pool_create(name, 64, 0, sizeof(struct mt_muf_priv_data),
                                          RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
  snprintf(name, sizeof(name), "ut_sr_ring_%d", create_idx++);
  s->ring[MTL_SESSION_PORT_P] = ut_ring_create(name, 8);
  s->pad[MTL_SESSION_PORT_P][ST20_PKT_TYPE_NORMAL] = ut_sr_pad(UT_SR_PAD_SIZE);
  if (!ctx->pkt_pool || !s->ring[MTL_SESSION_PORT_P] ||
      !s->pad[MTL_SESSION_PORT_P][ST20_PKT_TYPE_NORMAL]) {
    ut_sr_destroy(ctx);
    return NULL;
  }

  ctx->impl.type = MT_HANDLE_MAIN;
  ctx->impl.inf[MTL_PORT_P].socket_id = rte_socket_id();
  ctx->impl.inf[MTL_PORT_P].nb_tx_desc = 64;
  /* mt_macaddr_get() reads k_mac_addr, no ethdev */
  ctx->impl.inf[MTL_PORT_P].drv_info.flags = MT_DRV_F_NOT_DPDK_PMD;
  memcpy(ctx->impl.inf[MTL_PORT_P].k_mac_addr.addr_bytes, smac, sizeof(smac));
  ctx->mgr.parent = &ctx->impl;

  s->impl = &ctx->impl;
  s->mgr = &ctx->mgr;
  s->socket_id = rte_socket_id();
  s->ops.num_port = 1;
  s->ops.flags = ST20_TX_FLAG_IPMX;
  s->port_maps[MTL_SESSION_PORT_P] = MTL_PORT_P;
  s->eth_ipv4_cksum_offload[MTL_SESSION_PORT_P] = false;
  st_get_fps_timing(ST_FPS_P59_94, &s->fps_tm);
  s->bulk = 1;
  s->ops.width = 1920;
  s->ops.height = 1080;
  s->ops.fmt = ST20_FMT_YUV_422_10BIT;
  s->ops.packing = ST20_PACKING_BPM;
  s->ops.fps = ST_FPS_P59_94;
  s->tx_hang_detect_time_thresh = NS_PER_S;
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++)
    s->queue[i] = (struct mt_txq_entry*)&ctx->queue_tag[i];

  struct st_rfc4175_video_hdr* hdr = &s->s_hdr[MTL_SESSION_PORT_P];
  memcpy(mt_eth_d_addr(&hdr->eth)->addr_bytes, dmac, sizeof(dmac));
  memcpy(mt_eth_s_addr(&hdr->eth)->addr_bytes, smac, sizeof(smac));
  hdr->eth.ether_type = htons(RTE_ETHER_TYPE_IPV4);
  hdr->ipv4.version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
  hdr->ipv4.time_to_live = 64;
  hdr->ipv4.type_of_service = IPMX_VIDEO_IPV4_TOS;
  hdr->ipv4.fragment_offset = MT_IP_DONT_FRAGMENT_FLAG;
  hdr->ipv4.next_proto_id = IPPROTO_UDP;
  memcpy(&hdr->ipv4.src_addr, sip, MTL_IP_ADDR_LEN);
  memcpy(&hdr->ipv4.dst_addr, dip, MTL_IP_ADDR_LEN);
  hdr->udp.src_port = htons(media_dst_port);
  hdr->udp.dst_port = htons(media_dst_port);
  hdr->rtp.base.version = ST_RVRTP_VERSION_2;
  hdr->rtp.base.ssrc = htonl(ssrc);

  s->st20_total_pkts = 10;
  s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].size =
      sizeof(struct st_rfc4175_video_hdr) + 1200;
  s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].number = 10;
  return ctx;
}

void ut_sr_destroy(ut_sr_ctx* ctx) {
  struct st_tx_video_session_impl* s;

  if (!ctx) return;
  s = &ctx->session;
  st_tx_video_transmitter_state_cleanup(s);
  if (s->ring[MTL_SESSION_PORT_P]) {
    ut_ring_drain(s->ring[MTL_SESSION_PORT_P]);
    rte_ring_free(s->ring[MTL_SESSION_PORT_P]);
  }
  ut_sr_free_pads(s);
  ut_sr_flush(ctx);
  ut_sr_detach(ctx);
  /* a pool ipmx_sr_tx_uinit() had to leave behind */
  if (ctx->pool_name[0]) {
    struct rte_mempool* pool = rte_mempool_lookup(ctx->pool_name);
    if (pool) rte_mempool_free(pool);
  }
  if (ctx->pkt_pool) rte_mempool_free(ctx->pkt_pool);
  free(ctx);
}

void ut_sr_set_frame_pkts(ut_sr_ctx* ctx, uint32_t total_pkts) {
  ctx->session.st20_total_pkts = total_pkts;
}

void ut_sr_set_pkt_info(ut_sr_ctx* ctx, int type, uint32_t size, uint32_t number) {
  ctx->session.st20_pkt_info[type].size = size;
  ctx->session.st20_pkt_info[type].number = number;
}

int ut_sr_set_video(ut_sr_ctx* ctx, enum st20_fmt fmt, uint32_t width, uint32_t height,
                    enum st20_packing packing, enum st_fps fps) {
  struct st_tx_video_session_impl* s = &ctx->session;

  s->ops.fmt = fmt;
  s->ops.width = width;
  s->ops.height = height;
  s->ops.packing = packing;
  s->ops.fps = fps;
  return st_get_fps_timing(fps, &s->fps_tm);
}

void ut_sr_set_mac(ut_sr_ctx* ctx, const uint8_t mac[6]) {
  memcpy(ctx->impl.inf[MTL_PORT_P].k_mac_addr.addr_bytes, mac, RTE_ETHER_ADDR_LEN);
}

int ut_sr_video_info(ut_sr_ctx* ctx, struct ipmx_video_info* info) {
  return ipmx_video_info_from_session(&ctx->impl, &ctx->session, info);
}

int ut_sr_set_block_version(ut_sr_ctx* ctx, uint8_t version) {
  return ut_sr_tx_set_block_version(&ctx->session, version);
}

int ut_sr_get_info(ut_sr_ctx* ctx, struct ipmx_video_info* info) {
  return ut_sr_tx_get_info(&ctx->session, info);
}

int ut_sr_attach(ut_sr_ctx* ctx) {
  int ret = ut_sr_tx_init(&ctx->impl, &ctx->session);
  if (!ret)
    snprintf(ctx->pool_name, sizeof(ctx->pool_name), "%s",
             ctx->session.ipmx_sr->pool->name);
  return ret;
}

void ut_sr_detach(ut_sr_ctx* ctx) {
  ut_sr_tx_uinit(&ctx->session);
}

bool ut_sr_attached(const ut_sr_ctx* ctx) {
  return ctx->session.ipmx_sr != NULL;
}

uint16_t ut_sr_build(ut_sr_ctx* ctx, uint8_t* rtcp, uint64_t tai_ns, uint32_t rtp_ts) {
  return ipmx_sr_tx_build(ctx->session.ipmx_sr, rtcp, tai_ns, rtp_ts);
}

static struct rte_mbuf* ut_sr_pkt(ut_sr_ctx* ctx, uint32_t pkt_idx, uint32_t rtp_ts,
                                  uint64_t target) {
  struct rte_mbuf* m = rte_pktmbuf_alloc(ctx->pkt_pool);
  struct st_rfc4175_video_hdr* hdr;

  if (!m) return NULL;
  hdr = (struct st_rfc4175_video_hdr*)rte_pktmbuf_append(m, sizeof(*hdr));
  memcpy(hdr, &ctx->session.s_hdr[MTL_SESSION_PORT_P], sizeof(*hdr));
  hdr->rtp.base.tmstamp = htonl(rtp_ts);
  st_tx_mbuf_set_idx(m, pkt_idx);
  st_tx_mbuf_set_tsc(m, target);
  st_tx_mbuf_set_ptp(m, target);
  st_tx_mbuf_set_priv(m, NULL);
  return m;
}

void ut_sr_dequeue(ut_sr_ctx* ctx, int s_port, uint64_t ptp_ns, uint32_t rtp_ts,
                   bool late) {
  struct rte_mbuf* pkt0 = ut_sr_pkt(ctx, 0, rtp_ts, ptp_ns);
  if (!pkt0) return;
  ut_sr_tx_frame_start(ctx->session.ipmx_sr, s_port, pkt0, late);
  rte_pktmbuf_free(pkt0);
}

int ut_sr_burst_pkt0(ut_sr_ctx* ctx, int s_port, uint64_t ptp_ns, uint32_t rtp_ts) {
  struct rte_mbuf* pkt0 = ut_sr_pkt(ctx, 0, rtp_ts, ptp_ns);
  uint16_t tx;
  if (!pkt0) return -ENOMEM;

  ut_sr_active_ctx = ctx;
  tx = video_trs_burst(&ctx->impl, &ctx->session, s_port, &pkt0, 1);
  ut_sr_active_ctx = NULL;
  if (!tx) rte_pktmbuf_free(pkt0);
  return tx;
}

int ut_sr_burst_empty(ut_sr_ctx* ctx, uint64_t ptp_ns, uint32_t rtp_ts) {
  struct rte_mbuf* pkt0 = ut_sr_pkt(ctx, 0, rtp_ts, ptp_ns);
  uint16_t tx;
  if (!pkt0) return -ENOMEM;

  ut_sr_active_ctx = ctx;
  tx = video_trs_burst(&ctx->impl, &ctx->session, MTL_SESSION_PORT_P, &pkt0, 0);
  ut_sr_active_ctx = NULL;
  rte_pktmbuf_free(pkt0);
  return tx;
}

void ut_sr_frame(ut_sr_ctx* ctx, int s_port, uint64_t ptp_ns, uint32_t rtp_ts,
                 bool late) {
  ut_sr_dequeue(ctx, s_port, ptp_ns, rtp_ts, late);
  ut_sr_burst_pkt0(ctx, s_port, ptp_ns, rtp_ts);
}

void ut_sr_set_refuse(ut_sr_ctx* ctx, enum ut_sr_wire_kind kind, bool refuse) {
  ctx->refuse[kind] = refuse;
}

void ut_sr_set_hold(ut_sr_ctx* ctx, bool hold) {
  ctx->hold = hold;
}

void ut_sr_flush(ut_sr_ctx* ctx) {
  for (int i = 0; i < ctx->held_num; i++) rte_pktmbuf_free(ctx->held[i]);
  ctx->held_num = 0;
}

int ut_sr_wire_count(const ut_sr_ctx* ctx) {
  return RTE_MIN(ctx->wire_num, UT_SR_WIRE_MAX);
}

struct ut_sr_wire ut_sr_wire_at(const ut_sr_ctx* ctx, int i) {
  return ctx->wire[i];
}

uint16_t ut_sr_last_sr(const ut_sr_ctx* ctx, uint8_t pkt[UT_SR_PKT_MAX]) {
  memcpy(pkt, ctx->last_sr, ctx->last_sr_len);
  return ctx->last_sr_len;
}

void ut_sr_counts(const ut_sr_ctx* ctx, struct ut_sr_counts* counts) {
  const struct ipmx_sr_stat* stat = &ctx->session.ipmx_sr->stat;

  counts->sent = stat->sent;
  counts->late = stat->late;
  counts->alloc_fail = stat->alloc_fail;
  counts->burst_fail = stat->burst_fail;
  counts->dropped = stat->dropped;
}

unsigned int ut_sr_pool_n(ut_sr_ctx* ctx) {
  return ipmx_sr_pool_n(&ctx->impl, &ctx->session);
}

unsigned int ut_sr_pool_in_use(const ut_sr_ctx* ctx) {
  return rte_mempool_in_use_count(ctx->session.ipmx_sr->pool);
}

const char* ut_sr_pool_name(const ut_sr_ctx* ctx) {
  return ctx->pool_name;
}

void ut_sr_trs_setup(ut_sr_ctx* ctx, unsigned int bulk, long double trs_ns,
                     uint32_t warm_pkts) {
  ctx->session.bulk = bulk;
  ctx->session.pacing.trs = trs_ns;
  ctx->session.pacing.warm_pkts = warm_pkts;
  ctx->session.st20_pkt_info[ST20_PKT_TYPE_NORMAL].size = UT_SR_PAD_SIZE;
}

void ut_sr_set_now(ut_sr_ctx* ctx, uint64_t now_ns) {
  ctx->now = now_ns;
}

int ut_sr_trs_arm(ut_sr_ctx* ctx, long double pad_ns, long double credit_ns) {
  struct st_tx_video_session_impl* s = &ctx->session;
  struct st_tx_video_rl_plan* plan = &s->rl_plan[MTL_SESSION_PORT_P];
  struct rte_mbuf** trim_pad = s->trim_pad[MTL_SESSION_PORT_P];

  trim_pad[ST_TX_VIDEO_RL_TRIM_PAD_COARSE] =
      ut_sr_pad(UT_SR_PAD_SIZE - ST_TX_VIDEO_RL_TRIM_COARSE);
  trim_pad[ST_TX_VIDEO_RL_TRIM_PAD_FINE] =
      ut_sr_pad(UT_SR_PAD_SIZE - ST_TX_VIDEO_RL_TRIM_FINE);
  if (!trim_pad[ST_TX_VIDEO_RL_TRIM_PAD_COARSE] ||
      !trim_pad[ST_TX_VIDEO_RL_TRIM_PAD_FINE])
    return -ENOMEM;
  plan->pad_ns = pad_ns;
  plan->bucket_ns = credit_ns;
  plan->pad_size = UT_SR_PAD_SIZE;
  plan->trim_pads = st_tx_video_rl_trim_pads(UT_SR_PAD_SIZE);
  plan->armed = true;
  return 0;
}

int ut_sr_trs_enqueue(ut_sr_ctx* ctx, uint32_t pkt_idx, uint32_t rtp_ts,
                      uint64_t target) {
  void* m = ut_sr_pkt(ctx, pkt_idx, rtp_ts, target);
  if (!m) return -ENOMEM;
  if (rte_ring_sp_enqueue(ctx->session.ring[MTL_SESSION_PORT_P], m) < 0) {
    rte_pktmbuf_free(m);
    return -ENOSPC;
  }
  return 0;
}

static int ut_sr_call(ut_sr_ctx* ctx, int (*tasklet)(struct mtl_main_impl*,
                                                     struct st_tx_video_session_impl*,
                                                     enum mtl_session_port)) {
  ut_sr_active_ctx = ctx;
  int ret = tasklet(&ctx->impl, &ctx->session, MTL_SESSION_PORT_P);
  ut_sr_active_ctx = NULL;
  return ret;
}

int ut_sr_call_rl(ut_sr_ctx* ctx) {
  return ut_sr_call(ctx, video_trs_rl_tasklet);
}

int ut_sr_call_tsc(ut_sr_ctx* ctx) {
  return ut_sr_call(ctx, video_trs_tsc_tasklet);
}

int ut_sr_call_ptp(ut_sr_ctx* ctx) {
  return ut_sr_call(ctx, video_trs_ptp_tasklet);
}

int ut_sr_call_launch_time(ut_sr_ctx* ctx) {
  static const struct rte_mbuf_dynfield desc = {
      .name = "ut_sr_launch_time",
      .size = sizeof(uint64_t),
      .align = __alignof__(uint64_t),
  };
  static const struct rte_mbuf_dynflag flag = {.name = "ut_sr_launch_time_flag"};
  struct mt_interface* inf = mt_if(&ctx->impl, MTL_PORT_P);
  int offset = rte_mbuf_dynfield_register(&desc);
  int bit = rte_mbuf_dynflag_register(&flag);

  if (offset < 0 || bit < 0) return -EIO;
  inf->tx_dynfield_offset = offset;
  inf->tx_launch_time_flag = RTE_BIT64(bit);
  return ut_sr_call(ctx, video_trs_launch_time_tasklet);
}
