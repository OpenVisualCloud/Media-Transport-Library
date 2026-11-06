/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_video_transmitter.h"

#include <math.h>

#include "../datapath/mt_queue.h"
#include "../mt_log.h"
#include "../mt_ptp.h"
#include "../mt_rtcp.h"
#include "st_err.h"
#include "st_tx_video_session.h"

static int video_trs_tasklet_start(void* priv) {
  struct st_video_transmitter_impl* trs = priv;
  int idx = trs->idx;

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int video_trs_tasklet_stop(void* priv) {
  struct st_video_transmitter_impl* trs = priv;
  int idx = trs->idx;

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static uint16_t video_trs_burst_fail(struct mtl_main_impl* impl,
                                     struct st_tx_video_session_impl* s,
                                     enum mtl_session_port s_port) {
  uint64_t cur_tsc = mt_get_tsc(impl);
  uint64_t fail_duration = cur_tsc - s->last_burst_succ_time_tsc[s_port];

  if (fail_duration > s->tx_hang_detect_time_thresh) {
    err("%s(%d,%d), hang duration %" PRIu64 " ms\n", __func__, s->idx, s_port,
        fail_duration / NS_PER_MS);
    s->tx_queue_recovery_pending[s_port] = true;
    s->last_burst_succ_time_tsc[s_port] = cur_tsc;
  }

  return 0;
}

static inline void video_trs_burst_succ(struct mtl_main_impl* impl,
                                        struct st_tx_video_session_impl* s,
                                        enum mtl_session_port s_port, uint16_t tx) {
  s->stat_pkts_burst += tx;
  s->last_burst_succ_time_tsc[s_port] = mt_get_tsc(impl);
}

static uint16_t video_trs_burst_pad(struct mtl_main_impl* impl,
                                    struct st_tx_video_session_impl* s,
                                    enum mtl_session_port s_port,
                                    struct rte_mbuf** tx_pkts, uint16_t nb_pkts) {
  uint16_t tx = mt_txq_burst(s->queue[s_port], tx_pkts, nb_pkts);
  if (!tx) return video_trs_burst_fail(impl, s, s_port);
  video_trs_burst_succ(impl, s, s_port, tx);
  return tx;
}

/* the pkts of a burst, read before it: packet 0's frame and each pkt's bytes */
struct video_trs_pkts {
  struct st_frame_trans* frame;
  uint32_t bytes[ST_TX_VIDEO_MAX_BULK];
};

/* before a burst of pkts (at most ST_TX_VIDEO_MAX_BULK): read them, and keep them for
 * RTCP */
static void video_trs_pkts_prepare(struct st_tx_video_session_impl* s,
                                   enum mtl_session_port s_port, struct rte_mbuf** pkts,
                                   uint16_t nb_pkts, struct video_trs_pkts* p) {
  p->frame = st_tx_mbuf_get_idx(pkts[0]) ? NULL : st_tx_mbuf_get_priv(pkts[0]);
  for (uint16_t i = 0; i < nb_pkts; i++) p->bytes[i] = pkts[i]->pkt_len;
  if (s->rtcp_tx[s_port]) mt_mbuf_refcnt_inc_bulk(pkts, nb_pkts);
}

/* after it sent the first tx of them: RTCP buffers those, packet 0 starts its frame, the
 * port counts them */
static void video_trs_pkts_sent(struct mtl_main_impl* impl,
                                struct st_tx_video_session_impl* s,
                                enum mtl_session_port s_port, struct rte_mbuf** pkts,
                                uint16_t nb_pkts, uint16_t tx,
                                const struct video_trs_pkts* p) {
  if (s->rtcp_tx[s_port]) {
    if (tx) mt_rtcp_tx_buffer_rtp_packets(s->rtcp_tx[s_port], pkts, tx);
    rte_pktmbuf_free_bulk(pkts, nb_pkts);
  }
  if (tx && p->frame) st20_frame_tx_start(impl, s, s_port, p->frame);
  for (uint16_t i = 0; i < RTE_MIN(tx, nb_pkts); i++) {
    s->port_user_stats.common.port[s_port].bytes += p->bytes[i];
    s->port_user_stats.common.port[s_port].packets++;
  }
}

/* for normal pkts, pad should call the video_trs_burst_pad */
static uint16_t video_trs_burst(struct mtl_main_impl* impl,
                                struct st_tx_video_session_impl* s,
                                enum mtl_session_port s_port, struct rte_mbuf** tx_pkts,
                                uint16_t nb_pkts) {
  struct video_trs_pkts p;

  video_trs_pkts_prepare(s, s_port, tx_pkts, nb_pkts, &p);
  uint16_t tx = mt_txq_burst(s->queue[s_port], tx_pkts, nb_pkts);
  if (!tx) {
    video_trs_pkts_sent(impl, s, s_port, tx_pkts, nb_pkts, 0, &p);
    return video_trs_burst_fail(impl, s, s_port);
  }
  video_trs_burst_succ(impl, s, s_port, tx);
  if (s->pacing_way[s_port] == ST21_TX_PACING_WAY_RL)
    s->trs_wire_tsc[s_port] =
        RTE_MAX(s->trs_wire_tsc[s_port], s->last_burst_succ_time_tsc[s_port]) +
        (uint64_t)(tx * s->pacing.trs);
  video_trs_pkts_sent(impl, s, s_port, tx_pkts, nb_pkts, tx, &p);
  return tx;
}

static inline int64_t video_trs_rl_pkts_needed(struct mtl_main_impl* impl,
                                               uint64_t target_tsc, long double trs) {
  uint64_t cur_tsc = mt_get_tsc(impl);
  /* Keep overdue targets negative. */
  int64_t delta = (int64_t)(target_tsc - cur_tsc);
  return (int64_t)((delta + trs - 1) / trs);
}

/* warm start for the first packet */
static void video_trs_rl_warm_up(struct mtl_main_impl* impl,
                                 struct st_tx_video_session_impl* s,
                                 enum mtl_session_port s_port) {
  struct st_tx_video_pacing* pacing = &s->pacing;
  uint64_t target_tsc = s->trs_target_tsc[s_port];
  struct rte_mbuf* pad = s->pad[s_port][ST20_PKT_TYPE_NORMAL];
  int64_t pkts_needed;
  unsigned int tx;
  int sent = 0;

  if (!target_tsc) {
    err("%s(%d), target_tsc is zero\n", __func__, s->idx);
    return;
  }

  pkts_needed = video_trs_rl_pkts_needed(impl, target_tsc, pacing->trs);
  if (pkts_needed < 0 || pkts_needed > pacing->warm_pkts) {
    dbg("%s(%d), mismatch timing with %ld\n", __func__, s->idx, pkts_needed);
    s->port_user_stats.stat_trans_troffset_mismatch++;
    return;
  }

  /* Each attempted pad consumes one interval from the bounded plan. */
  while (pkts_needed > 0) {
    rte_mbuf_refcnt_update(pad, 1);
    tx = video_trs_burst_pad(impl, s, s_port, &s->pad[s_port][ST20_PKT_TYPE_NORMAL], 1);
    if (tx < 1) s->trs_pad_inflight_num[s_port]++;
    sent++;

    int64_t remaining = video_trs_rl_pkts_needed(impl, target_tsc, pacing->trs);
    if (remaining < pkts_needed - 1) {
      s->port_user_stats.stat_trans_recalculate_warmup++;
      dbg("%s(%d), recalculated pkts_needed %ld -> %ld after %d pkts\n", __func__, s->idx,
          pkts_needed - 1, remaining, sent);
    }
    pkts_needed = RTE_MIN(pkts_needed - 1, remaining);
  }
}

/* the burst's first coarse pads become the coarse trim pad, the next fine ones the fine
 */
static void video_trs_rl_trim(struct st_tx_video_session_impl* s,
                              enum mtl_session_port s_port, struct rte_mbuf** pads,
                              const struct st_tx_video_rl_burst* b) {
  for (unsigned int i = 0; i < b->coarse + b->fine; i++)
    pads[i] = s->trim_pad[s_port][i < b->coarse ? ST_TX_VIDEO_RL_TRIM_PAD_COARSE
                                                : ST_TX_VIDEO_RL_TRIM_PAD_FINE];
}

/* one update per mbuf, as each same-mbuf atomic ahead of the doorbell delays packet 0 */
static inline void video_trs_rl_pads_ref(struct st_tx_video_session_impl* s,
                                         enum mtl_session_port s_port, int16_t coarse,
                                         int16_t fine, int16_t normal) {
  struct rte_mbuf** trim_pad = s->trim_pad[s_port];

  if (coarse) rte_mbuf_refcnt_update(trim_pad[ST_TX_VIDEO_RL_TRIM_PAD_COARSE], coarse);
  if (fine) rte_mbuf_refcnt_update(trim_pad[ST_TX_VIDEO_RL_TRIM_PAD_FINE], fine);
  if (normal) rte_mbuf_refcnt_update(s->pad[s_port][ST20_PKT_TYPE_NORMAL], normal);
}

/* An RL port at IDLE whose last burst found its NIC TX ring full, with packet 0 still in
 * its inflight: a pre-arm not deferred, or a packet 0 dequeued past its target. Refused
 * pads count as queued, so packet 0 is late by as much as their retry, in its visit.
 * Deferred and tail ports keep trs_target_tsc and wait in the scans instead. */
static inline bool video_trs_rl_refused(const struct st_tx_video_session_impl* s,
                                        int s_port) {
  int ret = s->stat_trs_ret_code[s_port];
  if (ret != -STI_RLTRS_BURST_INFLIGHT_FAIL && ret != -STI_RLTRS_BURST_PAD_INFLIGHT_FAIL)
    return false;
  if (s->pacing_way[s_port] != ST21_TX_PACING_WAY_RL ||
      s->rl_state[s_port] != ST_TX_VIDEO_RL_STATE_IDLE || s->trs_target_tsc[s_port] ||
      !s->trs_inflight_num[s_port])
    return false;
  return !st_tx_mbuf_get_idx(s->trs_inflight[s_port][s->trs_inflight_idx[s_port]]);
}

/* after a visit of s, for video_trs_rl_any_refused() */
static void video_trs_rl_refused_update(struct st_tx_video_session_impl* s) {
  uint64_t bit = UINT64_C(1) << s->idx;
  bool refused = false;

  for (int p = 0; p < s->ops.num_port; p++) refused |= video_trs_rl_refused(s, p);
  if (refused)
    s->mgr->trs_refused |= bit;
  else
    s->mgr->trs_refused &= ~bit;
}

/* A refused port in another session as of its last visit, unless its slot is held or
 * empty when read: reading that session would take the tasklet out of the slot of s */
static bool video_trs_rl_any_refused(const struct st_tx_video_session_impl* s) {
  struct st_tx_video_sessions_mgr* mgr = s->mgr;
  uint64_t refused = mgr->trs_refused & ~(UINT64_C(1) << s->idx);

  for (int i = 0; refused; i++, refused >>= 1) {
    if ((refused & 1) && mgr->sessions[i] && !rte_spinlock_is_locked(&mgr->mutex[i]))
      return true;
  }
  return false;
}

/* Queues NORMAL pads for packet 0 to launch behind, timed so that a shaper with the
 * plan's bucket ends them at the target within a fine trim; the real bucket is an aim
 * smaller, so the real shaper ends them an aim later. Returns false to hold packet 0
 * until its target. Else *defer says a stall within absorb_ns leaves packet 0 to go after
 * the visits, and trs_rl_tail that it goes with the last burst, sized from
 * trs_rl_drained. */
static bool video_trs_rl_pre_arm(struct mtl_main_impl* impl,
                                 struct st_tx_video_session_impl* s,
                                 enum mtl_session_port s_port, bool* defer) {
  struct st_tx_video_pacing* pacing = &s->pacing;
  const struct st_tx_video_rl_plan* plan = &s->rl_plan[s_port];
  uint64_t target_tsc = s->trs_target_tsc[s_port];
  long double drain = plan->pad_ns;
  long double credit = plan->bucket_ns;
  uint16_t pad_size = plan->pad_size;
  int64_t trim_pads = plan->trim_pads;
  struct rte_mbuf* pad = s->pad[s_port][ST20_PKT_TYPE_NORMAL];
  struct rte_mbuf* pads[ST_TX_VIDEO_RL_MAX_PADS_PER_BURST];
  struct st_tx_video_rl_burst b;
  int64_t gap;
  long double drained, stall_gap = 0;
  bool tail = false;
  unsigned int tx;
  int sent = 0;
  bool hold = false;

  s->trs_rl_tail[s_port] = false;
  if (!target_tsc) {
    err("%s(%d), target_tsc is zero\n", __func__, s->idx);
    return false;
  }

  gap = (int64_t)(target_tsc - mt_get_tsc(impl));
  /* the lower bound is video_trs_rl_warm_up()'s pkts_needed < 0 */
  if (gap > pacing->warm_pkts * pacing->trs || gap <= 1 - 2 * pacing->trs) {
    dbg("%s(%d), mismatch timing with gap %" PRId64 "\n", __func__, s->idx, gap);
    s->port_user_stats.stat_trans_troffset_mismatch++;
    return false;
  }
  if (gap <= 0) return false;

  /* when the shaper finishes what is queued: an idle one sends the plan's bucket, credit
   * ns, at once */
  drained = target_tsc - gap - credit;
  /* Late entry: too few pads fit to trim packet 0 onto its target. Within absorb_ns it
   * goes behind the last pads in one call, up to a pad late; else hold. */
  if (ceill((target_tsc - drained) / drain) < trim_pads) {
    dbg("%s(%d), late entry with gap %" PRId64 "\n", __func__, s->idx, gap);
    /* behind refused pads, its other port's or another session's, a call more than the
     * hold's would delay their retry */
    if (drain > plan->absorb_ns ||
        (s->ops.num_port > 1 && video_trs_rl_refused(s, !s_port)) ||
        video_trs_rl_any_refused(s))
      return false;
    tail = true;
  }

  /* Each burst is sized from a fresh tsc. Pads the NIC refuses are retried ahead of
   * packet 0, so counting them as queued now can only make the launch later. */
  while (true) {
    long double start = mt_get_tsc(impl) - credit;
    if (drained < start) {
      drained = start;
      if (sent) {
        s->port_user_stats.stat_trans_recalculate_warmup++;
        dbg("%s(%d), shaper idle after %d pkts\n", __func__, s->idx, sent);
        /* the rest no longer fits trim_pads: hold packet 0 behind the queued pads */
        if (ceill((target_tsc - drained) / drain) < trim_pads) {
          dbg("%s(%d), too few pads left, hold\n", __func__, s->idx);
          hold = true;
          break;
        }
      }
    }
    if (target_tsc <= drained) break;

    b = st_tx_video_rl_next_burst(drain, pad_size, trim_pads, target_tsc, drained);
    if (tail && !hold && b.last) break;
    /* the last pads cannot cut packet 0 onto its target: a late entry, tail within
     * absorb_ns, else hold */
    if (b.last && !hold) {
      long double cut_short =
          st_tx_video_rl_burst_ns(&b, drain, pad_size) - (target_tsc - drained);
      if (cut_short >= drain * ST_TX_VIDEO_RL_TRIM_FINE / pad_size) {
        hold = cut_short > plan->absorb_ns;
        tail = !hold;
        break;
      }
    }
    for (unsigned int i = 0; i < b.n; i++) pads[i] = pad;
    video_trs_rl_trim(s, s_port, pads, &b);
    video_trs_rl_pads_ref(s, s_port, (int16_t)b.coarse, (int16_t)b.fine,
                          (int16_t)(b.n - b.coarse - b.fine));
    tx = video_trs_burst_pad(impl, s, s_port, pads, b.n);
    /* a refused trim pad is retried as a NORMAL one: later, never early */
    if (tx < b.coarse + b.fine) {
      int16_t coarse = (int16_t)(b.coarse - RTE_MIN(b.coarse, tx));
      int16_t fine = (int16_t)(b.coarse + b.fine - RTE_MAX(b.coarse, tx));
      video_trs_rl_pads_ref(s, s_port, (int16_t)-coarse, (int16_t)-fine,
                            (int16_t)(coarse + fine));
    }
    s->trs_pad_inflight_num[s_port] += b.n - tx;

    /* A burst returning ST_TX_VIDEO_RL_STALL_NS past the plan stalled: its doorbell may
     * have found the shaper idle. Going on from the plan is never early and late by at
     * most the largest gap: a later gap the shaper still covers adds nothing. Past
     * absorb_ns plan on from the return, which never overfills, and hold packet 0,
     * which behind these pads could leave late or early. */
    long double latest = s->last_burst_succ_time_tsc[s_port] - credit;
    if (tx && drained + ST_TX_VIDEO_RL_STALL_NS < latest) {
      s->port_user_stats.stat_trans_recalculate_warmup++;
      dbg("%s(%d), burst stalled after %d pkts\n", __func__, s->idx, sent);
      stall_gap = RTE_MAX(stall_gap, latest - drained);
      if (stall_gap > plan->absorb_ns) {
        drained = latest;
        hold = true;
      } else if (!b.last) {
        /* the pads planned from drained may need a burst more than from the return:
         * the last one then goes with packet 0, so no visit makes more calls than the
         * hold's */
        long double burst = st_tx_video_rl_burst_ns(&b, drain, pad_size);
        tail = tail ||
               st_tx_video_rl_bursts(drain, trim_pads, target_tsc, drained + burst) >
                   st_tx_video_rl_bursts(drain, trim_pads, target_tsc, latest + burst);
      }
    }
    drained += st_tx_video_rl_burst_ns(&b, drain, pad_size);
    sent += (int)b.n;
    if (b.last) break;
  }

  /* a visit no longer than the hold's, whose packet 0 goes after the visits */
  *defer = !hold && stall_gap > 0 && s->last_burst_succ_time_tsc[s_port] < target_tsc;
  s->trs_rl_tail[s_port] = tail && !hold;
  s->trs_rl_drained[s_port] = drained;
  return !hold;
}

/* Bursts pkts with an RL pad after each due ST_SESSION_MAX_BULK slice, as such bulks do.
 * Stops at a short burst, padding its slice if due, or a refused pad: *new_slice. */
static unsigned int video_trs_rl_burst(struct mtl_main_impl* impl,
                                       struct st_tx_video_session_impl* s,
                                       enum mtl_session_port s_port,
                                       struct rte_mbuf** pkts, unsigned int n,
                                       bool* new_slice) {
  struct st_tx_video_pacing* pacing = &s->pacing;
  struct rte_mbuf** pad = &s->pad[s_port][ST20_PKT_TYPE_NORMAL];
  int pkt_idx = (int)st_tx_mbuf_get_idx(pkts[0]);
  /* only an RL bulk retries a tried slice, and its bulks are frame aligned */
  unsigned int q = *new_slice ? 0 : ST_SESSION_MAX_BULK - pkt_idx % ST_SESSION_MAX_BULK;
  unsigned int sent = 0;

  *new_slice = false;
  for (; q < n; q += ST_SESSION_MAX_BULK) {
    unsigned int k = RTE_MIN((unsigned int)ST_SESSION_MAX_BULK, n - q);
    int idx = pkt_idx + (int)q;
    if (fmodf((float)(idx + 1) + pacing->pad_interval / 2, pacing->pad_interval) <
        (float)k) {
      unsigned int nb = q + k - sent;
      unsigned int tx = video_trs_burst(impl, s, s_port, &pkts[sent], nb);
      sent += tx;
      if (sent < q) return sent;
      rte_mbuf_refcnt_update(*pad, 1);
      bool padded = video_trs_burst_pad(impl, s, s_port, pad, 1);
      if (!padded) s->trs_pad_inflight_num[s_port]++;
      if (tx < nb) return sent;
      if (!padded) {
        *new_slice = true;
        return sent;
      }
    }
  }
  if (sent < n) sent += video_trs_burst(impl, s, s_port, &pkts[sent], n - sent);
  return sent;
}

/* The last pre-arm burst, sized now from the drain end the pre-arm left, and the
 * inflight pkts of packet 0's slice behind it, in one call. Returns the pkts sent and
 * sets *pads_tx; refused pads retry ahead of the pkts. */
static uint16_t video_trs_rl_tail_burst(struct mtl_main_impl* impl,
                                        struct st_tx_video_session_impl* s,
                                        enum mtl_session_port s_port, uint64_t target_tsc,
                                        uint16_t* pads_tx) {
  const struct st_tx_video_rl_plan* plan = &s->rl_plan[s_port];
  struct rte_mbuf* tx_pkts[ST_TX_VIDEO_RL_MAX_PADS_PER_BURST + ST_SESSION_MAX_BULK];
  struct rte_mbuf** pkts = &s->trs_inflight[s_port][s->trs_inflight_idx[s_port]];
  /* a larger bulk's later slices go through video_trs_rl_burst() for their pads */
  uint16_t nb_pkts = RTE_MIN(s->trs_inflight_num[s_port], ST_SESSION_MAX_BULK);
  struct st_tx_video_rl_burst b = {0};
  struct video_trs_pkts p;

  s->trs_rl_tail[s_port] = false;
  long double start = (long double)mt_get_tsc(impl) - plan->bucket_ns;
  long double drained = RTE_MAX(s->trs_rl_drained[s_port], start);

  if (target_tsc > drained)
    b = st_tx_video_rl_next_burst(plan->pad_ns, plan->pad_size, plan->trim_pads,
                                  target_tsc, drained);
  for (unsigned int i = 0; i < b.n; i++)
    tx_pkts[i] = s->pad[s_port][ST20_PKT_TYPE_NORMAL];
  video_trs_rl_trim(s, s_port, tx_pkts, &b);
  video_trs_rl_pads_ref(s, s_port, (int16_t)b.coarse, (int16_t)b.fine,
                        (int16_t)(b.n - b.coarse - b.fine));
  for (uint16_t i = 0; i < nb_pkts; i++) tx_pkts[b.n + i] = pkts[i];
  video_trs_pkts_prepare(s, s_port, pkts, nb_pkts, &p);

  uint16_t tx = mt_txq_burst(s->queue[s_port], tx_pkts, b.n + nb_pkts);
  *pads_tx = RTE_MIN(tx, b.n);
  uint16_t pkts_tx = tx - *pads_tx;
  if (*pads_tx < b.coarse + b.fine) {
    int16_t coarse = (int16_t)(b.coarse - RTE_MIN(b.coarse, *pads_tx));
    int16_t fine = (int16_t)(b.coarse + b.fine - RTE_MAX(b.coarse, *pads_tx));
    video_trs_rl_pads_ref(s, s_port, (int16_t)-coarse, (int16_t)-fine,
                          (int16_t)(coarse + fine));
  }
  if (!tx) {
    /* A full ring took none: drop the pads and hold packet 0 to its target, which costs
     * no call until then, as the hold. */
    video_trs_rl_pads_ref(s, s_port, 0, 0, (int16_t)-b.n);
    s->rl_state[s_port] = ST_TX_VIDEO_RL_STATE_WAIT_TARGET;
    video_trs_pkts_sent(impl, s, s_port, pkts, nb_pkts, 0, &p);
    video_trs_burst_fail(impl, s, s_port);
    return 0;
  }
  s->trs_pad_inflight_num[s_port] += b.n - *pads_tx;
  video_trs_burst_succ(impl, s, s_port, tx);
  /* a stall before the doorbell, as video_trs_rl_pre_arm() counts one */
  long double latest = s->last_burst_succ_time_tsc[s_port] - plan->bucket_ns;
  if (*pads_tx && drained + ST_TX_VIDEO_RL_STALL_NS < latest)
    s->port_user_stats.stat_trans_recalculate_warmup++;
  if (pkts_tx == nb_pkts) s->trs_inflight_new_slice[s_port] = true;
  video_trs_pkts_sent(impl, s, s_port, pkts, nb_pkts, pkts_tx, &p);
  return pkts_tx;
}

static int video_burst_packet(struct mtl_main_impl* impl,
                              struct st_tx_video_session_impl* s,
                              enum mtl_session_port s_port, struct rte_mbuf** pkts,
                              int bulk, bool use_two) {
  bool new_slice = true;
  int tx = (int)video_trs_rl_burst(impl, s, s_port, pkts, bulk, &new_slice);

  if (tx < bulk) {
    unsigned int i;
    unsigned int remaining = bulk - tx;

    if (!use_two) {
      s->trs_inflight_num[s_port] = remaining;
      s->trs_inflight_idx[s_port] = 0;
      s->trs_inflight_new_slice[s_port] = new_slice;
      s->trs_inflight_cnt[s_port]++;
      for (i = 0; i < remaining; i++) s->trs_inflight[s_port][i] = pkts[tx + i];
    } else {
      s->trs_inflight_num2[s_port] = remaining;
      s->trs_inflight_idx2[s_port] = 0;
      s->trs_inflight_cnt2[s_port]++;
      for (i = 0; i < remaining; i++) s->trs_inflight2[s_port][i] = pkts[tx + i];
    }
  }

  return 0;
}

static bool video_trs_rl_target_reached(struct mtl_main_impl* impl,
                                        struct st_tx_video_session_impl* s,
                                        uint64_t target_tsc, int* ret_status,
                                        int* tasklet_ret) {
  uint64_t cur_tsc = mt_get_tsc(impl);
  if (cur_tsc >= target_tsc) return true;

  uint64_t delta = target_tsc - cur_tsc;
  if (likely(delta <= NS_PER_S)) {
    *ret_status = -STI_RLTRS_TARGET_TSC_NOT_REACH;
    *tasklet_ret =
        delta < mt_sch_schedule_ns(impl) ? MTL_TASKLET_HAS_PENDING : MTL_TASKLET_ALL_DONE;
  } else {
    err("%s(%d), invalid trs tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, s->idx,
        cur_tsc, target_tsc);
    *ret_status = -STI_RLTRS_TARGET_TSC_NOT_REACH;
    *tasklet_ret = MTL_TASKLET_ALL_DONE;
  }
  return false;
}

static inline void video_trs_rl_set_idle(struct st_tx_video_session_impl* s,
                                         enum mtl_session_port s_port) {
  s->rl_state[s_port] = ST_TX_VIDEO_RL_STATE_IDLE;
  s->trs_target_tsc[s_port] = 0;
}

/* Frees one done rs batch of the frame before, so the warm-up pad bursts need not. A
 * batch holds at least rs_thresh / 2 pkts, each a NORMAL pkt's shaper time but the one
 * FRAME_TAIL, so a call frees at most one a batch time; behind by up to the ring's
 * batches, it catches up one a call. */
static inline void video_trs_rl_reclaim(struct mtl_main_impl* impl,
                                        struct st_tx_video_session_impl* s,
                                        enum mtl_session_port s_port) {
  enum mtl_port port = mt_port_logic2phy(s->port_maps, s_port);
  uint16_t rs = mt_if_tx_reclaim_rs_thresh(impl, port);
  if (!rs) return;

  uint64_t now = mt_get_tsc(impl);
  uint64_t next = s->trs_reclaim_tsc[s_port];
  if (now < next) return;
  mt_txq_reclaim(s->queue[s_port]);
  const struct st_tx_video_rl_plan* plan = &s->rl_plan[s_port];
  uint64_t batch_ns = (uint64_t)(rs / 2 * (plan->armed ? plan->pad_ns : s->pacing.trs));
  uint64_t behind = (uint64_t)mt_if_nb_tx_desc(impl, port) / rs * batch_ns;
  s->trs_reclaim_tsc[s_port] = RTE_MAX(next, now > behind ? now - behind : 0) + batch_ns;
}

/* The state a port goes to once the warm-up window of packet 0 opens. Without a plan it
 * sends the warm-up pads and waits for the target. With one it pre-arms the shaper,
 * which then releases packet 0, unless the pre-arm fails and it waits for the target. */
static enum st_tx_video_rl_state video_trs_rl_frame_start(
    struct mtl_main_impl* impl, struct st_tx_video_session_impl* s,
    enum mtl_session_port s_port, bool* defer) {
  if (!s->rl_plan[s_port].armed) {
    video_trs_rl_warm_up(impl, s, s_port);
    return ST_TX_VIDEO_RL_STATE_WAIT_TARGET;
  }
  if (!video_trs_rl_pre_arm(impl, s, s_port, defer))
    return ST_TX_VIDEO_RL_STATE_WAIT_TARGET;
  /* a deferred packet 0, or one behind tail pads, keeps it for the call that sends it */
  if (!*defer && !s->trs_rl_tail[s_port]) s->trs_target_tsc[s_port] = 0;
  return ST_TX_VIDEO_RL_STATE_IDLE;
}

static int _video_trs_rl_tasklet(struct mtl_main_impl* impl,
                                 struct st_tx_video_session_impl* s,
                                 enum mtl_session_port s_port, int* ret_status) {
  unsigned int bulk = s->bulk;
  struct rte_ring* ring = s->ring[s_port];
  int idx = s->idx;
  unsigned int n, tx;
  uint32_t pkt_idx = 0;

  /* check if any inflight pkts in transmitter inflight 2 */
  if (s->trs_inflight_num2[s_port] > 0) {
    tx = video_trs_burst(impl, s, s_port,
                         &s->trs_inflight2[s_port][s->trs_inflight_idx2[s_port]],
                         s->trs_inflight_num2[s_port]);
    s->trs_inflight_num2[s_port] -= tx;
    s->trs_inflight_idx2[s_port] += tx;
    if (tx > 0) {
      *ret_status = 1;
      return MTL_TASKLET_HAS_PENDING;
    } else {
      *ret_status = -STI_RLTRS_BURST_INFLIGHT2_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  uint64_t target_tsc = s->trs_target_tsc[s_port];
  if (s->rl_state[s_port] == ST_TX_VIDEO_RL_STATE_WAIT_WARMUP) {
    uint64_t warmup_ns = s->pacing.warm_pkts * s->pacing.trs;
    uint64_t warmup_tsc = target_tsc > warmup_ns ? target_tsc - warmup_ns : 0;
    int tasklet_ret;
    if (!video_trs_rl_target_reached(impl, s, warmup_tsc, ret_status, &tasklet_ret)) {
      video_trs_rl_reclaim(impl, s, s_port);
      return tasklet_ret;
    }
    bool defer = false;
    s->rl_state[s_port] = video_trs_rl_frame_start(impl, s, s_port, &defer);
    /* packet 0 goes after the visits, behind the pads, which end at its target; a visit
     * carried past the target after the last pad doorbell sends it now, as a release */
    if (defer && mt_get_tsc(impl) < target_tsc) return MTL_TASKLET_HAS_PENDING;
  }

  /* check if any padding inflight pkts in transmitter */
  if (s->trs_pad_inflight_num[s_port] > 0) {
    struct rte_mbuf* pads[ST_TX_VIDEO_RL_MAX_PADS_PER_BURST];
    n = s->rl_plan[s_port].armed
            ? RTE_MIN(s->trs_pad_inflight_num[s_port], ST_TX_VIDEO_RL_MAX_PADS_PER_BURST)
            : 1;
    dbg("%s(%d), inflight padding pkts %d\n", __func__, idx,
        s->trs_pad_inflight_num[s_port]);
    for (unsigned int i = 0; i < n; i++) pads[i] = s->pad[s_port][ST20_PKT_TYPE_NORMAL];
    tx = video_trs_burst_pad(impl, s, s_port, pads, n);
    s->trs_pad_inflight_num[s_port] -= tx;
    if (tx > 0) {
      *ret_status = 1;
      return MTL_TASKLET_HAS_PENDING;
    } else {
      *ret_status = -STI_RLTRS_BURST_PAD_INFLIGHT_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  if (s->rl_state[s_port] == ST_TX_VIDEO_RL_STATE_WAIT_TARGET) {
    int tasklet_ret;
    if (!video_trs_rl_target_reached(impl, s, target_tsc, ret_status, &tasklet_ret))
      return tasklet_ret;
    video_trs_rl_set_idle(s, s_port);
  }

  /* check if any inflight pkts in transmitter */
  if (s->trs_inflight_num[s_port] > 0) {
    struct rte_mbuf** inflight = &s->trs_inflight[s_port][s->trs_inflight_idx[s_port]];
    uint16_t pads_tx = 0;
    if (s->trs_rl_tail[s_port])
      tx = video_trs_rl_tail_burst(impl, s, s_port, s->trs_target_tsc[s_port], &pads_tx);
    /* up to ST_SESSION_MAX_BULK it is the rest of one slice, its pad decided */
    else if (bulk > ST_SESSION_MAX_BULK)
      tx = video_trs_rl_burst(impl, s, s_port, inflight, s->trs_inflight_num[s_port],
                              &s->trs_inflight_new_slice[s_port]);
    else
      tx = video_trs_burst(impl, s, s_port, inflight, s->trs_inflight_num[s_port]);
    /* a target is kept only while packet 0 heads the inflight pkts */
    if (tx && target_tsc) {
      uint64_t back = s->last_burst_succ_time_tsc[s_port];
      s->trs_target_tsc[s_port] = 0;
      s->trs_wire_tsc[s_port] =
          RTE_MAX(s->trs_wire_tsc[s_port],
                  RTE_MAX(target_tsc, back) + (uint64_t)(tx * s->pacing.trs));
    }
    s->trs_inflight_num[s_port] -= tx;
    s->trs_inflight_idx[s_port] += tx;
    /* pads alone the NIC took ahead of packet 0 are work too */
    if (tx > 0 || pads_tx > 0) {
      *ret_status = 1;
      return MTL_TASKLET_HAS_PENDING;
    } else {
      *ret_status = -STI_RLTRS_BURST_INFLIGHT_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  /* dequeue from ring */
  struct rte_mbuf* pkts[ST_TX_VIDEO_MAX_BULK] = {NULL};
  n = mt_rte_ring_sc_dequeue_bulk(ring, (void**)&pkts[0], bulk, NULL);
  if (n == 0) {
    video_trs_rl_reclaim(impl, s, s_port);
    *ret_status = -STI_RLTRS_DEQUEUE_FAIL;
    return MTL_TASKLET_ALL_DONE;
  }

  int valid_bulk = bulk;
  for (int i = 0; i < bulk; i++) {
    pkt_idx = st_tx_mbuf_get_idx(pkts[i]);
    if ((pkt_idx == 0) || (pkt_idx == ST_TX_DUMMY_PKT_IDX)) {
      valid_bulk = i;
      break; /* break if it's the first pkt of frame or it's the start of dummy */
    }
  }
  dbg("%s(%d), pkt_idx %u valid_bulk %d\n", __func__, idx, pkt_idx, valid_bulk);

  /* builder always build bulk pkts per enqueue, pkts after dummy are all dummy */
  if (unlikely(pkt_idx == ST_TX_DUMMY_PKT_IDX)) {
    video_burst_packet(impl, s, s_port, pkts, valid_bulk, false);
    rte_pktmbuf_free_bulk(&pkts[valid_bulk], bulk - valid_bulk);
    s->stat_pkts_burst_dummy += bulk - valid_bulk;
    dbg("%s(%d), pkt_idx %" PRIu64 "\n", __func__, idx, (uint64_t)pkt_idx);
    *ret_status = -STI_RLTRS_BURST_HAS_DUMMY;
    return MTL_TASKLET_HAS_PENDING;
  }

  if (unlikely(!pkt_idx)) {
    uint64_t cur_tsc = mt_get_tsc(impl);
    if (valid_bulk != 0) {
      video_burst_packet(impl, s, s_port, pkts, valid_bulk, true);
    }
    uint64_t target_tsc = st_tx_mbuf_get_tsc(pkts[valid_bulk]);
    dbg("%s(%d), first pkt, ts cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
        cur_tsc, target_tsc);
    if (likely(cur_tsc < target_tsc || s->trs_inflight_num2[s_port])) {
      unsigned int i;
      /* Keep an already-overdue frame pending. */
      int64_t delta = (int64_t)(target_tsc - cur_tsc);

      /* A partial pre-boundary burst populates inflight2 in this invocation.
       * Retain the new frame until those older packets are retried. */
      if (likely(delta <= (int64_t)NS_PER_S || s->trs_inflight_num2[s_port])) {
        s->trs_target_tsc[s_port] = target_tsc;
        s->rl_state[s_port] = ST_TX_VIDEO_RL_STATE_WAIT_WARMUP;
        /* save it on inflight, its first slice tried: no pad after it */
        s->trs_inflight_num[s_port] = bulk - valid_bulk;
        s->trs_inflight_idx[s_port] = 0;
        s->trs_inflight_new_slice[s_port] = false;
        s->trs_inflight_cnt[s_port]++;
        for (i = 0; i < bulk - valid_bulk; i++)
          s->trs_inflight[s_port][i] = pkts[i + valid_bulk];
        *ret_status = -STI_RLTRS_1ST_PKT_TSC;
        return delta < (int64_t)mt_sch_schedule_ns(impl) ? MTL_TASKLET_HAS_PENDING
                                                         : MTL_TASKLET_ALL_DONE;
      } else {
        err("%s(%d), invalid tsc for first pkt cur %" PRIu64 " target %" PRIu64 "\n",
            __func__, idx, cur_tsc, target_tsc);
      }
    } else {
      s->trs_target_tsc[s_port] = target_tsc;
      video_trs_rl_warm_up(impl, s, s_port);
      video_trs_rl_set_idle(s, s_port);
    }
  }

  int pos = (valid_bulk == bulk) ? 0 : valid_bulk;

  video_burst_packet(impl, s, s_port, &pkts[pos], bulk - pos, false);

  *ret_status = 1;
  return MTL_TASKLET_HAS_PENDING;
}

static int video_trs_rl_tasklet(struct mtl_main_impl* impl,
                                struct st_tx_video_session_impl* s,
                                enum mtl_session_port s_port) {
  int pending = MTL_TASKLET_ALL_DONE;
  int ret_status = 0;

  pending += _video_trs_rl_tasklet(impl, s, s_port, &ret_status);
  /*
   * Try to burst pkts again for the performance, in this way nic tx get double
   * bulk since tx pkt is in critical path
   */
  if (ret_status > 0) {
    ret_status = 0;
    pending += _video_trs_rl_tasklet(impl, s, s_port, &ret_status);
  }
  s->stat_trs_ret_code[s_port] = ret_status;
  return pending;
}

static int video_trs_tsc_tasklet(struct mtl_main_impl* impl,
                                 struct st_tx_video_session_impl* s,
                                 enum mtl_session_port s_port) {
  unsigned int bulk = s->bulk;
  if (s->pacing_way[s_port] == ST21_TX_PACING_WAY_BE) bulk = 1;
  struct rte_ring* ring = s->ring[s_port];
  int idx = s->idx, tx;
  unsigned int n;
  uint64_t target_tsc, cur_tsc;

  /* check if it's pending on the tsc */
  target_tsc = s->trs_target_tsc[s_port];
  if (target_tsc) {
    cur_tsc = mt_get_tsc(impl);
    if (cur_tsc < target_tsc) {
      uint64_t delta = target_tsc - cur_tsc;
      if (likely(delta <= NS_PER_S)) {
        s->stat_trs_ret_code[s_port] = -STI_TSCTRS_TARGET_TSC_NOT_REACH;
        return delta < mt_sch_schedule_ns(impl) ? MTL_TASKLET_HAS_PENDING
                                                : MTL_TASKLET_ALL_DONE;
      } else {
        err("%s(%d), invalid trs tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
            cur_tsc, target_tsc);
      }
    }
    s->trs_target_tsc[s_port] = 0;
  }

  /* check if any inflight pkts in transmitter */
  if (s->trs_inflight_num[s_port] > 0) {
    tx = video_trs_burst(impl, s, s_port,
                         &s->trs_inflight[s_port][s->trs_inflight_idx[s_port]],
                         s->trs_inflight_num[s_port]);
    s->trs_inflight_num[s_port] -= tx;
    s->trs_inflight_idx[s_port] += tx;
    if (tx > 0) {
      return MTL_TASKLET_HAS_PENDING;
    } else {
      s->stat_trs_ret_code[s_port] = -STI_TSCTRS_BURST_INFLIGHT_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  /* dequeue from ring */
  struct rte_mbuf* pkts[ST_TX_VIDEO_MAX_BULK] = {NULL};
  n = mt_rte_ring_sc_dequeue_bulk(ring, (void**)&pkts[0], bulk, NULL);
  if (n == 0) {
    s->stat_trs_ret_code[s_port] = -STI_TSCTRS_DEQUEUE_FAIL;
    return MTL_TASKLET_ALL_DONE;
  }

  /* check valid bulk */
  int valid_bulk = bulk;
  uint32_t pkt_idx = 0;
  for (int i = 0; i < bulk; i++) {
    pkt_idx = st_tx_mbuf_get_idx(pkts[i]);
    if (pkt_idx == ST_TX_DUMMY_PKT_IDX) {
      valid_bulk = i;
      break;
    }
  }

  if (unlikely(pkt_idx == ST_TX_DUMMY_PKT_IDX)) {
    rte_pktmbuf_free_bulk(&pkts[valid_bulk], bulk - valid_bulk);
    s->stat_pkts_burst_dummy += bulk - valid_bulk;
    s->stat_trs_ret_code[s_port] = -STI_TSCTRS_BURST_HAS_DUMMY;
  }

  if (s->pacing_way[s_port] != ST21_TX_PACING_WAY_BE || pkt_idx == 0) {
    cur_tsc = mt_get_tsc(impl);
    target_tsc = st_tx_mbuf_get_tsc(pkts[0]);
    if (cur_tsc < target_tsc) {
      unsigned int i;
      uint64_t delta = target_tsc - cur_tsc;

      if (likely(delta <= NS_PER_S)) {
        s->trs_target_tsc[s_port] = target_tsc;
        /* save it on inflight */
        s->trs_inflight_num[s_port] = valid_bulk;
        s->trs_inflight_idx[s_port] = 0;
        s->trs_inflight_cnt[s_port]++;
        for (i = 0; i < valid_bulk; i++) s->trs_inflight[s_port][i] = pkts[i];
        s->stat_trs_ret_code[s_port] = -STI_TSCTRS_TARGET_TSC_NOT_REACH;
        return delta < mt_sch_schedule_ns(impl) ? MTL_TASKLET_HAS_PENDING
                                                : MTL_TASKLET_ALL_DONE;
      } else {
        err("%s(%d), invalid tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
            cur_tsc, target_tsc);
      }
    }
  }

  tx = video_trs_burst(impl, s, s_port, &pkts[0], valid_bulk);

  if (tx < valid_bulk) {
    unsigned int i;
    unsigned int remaining = valid_bulk - tx;

    s->trs_inflight_num[s_port] = remaining;
    s->trs_inflight_idx[s_port] = 0;
    s->trs_inflight_cnt[s_port]++;
    for (i = 0; i < remaining; i++) s->trs_inflight[s_port][i] = pkts[tx + i];
  }

  return MTL_TASKLET_HAS_PENDING;
}

static int video_trs_launch_time_tasklet(struct mtl_main_impl* impl,
                                         struct st_tx_video_session_impl* s,
                                         enum mtl_session_port s_port) {
  unsigned int bulk = s->bulk;
  struct rte_ring* ring = s->ring[s_port];
  int tx = 0;
  unsigned int n;
  uint64_t i;
  uint64_t target_ptp;
  enum mtl_port port = mt_port_logic2phy(s->port_maps, s_port);
  struct mt_interface* inf = mt_if(impl, port);

  /* check if any inflight pkts in transmitter */
  if (s->trs_inflight_num[s_port] > 0) {
    tx = video_trs_burst(impl, s, s_port,
                         &s->trs_inflight[s_port][s->trs_inflight_idx[s_port]],
                         s->trs_inflight_num[s_port]);

    s->trs_inflight_num[s_port] -= tx;
    s->trs_inflight_idx[s_port] += tx;

    if (tx > 0) {
      return MTL_TASKLET_HAS_PENDING;
    } else {
      s->stat_trs_ret_code[s_port] = -STI_TSCTRS_BURST_INFLIGHT_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  /* dequeue from ring */
  struct rte_mbuf* pkts[ST_TX_VIDEO_MAX_BULK] = {NULL};
  n = mt_rte_ring_sc_dequeue_bulk(ring, (void**)&pkts[0], bulk, NULL);
  if (n == 0) {
    s->stat_trs_ret_code[s_port] = -STI_TSCTRS_DEQUEUE_FAIL;
    return MTL_TASKLET_ALL_DONE;
  }

  /* check valid bulk */
  int valid_bulk = bulk;
  uint32_t pkt_idx;
  for (i = 0; i < bulk; i++) {
    pkt_idx = st_tx_mbuf_get_idx(pkts[i]);
    if (pkt_idx == ST_TX_DUMMY_PKT_IDX) {
      valid_bulk = i;
      break;
    }
  }

  if (unlikely(pkt_idx == ST_TX_DUMMY_PKT_IDX)) {
    rte_pktmbuf_free_bulk(&pkts[valid_bulk], bulk - valid_bulk);
  }

  if (valid_bulk > 0) {
    for (i = 0; i < valid_bulk; i++) {
      target_ptp = st_tx_mbuf_get_ptp(pkts[i]);
      /* Put tx timestamp into transmit descriptor */
      pkts[i]->ol_flags |= inf->tx_launch_time_flag;
      *RTE_MBUF_DYNFIELD(pkts[i], inf->tx_dynfield_offset, uint64_t*) = target_ptp;
    }

    tx = video_trs_burst(impl, s, s_port, &pkts[0], valid_bulk);

    if (tx < valid_bulk) {
      unsigned int remaining = valid_bulk - tx;

      s->trs_inflight_num[s_port] = remaining;
      s->trs_inflight_idx[s_port] = 0;
      s->trs_inflight_cnt[s_port]++;
      for (i = 0; i < remaining; i++) s->trs_inflight[s_port][i] = pkts[tx + i];
    }
  }

  if (unlikely(pkt_idx == ST_TX_DUMMY_PKT_IDX)) {
    s->stat_pkts_burst_dummy += bulk - valid_bulk;
    s->stat_trs_ret_code[s_port] = -STI_TSCTRS_BURST_HAS_DUMMY;
    return MTL_TASKLET_ALL_DONE;
  } else {
    return MTL_TASKLET_HAS_PENDING;
  }
}

static int video_trs_ptp_tasklet(struct mtl_main_impl* impl,
                                 struct st_tx_video_session_impl* s,
                                 enum mtl_session_port s_port) {
  unsigned int bulk = s->bulk;
  struct rte_ring* ring = s->ring[s_port];
  int idx = s->idx, tx;
  unsigned int n;
  uint64_t target_ptp, cur_ptp;

  /* check if it's pending on the tsc */
  target_ptp = s->trs_target_tsc[s_port];
  if (target_ptp) {
    cur_ptp = mt_get_ptp_time(impl, MTL_PORT_P);
    if (cur_ptp < target_ptp) {
      uint64_t delta = target_ptp - cur_ptp;
      if (likely(delta <= NS_PER_S)) {
        s->stat_trs_ret_code[s_port] = -STI_TSCTRS_TARGET_TSC_NOT_REACH;
        return delta < mt_sch_schedule_ns(impl) ? MTL_TASKLET_HAS_PENDING
                                                : MTL_TASKLET_ALL_DONE;
      } else {
        err("%s(%d), invalid trs tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
            cur_ptp, target_ptp);
      }
    }
    s->trs_target_tsc[s_port] = 0;
  }

  /* check if any inflight pkts in transmitter */
  if (s->trs_inflight_num[s_port] > 0) {
    tx = video_trs_burst(impl, s, s_port,
                         &s->trs_inflight[s_port][s->trs_inflight_idx[s_port]],
                         s->trs_inflight_num[s_port]);
    s->trs_inflight_num[s_port] -= tx;
    s->trs_inflight_idx[s_port] += tx;
    if (tx > 0) {
      return MTL_TASKLET_HAS_PENDING;
    } else {
      s->stat_trs_ret_code[s_port] = -STI_TSCTRS_BURST_INFLIGHT_FAIL;
      return MTL_TASKLET_ALL_DONE;
    }
  }

  /* dequeue from ring */
  struct rte_mbuf* pkts[ST_TX_VIDEO_MAX_BULK] = {NULL};
  n = mt_rte_ring_sc_dequeue_bulk(ring, (void**)&pkts[0], bulk, NULL);
  if (n == 0) {
    s->stat_trs_ret_code[s_port] = -STI_TSCTRS_DEQUEUE_FAIL;
    return MTL_TASKLET_ALL_DONE;
  }

  /* check valid bulk */
  int valid_bulk = bulk;
  uint32_t pkt_idx = 0;
  for (int i = 0; i < bulk; i++) {
    pkt_idx = st_tx_mbuf_get_idx(pkts[i]);
    if (pkt_idx == ST_TX_DUMMY_PKT_IDX) {
      valid_bulk = i;
      break;
    }
  }

  if (unlikely(pkt_idx == ST_TX_DUMMY_PKT_IDX)) {
    rte_pktmbuf_free_bulk(&pkts[valid_bulk], bulk - valid_bulk);
    s->stat_pkts_burst_dummy += bulk - valid_bulk;
    s->stat_trs_ret_code[s_port] = -STI_TSCTRS_BURST_HAS_DUMMY;
  }

  cur_ptp = mt_get_ptp_time(impl, MTL_PORT_P);
  target_ptp = st_tx_mbuf_get_ptp(pkts[0]);
  if (cur_ptp < target_ptp) {
    unsigned int i;
    uint64_t delta = target_ptp - cur_ptp;

    if (likely(delta <= NS_PER_S)) {
      s->trs_target_tsc[s_port] = target_ptp;
      /* save it on inflight */
      s->trs_inflight_num[s_port] = valid_bulk;
      s->trs_inflight_idx[s_port] = 0;
      s->trs_inflight_cnt[s_port]++;
      for (i = 0; i < valid_bulk; i++) s->trs_inflight[s_port][i] = pkts[i];
      s->stat_trs_ret_code[s_port] = -STI_TSCTRS_TARGET_TSC_NOT_REACH;
      return delta < mt_sch_schedule_ns(impl) ? MTL_TASKLET_HAS_PENDING
                                              : MTL_TASKLET_ALL_DONE;
    } else {
      err("%s(%d), invalid tsc cur %" PRIu64 " target %" PRIu64 "\n", __func__, idx,
          cur_ptp, target_ptp);
    }
  }

  tx = video_trs_burst(impl, s, s_port, &pkts[0], valid_bulk);

  if (tx < valid_bulk) {
    unsigned int i;
    unsigned int remaining = valid_bulk - tx;

    s->trs_inflight_num[s_port] = remaining;
    s->trs_inflight_idx[s_port] = 0;
    s->trs_inflight_cnt[s_port]++;
    for (i = 0; i < remaining; i++) s->trs_inflight[s_port][i] = pkts[tx + i];
  }

  return MTL_TASKLET_HAS_PENDING;
}

/* the target of the packet 0 an RL port holds, or deferred behind its pads, 0 if none */
static inline uint64_t video_trs_rl_pkt0_tsc(const struct st_tx_video_session_impl* s,
                                             int s_port) {
  if (s->pacing_way[s_port] != ST21_TX_PACING_WAY_RL ||
      s->rl_state[s_port] == ST_TX_VIDEO_RL_STATE_WAIT_WARMUP)
    return 0;
  return s->trs_target_tsc[s_port];
}

static inline uint64_t video_trs_rl_pkt0_first(uint64_t a, uint64_t b) {
  return a && (!b || a < b) ? a : b;
}

/* packets 0 past their target, and with pkt0_deferred those behind their pads; once a
 * call */
static int video_trs_rl_release(struct mtl_main_impl* impl,
                                struct st_video_transmitter_impl* trs,
                                bool pkt0_deferred) {
  struct st_tx_video_sessions_mgr* mgr = trs->mgr;
  int pending = MTL_TASKLET_ALL_DONE;
  uint64_t now, next = 0;
  bool blocked = false;

  if (!trs->pkt0_tsc) return pending;
  now = mt_get_tsc(impl);
  if (now < trs->pkt0_tsc && !pkt0_deferred) return pending;
  for (int sidx = 0; sidx < mgr->max_idx; sidx++) {
    struct st_tx_video_session_impl* s = tx_video_session_dp_get(mgr, sidx);
    if (!s) {
      mgr->trs_refused &= ~(UINT64_C(1) << sidx);
      continue;
    }
    for (int s_port = 0; s_port < s->ops.num_port; s_port++) {
      uint64_t target = video_trs_rl_pkt0_tsc(s, s_port);
      bool queued = pkt0_deferred && s->rl_state[s_port] == ST_TX_VIDEO_RL_STATE_IDLE;
      /* Nothing of the ports after a refused one goes ahead of its retry; a port the NIC
       * never drains blocks them until hang detection recovers it, within
       * tx_hang_detect_time_thresh (1 s). A blocked due port stays in next, for the scan
       * after that retry. */
      blocked |= video_trs_rl_refused(s, s_port);
      if ((target > now && !queued) || (target && blocked))
        next = video_trs_rl_pkt0_first(next, target);
      else if (target && s->trs_released[s_port] != trs->visit && s->queue[s_port] &&
               !s->tx_queue_recovery_pending[s_port]) {
        s->trs_released[s_port] = trs->visit;
        pending += s->pacing_tasklet_func[s_port](impl, s, s_port);
        /* the scan's own sends may carry it past the targets of the ports after */
        now = mt_get_tsc(impl);
        blocked |= video_trs_rl_refused(s, s_port);
        video_trs_rl_refused_update(s);
      }
    }
  }
  trs->pkt0_tsc = next;
  return pending;
}

/* An RL port mid-frame whose last call sent pkts and whose NIC TX queue holds less than
 * the scheduler may be away, mt_sch_schedule_ns(). */
static bool video_trs_rl_short(struct mtl_main_impl* impl,
                               struct st_tx_video_session_impl* s, int s_port) {
  if (s->pacing_way[s_port] != ST21_TX_PACING_WAY_RL || s->stat_trs_ret_code[s_port] <= 0)
    return false;
  if (s->rl_state[s_port] != ST_TX_VIDEO_RL_STATE_IDLE || s->trs_target_tsc[s_port])
    return false;
  return s->trs_wire_tsc[s_port] <
         s->last_burst_succ_time_tsc[s_port] + mt_sch_schedule_ns(impl);
}

/* Tops the short ports up a bulk each in turn, so none waits on another's whole
 * catch-up, until each holds mt_sch_schedule_ns() or its NIC or builder ring stops it. */
static int video_trs_rl_catch_up(struct mtl_main_impl* impl,
                                 struct st_tx_video_sessions_mgr* mgr) {
  int pending = MTL_TASKLET_ALL_DONE;
  bool more = true;

  while (more) {
    bool blocked = false;
    more = false;
    for (int sidx = 0; sidx < mgr->max_idx; sidx++) {
      struct st_tx_video_session_impl* s = tx_video_session_dp_get(mgr, sidx);
      if (!s) {
        mgr->trs_refused &= ~(UINT64_C(1) << sidx);
        continue;
      }
      for (int s_port = 0; s_port < s->ops.num_port; s_port++) {
        /* nothing of the ports after a refused one, as in the release scans */
        blocked |= video_trs_rl_refused(s, s_port);
        if (blocked || !s->queue[s_port] || s->tx_queue_recovery_pending[s_port] ||
            !video_trs_rl_short(impl, s, s_port))
          continue;
        int ret_status = 0;
        pending += _video_trs_rl_tasklet(impl, s, s_port, &ret_status);
        s->stat_trs_ret_code[s_port] = ret_status;
        blocked |= video_trs_rl_refused(s, s_port);
        video_trs_rl_refused_update(s);
        if (ret_status > 0) more = true;
      }
    }
  }
  return pending;
}

static int video_trs_tasklet_handler(void* priv) {
  struct st_video_transmitter_impl* trs = priv;
  struct mtl_main_impl* impl = trs->parent;
  struct st_tx_video_sessions_mgr* mgr = trs->mgr;
  struct st_tx_video_session_impl* s;
  int sidx, s_port;
  int pending = MTL_TASKLET_ALL_DONE;
  bool catch_up = false, pkt0_deferred = false;
  /* a visited RL packet 0 dequeued is due within two of its warm-up windows from then:
   * its window opens within one, and its visits must not wait for a catch-up */
  uint64_t near_tsc = UINT64_MAX;

  trs->visit++;
  /* the slots past max_idx are empty */
  RTE_BUILD_BUG_ON(ST_SCH_MAX_TX_VIDEO_SESSIONS >= 64);
  mgr->trs_refused &= (UINT64_C(1) << mgr->max_idx) - 1;
  for (sidx = 0; sidx < mgr->max_idx; sidx++) {
    pending += video_trs_rl_release(impl, trs, false);
    s = tx_video_session_dp_get(mgr, sidx);
    if (!s) {
      mgr->trs_refused &= ~(UINT64_C(1) << sidx);
      continue;
    }

    for (s_port = 0; s_port < s->ops.num_port; s_port++) {
      if (!s->queue[s_port]) continue;
      if (s->tx_queue_recovery_pending[s_port]) {
        s->tx_queue_recovery_pending[s_port] = false;
        st20_tx_queue_fatal_error(impl, s, s_port);
        continue;
      }
      pending += s->pacing_tasklet_func[s_port](impl, s, s_port);
      if (s->trs_released[s_port] != trs->visit)
        trs->pkt0_tsc =
            video_trs_rl_pkt0_first(trs->pkt0_tsc, video_trs_rl_pkt0_tsc(s, s_port));
      catch_up |= video_trs_rl_short(impl, s, s_port);
      uint64_t target = s->trs_target_tsc[s_port];
      if (target && s->pacing_way[s_port] == ST21_TX_PACING_WAY_RL) {
        uint64_t window = (uint64_t)(2 * s->pacing.warm_pkts * s->pacing.trs);
        near_tsc = RTE_MIN(near_tsc, target > window ? target - window : 0);
        pkt0_deferred |= s->rl_state[s_port] == ST_TX_VIDEO_RL_STATE_IDLE;
      }
    }
    video_trs_rl_refused_update(s);
  }
  pending += video_trs_rl_release(impl, trs, pkt0_deferred);
  if (catch_up && mt_get_tsc(impl) <= near_tsc)
    pending += video_trs_rl_catch_up(impl, mgr);
  tx_video_session_dp_done(mgr);

  return pending;
}

int st_video_resolve_pacing_tasklet(struct st_tx_video_session_impl* s,
                                    enum mtl_session_port port) {
  int idx = s->idx;

  switch (s->pacing_way[port]) {
    case ST21_TX_PACING_WAY_RL:
      s->pacing_tasklet_func[port] = video_trs_rl_tasklet;
      break;
    case ST21_TX_PACING_WAY_TSN:
      s->pacing_tasklet_func[port] = video_trs_launch_time_tasklet;
      break;
    case ST21_TX_PACING_WAY_TSC:
    case ST21_TX_PACING_WAY_BE:
    case ST21_TX_PACING_WAY_TSC_NARROW:
      s->pacing_tasklet_func[port] = video_trs_tsc_tasklet;
      break;
    case ST21_TX_PACING_WAY_PTP:
      s->pacing_tasklet_func[port] = video_trs_ptp_tasklet;
      break;
    default:
      err("%s(%d), unknow pacing %d\n", __func__, idx, s->pacing_way[port]);
      return -EIO;
  }
  return 0;
}

int st_video_transmitter_init(struct mtl_main_impl* impl, struct mtl_sch_impl* sch,
                              struct st_tx_video_sessions_mgr* mgr,
                              struct st_video_transmitter_impl* trs) {
  int idx = sch->idx;
  struct mtl_tasklet_ops ops;

  trs->parent = impl;
  trs->idx = idx;
  trs->mgr = mgr;

  memset(&ops, 0x0, sizeof(ops));
  ops.priv = trs;
  ops.name = "video_transmitter";
  ops.start = video_trs_tasklet_start;
  ops.stop = video_trs_tasklet_stop;
  ops.handler = video_trs_tasklet_handler;

  trs->tasklet = mtl_sch_register_tasklet(sch, &ops);
  if (!trs->tasklet) {
    err("%s(%d), mtl_sch_register_tasklet fail\n", __func__, idx);
    return -EIO;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

int st_video_transmitter_uinit(struct st_video_transmitter_impl* trs) {
  int idx = trs->idx;

  if (trs->tasklet) {
    mtl_sch_unregister_tasklet(trs->tasklet);
    trs->tasklet = NULL;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}
