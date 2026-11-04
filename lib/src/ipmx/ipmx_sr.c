/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

#include "ipmx_sr.h"

#include "../datapath/mt_queue.h"
#include "../mt_log.h"
#include "../mt_ptp.h"
#include "../mt_util.h"
#include "../st2110/st_tx_video_session.h"

#define IPMX_SR_POOL_DATA_SIZE (320)
#define IPMX_RTCP_PT_SR (200)
#define IPMX_RTCP_PT_SDES (202)
#define IPMX_RTCP_SDES_CNAME (1)
#define IPMX_RTCP_SR_LEN (28)
#define IPMX_RTCP_SR_IPMX_MAX_LEN (IPMX_RTCP_SR_LEN + IPMX_INFO_BLOCK_JXS_LEN)
#define IPMX_RTCP_SDES_MAX_LEN (8 + RTE_ALIGN_CEIL(2 + IPMX_SR_CNAME_MAX, 4))
#define IPMX_RTP_HDR_LEN (12)
#define IPMX_RFC9134_HDR_LEN (sizeof(struct st22_rfc9134_rtp_hdr) - IPMX_RTP_HDR_LEN)

static inline void ipmx_put_be16(uint8_t* p, uint16_t v) {
  v = htons(v);
  memcpy(p, &v, sizeof(v));
}

static inline void ipmx_put_be32(uint8_t* p, uint32_t v) {
  v = htonl(v);
  memcpy(p, &v, sizeof(v));
}

/* SR with the IPMX Info Block, then SDES with the CNAME item null terminated to the word
 * end (RFC 3550 6.5) */
static uint16_t ipmx_sr_tx_build(const struct ipmx_sr_tx* sr,
                                 const struct ipmx_sr_leg* leg, uint8_t* rtcp,
                                 uint64_t tai_ns, uint32_t rtp_ts) {
  uint16_t sr_len = IPMX_RTCP_SR_LEN + sr->info_block_len;
  uint16_t sdes_len = 8 + RTE_ALIGN_CEIL(2 + leg->cname_len + 1, 4);
  uint8_t* sdes = rtcp + sr_len;
  uint32_t ssrc = ntohl(leg->media_hdr->rtp.base.ssrc);

  rtcp[0] = 2 << 6; /* V=2, P=0, RC=0 */
  rtcp[1] = IPMX_RTCP_PT_SR;
  ipmx_put_be16(&rtcp[2], sr_len / 4 - 1);
  ipmx_put_be32(&rtcp[4], ssrc);
  /* TR-10-1 section 8.7: PTP truncated timestamp, not the RFC 3550 NTP format */
  ipmx_put_be32(&rtcp[8], (uint32_t)(tai_ns / NS_PER_S));
  ipmx_put_be32(&rtcp[12], (uint32_t)(tai_ns % NS_PER_S));
  ipmx_put_be32(&rtcp[16], rtp_ts);
  ipmx_put_be32(&rtcp[20], leg->pkt_count);
  ipmx_put_be32(&rtcp[24], leg->octet_count);
  memcpy(&rtcp[IPMX_RTCP_SR_LEN], sr->info_block, sr->info_block_len);

  memset(sdes, 0, sdes_len);
  sdes[0] = (2 << 6) | 1; /* V=2, P=0, SC=1 */
  sdes[1] = IPMX_RTCP_PT_SDES;
  ipmx_put_be16(&sdes[2], sdes_len / 4 - 1);
  ipmx_put_be32(&sdes[4], ssrc);
  sdes[8] = IPMX_RTCP_SDES_CNAME;
  sdes[9] = leg->cname_len;
  memcpy(&sdes[10], leg->cname, leg->cname_len);

  return sr_len + sdes_len;
}

static struct rte_mbuf* ipmx_sr_tx_pkt(const struct ipmx_sr_tx* sr,
                                       const struct ipmx_sr_leg* leg, uint64_t tai_ns,
                                       uint32_t rtp_ts) {
  struct rte_mbuf* pkt = rte_pktmbuf_alloc(leg->pool);
  struct mt_udp_hdr* hdr;
  uint16_t rtcp_len;

  if (!pkt) return NULL;

  hdr = rte_pktmbuf_mtod(pkt, struct mt_udp_hdr*);
  mt_memcpy(hdr, leg->media_hdr, sizeof(*hdr));
  hdr->udp.dst_port = htons(ntohs(hdr->udp.dst_port) + 1);
  rtcp_len = ipmx_sr_tx_build(sr, leg, (uint8_t*)&hdr[1], tai_ns, rtp_ts);

  mt_mbuf_init_ipv4(pkt);
  pkt->data_len = sizeof(*hdr) + rtcp_len;
  pkt->pkt_len = pkt->data_len;
  hdr->udp.dgram_len = htons(sizeof(hdr->udp) + rtcp_len);
  hdr->ipv4.total_length = htons(pkt->pkt_len - pkt->l2_len);
  hdr->ipv4.hdr_checksum = 0;
  if (!leg->ipv4_cksum_offload) hdr->ipv4.hdr_checksum = rte_ipv4_cksum(&hdr->ipv4);
  return pkt;
}

/* The first ns of the rtp_ts tick nearest anchor_ns, so that both floor and round of it
 * convert back to rtp_ts. From pkt0 alone: its frame may be reused by now. */
static uint64_t ipmx_sr_rtp_tai(uint64_t anchor_ns, uint32_t rtp_ts, uint32_t rate) {
  uint64_t near = st10_media_clk_to_tai(anchor_ns, rtp_ts, rate);
  uint64_t tick = ((__uint128_t)near * rate + NS_PER_S / 2) / NS_PER_S;

  return ((__uint128_t)tick * NS_PER_S + rate - 1) / rate;
}

/* false while an Announce is being stored */
static bool ipmx_sr_ptp_snap(struct mtl_main_impl* impl, enum mtl_port port,
                             struct ipmx_ptp_snap* snap) {
  const struct mt_ptp_impl* ptp = mt_get_ptp(impl, port);
  uint32_t sn;

  memset(snap, 0, sizeof(*snap));
  /* an application time source is not known to follow the master the PTP stack sees */
  if (!ptp || !ptp->active || !mt_user_ptp_service(impl) || mt_user_ptp_time_fn(impl) ||
      mt_user_ptp_tsc_source(impl))
    return true;
  snap->builtin = true;
  snap->connected = ptp->connected;
  sn = rte_seqcount_read_begin(&ptp->master_announce_seq);
  snap->announce_ns =
      atomic_load_explicit(&ptp->master_announce_ns, memory_order_relaxed);
  snap->log_announce_interval =
      atomic_load_explicit(&ptp->master_log_announce_interval, memory_order_relaxed);
  snap->gm_id = atomic_load_explicit(&ptp->master_gm_id, memory_order_relaxed);
  snap->domain = atomic_load_explicit(&ptp->master_domain, memory_order_relaxed);
  return !rte_seqcount_read_retry(&ptp->master_announce_seq, sn);
}

/* from the P port PTP only, for the Info Block both legs share */
static bool ipmx_sr_refclk_now(const struct ipmx_sr_tx* sr, struct ipmx_refclk* clk) {
  struct ipmx_ptp_snap snap;

  if (!ipmx_sr_ptp_snap(sr->impl, sr->leg[MTL_SESSION_PORT_P].port, &snap)) return false;
  *clk = ipmx_refclk_from_ptp(&snap, mt_get_tsc(sr->impl));
  return true;
}

/* 1 with the staged string in ts_refclk when the setter changed it, 0 when it did not,
 * -EBUSY while it writes */
static int ipmx_sr_app_refclk_take(struct ipmx_sr_tx* sr, char* ts_refclk) {
  const uint32_t sn = rte_seqcount_read_begin(&sr->app_seq);
  bool on;

  if (sn == sr->app_seq_seen) return 0;
  on = sr->app_on;
  memcpy(ts_refclk, sr->app_refclk, IPMX_TS_REFCLK_LEN);
  if (rte_seqcount_read_retry(&sr->app_seq, sn)) return -EBUSY;
  sr->app_seq_seen = sn;
  sr->app_active = on;
  if (!on) sr->refclk_known = false;
  return 1;
}

/* TR-10-1 section 8.7 and TR-10-9 section 12: a new ts-refclk is a new block version,
 * except before the first SR, which no receiver saw */
static void ipmx_sr_refclk_update(struct ipmx_sr_tx* sr) {
  char ts_refclk[IPMX_TS_REFCLK_LEN];
  const int app = ipmx_sr_app_refclk_take(sr, ts_refclk);
  struct ipmx_refclk clk;

  if (app < 0 || (sr->app_active && !app)) return;
  if (!sr->app_active) {
    if (!ipmx_sr_refclk_now(sr, &clk)) return;
    if (sr->refclk_known && clk.ptp == sr->refclk.ptp && clk.gm_id == sr->refclk.gm_id &&
        clk.domain == sr->refclk.domain)
      return;
    sr->refclk = clk;
    sr->refclk_known = true;
    ipmx_refclk_print(&clk, sr->localmac, ts_refclk);
  }
  if (!memcmp(ts_refclk, sr->info.ts_refclk, IPMX_TS_REFCLK_LEN)) return;

  rte_seqcount_write_begin(&sr->info_seq);
  memcpy(sr->info.ts_refclk, ts_refclk, IPMX_TS_REFCLK_LEN);
  memcpy(&sr->info_block[IPMX_TS_REFCLK_OFFSET], ts_refclk, IPMX_TS_REFCLK_LEN);
  if (sr->sr_built)
    atomic_fetch_add_explicit(&sr->block_version, 1, memory_order_relaxed);
  rte_seqcount_write_end(&sr->info_seq);
  dbg("%s, ts-refclk %s\n", __func__, ts_refclk);
}

static inline uint32_t ipmx_sr_tx_frame_size(struct rte_mbuf* pkt0) {
  return ((struct mt_muf_priv_data*)rte_mbuf_to_priv(pkt0))->tx_priv.ipmx_frame_size;
}

static inline uint32_t ipmx_sr_pkt_rtp_ts(struct rte_mbuf* pkt) {
  return ntohl(rte_pktmbuf_mtod(pkt, struct st_rfc4175_video_hdr*)->rtp.base.tmstamp);
}

/* the leg first to start a frame takes the ts-refclk and version both legs send in it;
 * a leg that already sent the current block starts a new frame even on a repeated RTP
 * timestamp */
static void ipmx_sr_frame_info(struct ipmx_sr_tx* sr, struct ipmx_sr_leg* leg,
                               uint32_t rtp_ts) {
  bool sent = leg->info_gen == sr->info_gen;

  leg->info_gen = sr->info_gen;
  if (!sent && (int32_t)(rtp_ts - sr->info_rtp_ts) <= 0) return;
  leg->info_gen = ++sr->info_gen;
  sr->info_rtp_ts = rtp_ts;
  ipmx_sr_refclk_update(sr);
  sr->info_block[IPMX_INFO_BLOCK_VERSION_OFFSET] =
      atomic_load_explicit(&sr->block_version, memory_order_relaxed);
}

void ipmx_sr_tx_frame_start(struct ipmx_sr_tx* sr, enum mtl_session_port s_port,
                            struct rte_mbuf* pkt0, bool late) {
  struct ipmx_sr_leg* leg = &sr->leg[s_port];
  uint32_t rtp_ts = ipmx_sr_pkt_rtp_ts(pkt0);
  uint64_t tai_ns;

  leg->last_rtp_ts = rtp_ts;
  if (leg->pending) {
    rte_pktmbuf_free(leg->pending);
    leg->stat.dropped++;
  }
  if (late) leg->stat.late++;
  ipmx_sr_frame_info(sr, leg, rtp_ts);
  tai_ns = ipmx_sr_rtp_tai(st_tx_mbuf_get_ptp(pkt0), rtp_ts, sr->sampling_rate);
  leg->pending = ipmx_sr_tx_pkt(sr, leg, tai_ns, rtp_ts);
  if (leg->pending)
    sr->sr_built = true;
  else
    leg->stat.alloc_fail++;
  if (sr->st22_pkt_len) {
    uint32_t size = ipmx_sr_tx_frame_size(pkt0);
    uint32_t pkts = (size + sr->st22_pkt_len - 1) / sr->st22_pkt_len;

    leg->pkt_count += pkts;
    leg->octet_count += size + pkts * IPMX_RFC9134_HDR_LEN;
  } else {
    leg->pkt_count += sr->frame_pkts;
    leg->octet_count += sr->frame_octets;
  }
}

bool ipmx_sr_tx_send_pending(struct ipmx_sr_tx* sr, enum mtl_session_port s_port,
                             struct mt_txq_entry* queue, struct rte_mbuf* pkt0) {
  struct ipmx_sr_leg* leg = &sr->leg[s_port];
  struct rte_mbuf* pkt = leg->pending;
  struct mt_interface* inf;

  if (st_tx_mbuf_get_idx(pkt0)) return true;
  if (ipmx_sr_pkt_rtp_ts(pkt0) != leg->last_rtp_ts) return true;

  inf = mt_if(sr->impl, leg->port);
  if (pkt0->ol_flags & inf->tx_launch_time_flag) {
    pkt->ol_flags |= inf->tx_launch_time_flag;
    *RTE_MBUF_DYNFIELD(pkt, inf->tx_dynfield_offset, uint64_t*) =
        *RTE_MBUF_DYNFIELD(pkt0, inf->tx_dynfield_offset, uint64_t*);
  }
  if (!mt_txq_burst(queue, &pkt, 1)) {
    leg->stat.burst_fail++;
    return false;
  }
  leg->pending = NULL;
  leg->stat.sent++;
  return true;
}

uint64_t ipmx_sr_tx_rl_lead_ns(const struct ipmx_sr_tx* sr, enum mtl_session_port s_port,
                               const struct st_tx_video_rl_plan* plan) {
  const struct rte_mbuf* pending = sr->leg[s_port].pending;

  if (!pending || !plan->pad_size) return 0;
  return plan->pad_ns * pending->pkt_len / plan->pad_size;
}

/* one SR a frame: the frames the TX ring can hold, the pending one and slack; an st22
 * frame can shrink to one packet and its SR */
static unsigned int ipmx_sr_pool_n(struct mtl_main_impl* impl,
                                   struct st_tx_video_session_impl* s,
                                   enum mtl_session_port s_port) {
  enum mtl_port port = mt_port_logic2phy(s->port_maps, s_port);
  int desc_per_frame = s->st22_info ? 2 : RTE_MAX(s->st20_total_pkts, 1);

  return mt_if_nb_tx_desc(impl, port) / desc_per_frame + 4;
}

static void ipmx_sr_tx_free(struct ipmx_sr_tx* sr) {
  for (int i = sr->num_port - 1; i >= 0; i--) {
    struct ipmx_sr_leg* leg = &sr->leg[i];

    if (leg->pending) rte_pktmbuf_free(leg->pending);
    if (leg->pool) mt_mempool_free(leg->pool);
  }
  mt_rte_free(sr);
}

static int ipmx_sr_leg_init(struct mtl_main_impl* impl,
                            struct st_tx_video_session_impl* s, struct ipmx_sr_tx* sr,
                            enum mtl_session_port s_port) {
  struct ipmx_sr_leg* leg = &sr->leg[s_port];
  const uint8_t* sip = (const uint8_t*)&s->s_hdr[s_port].ipv4.src_addr;
  char pool_name[32];

  leg->port = mt_port_logic2phy(s->port_maps, s_port);
  snprintf(pool_name, sizeof(pool_name), "%sM%dS%d_SR%d_%d", ST_TX_VIDEO_PREFIX,
           s->mgr->idx, s->idx, s_port, s->recovery_idx);
  leg->pool =
      mt_mempool_create(impl, leg->port, pool_name, ipmx_sr_pool_n(impl, s, s_port), 0, 0,
                        IPMX_SR_POOL_DATA_SIZE);
  if (!leg->pool) return -ENOMEM;
  leg->media_hdr = &s->s_hdr[s_port];
  leg->ipv4_cksum_offload = s->eth_ipv4_cksum_offload[s_port];
  /* TR-10-9 section 8 asks for a CNAME, RFC 3550 section 6.5.1 allows the numeric host */
  leg->cname_len = snprintf(leg->cname, sizeof(leg->cname), "%u.%u.%u.%u", sip[0], sip[1],
                            sip[2], sip[3]);
  info("%s(%d,%d), cname %s\n", __func__, s->idx, s_port, leg->cname);
  return 0;
}

int ipmx_sr_tx_init(struct mtl_main_impl* impl, struct st_tx_video_session_impl* s) {
  enum mtl_port port = mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P);
  struct ipmx_sr_tx* sr;
  int ret;

  RTE_BUILD_BUG_ON(sizeof(struct mt_udp_hdr) + IPMX_RTCP_SR_IPMX_MAX_LEN +
                       IPMX_RTCP_SDES_MAX_LEN >
                   IPMX_SR_POOL_DATA_SIZE);
  sr = mt_rte_zmalloc_socket(sizeof(*sr), mt_socket_id(impl, port));
  if (!sr) return -ENOMEM;
  ret = ipmx_video_info_from_session(impl, s, &sr->info);
  if (ret < 0) {
    mt_rte_free(sr);
    return ret;
  }
  for (int i = 0; i < s->ops.num_port; i++) {
    sr->num_port++;
    ret = ipmx_sr_leg_init(impl, s, sr, i);
    if (ret < 0) {
      ipmx_sr_tx_free(sr);
      return ret;
    }
  }

  sr->impl = impl;
  sr->sampling_rate = s->fps_tm.sampling_clock_rate;
  /* the VSF SDP parser rejects a zero id, which an app ptp_get_time_fn can give */
  sr->sdp_sess_id = RTE_MAX(mt_get_ptp_time(impl, port), (uint64_t)1);
  sr->sdp_sess_version = 1;
  rte_seqcount_init(&sr->info_seq);
  rte_seqcount_init(&sr->app_seq);
  memcpy(sr->localmac, sr->info.ts_refclk, sizeof(sr->localmac));
  while (!ipmx_sr_refclk_now(sr, &sr->refclk)) rte_pause();
  sr->refclk_known = true;
  ipmx_refclk_print(&sr->refclk, sr->localmac, sr->info.ts_refclk);
  if (s->st22_info) {
    const struct st22_rfc9134_rtp_hdr* rtp = &s->st22_info->rtp_hdr[MTL_SESSION_PORT_P];
    const struct st22_jxpl* jxpl = &s->st22_info->st22_boxes.jpvs.jxpl;

    sr->jxs.transmode = rtp->trans_order;
    sr->jxs.packetmode = rtp->kmode;
    sr->jxs.ppih = ntohs(jxpl->ppih);
    sr->jxs.plev = ntohs(jxpl->plev);
    sr->info_block_len = ipmx_info_block_jxs_build(&sr->info, &sr->jxs, sr->info_block);
    sr->st22_pkt_len = s->st20_pkt_len;
    sr->jxsv_kbps =
        ipmx_sdp_jxsv_kbps((uint64_t)s->st22_box_hdr_length + s->st22_codestream_size,
                           s->st20_pkt_len, sr->info.rate_num, sr->info.rate_den);
  } else {
    sr->info_block_len = ipmx_info_block_build(&sr->info, sr->info_block);
    for (int i = 0; i < ST20_PKT_TYPE_MAX; i++) {
      const struct st20_packet_group_info* info = &s->st20_pkt_info[i];

      sr->frame_pkts += info->number;
      sr->frame_octets +=
          info->number * (info->size - sizeof(struct mt_udp_hdr) - IPMX_RTP_HDR_LEN);
    }
  }

  s->ipmx_sr = sr;
  if (sr->st22_pkt_len)
    info("%s(%d), %s, st22 %u payload octets per packet\n", __func__, s->idx,
         sr->info.ts_refclk, sr->st22_pkt_len);
  else
    info("%s(%d), %s, %u pkts %u octets per frame\n", __func__, s->idx,
         sr->info.ts_refclk, sr->frame_pkts, sr->frame_octets);
  return 0;
}

void ipmx_sr_tx_uinit(struct st_tx_video_session_impl* s) {
  if (!s->ipmx_sr) return;
  ipmx_sr_tx_free(s->ipmx_sr);
  s->ipmx_sr = NULL;
}

int ipmx_sr_tx_set_block_version(struct st_tx_video_session_impl* s, uint8_t version) {
  if (!s->ipmx_sr) {
    err("%s(%d), IPMX not enabled\n", __func__, s->idx);
    return -EIO;
  }
  atomic_store_explicit(&s->ipmx_sr->block_version, version, memory_order_relaxed);
  return 0;
}

static bool ipmx_sr_ts_refclk_valid(const char* ts_refclk) {
  const size_t len = strnlen(ts_refclk, IPMX_TS_REFCLK_LEN);

  if (len >= IPMX_TS_REFCLK_LEN) return false;
  if (!(len > 4 && !strncmp(ts_refclk, "ptp=", 4)) &&
      !(len > 9 && !strncmp(ts_refclk, "localmac=", 9)))
    return false;
  for (size_t i = 0; i < len; i++) {
    /* visible ASCII, an SDP attribute value has no space */
    if (ts_refclk[i] <= ' ' || ts_refclk[i] > '~') return false;
  }
  return true;
}

int ipmx_sr_tx_set_ts_refclk(struct st_tx_video_session_impl* s, const char* ts_refclk) {
  struct ipmx_sr_tx* sr = s->ipmx_sr;

  if (!sr) {
    err("%s(%d), IPMX not enabled\n", __func__, s->idx);
    return -EIO;
  }
  if (ts_refclk && !ipmx_sr_ts_refclk_valid(ts_refclk)) {
    err("%s(%d), invalid ts-refclk\n", __func__, s->idx);
    return -EINVAL;
  }

  rte_seqcount_write_begin(&sr->app_seq);
  sr->app_on = ts_refclk != NULL;
  memset(sr->app_refclk, 0, sizeof(sr->app_refclk));
  if (ts_refclk) memcpy(sr->app_refclk, ts_refclk, strlen(ts_refclk));
  rte_seqcount_write_end(&sr->app_seq);
  info("%s(%d), ts-refclk %s\n", __func__, s->idx, ts_refclk ? ts_refclk : "automatic");
  return 0;
}

int ipmx_sr_tx_get_info(struct st_tx_video_session_impl* s,
                        struct ipmx_video_info* info) {
  struct ipmx_sr_tx* sr = s->ipmx_sr;
  uint32_t sn;

  if (!sr) {
    err("%s(%d), IPMX not enabled\n", __func__, s->idx);
    return -EIO;
  }
  if (!info) {
    err("%s(%d), NULL info\n", __func__, s->idx);
    return -EINVAL;
  }
  do {
    sn = rte_seqcount_read_begin(&sr->info_seq);
    mt_memcpy(info, &sr->info, sizeof(*info));
    info->version = atomic_load_explicit(&sr->block_version, memory_order_relaxed);
  } while (rte_seqcount_read_retry(&sr->info_seq, sn));
  return 0;
}

void ipmx_sr_stat_take(struct ipmx_sr_tx* sr, struct ipmx_sr_stats* stats) {
  stats->num_port = sr->num_port;
  for (int i = 0; i < sr->num_port; i++) {
    stats->leg[i] = sr->leg[i].stat;
    memset(&sr->leg[i].stat, 0, sizeof(sr->leg[i].stat));
  }
}

void ipmx_sr_stat_log(const struct ipmx_sr_stats* stats, int m_idx, int idx) {
  for (int i = 0; i < stats->num_port; i++) {
    const struct ipmx_sr_stat* stat = &stats->leg[i];

    notice("TX_VIDEO_SESSION(%d,%d): IPMX SR %c sent %" PRIu64 " late %" PRIu64
           " alloc fail %" PRIu64 " burst fail %" PRIu64 " dropped %" PRIu64 "\n",
           m_idx, idx, i == MTL_SESSION_PORT_P ? 'P' : 'R', stat->sent, stat->late,
           stat->alloc_fail, stat->burst_fail, stat->dropped);
  }
}
