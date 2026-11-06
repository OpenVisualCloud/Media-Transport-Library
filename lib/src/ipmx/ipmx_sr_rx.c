/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "ipmx_sr_rx.h"

#include "../datapath/mt_queue.h"
#include "../mt_cni.h"
#include "../mt_log.h"
#include "../st2110/st_rx_video_session.h"
#include "ipmx_info.h"

#define IPMX_RTCP_VERSION (2)
#define IPMX_RTCP_PADDING (0x20)
#define IPMX_RTCP_COUNT_MASK (0x1f)
#define IPMX_RTCP_PT_SR (200)
#define IPMX_RTCP_PT_SDES (202)
#define IPMX_RTCP_HDR_LEN (4)
#define IPMX_RTCP_SR_LEN (28)
#define IPMX_RTCP_REPORT_BLOCK_LEN (24)
#define IPMX_RTCP_SDES_END (0)
#define IPMX_RTCP_SDES_CNAME (1)

static inline uint16_t ipmx_get_be16(const uint8_t* p) {
  return (uint16_t)(p[0] << 8 | p[1]);
}

static inline uint32_t ipmx_get_be32(const uint8_t* p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* the inverse of ipmx_info_block_build() for one IPMX_MEDIA_INFO_VIDEO_LEN block */
static void ipmx_sr_rx_video_mib(const uint8_t* mib, struct ipmx_video_info* info) {
  uint32_t rate = ipmx_get_be32(&mib[76]);

  memcpy(info->sampling, &mib[4], sizeof(info->sampling));
  info->floating = mib[20] >> 7;
  info->depth = mib[20] & 0x7f;
  info->gpm = (mib[21] >> 7) & 1;
  info->interlaced = (mib[21] >> 6) & 1;
  info->segmented = (mib[21] >> 5) & 1;
  info->par_w = mib[22];
  info->par_h = mib[23];
  memcpy(info->range, &mib[24], sizeof(info->range));
  memcpy(info->colorimetry, &mib[36], sizeof(info->colorimetry));
  memcpy(info->tcs, &mib[56], sizeof(info->tcs));
  info->width = ipmx_get_be16(&mib[72]);
  info->height = ipmx_get_be16(&mib[74]);
  info->rate_num = rate >> 10;
  info->rate_den = rate & 0x3ff;
  info->pixclk = (uint64_t)ipmx_get_be32(&mib[80]) << 32 | ipmx_get_be32(&mib[84]);
  info->htotal = ipmx_get_be16(&mib[88]);
  info->vtotal = ipmx_get_be16(&mib[90]);
}

/* TR-10-15 Part 1 section 9, the reserved bits dropped */
static void ipmx_sr_rx_jxs_mib(const uint8_t* mib, struct ipmx_jxs_info* jxs) {
  uint32_t word1 = ipmx_get_be32(&mib[4]);

  jxs->transmode = word1 >> 31;
  jxs->packetmode = (word1 >> 30) & 1;
  jxs->ppih = word1 & 0xffff;
  jxs->plev = ipmx_get_be16(&mib[8]);
}

/*
 * TR-10-1 section 8.7: the profile extension, an IPMX Info Block if tagged so. A defect
 * keeps the SR: in the block header it drops the block, in a Media Info Block it ends the
 * walk with the header and the MIBs before it kept; both count in info_malformed.
 */
static void ipmx_sr_rx_info_block(const uint8_t* ext, size_t len, uint16_t video_mib,
                                  struct ipmx_rx_sr_info* sr) {
  size_t block_len, mib_len;

  if (len < IPMX_RTCP_HDR_LEN || ipmx_get_be16(ext) != IPMX_INFO_TAG) return;
  block_len = ((size_t)ipmx_get_be16(&ext[2]) + 1) * 4;
  if (block_len > len || block_len < IPMX_INFO_HDR_LEN) {
    sr->info_malformed = 1;
    return;
  }

  sr->has_info = true;
  sr->info.version = ext[IPMX_INFO_BLOCK_VERSION_OFFSET];
  memcpy(sr->info.ts_refclk, &ext[IPMX_TS_REFCLK_OFFSET], sizeof(sr->info.ts_refclk));
  memcpy(sr->info.mediaclk, &ext[IPMX_MEDIACLK_OFFSET], sizeof(sr->info.mediaclk));
  for (size_t off = IPMX_INFO_HDR_LEN; off < block_len; off += mib_len) {
    const uint8_t* mib = &ext[off];
    uint16_t type;

    mib_len = ((size_t)ipmx_get_be16(&mib[2]) + 1) * 4;
    if (mib_len > block_len - off) {
      sr->info_malformed = 1;
      return;
    }
    type = ipmx_get_be16(mib);
    if (type == video_mib) {
      if (mib_len < IPMX_MEDIA_INFO_VIDEO_LEN) {
        sr->info_malformed = 1;
        return;
      }
      if (!sr->has_video_mib) ipmx_sr_rx_video_mib(mib, &sr->info);
      sr->has_video_mib = true;
    } else if (type == IPMX_MEDIA_INFO_JXS && video_mib == IPMX_MEDIA_INFO_CBR_VIDEO) {
      if (mib_len < IPMX_MEDIA_INFO_JXS_LEN) {
        sr->info_malformed = 1;
        return;
      }
      if (!sr->has_jxs_mib) ipmx_sr_rx_jxs_mib(mib, &sr->jxs);
      sr->has_jxs_mib = true;
    } else {
      sr->unknown_mib++;
    }
  }
}

/* an SR of len content octets with rc report blocks; only checked if sr is NULL */
static int ipmx_sr_rx_sr(const uint8_t* pkt, size_t len, uint8_t rc, uint16_t video_mib,
                         struct ipmx_rx_sr_info* sr) {
  size_t ext = IPMX_RTCP_SR_LEN + (size_t)rc * IPMX_RTCP_REPORT_BLOCK_LEN;

  if (len < ext) return -EINVAL;
  if (!sr) return 0;
  sr->ssrc = ipmx_get_be32(&pkt[4]);
  sr->ptp_sec = ipmx_get_be32(&pkt[8]);
  sr->ptp_ns = ipmx_get_be32(&pkt[12]);
  sr->rtp_timestamp = ipmx_get_be32(&pkt[16]);
  sr->pkt_count = ipmx_get_be32(&pkt[20]);
  sr->octet_count = ipmx_get_be32(&pkt[24]);
  ipmx_sr_rx_info_block(&pkt[ext], len - ext, video_mib, sr);
  return 0;
}

/* RFC 3550 section 6.5: sc chunks; the CNAME of ssrc goes to cname if want */
static int ipmx_sr_rx_sdes(const uint8_t* pkt, size_t len, uint8_t sc, bool want,
                           uint32_t ssrc, char* cname) {
  size_t off = IPMX_RTCP_HDR_LEN;

  for (uint8_t c = 0; c < sc; c++) {
    bool match;

    if (len - off < 4) return -EINVAL;
    match = want && ipmx_get_be32(&pkt[off]) == ssrc;
    off += 4;
    while (off < len && pkt[off] != IPMX_RTCP_SDES_END) {
      uint8_t item_len;

      if (len - off < 2) return -EINVAL;
      item_len = pkt[off + 1];
      if (len - off - 2 < item_len) return -EINVAL;
      if (match && pkt[off] == IPMX_RTCP_SDES_CNAME) {
        memcpy(cname, &pkt[off + 2], item_len);
        cname[item_len] = '\0';
      }
      off += 2 + item_len;
    }
    /* the null item, then up to the word end */
    off = RTE_MIN(RTE_ALIGN_CEIL(off + 1, 4), len);
  }
  return 0;
}

int ipmx_sr_rx_parse(const uint8_t* rtcp, size_t len, uint16_t video_mib,
                     struct ipmx_rx_sr_info* sr) {
  bool found = false;
  int ret;

  memset(sr, 0, sizeof(*sr));
  if (len < IPMX_RTCP_HDR_LEN) return -EINVAL;

  for (size_t off = 0; off < len;) {
    const uint8_t* pkt = &rtcp[off];
    size_t pkt_len, content;
    uint8_t count;

    if (len - off < IPMX_RTCP_HDR_LEN) return -EINVAL;
    if ((pkt[0] >> 6) != IPMX_RTCP_VERSION) return -EINVAL;
    pkt_len = ((size_t)ipmx_get_be16(&pkt[2]) + 1) * 4;
    if (pkt_len > len - off) return -EINVAL;
    content = pkt_len;
    if (pkt[0] & IPMX_RTCP_PADDING) {
      uint8_t pad = pkt[pkt_len - 1];

      if (!pad || pad > pkt_len - IPMX_RTCP_HDR_LEN) return -EINVAL;
      content -= pad;
    }
    count = pkt[0] & IPMX_RTCP_COUNT_MASK;

    if (pkt[1] == IPMX_RTCP_PT_SR) {
      ret = ipmx_sr_rx_sr(pkt, content, count, video_mib, found ? NULL : sr);
      if (ret < 0) return ret;
      found = true;
    } else if (pkt[1] == IPMX_RTCP_PT_SDES) {
      ret = ipmx_sr_rx_sdes(pkt, content, count, found, sr->ssrc, sr->cname);
      if (ret < 0) return ret;
    }
    off += pkt_len;
  }
  return found ? 0 : -ENOENT;
}

static void ipmx_sr_rx_port_reset(struct ipmx_sr_rx* sr, struct ipmx_sr_rx_port* p) {
  p->media_seen = sr->ops_ssrc != 0;
  p->media_ssrc = sr->ops_ssrc;
  p->version_seen = false;
  rte_seqcount_write_begin(&p->seq);
  memset(&p->info, 0, sizeof(p->info));
  rte_seqcount_write_end(&p->seq);
}

int ipmx_sr_rx_init(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s) {
  struct ipmx_sr_rx* sr;

  if (!(s->ops.flags & ST20_RX_FLAG_IPMX)) return 0;
  if (s->ops.flags & ST20_RX_FLAG_DATA_PATH_ONLY) {
    info("%s(%d), no sender report queue as data path only\n", __func__, s->idx);
    return 0;
  }
  sr = mt_rte_zmalloc_socket(sizeof(*sr), s->socket_id);
  if (!sr) {
    err("%s(%d), malloc fail\n", __func__, s->idx);
    return -ENOMEM;
  }
  sr->impl = impl;
  sr->ops_ssrc = s->ops.ssrc;
  /* TR-10-11 section 12 */
  sr->video_mib = (s->st22_ops_flags & ST22_RX_FLAG_IPMX) ? IPMX_MEDIA_INFO_CBR_VIDEO
                                                          : IPMX_MEDIA_INFO_VIDEO;
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) rte_seqcount_init(&sr->port[i].seq);
  s->ipmx_sr = sr;
  return 0;
}

void ipmx_sr_rx_uinit(struct st_rx_video_session_impl* s) {
  if (!s->ipmx_sr) return;
  mt_rte_free(s->ipmx_sr);
  s->ipmx_sr = NULL;
}

int ipmx_sr_rx_queue_get(struct st_rx_video_session_impl* s, enum mtl_session_port s_port,
                         enum mtl_port port, const struct mt_rxq_flow* media_flow) {
  struct ipmx_sr_rx* sr = s->ipmx_sr;
  struct ipmx_sr_rx_port* p = &sr->port[s_port];
  struct mt_rxq_flow flow = *media_flow;

  flow.dst_port++;
  flow.flags &= ~MT_RXQ_FLOW_F_HDR_SPLIT;
  flow.hdr_split_mbuf_cb_priv = NULL;
#ifdef ST_HAS_DPDK_HDR_SPLIT
  flow.hdr_split_mbuf_cb = NULL;
#endif
  flow.bytes_per_sec = 0;
  /* a ring of the CNI queue, not one more NIC queue; a CNI thread busy-polls once a
   * ring exists, so only with the CNI tasklet or a media flow already on CNI */
  if (mt_has_cni(sr->impl, port) && mt_get_cni(sr->impl)->lcore_tasklet)
    flow.flags |= MT_RXQ_FLOW_F_FORCE_CNI;

  ipmx_sr_rx_port_reset(sr, p);
  p->port = port;
  p->rxq = mt_rxq_get(sr->impl, port, &flow);
  if (!p->rxq) {
    err("%s(%d,%d), rxq get fail for udp %u\n", __func__, s->idx, s_port, flow.dst_port);
    return -EIO;
  }
  info("%s(%d,%d), sender report udp %u\n", __func__, s->idx, s_port, flow.dst_port);
  return 0;
}

void ipmx_sr_rx_queue_put(struct st_rx_video_session_impl* s) {
  struct ipmx_sr_rx* sr = s->ipmx_sr;

  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    if (!sr->port[i].rxq) continue;
    mt_rxq_put(sr->port[i].rxq);
    sr->port[i].rxq = NULL;
  }
}

/* the RTCP payload of one frame: as tv_tasklet_rtcp(), the bytes both the UDP header
 * declares and the mbuf holds, after an IPv4 header without options */
static int ipmx_sr_rx_parse_pkt(struct rte_mbuf* pkt, uint16_t video_mib,
                                struct ipmx_rx_sr_info* rep) {
  const struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(pkt, const struct mt_udp_hdr*);
  uint16_t data_len = rte_pktmbuf_data_len(pkt);
  uint16_t dgram_len;

  if (data_len < sizeof(*hdr)) return -EINVAL;
  if ((hdr->ipv4.version_ihl & RTE_IPV4_HDR_IHL_MASK) != RTE_IPV4_MIN_IHL) return -EINVAL;
  dgram_len = ntohs(hdr->udp.dgram_len);
  if (dgram_len < sizeof(hdr->udp)) return -EINVAL;
  return ipmx_sr_rx_parse(
      (const uint8_t*)&hdr[1],
      RTE_MIN((size_t)(data_len - sizeof(*hdr)), (size_t)(dgram_len - sizeof(hdr->udp))),
      video_mib, rep);
}

static void ipmx_sr_rx_pkt(struct st_rx_video_session_impl* s, struct ipmx_sr_rx_port* p,
                           struct rte_mbuf* pkt) {
  struct ipmx_sr_rx* sr = s->ipmx_sr;
  struct ipmx_rx_sr_info rep;
  struct ipmx_rx_sr_info* last = &p->info;
  bool mismatch, changed = false;
  int ret = ipmx_sr_rx_parse_pkt(pkt, sr->video_mib, &rep);

  if (ret == -ENOENT) return;
  mismatch = !ret && p->media_seen && rep.ssrc != p->media_ssrc;
  if (!ret && !mismatch) {
    if (rep.has_info) {
      changed = p->version_seen && rep.info.version != p->version;
      p->version_seen = true;
      p->version = rep.info.version;
    }
    rep.received = last->received + 1;
    rep.malformed = last->malformed;
    rep.no_info_block = last->no_info_block + (!rep.has_info && !rep.info_malformed);
    rep.info_malformed += last->info_malformed;
    rep.ssrc_mismatch = last->ssrc_mismatch;
    rep.unknown_mib += last->unknown_mib;
    rep.version_changes = last->version_changes + changed;
    rep.arrival_hw = mt_mbuf_has_hw_time_stamp(sr->impl, pkt, p->port);
    rep.arrival_ns = rep.arrival_hw ? mt_mbuf_time_stamp(sr->impl, pkt, p->port)
                                    : mt_get_ptp_time(sr->impl, p->port);
  }

  rte_seqcount_write_begin(&p->seq);
  if (ret < 0)
    last->malformed++;
  else if (mismatch)
    last->ssrc_mismatch++;
  else
    *last = rep;
  rte_seqcount_write_end(&p->seq);
  if (ret < 0) dbg("%s(%d), malformed rtcp %d\n", __func__, s->idx, ret);
}

int ipmx_sr_rx_poll(struct st_rx_video_session_impl* s, enum mtl_session_port s_port) {
  struct ipmx_sr_rx_port* p = &s->ipmx_sr->port[s_port];
  struct rte_mbuf* pkts[IPMX_SR_RX_BURST];
  uint16_t n;

  if (!p->rxq) return 0;
  n = mt_rxq_burst(p->rxq, pkts, IPMX_SR_RX_BURST);
  for (uint16_t i = 0; i < n; i++) ipmx_sr_rx_pkt(s, p, pkts[i]);
  rte_pktmbuf_free_bulk(pkts, n);
  return n;
}

int ipmx_sr_rx_get_info(struct st_rx_video_session_impl* s, enum mtl_session_port port,
                        struct ipmx_rx_sr_info* info) {
  struct ipmx_sr_rx_port* p;
  uint32_t sn;

  if (!(s->ops.flags & ST20_RX_FLAG_IPMX)) {
    dbg("%s(%d), IPMX not enabled\n", __func__, s->idx);
    return -EIO;
  }
  if (!info || port >= s->ops.num_port) {
    err("%s(%d), invalid port %d or info %p\n", __func__, s->idx, port, info);
    return -EINVAL;
  }
  if (!s->ipmx_sr) {
    err("%s(%d), no sender report as data path only\n", __func__, s->idx);
    return -ENOTSUP;
  }

  p = &s->ipmx_sr->port[port];
  do {
    sn = rte_seqcount_read_begin(&p->seq);
    mt_memcpy(info, &p->info, sizeof(*info));
  } while (rte_seqcount_read_retry(&p->seq, sn));
  return info->received ? 0 : -EBUSY;
}

void ipmx_sr_rx_stat_log(struct st_rx_video_session_impl* s, int m_idx) {
  for (int i = 0; i < s->ops.num_port; i++) {
    const struct ipmx_rx_sr_info* info = &s->ipmx_sr->port[i].info;

    notice("RX_VIDEO_SESSION(%d,%d): IPMX SR %c total received %" PRIu64
           " malformed %" PRIu64 " info malformed %" PRIu64 " no info block %" PRIu64
           " ssrc mismatch %" PRIu64 " unknown mib %" PRIu64 " version changes %" PRIu64
           "\n",
           m_idx, s->idx, i == MTL_SESSION_PORT_P ? 'P' : 'R', info->received,
           info->malformed, info->info_malformed, info->no_info_block,
           info->ssrc_mismatch, info->unknown_mib, info->version_changes);
  }
}
