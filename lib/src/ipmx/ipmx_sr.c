/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

#include "ipmx_sr.h"

#include "../datapath/mt_queue.h"
#include "../mt_log.h"
#include "../mt_util.h"
#include "../st2110/st_tx_video_session.h"

#define IPMX_SR_POOL_DATA_SIZE (320)
#define IPMX_RTCP_PT_SR (200)
#define IPMX_RTCP_PT_SDES (202)
#define IPMX_RTCP_SDES_CNAME (1)
#define IPMX_RTCP_SR_LEN (28)
#define IPMX_RTCP_SR_IPMX_LEN (IPMX_RTCP_SR_LEN + IPMX_INFO_BLOCK_LEN)
#define IPMX_RTCP_SDES_MAX_LEN (8 + RTE_ALIGN_CEIL(2 + IPMX_SR_CNAME_MAX, 4))
#define IPMX_RTP_HDR_LEN (12)

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
static uint16_t ipmx_sr_tx_build(const struct ipmx_sr_tx* sr, uint8_t* rtcp,
                                 uint64_t tai_ns, uint32_t rtp_ts) {
  uint16_t sdes_len = 8 + RTE_ALIGN_CEIL(2 + sr->cname_len + 1, 4);
  uint8_t* sdes = rtcp + IPMX_RTCP_SR_IPMX_LEN;
  uint32_t ssrc = ntohl(sr->media_hdr->rtp.base.ssrc);

  rtcp[0] = 2 << 6; /* V=2, P=0, RC=0 */
  rtcp[1] = IPMX_RTCP_PT_SR;
  ipmx_put_be16(&rtcp[2], IPMX_RTCP_SR_IPMX_LEN / 4 - 1);
  ipmx_put_be32(&rtcp[4], ssrc);
  /* TR-10-1 section 8.7: PTP truncated timestamp, not the RFC 3550 NTP format */
  ipmx_put_be32(&rtcp[8], (uint32_t)(tai_ns / NS_PER_S));
  ipmx_put_be32(&rtcp[12], (uint32_t)(tai_ns % NS_PER_S));
  ipmx_put_be32(&rtcp[16], rtp_ts);
  ipmx_put_be32(&rtcp[20], sr->pkt_count);
  ipmx_put_be32(&rtcp[24], sr->octet_count);
  memcpy(&rtcp[IPMX_RTCP_SR_LEN], sr->info_block, IPMX_INFO_BLOCK_LEN);

  memset(sdes, 0, sdes_len);
  sdes[0] = (2 << 6) | 1; /* V=2, P=0, SC=1 */
  sdes[1] = IPMX_RTCP_PT_SDES;
  ipmx_put_be16(&sdes[2], sdes_len / 4 - 1);
  ipmx_put_be32(&sdes[4], ssrc);
  sdes[8] = IPMX_RTCP_SDES_CNAME;
  sdes[9] = sr->cname_len;
  memcpy(&sdes[10], sr->cname, sr->cname_len);

  return IPMX_RTCP_SR_IPMX_LEN + sdes_len;
}

static struct rte_mbuf* ipmx_sr_tx_pkt(struct ipmx_sr_tx* sr, uint64_t tai_ns,
                                       uint32_t rtp_ts) {
  struct rte_mbuf* pkt = rte_pktmbuf_alloc(sr->pool);
  struct mt_udp_hdr* hdr;
  uint16_t rtcp_len;

  if (!pkt) return NULL;

  hdr = rte_pktmbuf_mtod(pkt, struct mt_udp_hdr*);
  mt_memcpy(hdr, sr->media_hdr, sizeof(*hdr));
  hdr->udp.dst_port = htons(ntohs(hdr->udp.dst_port) + 1);
  rtcp_len = ipmx_sr_tx_build(sr, (uint8_t*)&hdr[1], tai_ns, rtp_ts);

  mt_mbuf_init_ipv4(pkt);
  pkt->data_len = sizeof(*hdr) + rtcp_len;
  pkt->pkt_len = pkt->data_len;
  hdr->udp.dgram_len = htons(sizeof(hdr->udp) + rtcp_len);
  hdr->ipv4.total_length = htons(pkt->pkt_len - pkt->l2_len);
  hdr->ipv4.hdr_checksum = 0;
  if (!sr->ipv4_cksum_offload) hdr->ipv4.hdr_checksum = rte_ipv4_cksum(&hdr->ipv4);
  return pkt;
}

/* The first ns of the rtp_ts tick nearest anchor_ns, so that both floor and round of it
 * convert back to rtp_ts. From pkt0 alone: its frame may be reused by now. */
static uint64_t ipmx_sr_rtp_tai(uint64_t anchor_ns, uint32_t rtp_ts, uint32_t rate) {
  uint64_t near = st10_media_clk_to_tai(anchor_ns, rtp_ts, rate);
  uint64_t tick = ((__uint128_t)near * rate + NS_PER_S / 2) / NS_PER_S;

  return ((__uint128_t)tick * NS_PER_S + rate - 1) / rate;
}

static inline uint32_t ipmx_sr_pkt_rtp_ts(struct rte_mbuf* pkt) {
  return ntohl(rte_pktmbuf_mtod(pkt, struct st_rfc4175_video_hdr*)->rtp.base.tmstamp);
}

void ipmx_sr_tx_frame_start(struct ipmx_sr_tx* sr, enum mtl_session_port s_port,
                            struct rte_mbuf* pkt0, bool late) {
  uint32_t rtp_ts;
  uint64_t tai_ns;

  /* the ST 2022-7 R leg sends no SR yet */
  if (s_port != MTL_SESSION_PORT_P) return;
  rtp_ts = ipmx_sr_pkt_rtp_ts(pkt0);
  sr->last_rtp_ts = rtp_ts;

  if (sr->pending) {
    rte_pktmbuf_free(sr->pending);
    sr->stat.dropped++;
  }
  if (late) sr->stat.late++;
  tai_ns = ipmx_sr_rtp_tai(st_tx_mbuf_get_ptp(pkt0), rtp_ts, sr->sampling_rate);
  sr->pending = ipmx_sr_tx_pkt(sr, tai_ns, rtp_ts);
  if (!sr->pending) sr->stat.alloc_fail++;
  sr->pkt_count += sr->frame_pkts;
  sr->octet_count += sr->frame_octets;
}

bool ipmx_sr_tx_send_pending(struct ipmx_sr_tx* sr, enum mtl_session_port s_port,
                             struct mt_txq_entry* queue, struct rte_mbuf* pkt0) {
  struct rte_mbuf* pkt = sr->pending;
  struct mt_interface* inf;

  if (s_port != MTL_SESSION_PORT_P || st_tx_mbuf_get_idx(pkt0)) return true;
  if (ipmx_sr_pkt_rtp_ts(pkt0) != sr->last_rtp_ts) return true;

  inf = mt_if(sr->impl, sr->port);
  if (pkt0->ol_flags & inf->tx_launch_time_flag) {
    pkt->ol_flags |= inf->tx_launch_time_flag;
    *RTE_MBUF_DYNFIELD(pkt, inf->tx_dynfield_offset, uint64_t*) =
        *RTE_MBUF_DYNFIELD(pkt0, inf->tx_dynfield_offset, uint64_t*);
  }
  if (!mt_txq_burst(queue, &pkt, 1)) {
    sr->stat.burst_fail++;
    return false;
  }
  sr->pending = NULL;
  sr->stat.sent++;
  return true;
}

uint64_t ipmx_sr_tx_rl_lead_ns(const struct ipmx_sr_tx* sr, enum mtl_session_port s_port,
                               const struct st_tx_video_rl_plan* plan) {
  if (!sr->pending || s_port != MTL_SESSION_PORT_P || !plan->pad_size) return 0;
  return plan->pad_ns * sr->pending->pkt_len / plan->pad_size;
}

/* one SR a frame: the frames the TX ring can hold, the pending one and slack */
static unsigned int ipmx_sr_pool_n(struct mtl_main_impl* impl,
                                   struct st_tx_video_session_impl* s) {
  enum mtl_port port = mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P);

  return mt_if_nb_tx_desc(impl, port) / RTE_MAX(s->st20_total_pkts, 1) + 4;
}

int ipmx_sr_tx_init(struct mtl_main_impl* impl, struct st_tx_video_session_impl* s) {
  enum mtl_port port = mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P);
  const uint8_t* sip = (const uint8_t*)&s->s_hdr[MTL_SESSION_PORT_P].ipv4.src_addr;
  struct ipmx_video_info vinfo;
  struct ipmx_sr_tx* sr;
  char pool_name[32];
  unsigned int n;
  int ret;

  RTE_BUILD_BUG_ON(sizeof(struct mt_udp_hdr) + IPMX_RTCP_SR_IPMX_LEN +
                       IPMX_RTCP_SDES_MAX_LEN >
                   IPMX_SR_POOL_DATA_SIZE);
  ret = ipmx_video_info_from_session(impl, s, &vinfo);
  if (ret < 0) return ret;

  sr = mt_rte_zmalloc_socket(sizeof(*sr), mt_socket_id(impl, port));
  if (!sr) return -ENOMEM;
  snprintf(pool_name, sizeof(pool_name), "%sM%dS%d_SR_%d", ST_TX_VIDEO_PREFIX,
           s->mgr->idx, s->idx, s->recovery_idx);
  n = ipmx_sr_pool_n(impl, s);
  sr->pool = mt_mempool_create(impl, port, pool_name, n, 0, 0, IPMX_SR_POOL_DATA_SIZE);
  if (!sr->pool) {
    mt_rte_free(sr);
    return -ENOMEM;
  }

  sr->impl = impl;
  sr->port = port;
  sr->media_hdr = &s->s_hdr[MTL_SESSION_PORT_P];
  sr->ipv4_cksum_offload = s->eth_ipv4_cksum_offload[MTL_SESSION_PORT_P];
  sr->sampling_rate = s->fps_tm.sampling_clock_rate;
  ipmx_info_block_build(&vinfo, sr->info_block);
  /* TR-10-9 section 8 asks for a CNAME, RFC 3550 section 6.5.1 allows the numeric host */
  sr->cname_len = snprintf(sr->cname, sizeof(sr->cname), "%u.%u.%u.%u", sip[0], sip[1],
                           sip[2], sip[3]);
  for (int i = 0; i < ST20_PKT_TYPE_MAX; i++) {
    const struct st20_packet_group_info* info = &s->st20_pkt_info[i];

    sr->frame_pkts += info->number;
    sr->frame_octets +=
        info->number * (info->size - sizeof(struct mt_udp_hdr) - IPMX_RTP_HDR_LEN);
  }

  s->ipmx_sr = sr;
  info("%s(%d), cname %s, %s, %u pkts %u octets per frame\n", __func__, s->idx, sr->cname,
       vinfo.ts_refclk, sr->frame_pkts, sr->frame_octets);
  return 0;
}

void ipmx_sr_tx_uinit(struct st_tx_video_session_impl* s) {
  struct ipmx_sr_tx* sr = s->ipmx_sr;

  if (!sr) return;
  if (sr->pending) rte_pktmbuf_free(sr->pending);
  mt_mempool_free(sr->pool);
  mt_rte_free(sr);
  s->ipmx_sr = NULL;
}

void ipmx_sr_stat_log(const struct ipmx_sr_stat* stat, int m_idx, int idx) {
  notice("TX_VIDEO_SESSION(%d,%d): IPMX SR sent %" PRIu64 " late %" PRIu64
         " alloc fail %" PRIu64 " burst fail %" PRIu64 " dropped %" PRIu64 "\n",
         m_idx, idx, stat->sent, stat->late, stat->alloc_fail, stat->burst_fail,
         stat->dropped);
}
