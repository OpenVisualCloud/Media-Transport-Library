/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

#ifndef _IPMX_SR_RX_H_
#define _IPMX_SR_RX_H_

#include <rte_seqcount.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../mt_main.h"
#include "ipmx/ipmx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* vector PMDs return nothing below their descriptor batch (iavf AVX2: 8), so not less */
#define IPMX_SR_RX_BURST (32)

struct ipmx_sr_rx_port {
  struct mt_rxq_entry* rxq;
  enum mtl_port port;
  /* tasklet, or source update under the session lock */
  bool media_seen;
  uint32_t media_ssrc;
  bool version_seen;
  uint8_t version;
  /* the tasklet writes info under seq, the getter retries */
  rte_seqcount_t seq;
  struct ipmx_rx_sr_info info;
};

/* The Sender Reports of one st20 RX session, a queue on udp_port + 1 per port */
struct ipmx_sr_rx {
  struct mtl_main_impl* impl;
  uint32_t ops_ssrc; /* the SSRC the media RX filters on, 0 for any */
  struct ipmx_sr_rx_port port[MTL_SESSION_PORT_MAX];
};

/**
 * Parse one compound RTCP packet of len bytes, never reading past it. On 0 sr holds the
 * first Sender Report, its Info Block and CNAME, unknown_mib the Media Info Blocks of
 * unknown type in it and info_malformed 1 for an Info Block defect; the other counters
 * are 0. -ENOENT: no Sender Report, -EINVAL: malformed.
 */
int ipmx_sr_rx_parse(const uint8_t* rtcp, size_t len, struct ipmx_rx_sr_info* sr);

struct st_rx_video_session_impl;
/** Create s->ipmx_sr with ST20_RX_FLAG_IPMX, unless DATA_PATH_ONLY or an st22 session. */
int ipmx_sr_rx_init(struct mtl_main_impl* impl, struct st_rx_video_session_impl* s);
void ipmx_sr_rx_uinit(struct st_rx_video_session_impl* s);
/**
 * Get the s_port queue of s->ipmx_sr: media_flow with udp port + 1, no header split, on
 * the CNI queue when the media flow is (MTL_FLAG_RX_USE_CNI), or when the port has one
 * and CNI runs as a tasklet. Resets the port's counters and last SR.
 */
int ipmx_sr_rx_queue_get(struct st_rx_video_session_impl* s, enum mtl_session_port s_port,
                         enum mtl_port port, const struct mt_rxq_flow* media_flow);
void ipmx_sr_rx_queue_put(struct st_rx_video_session_impl* s);
/** Read the s_port Sender Reports, tasklet safe; returns the packets read. */
int ipmx_sr_rx_poll(struct st_rx_video_session_impl* s, enum mtl_session_port s_port);
/** -EIO without ST20_RX_FLAG_IPMX, see ipmx_st20_rx_get_sr_info() for the others. */
int ipmx_sr_rx_get_info(struct st_rx_video_session_impl* s, enum mtl_session_port port,
                        struct ipmx_rx_sr_info* info);
/** Log the counters of each port, under the session lock. */
void ipmx_sr_rx_stat_log(struct st_rx_video_session_impl* s, int m_idx);

/** Take the media SSRC on s_port from pkt, one media packet of a burst. */
static inline void ipmx_sr_rx_media(struct ipmx_sr_rx* sr, enum mtl_session_port s_port,
                                    struct rte_mbuf* pkt) {
  struct ipmx_sr_rx_port* p = &sr->port[s_port];
  struct mt_udp_hdr* hdr = rte_pktmbuf_mtod(pkt, struct mt_udp_hdr*);
  struct st_rfc3550_rtp_hdr* rtp = (struct st_rfc3550_rtp_hdr*)&hdr[1];

  if (sr->ops_ssrc) return; /* fixed at queue get, pkt may be one the RX dropped */
  if (unlikely(pkt->data_len < sizeof(*hdr) + sizeof(*rtp))) return;
  p->media_ssrc = ntohl(rtp->ssrc);
  p->media_seen = true;
}

#ifdef __cplusplus
}
#endif

#endif
