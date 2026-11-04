/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/*
 * Compiles lib/src/ipmx/ipmx_sr_rx.c into the test binary with fakes for the rxq
 * (mt_rxq_get/put/burst, the burst floored to 8 like the iavf AVX2 vector RX), the PTP
 * time and the NIC RX timestamp, so the SR queue life cycle and poll run without a NIC.
 * libmtl exports the same ipmx_sr_rx_*() symbols, so the copies here are renamed.
 */

#include <stdlib.h>
#include <string.h>

/* libmtl exports the real ipmx_sr_rx_*(), give the copies compiled here private names */
#define ipmx_sr_rx_parse ut_srrx_lib_parse
#define ipmx_sr_rx_init ut_srrx_lib_init
#define ipmx_sr_rx_uinit ut_srrx_lib_uinit
#define ipmx_sr_rx_queue_get ut_srrx_lib_queue_get
#define ipmx_sr_rx_queue_put ut_srrx_lib_queue_put
#define ipmx_sr_rx_poll ut_srrx_lib_poll
#define ipmx_sr_rx_get_info ut_srrx_lib_get_info
#define ipmx_sr_rx_stat_log ut_srrx_lib_stat_log

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "datapath/mt_queue.h"
#include "ipmx/ipmx_sr_rx.h"
#include "ipmx/ipmx_sr_rx_harness.h"
#include "mt_main.h"
#include "st2110/pipeline/st20_pipeline_rx.h"
#include "st2110/pipeline/st22_pipeline_rx.h"
#include "st2110/st_rx_video_session.h"

#define UT_SRRX_QUEUED_MAX (64)
#define UT_SRRX_MEDIA_PORT (20000)
/* iavf / ice AVX2 vector RX floors nb_pkts to 8 descriptors: fewer return nothing */
#define UT_SRRX_VEC_RX_DESCS (8)

struct ut_srrx_port {
  int tag; /* the address is the fake mt_rxq_entry */
  bool has_rxq;
  struct ut_srrx_flow flow;
  struct rte_mbuf* queued[UT_SRRX_QUEUED_MAX];
  int queued_num;
};

struct ut_srrx_ctx {
  struct mtl_main_impl impl;
  struct st_rx_video_session_impl session;
  struct st_rx_video_session_handle_impl handle;
  struct st20p_rx_ctx pipeline;
  struct st22_rx_video_session_handle_impl st22_handle;
  struct st22p_rx_ctx st22_pipeline;
  struct rte_mempool* pool;
  struct ut_srrx_port port[MTL_SESSION_PORT_MAX];
  bool fail_next_get;
  int gets;
  int puts;
  uint64_t ptp_ns;
  bool hw_ts;
  uint64_t hw_ts_ns;
  int cni_tag; /* the address is the fake CNI rxq */
  bool media_force_cni;
};

/* mt_rxq_get() and mt_get_ptp_time() reach the ctx through impl, its first member */
static struct ut_srrx_ctx* ut_srrx_of(struct mtl_main_impl* impl) {
  return (struct ut_srrx_ctx*)impl;
}

static struct ut_srrx_port* ut_srrx_port_of(struct mt_rxq_entry* entry) {
  return (struct ut_srrx_port*)entry; /* tag is the first member */
}

static struct mt_rxq_entry* ut_srrx_rxq_get(struct mtl_main_impl* impl,
                                            enum mtl_port port,
                                            struct mt_rxq_flow* flow) {
  struct ut_srrx_ctx* ctx = ut_srrx_of(impl);
  struct ut_srrx_port* p = &ctx->port[port]; /* the harness maps s_port i to port i */

  ctx->gets++;
  if (ctx->fail_next_get) {
    ctx->fail_next_get = false;
    return NULL;
  }
  memcpy(p->flow.dip, flow->dip_addr, sizeof(p->flow.dip));
  memcpy(p->flow.sip, flow->sip_addr, sizeof(p->flow.sip));
  p->flow.dst_port = flow->dst_port;
  p->flow.hdr_split = flow->flags & MT_RXQ_FLOW_F_HDR_SPLIT;
  p->flow.hdr_split_cb = flow->hdr_split_mbuf_cb_priv != NULL;
  p->flow.force_cni = flow->flags & MT_RXQ_FLOW_F_FORCE_CNI;
  p->has_rxq = true;
  return (struct mt_rxq_entry*)p;
}

static int ut_srrx_rxq_put(struct mt_rxq_entry* entry) {
  struct ut_srrx_port* p = ut_srrx_port_of(entry);

  p->has_rxq = false;
  for (int i = 0; i < p->queued_num; i++) rte_pktmbuf_free(p->queued[i]);
  p->queued_num = 0;
  return 0;
}

static uint16_t ut_srrx_rxq_burst(struct mt_rxq_entry* entry, struct rte_mbuf** rx_pkts,
                                  const uint16_t nb_pkts) {
  struct ut_srrx_port* p = ut_srrx_port_of(entry);
  uint16_t n =
      RTE_MIN(RTE_ALIGN_FLOOR(nb_pkts, UT_SRRX_VEC_RX_DESCS), (uint16_t)p->queued_num);

  memcpy(rx_pkts, p->queued, n * sizeof(*rx_pkts));
  memmove(p->queued, &p->queued[n], (p->queued_num - n) * sizeof(*p->queued));
  p->queued_num -= n;
  return n;
}

static uint64_t ut_srrx_ptp(struct mtl_main_impl* impl, enum mtl_port port) {
  MTL_MAY_UNUSED(port);
  return ut_srrx_of(impl)->ptp_ns;
}

static bool ut_srrx_has_hw_ts(struct mtl_main_impl* impl, struct rte_mbuf* mbuf,
                              enum mtl_port port) {
  MTL_MAY_UNUSED(mbuf);
  MTL_MAY_UNUSED(port);
  return ut_srrx_of(impl)->hw_ts;
}

static uint64_t ut_srrx_hw_ts(struct mtl_main_impl* impl, struct rte_mbuf* mbuf,
                              enum mtl_port port) {
  MTL_MAY_UNUSED(mbuf);
  MTL_MAY_UNUSED(port);
  return ut_srrx_of(impl)->hw_ts_ns;
}

/* the puts are counted here, the ctx is not reachable from the entry alone */
static struct ut_srrx_ctx* ut_srrx_active;

static int ut_srrx_rxq_put_counted(struct mt_rxq_entry* entry) {
  if (ut_srrx_active) ut_srrx_active->puts++;
  return ut_srrx_rxq_put(entry);
}

#define mt_rxq_get ut_srrx_rxq_get
#define mt_rxq_put ut_srrx_rxq_put_counted
#define mt_rxq_burst ut_srrx_rxq_burst
#define mt_get_ptp_time ut_srrx_ptp
#define mt_mbuf_has_hw_time_stamp ut_srrx_has_hw_ts
#define mt_mbuf_time_stamp ut_srrx_hw_ts
#include "ipmx/ipmx_sr_rx.c"
#undef mt_mbuf_time_stamp
#undef mt_mbuf_has_hw_time_stamp
#undef mt_get_ptp_time
#undef mt_rxq_burst
#undef mt_rxq_put
#undef mt_rxq_get

int ut_srrx_init(void) {
  return ut_eal_init();
}

int ut_srrx_parse(const uint8_t* rtcp, size_t len, uint16_t video_mib,
                  struct ipmx_rx_sr_info* sr) {
  return ut_srrx_lib_parse(rtcp, len, video_mib, sr);
}

int ut_srrx_burst_size(void) {
  return IPMX_SR_RX_BURST;
}

ut_srrx_ctx* ut_srrx_create(int num_port, uint32_t flags) {
  static int create_idx;
  char name[32];
  ut_srrx_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  snprintf(name, sizeof(name), "ut_srrx_%d", create_idx++);
  ctx->pool = rte_pktmbuf_pool_create(name, 256, 0, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
                                      rte_socket_id());
  if (!ctx->pool) {
    free(ctx);
    return NULL;
  }
  ctx->impl.type = MT_HANDLE_MAIN;
  for (int i = 0; i < MTL_PORT_MAX; i++) ctx->impl.inf[i].socket_id = rte_socket_id();

  struct st_rx_video_session_impl* s = &ctx->session;
  s->impl = &ctx->impl;
  s->socket_id = rte_socket_id();
  s->ops.num_port = num_port;
  s->ops.flags = flags;
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) s->port_maps[i] = (enum mtl_port)i;

  ctx->handle.type = MT_HANDLE_RX_VIDEO;
  ctx->handle.impl = s;
  ctx->pipeline.type = MT_ST20_HANDLE_PIPELINE_RX;
  ctx->pipeline.transport = &ctx->handle;
  ctx->st22_handle.type = MT_ST22_HANDLE_RX_VIDEO;
  ctx->st22_handle.impl = s;
  ctx->st22_pipeline.type = MT_ST22_HANDLE_PIPELINE_RX;
  ctx->st22_pipeline.transport = &ctx->st22_handle;
  ut_srrx_active = ctx;
  return ctx;
}

void ut_srrx_destroy(ut_srrx_ctx* ctx) {
  if (!ctx) return;
  ut_srrx_detach(ctx);
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    struct ut_srrx_port* p = &ctx->port[i];
    for (int j = 0; j < p->queued_num; j++) rte_pktmbuf_free(p->queued[j]);
  }
  rte_mempool_free(ctx->pool);
  if (ut_srrx_active == ctx) ut_srrx_active = NULL;
  free(ctx);
}

int ut_srrx_attach(ut_srrx_ctx* ctx) {
  int ret = ut_srrx_lib_init(&ctx->impl, &ctx->session);
  if (ret < 0) return ret;
  return ut_srrx_queue_get(ctx);
}

void ut_srrx_set_st22_flags(ut_srrx_ctx* ctx, uint32_t st22_flags) {
  ctx->session.st22_ops_flags = st22_flags;
}

bool ut_srrx_has_state(const ut_srrx_ctx* ctx) {
  return ctx->session.ipmx_sr != NULL;
}

void ut_srrx_detach(ut_srrx_ctx* ctx) {
  ut_srrx_queue_put(ctx);
  ut_srrx_lib_uinit(&ctx->session);
}

void ut_srrx_queue_put(ut_srrx_ctx* ctx) {
  if (ctx->session.ipmx_sr) ut_srrx_lib_queue_put(&ctx->session);
}

int ut_srrx_queue_get(ut_srrx_ctx* ctx) {
  struct st_rx_video_session_impl* s = &ctx->session;

  if (!s->ipmx_sr) return 0;
  for (int i = 0; i < s->ops.num_port; i++) {
    struct mt_rxq_flow flow;
    static int hdr_split_priv;
    uint8_t dip[MTL_IP_ADDR_LEN] = {239, 0, 0, (uint8_t)(1 + i)};
    uint8_t sip[MTL_IP_ADDR_LEN] = {192, 168, 1, (uint8_t)(10 + i)};

    memset(&flow, 0, sizeof(flow));
    memcpy(flow.dip_addr, dip, sizeof(dip));
    memcpy(flow.sip_addr, sip, sizeof(sip));
    flow.dst_port = UT_SRRX_MEDIA_PORT;
    flow.flags = MT_RXQ_FLOW_F_HDR_SPLIT;
    if (ctx->media_force_cni) flow.flags |= MT_RXQ_FLOW_F_FORCE_CNI;
    flow.bytes_per_sec = 1000000000;
    flow.hdr_split_mbuf_cb_priv = &hdr_split_priv;
    int ret = ut_srrx_lib_queue_get(s, i, s->port_maps[i], &flow);
    if (ret < 0) return ret;
  }
  return 0;
}

void ut_srrx_fail_next_get(ut_srrx_ctx* ctx) {
  ctx->fail_next_get = true;
}

int ut_srrx_rxq_gets(const ut_srrx_ctx* ctx) {
  return ctx->gets;
}

int ut_srrx_rxq_puts(const ut_srrx_ctx* ctx) {
  return ctx->puts;
}

bool ut_srrx_has_rxq(const ut_srrx_ctx* ctx, int s_port) {
  return ctx->port[s_port].has_rxq;
}

void ut_srrx_flow(const ut_srrx_ctx* ctx, int s_port, struct ut_srrx_flow* flow) {
  *flow = ctx->port[s_port].flow;
}

int ut_srrx_push_raw(ut_srrx_ctx* ctx, int s_port, const uint8_t* frame, size_t len) {
  struct ut_srrx_port* p = &ctx->port[s_port];
  struct rte_mbuf* m;
  char* data;

  if (p->queued_num >= UT_SRRX_QUEUED_MAX) return -ENOSPC;
  m = rte_pktmbuf_alloc(ctx->pool);
  if (!m) return -ENOMEM;
  data = rte_pktmbuf_append(m, len);
  if (!data) {
    rte_pktmbuf_free(m);
    return -ENOSPC;
  }
  memcpy(data, frame, len);
  p->queued[p->queued_num++] = m;
  return 0;
}

int ut_srrx_push(ut_srrx_ctx* ctx, int s_port, const uint8_t* rtcp, size_t len) {
  uint8_t frame[RTE_MBUF_DEFAULT_DATAROOM];
  struct mt_udp_hdr* hdr = (struct mt_udp_hdr*)frame;

  if (sizeof(*hdr) + len > sizeof(frame)) return -ENOSPC;
  memset(hdr, 0, sizeof(*hdr));
  hdr->eth.ether_type = htons(RTE_ETHER_TYPE_IPV4);
  hdr->ipv4.version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
  hdr->ipv4.next_proto_id = IPPROTO_UDP;
  hdr->ipv4.total_length = htons(sizeof(hdr->ipv4) + sizeof(hdr->udp) + len);
  hdr->udp.src_port = htons(UT_SRRX_MEDIA_PORT + 1);
  hdr->udp.dst_port = htons(UT_SRRX_MEDIA_PORT + 1);
  hdr->udp.dgram_len = htons(sizeof(hdr->udp) + len);
  memcpy(&hdr[1], rtcp, len);
  return ut_srrx_push_raw(ctx, s_port, frame, sizeof(*hdr) + len);
}

int ut_srrx_poll(ut_srrx_ctx* ctx, int s_port) {
  return ut_srrx_lib_poll(&ctx->session, s_port);
}

void ut_srrx_media(ut_srrx_ctx* ctx, int s_port, uint32_t ssrc) {
  struct rte_mbuf* m = rte_pktmbuf_alloc(ctx->pool);
  struct st_rfc4175_video_hdr* hdr;

  if (!m) return;
  hdr = (struct st_rfc4175_video_hdr*)rte_pktmbuf_append(m, sizeof(*hdr));
  if (hdr) {
    memset(hdr, 0, sizeof(*hdr));
    hdr->rtp.base.version = ST_RVRTP_VERSION_2;
    hdr->rtp.base.ssrc = htonl(ssrc);
    ipmx_sr_rx_media(ctx->session.ipmx_sr, s_port, m);
  }
  rte_pktmbuf_free(m);
}

void ut_srrx_set_ptp(ut_srrx_ctx* ctx, uint64_t ns) {
  ctx->ptp_ns = ns;
}

unsigned int ut_srrx_mbufs_in_use(const ut_srrx_ctx* ctx) {
  return rte_mempool_in_use_count(ctx->pool);
}

int ut_srrx_get(ut_srrx_ctx* ctx, int s_port, struct ipmx_rx_sr_info* info) {
  return ut_srrx_lib_get_info(&ctx->session, s_port, info);
}

int ut_srrx_api_get(ut_srrx_ctx* ctx, int s_port, struct ipmx_rx_sr_info* info,
                    bool valid_handle) {
  ctx->handle.type = valid_handle ? MT_HANDLE_RX_VIDEO : MT_HANDLE_TX_VIDEO;
  int ret = ipmx_st20_rx_get_sr_info(&ctx->handle, s_port, info);
  ctx->handle.type = MT_HANDLE_RX_VIDEO;
  return ret;
}

int ut_srrx_api_get_pipeline(ut_srrx_ctx* ctx, int s_port, struct ipmx_rx_sr_info* info,
                             bool valid_handle) {
  ctx->pipeline.type =
      valid_handle ? MT_ST20_HANDLE_PIPELINE_RX : MT_ST20_HANDLE_PIPELINE_TX;
  int ret = ipmx_st20p_rx_get_sr_info(&ctx->pipeline, s_port, info);
  ctx->pipeline.type = MT_ST20_HANDLE_PIPELINE_RX;
  return ret;
}

int ut_srrx_api_get_st22(ut_srrx_ctx* ctx, int s_port, struct ipmx_rx_sr_info* info,
                         bool valid_handle) {
  ctx->st22_handle.type = valid_handle ? MT_ST22_HANDLE_RX_VIDEO : MT_HANDLE_RX_VIDEO;
  int ret = ipmx_st22_rx_get_sr_info(&ctx->st22_handle, s_port, info);
  ctx->st22_handle.type = MT_ST22_HANDLE_RX_VIDEO;
  return ret;
}

int ut_srrx_api_get_st22p(ut_srrx_ctx* ctx, int s_port, struct ipmx_rx_sr_info* info,
                          bool valid_handle) {
  ctx->st22_pipeline.type =
      valid_handle ? MT_ST22_HANDLE_PIPELINE_RX : MT_ST20_HANDLE_PIPELINE_RX;
  int ret = ipmx_st22p_rx_get_sr_info(&ctx->st22_pipeline, s_port, info);
  ctx->st22_pipeline.type = MT_ST22_HANDLE_PIPELINE_RX;
  return ret;
}

void ut_srrx_set_ops_ssrc(ut_srrx_ctx* ctx, uint32_t ssrc) {
  ctx->session.ops.ssrc = ssrc;
}

void ut_srrx_set_cni(ut_srrx_ctx* ctx, int s_port, bool on, bool tasklet) {
  ctx->impl.cni.entries[s_port].rxq = on ? (struct mt_rxq_entry*)&ctx->cni_tag : NULL;
  ctx->impl.cni.lcore_tasklet = tasklet;
}

void ut_srrx_set_hw_ts(ut_srrx_ctx* ctx, bool on, uint64_t ns) {
  ctx->hw_ts = on;
  ctx->hw_ts_ns = ns;
}

void ut_srrx_stat_log(ut_srrx_ctx* ctx) {
  ut_srrx_lib_stat_log(&ctx->session, 0);
}

void ut_srrx_set_media_force_cni(ut_srrx_ctx* ctx, bool on) {
  ctx->media_force_cni = on;
}
