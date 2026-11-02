/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

#ifndef _IPMX_SR_H_
#define _IPMX_SR_H_

#include <rte_seqcount.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "../mt_main.h"
#include "ipmx_info.h"
#include "ipmx_sdp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "255.255.255.255" and its terminating null */
#define IPMX_SR_CNAME_MAX (16)

struct ipmx_sr_stat {
  uint64_t sent;
  uint64_t late; /* packet 0 of the frame was already due when dequeued */
  uint64_t alloc_fail;
  uint64_t
      burst_fail;   /* refused attempts, each retried ahead of packet 0, not lost SRs */
  uint64_t dropped; /* freed as their packet 0 never went out, a queue recovery */
};

/* One ST 2022-7 leg's SR, to that leg's destination on that leg's queue */
struct ipmx_sr_leg {
  enum mtl_port port;
  /* the media header template, read per SR so destination updates carry over */
  const struct st_rfc4175_video_hdr* media_hdr;
  bool ipv4_cksum_offload;
  struct rte_mempool* pool;
  char cname[IPMX_SR_CNAME_MAX];
  uint8_t cname_len;
  uint32_t info_gen; /* the info_block generation this leg sent last */
  uint32_t pkt_count;
  uint32_t octet_count;
  uint32_t last_rtp_ts; /* of the pending SR, matched against the burst's packet 0 */
  /* built when packet 0 is dequeued, sent on its queue right before it */
  struct rte_mbuf* pending;
  struct ipmx_sr_stat stat;
};

/* The TR-10-1 section 8.8.2 Sender Report of one st20 TX session, a leg per port */
struct ipmx_sr_tx {
  struct mtl_main_impl* impl;
  uint32_t sampling_rate;
  /* version unused: block_version is the one on the wire; ts_refclk under info_seq */
  struct ipmx_video_info info;
  _Atomic uint8_t block_version;
  rte_seqcount_t info_seq; /* the transmitter writes ts_refclk and the version under it */
  char localmac[IPMX_TS_REFCLK_LEN];
  /* ipmx_sr_tx_set_ts_refclk() stages these, the transmitter takes them at packet 0 */
  rte_seqcount_t app_seq;
  bool app_on;
  char app_refclk[IPMX_TS_REFCLK_LEN];
  /* transmitter only */
  uint32_t app_seq_seen;
  bool app_active;
  struct ipmx_refclk refclk;
  bool refclk_known; /* false: refclk is not the one on the wire */
  bool sr_built;
  /* info_block refreshes once a frame, at the RTP timestamp of that frame */
  uint32_t info_gen;
  uint32_t info_rtp_ts;
  uint8_t info_block[IPMX_INFO_BLOCK_LEN];
  uint32_t frame_pkts;
  uint32_t frame_octets; /* RTP payload octets, RFC 3550 section 6.4.1 */
  uint8_t num_port;
  struct ipmx_sr_leg leg[MTL_SESSION_PORT_MAX];
  /* ipmx_sdp_tx_get() only, under the session lock */
  uint64_t sdp_sess_id;
  uint64_t sdp_sess_version;
  bool sdp_taken;
  struct ipmx_sdp_media sdp_last;
};

/* the per-leg stats of one session dump */
struct ipmx_sr_stats {
  uint8_t num_port;
  struct ipmx_sr_stat leg[MTL_SESSION_PORT_MAX];
};

struct st_tx_video_session_impl;
/** Create s->ipmx_sr for every session port, after the header templates. */
int ipmx_sr_tx_init(struct mtl_main_impl* impl, struct st_tx_video_session_impl* s);
/** Free s->ipmx_sr; only after the session queues are flushed, they hold its mbufs. */
void ipmx_sr_tx_uinit(struct st_tx_video_session_impl* s);
/**
 * Build the s_port SR + SDES CNAME compound of the frame pkt0 starts; late is true when
 * pkt0 is already due. Tasklet safe.
 */
void ipmx_sr_tx_frame_start(struct ipmx_sr_tx* sr, enum mtl_session_port s_port,
                            struct rte_mbuf* pkt0, bool late);
bool ipmx_sr_tx_send_pending(struct ipmx_sr_tx* sr, enum mtl_session_port s_port,
                             struct mt_txq_entry* queue, struct rte_mbuf* pkt0);
/**
 * Burst the SR of pkt0's frame on queue, the queue pkt0 is about to be burst on, with
 * pkt0's launch time. False when the queue refused the SR: pkt0 must not be burst now,
 * the SR goes ahead of it on its retry.
 */
static inline bool ipmx_sr_tx_before_burst(struct ipmx_sr_tx* sr,
                                           enum mtl_session_port s_port,
                                           struct mt_txq_entry* queue,
                                           struct rte_mbuf* pkt0) {
  if (likely(!sr->leg[s_port].pending)) return true;
  return ipmx_sr_tx_send_pending(sr, s_port, queue, pkt0);
}
/**
 * The shaper time the pending SR takes ahead of packet 0, by an armed RL pad plan. An
 * unarmed warm-up counts whole trs pads and packet 0 waits in software for its target,
 * so there packet 0 leaves up to one SR shaper time late.
 */
uint64_t ipmx_sr_tx_rl_lead_ns(const struct ipmx_sr_tx* sr, enum mtl_session_port s_port,
                               const struct st_tx_video_rl_plan* plan);
/** -EIO without s->ipmx_sr; never blocks the transmitter. */
int ipmx_sr_tx_set_block_version(struct st_tx_video_session_impl* s, uint8_t version);
/** -EIO without s->ipmx_sr, -EINVAL on a malformed ts_refclk; NULL drops the override. */
int ipmx_sr_tx_set_ts_refclk(struct st_tx_video_session_impl* s, const char* ts_refclk);
/** -EIO without s->ipmx_sr, -EINVAL on NULL info. */
int ipmx_sr_tx_get_info(struct st_tx_video_session_impl* s, struct ipmx_video_info* info);
/** Copy and reset the per-leg stats, under the session lock the transmitter try_gets. */
void ipmx_sr_stat_take(struct ipmx_sr_tx* sr, struct ipmx_sr_stats* stats);
void ipmx_sr_stat_log(const struct ipmx_sr_stats* stats, int m_idx, int idx);

#ifdef __cplusplus
}
#endif

#endif
