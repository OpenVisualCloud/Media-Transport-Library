/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_tx_video_session.h"

#include <math.h>

#include "st_dev.h"
#include "st_err.h"
#include "st_log.h"
#include "st_sch.h"
#include "st_util.h"
#include "st_video_transmitter.h"

static inline double pacing_tr_offset_time(struct st_tx_video_pacing* pacing,
                                           uint64_t epochs) {
  return (epochs * pacing->frame_time) + pacing->tr_offset -
         (pacing->tr_offset_vrx * pacing->trs);
}

static inline uint32_t pacing_time_stamp(struct st_tx_video_pacing* pacing,
                                         uint64_t epochs) {
  double tr_offset_time = pacing_tr_offset_time(pacing, epochs);
  uint64_t tmstamp64 =
      (tr_offset_time / pacing->frame_time) * pacing->frame_time_sampling;
  uint32_t tmstamp32 = tmstamp64;

  return tmstamp32;
}

static inline void pacing_set_mbuf_time_stamp(struct rte_mbuf* mbuf,
                                              struct st_tx_video_pacing* pacing) {
  st_tx_mbuf_set_tsc(mbuf, pacing->tsc_time_cursor);
  st_tx_mbuf_set_ptp(mbuf, pacing->ptp_time_cursor);
}

static inline void pacing_foward_cursor(struct st_tx_video_pacing* pacing) {
  /* pkt foward */
  pacing->tsc_time_cursor += pacing->trs;
  pacing->ptp_time_cursor += pacing->trs;
}

static inline uint64_t tv_rl_bps(struct st_tx_video_session_impl* s) {
  double ractive = 1.0;
  if (s->ops.interlaced && s->ops.height <= 576) {
    ractive = (s->ops.height == 480) ? 487.0 / 525.0 : 576.0 / 625.0;
  }
  return (uint64_t)(s->st20_pkt_size * s->st20_total_pkts * 1.0 * s->fps_tm.mul /
                    s->fps_tm.den / ractive);
}

static void tv_notify_frame_done(struct st_tx_video_session_impl* s, uint16_t frame_idx) {
  if (s->st22_info) {
    if (s->st22_info->notify_frame_done)
      s->st22_info->notify_frame_done(s->ops.priv, frame_idx,
                                      &s->st20_frames[frame_idx].tx_st22_meta);
  } else {
    if (s->ops.notify_frame_done)
      s->ops.notify_frame_done(s->ops.priv, frame_idx,
                               &s->st20_frames[frame_idx].tv_meta);
  }
}

static void tv_frame_free_cb(void* addr, void* opaque) {
  struct st_tx_video_session_impl* s = opaque;
  uint16_t frame_idx;
  int idx = s->idx;
  struct st_frame_trans* frame_info;

  for (frame_idx = 0; frame_idx < s->st20_frames_cnt; ++frame_idx) {
    frame_info = &s->st20_frames[frame_idx];
    if ((addr >= frame_info->addr) && (addr < (frame_info->addr + s->st20_fb_size)))
      break;
  }
  if (frame_idx >= s->st20_frames_cnt) {
    err("%s(%d), addr %p do not belong to the session\n", __func__, idx, addr);
    return;
  }

  tv_notify_frame_done(s, frame_idx);
  rte_atomic32_dec(&frame_info->refcnt);
  /* clear ext frame info */
  if (frame_info->flags & ST_FT_FLAG_EXT) {
    frame_info->addr = NULL;
    frame_info->iova = 0;
  }

  dbg("%s(%d), succ frame_idx %u\n", __func__, idx, frame_idx);
}

static int tv_alloc_frames(struct st_main_impl* impl,
                           struct st_tx_video_session_impl* s) {
  enum st_port port = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_P);
  int soc_id = st_socket_id(impl, port);
  int idx = s->idx;
  struct st_frame_trans* frame_info;
  struct st22_tx_video_info* st22_info = s->st22_info;

  s->st20_frames =
      st_rte_zmalloc_socket(sizeof(*s->st20_frames) * s->st20_frames_cnt, soc_id);
  if (!s->st20_frames) {
    err("%s(%d), st20_frames malloc fail\n", __func__, idx);
    return -ENOMEM;
  }

  for (int i = 0; i < s->st20_frames_cnt; i++) {
    frame_info = &s->st20_frames[i];
    rte_atomic32_set(&frame_info->refcnt, 0);
    frame_info->idx = i;
  }

  for (int i = 0; i < s->st20_frames_cnt; i++) {
    frame_info = &s->st20_frames[i];

    frame_info->sh_info.free_cb = tv_frame_free_cb;
    frame_info->sh_info.fcb_opaque = s;
    rte_mbuf_ext_refcnt_set(&frame_info->sh_info, 0);

    if (s->ops.flags & ST20_TX_FLAG_EXT_FRAME) {
      frame_info->iova = 0;
      frame_info->addr = NULL;
      frame_info->flags = ST_FT_FLAG_EXT;
      info("%s(%d), use external framebuffer, skip allocation\n", __func__, idx);
    } else {
      void* frame = st_rte_zmalloc_socket(s->st20_fb_size, soc_id);
      if (!frame) {
        err("%s(%d), rte_malloc %" PRIu64 " fail at %d\n", __func__, idx, s->st20_fb_size,
            i);
        return -ENOMEM;
      }
      if (st22_info) { /* copy boxes */
        st_memcpy(frame, &st22_info->st22_boxes, s->st22_box_hdr_length);
      }
      frame_info->iova = rte_mem_virt2iova(frame);
      frame_info->addr = frame;
      frame_info->flags = ST_FT_FLAG_RTE_MALLOC;
    }
  }

  dbg("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int tv_free_frames(struct st_tx_video_session_impl* s) {
  if (s->st20_frames) {
    struct st_frame_trans* frame;
    for (int i = 0; i < s->st20_frames_cnt; i++) {
      frame = &s->st20_frames[i];
      st_frame_trans_uinit(frame);
    }

    st_rte_free(s->st20_frames);
    s->st20_frames = NULL;
  }

  dbg("%s(%d), succ\n", __func__, s->idx);
  return 0;
}

static int tv_poll_vsync(struct st_main_impl* impl, struct st_tx_video_session_impl* s) {
  struct st_vsync_info* vsync = &s->vsync;
  uint64_t cur_tsc = st_get_tsc(impl);

  if (cur_tsc > vsync->next_epoch_tsc) {
    uint64_t tsc_delta = cur_tsc - vsync->next_epoch_tsc;
    dbg("%s(%d), vsync with epochs %" PRIu64 "\n", __func__, s->idx, vsync->meta.epoch);
    s->ops.notify_event(s->ops.priv, ST_EVENT_VSYNC, &vsync->meta);
    st_vsync_calculate(impl, vsync); /* set next vsync */
    /* check tsc delta for status */
    if (tsc_delta > NS_PER_MS) s->stat_vsync_mismatch++;
  }

  return 0;
}

static int double_cmp(const void* a, const void* b) {
  const double* ai = a;
  const double* bi = b;

  if (*ai < *bi) {
    return -1;
  } else if (*ai > *bi) {
    return 1;
  }
  return 0;
}

static int tv_train_pacing(struct st_main_impl* impl, struct st_tx_video_session_impl* s,
                           enum st_session_port s_port) {
  enum st_port port = st_port_logic2phy(s->port_maps, s_port);
  struct rte_mbuf* pad = s->pad[s_port][ST20_PKT_TYPE_NORMAL];
  int idx = s->idx;
  uint16_t port_id = s->port_id[s_port];
  uint16_t queue_id = s->queue_id[s_port];
  unsigned int bulk = s->bulk;
  int pad_pkts, ret;
  int loop_cnt = 30;
  int trim = 5;
  double array[loop_cnt];
  double pkts_per_sec_sum = 0;
  float pad_interval;
  uint64_t rl_bps = tv_rl_bps(s);
  uint64_t train_start_time, train_end_time;

  ret = st_pacing_train_result_search(impl, port, rl_bps, &pad_interval);
  if (ret >= 0) {
    s->pacing.pad_interval = pad_interval;
    info("%s(%d), use pre-train pad_interval %f\n", __func__, idx, pad_interval);
    return 0;
  }

  /* wait tsc calibrate done, pacing need fine tuned TSC */
  st_wait_tsc_stable(impl);

  train_start_time = st_get_tsc(impl);

  /* warm stage to consume all nix tx buf */
  pad_pkts = s->st20_total_pkts * 100;
  for (int i = 0; i < pad_pkts; i++) {
    rte_mbuf_refcnt_update(pad, 1);
    st_tx_burst_busy(port_id, queue_id, &pad, 1);
  }

  /* training stage */
  pad_pkts = s->st20_total_pkts * 2;
  for (int loop = 0; loop < loop_cnt; loop++) {
    uint64_t start = st_get_tsc(impl);
    for (int i = 0; i < ST20_PKT_TYPE_MAX; i++) {
      pad = s->pad[s_port][i];
      int pkts = s->st20_pkt_info[i].number * 2;

      struct rte_mbuf* bulk_pad[bulk];
      for (int j = 0; j < bulk; j++) {
        bulk_pad[j] = pad;
      }
      int bulk_batch = pkts / bulk;
      for (int j = 0; j < bulk_batch; j++) {
        rte_mbuf_refcnt_update(pad, bulk);
        st_tx_burst_busy(port_id, queue_id, bulk_pad, bulk);
      }
      int remaining = pkts % bulk;
      for (int j = 0; j < remaining; j++) {
        rte_mbuf_refcnt_update(pad, 1);
        st_tx_burst_busy(port_id, queue_id, &pad, 1);
      }
    }
    uint64_t end = st_get_tsc(impl);
    double time_sec = (double)(end - start) / NS_PER_S;
    array[loop] = pad_pkts / time_sec;
  }

  qsort(array, loop_cnt, sizeof(double), double_cmp);
  for (int i = trim; i < loop_cnt - trim; i++) {
    pkts_per_sec_sum += array[i];
  }
  double pkts_per_sec = pkts_per_sec_sum / (loop_cnt - trim * 2);

  /* parse the pad interval */
  double pkts_per_frame = pkts_per_sec * s->fps_tm.den / s->fps_tm.mul;
  /* adjust as tr offset */
  double ractive = (1080.0 / 1125.0);
  if (s->ops.interlaced && s->ops.height <= 576) {
    ractive = (s->ops.height == 480) ? 487.0 / 525.0 : 576.0 / 625.0;
  }
  pkts_per_frame = pkts_per_frame * ractive;
  if (pkts_per_frame < s->st20_total_pkts) {
    err("%s(%d), error pkts_per_frame %f, st20_total_pkts %d\n", __func__, idx,
        pkts_per_frame, s->st20_total_pkts);
    return -EINVAL;
  }

  pad_interval = (float)s->st20_total_pkts / (pkts_per_frame - s->st20_total_pkts);
  if (pad_interval < 32) {
    err("%s(%d), too small pad_interval %f pkts_per_frame %f, st20_total_pkts %d\n",
        __func__, idx, pad_interval, pkts_per_frame, s->st20_total_pkts);
    return -EINVAL;
  }

  s->pacing.pad_interval = pad_interval;
  st_pacing_train_result_add(impl, port, rl_bps, pad_interval);
  train_end_time = st_get_tsc(impl);
  info("%s(%d,%d), trained pad_interval %f pkts_per_frame %f with time %fs\n", __func__,
       idx, s_port, pad_interval, pkts_per_frame,
       (double)(train_end_time - train_start_time) / NS_PER_S);
  return 0;
}

static int tv_init_pacing(struct st_main_impl* impl, struct st_tx_video_session_impl* s) {
  int idx = s->idx;
  struct st_tx_video_pacing* pacing = &s->pacing;

  double frame_time = (double)1000000000.0 * s->fps_tm.den / s->fps_tm.mul;
  pacing->frame_time = frame_time;
  pacing->frame_time_sampling =
      (double)(s->fps_tm.sampling_clock_rate) * s->fps_tm.den / s->fps_tm.mul;
  double ractive = 1080.0 / 1125.0;
  pacing->tr_offset =
      s->ops.height >= 1080 ? frame_time * (43.0 / 1125.0) : frame_time * (28.0 / 750.0);
  pacing->tr_offset_vrx = s->st21_vrx_narrow;

  if (s->ops.interlaced) {
    if (s->ops.height <= 576)
      ractive = (s->ops.height == 480) ? 487.0 / 525.0 : 576.0 / 625.0;
    if (s->ops.height == 480) {
      pacing->tr_offset = frame_time * (20.0 / 525.0) * 2;
    } else if (s->ops.height == 576) {
      pacing->tr_offset = frame_time * (26.0 / 625.0) * 2;
    } else {
      pacing->tr_offset = frame_time * (22.0 / 1125.0) * 2;
    }
  }
  pacing->trs = frame_time * ractive / s->st20_total_pkts;
  /* always use ST_PORT_P for ptp now */
  pacing->cur_epochs = st_get_ptp_time(impl, ST_PORT_P) / frame_time;
  pacing->tsc_time_cursor = st_get_tsc(impl);

  /* 80 percent tr offset time as warmup pkts */
  uint32_t troffset_warm_pkts = pacing->tr_offset / pacing->trs;
  troffset_warm_pkts = troffset_warm_pkts * 8 / 10;
  troffset_warm_pkts = RTE_MIN(troffset_warm_pkts, 128); /* limit to 128 pkts */
  pacing->warm_pkts = troffset_warm_pkts;
  pacing->tr_offset_vrx += troffset_warm_pkts; /* time for warm pkts */
  pacing->tr_offset_vrx -= 2; /* VRX compensate to rl burst(max_burst_size=2048) */
  pacing->tr_offset_vrx -= 2; /* leave VRX space for deviation */
  pacing->pad_interval = s->st20_total_pkts; /* VRX compensate as rl accuracy */
  if (s->ops.height <= 576) {
    pacing->warm_pkts = 8; /* fix me */
    pacing->tr_offset_vrx = s->st21_vrx_narrow;
  }

  if (s->s_type == ST22_SESSION_TYPE_TX_VIDEO) {
    /* no vrx/warm_pkts for st22? */
    pacing->tr_offset_vrx = 0;
    pacing->warm_pkts = 0;
  }

  info("%s[%02d], trs %f trOffset %f warm pkts %u\n", __func__, idx, pacing->trs,
       pacing->tr_offset, troffset_warm_pkts);

  int num_port = s->ops.num_port;
  enum st_port port;
  int ret;

  for (int i = 0; i < num_port; i++) {
    port = st_port_logic2phy(s->port_maps, i);
    /* use system pacing way now */
    s->pacing_way[i] = st_tx_pacing_way(impl, port);
    if (s->pacing_way[i] == ST21_TX_PACING_WAY_RL) {
      ret = tv_train_pacing(impl, s, i);
      if (ret < 0) {
        /* fallback to tsc pacing */
        s->pacing_way[i] = ST21_TX_PACING_WAY_TSC;
      }
    }
  }

  if (num_port > 1) {
    if (s->pacing_way[ST_SESSION_PORT_P] != s->pacing_way[ST_SESSION_PORT_R]) {
      /* currently not support two different pacing? */
      warn("%s(%d), different pacing detected, all set to tsc\n", __func__, idx);
      s->pacing_way[ST_SESSION_PORT_P] = ST21_TX_PACING_WAY_TSC;
      s->pacing_way[ST_SESSION_PORT_R] = ST21_TX_PACING_WAY_TSC;
    }
  }

  /* reslove pacing tasklet */
  for (int i = 0; i < num_port; i++) {
    ret = st_video_reslove_pacing_tasklet(s, i);
    if (ret < 0) return ret;
  }

  return 0;
}

static int tv_sync_pacing(struct st_main_impl* impl, struct st_tx_video_session_impl* s,
                          bool sync, uint64_t required_tai) {
  int idx = s->idx;
  struct st_tx_video_pacing* pacing = &s->pacing;
  double frame_time = pacing->frame_time;
  /* always use ST_PORT_P for ptp now */
  uint64_t ptp_time = st_get_ptp_time(impl, ST_PORT_P);
  uint64_t next_epochs = pacing->cur_epochs + 1;
  uint64_t epochs;
  double to_epoch_tr_offset;

  if (required_tai) {
    uint64_t ptp_epochs = ptp_time / frame_time;
    epochs = required_tai / frame_time;
    dbg("%s(%d), required tai %" PRIu64 " ptp_epochs %" PRIu64 " epochs %" PRIu64 "\n",
        __func__, idx, required_tai, ptp_epochs, epochs);
    if (epochs < ptp_epochs) s->stat_error_user_timestamp++;
  } else {
    epochs = ptp_time / frame_time;
  }

  dbg("%s(%d), ptp epochs %" PRIu64 " cur_epochs %" PRIu64 ", ptp_time %" PRIu64 "ms\n",
      __func__, idx, epochs, pacing->cur_epochs, ptp_time / 1000 / 1000);
  if (epochs == pacing->cur_epochs) {
    /* likely most previous frame can enqueue within previous timing, use next epoch */
    epochs = next_epochs;
  }
  if ((epochs + 1) == pacing->cur_epochs) {
    /* sometimes it's still in previous epoch time since deep ring queue */
    epochs = next_epochs;
  }

  /* epoch resloved */
  double ptp_tr_offset_time = pacing_tr_offset_time(pacing, epochs);
  to_epoch_tr_offset = ptp_tr_offset_time - ptp_time;
  if (to_epoch_tr_offset < 0) {
    /* current time run out of tr offset already, sync to next epochs */
    dbg("%s(%d), to_epoch_tr_offset %f, ptp epochs %" PRIu64 " cur_epochs %" PRIu64
        ", ptp_time %" PRIu64 "ms\n",
        __func__, idx, to_epoch_tr_offset, epochs, pacing->cur_epochs,
        ptp_time / 1000 / 1000);
    s->stat_epoch_troffset_mismatch++;
    epochs++;
    ptp_tr_offset_time = pacing_tr_offset_time(pacing, epochs);
    to_epoch_tr_offset = ptp_tr_offset_time - ptp_time;
  }

  if (to_epoch_tr_offset < 0) {
    /* should never happen */
    err("%s(%d), error to_epoch_tr_offset %f, ptp_time %" PRIu64 ", epochs %" PRIu64
        " %" PRIu64 "\n",
        __func__, idx, to_epoch_tr_offset, ptp_time, epochs, pacing->cur_epochs);
    to_epoch_tr_offset = 0;
  }

  if (epochs > next_epochs) s->stat_epoch_drop += (epochs - next_epochs);
  pacing->cur_epochs = epochs;
  pacing->pacing_time_stamp = pacing_time_stamp(pacing, epochs);
  pacing->rtp_time_stamp = pacing->pacing_time_stamp;
  dbg("%s(%d), old time_cursor %fms\n", __func__, idx,
      pacing->tsc_time_cursor / 1000 / 1000);
  pacing->tsc_time_cursor = (double)st_get_tsc(impl) + to_epoch_tr_offset;
  dbg("%s(%d), epochs %lu time_stamp %u time_cursor %fms to_epoch_tr_offset %fms\n",
      __func__, idx, pacing->cur_epochs, pacing->pacing_time_stamp,
      pacing->tsc_time_cursor / 1000 / 1000, to_epoch_tr_offset / 1000 / 1000);
  pacing->ptp_time_cursor = ptp_tr_offset_time;

  if (sync) {
    dbg("%s(%d), delay to epoch_time %f, cur %" PRIu64 "\n", __func__, idx,
        pacing->tsc_time_cursor, st_get_tsc(impl));
    st_tsc_delay_to(impl, pacing->tsc_time_cursor);
  }

  return 0;
}

static int tv_init_st22_boxes(struct st_main_impl* impl,
                              struct st_tx_video_session_impl* s) {
  struct st22_tx_video_info* st22_info = s->st22_info;
  struct st22_jpvs* jpvs = &st22_info->st22_boxes.jpvs;
  uint32_t lbox = sizeof(*jpvs);
  jpvs->lbox = htonl(lbox);
  jpvs->tbox[0] = 'j';
  jpvs->tbox[1] = 'p';
  jpvs->tbox[2] = 'v';
  jpvs->tbox[3] = 's';

  struct st22_jpvi* jpvi = &jpvs->jpvi;
  lbox = sizeof(*jpvi);
  jpvi->lbox = htonl(lbox);
  jpvi->tbox[0] = 'j';
  jpvi->tbox[1] = 'p';
  jpvi->tbox[2] = 'v';
  jpvi->tbox[3] = 'i';
  uint32_t brat_m =
      8 * s->st22_codestream_size * s->fps_tm.mul / s->fps_tm.den / 1024 / 1024;
  jpvi->brat = htonl(brat_m);
  /* hardcode to 59.94 now */
  uint32_t frat = (1 << 24) | 60;
  jpvi->frat = htonl(frat);
  /* hardcode to 10bit ycbcr 422 */
  uint16_t schar = (0x1 << 15) | ((10 - 1) << 4);
  jpvi->schar = htons(schar);
  /* zero now */
  jpvi->tcod = htonl(0x0);

  struct st22_jxpl* jxpl = &jpvs->jxpl;
  lbox = sizeof(*jxpl);
  jxpl->lbox = htonl(lbox);
  jxpl->tbox[0] = 'j';
  jxpl->tbox[1] = 'x';
  jxpl->tbox[2] = 'p';
  jxpl->tbox[3] = 'l';
  /* Main 422.10 */
  jxpl->ppih = htons(0x3540);
  /* 4k-1 full */
  jxpl->plev = htons(0x2080);

  struct st22_colr* colr = &st22_info->st22_boxes.colr;
  lbox = sizeof(*colr);
  colr->lbox = htonl(lbox);
  colr->tbox[0] = 'c';
  colr->tbox[1] = 'o';
  colr->tbox[2] = 'l';
  colr->tbox[3] = 'r';
  colr->meth = 0x05; /* must 5 */
  /*  ITU-R BT.709-6 */
  colr->methdat[1] = 0x01;
  colr->methdat[3] = 0x01;
  colr->methdat[5] = 0x01;
  colr->methdat[6] = 0x80;

  return 0;
}

static int tv_init_hdr(struct st_main_impl* impl, struct st_tx_video_session_impl* s,
                       enum st_session_port s_port) {
  int idx = s->idx;
  enum st_port port = st_port_logic2phy(s->port_maps, s_port);
  int ret;
  struct st_rfc4175_video_hdr* hdr = &s->s_hdr[s_port];
  struct rte_ether_hdr* eth = &hdr->eth;
  struct rte_ipv4_hdr* ipv4 = &hdr->ipv4;
  struct rte_udp_hdr* udp = &hdr->udp;
  struct st20_rfc4175_rtp_hdr* rtp = &hdr->rtp;
  struct st20_tx_ops* ops = &s->ops;
  uint8_t* dip = ops->dip_addr[s_port];
  uint8_t* sip = st_sip_addr(impl, port);
  struct rte_ether_addr* d_addr = st_eth_d_addr(eth);

  /* ether hdr */
  if ((s_port == ST_SESSION_PORT_P) && (ops->flags & ST20_TX_FLAG_USER_P_MAC)) {
    rte_memcpy(d_addr, &ops->tx_dst_mac[s_port][0], RTE_ETHER_ADDR_LEN);
    info("%s, USER_P_TX_MAC\n", __func__);
  } else if ((s_port == ST_SESSION_PORT_R) && (ops->flags & ST20_TX_FLAG_USER_R_MAC)) {
    rte_memcpy(d_addr, &ops->tx_dst_mac[s_port][0], RTE_ETHER_ADDR_LEN);
    info("%s, USER_R_TX_MAC\n", __func__);
  } else {
    ret = st_dev_dst_ip_mac(impl, dip, d_addr, port);
    if (ret < 0) {
      err("%s(%d), st_dev_dst_ip_mac fail %d for %d.%d.%d.%d\n", __func__, idx, ret,
          dip[0], dip[1], dip[2], dip[3]);
      return ret;
    }
  }

  ret = rte_eth_macaddr_get(s->port_id[s_port], st_eth_s_addr(eth));
  if (ret < 0) {
    err("%s(%d), rte_eth_macaddr_get fail %d for port %d\n", __func__, idx, ret, s_port);
    return ret;
  }
  eth->ether_type = htons(RTE_ETHER_TYPE_IPV4);

  /* ipv4 hdr */
  memset(ipv4, 0x0, sizeof(*ipv4));
  ipv4->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
  ipv4->time_to_live = 64;
  ipv4->type_of_service = 0;
  ipv4->fragment_offset = ST_IP_DONT_FRAGMENT_FLAG;
  ipv4->next_proto_id = 17;
  st_memcpy(&ipv4->src_addr, sip, ST_IP_ADDR_LEN);
  st_memcpy(&ipv4->dst_addr, dip, ST_IP_ADDR_LEN);

  /* udp hdr */
  udp->src_port = htons(s->st20_src_port[s_port]);
  udp->dst_port = htons(s->st20_dst_port[s_port]);
  udp->dgram_cksum = 0;

  /* rtp hdr */
  memset(rtp, 0x0, sizeof(*rtp));
  rtp->base.csrc_count = 0;
  rtp->base.extension = 0;
  rtp->base.padding = 0;
  rtp->base.version = ST_RVRTP_VERSION_2;
  rtp->base.marker = 0;
  rtp->base.payload_type = st_is_valid_payload_type(ops->payload_type)
                               ? ops->payload_type
                               : ST_RVRTP_PAYLOAD_TYPE_RAW_VIDEO;
  rtp->base.ssrc = htonl(s->idx + 0x123450);
  rtp->row_length = htons(s->st20_pkt_len);
  rtp->row_number = 0;
  rtp->row_offset = 0;

  /* st22_rfc9134_rtp_hdr if st22 frame mode */
  if (s->st22_info) {
    struct st22_rfc9134_rtp_hdr* st22_hdr = &s->st22_info->rtp_hdr[s_port];
    /* copy base */
    st_memcpy(&st22_hdr->base, &rtp->base, sizeof(st22_hdr->base));
    st22_hdr->trans_order = 1; /* packets sent sequentially */
    st22_hdr->kmode = 0;       /* codestream packetization mode */
    st22_hdr->f_counter_hi = 0;
    st22_hdr->f_counter_lo = 0;
  }

  info("%s(%d), dst ip:port %d.%d.%d.%d:%d, port %d\n", __func__, idx, dip[0], dip[1],
       dip[2], dip[3], s->st20_dst_port[s_port], s_port);
  info("%s(%d), mac: %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx\n", __func__, idx,
       d_addr->addr_bytes[0], d_addr->addr_bytes[1], d_addr->addr_bytes[2],
       d_addr->addr_bytes[3], d_addr->addr_bytes[4], d_addr->addr_bytes[5]);
  return 0;
}

static int tv_build_pkt(struct st_main_impl* impl, struct st_tx_video_session_impl* s,
                        struct rte_mbuf* pkt, struct rte_mbuf* pkt_chain) {
  struct st_rfc4175_video_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  struct rte_udp_hdr* udp;
  struct st20_rfc4175_rtp_hdr* rtp;
  struct st20_rfc4175_extra_rtp_hdr* e_rtp = NULL;
  struct st20_tx_ops* ops = &s->ops;
  uint32_t offset;
  uint16_t line1_number, line1_offset;
  uint16_t line1_length, line2_length;
  bool single_line = (ops->packing == ST20_PACKING_GPM_SL);
  struct st_frame_trans* frame_info = &s->st20_frames[s->st20_frame_idx];

  hdr = rte_pktmbuf_mtod(pkt, struct st_rfc4175_video_hdr*);
  ipv4 = &hdr->ipv4;
  rtp = &hdr->rtp;
  udp = &hdr->udp;

  /* copy the hdr: eth, ip, udp, rtp */
  rte_memcpy(hdr, &s->s_hdr[ST_SESSION_PORT_P], sizeof(*hdr));

  /* update ipv4 hdr */
  ipv4->packet_id = htons(s->st20_ipv4_packet_id);
  s->st20_ipv4_packet_id++;

  if (single_line) {
    line1_number = s->st20_pkt_idx / s->st20_pkts_in_line;
    int pixel_in_pkt = s->st20_pkt_len / s->st20_pg.size * s->st20_pg.coverage;
    line1_offset = pixel_in_pkt * (s->st20_pkt_idx % s->st20_pkts_in_line);
    offset = line1_number * s->st20_linesize +
             line1_offset / s->st20_pg.coverage * s->st20_pg.size;
  } else {
    offset = s->st20_pkt_len * s->st20_pkt_idx;
    line1_number = offset / s->st20_bytes_in_line;
    line1_offset =
        (offset % s->st20_bytes_in_line) * s->st20_pg.coverage / s->st20_pg.size;
    if ((offset + s->st20_pkt_len > (line1_number + 1) * s->st20_bytes_in_line) &&
        (offset + s->st20_pkt_len < s->st20_frame_size))
      e_rtp =
          rte_pktmbuf_mtod_offset(pkt, struct st20_rfc4175_extra_rtp_hdr*, sizeof(*hdr));
  }

  /* update rtp */
  if (s->st20_pkt_idx >= (s->st20_total_pkts - 1)) rtp->base.marker = 1;
  rtp->base.seq_number = htons((uint16_t)s->st20_seq_id);
  rtp->seq_number_ext = htons((uint16_t)(s->st20_seq_id >> 16));
  s->st20_seq_id++;
  uint16_t field = frame_info->tv_meta.second_field ? ST20_SECOND_FIELD : 0x0000;
  rtp->row_number = htons(line1_number | field);
  rtp->row_offset = htons(line1_offset);
  rtp->base.tmstamp = htonl(s->pacing.rtp_time_stamp);

  uint32_t temp =
      single_line ? ((ops->width - line1_offset) / s->st20_pg.coverage * s->st20_pg.size)
                  : (s->st20_frame_size - offset);
  uint16_t left_len = RTE_MIN(s->st20_pkt_len, temp);
  rtp->row_length = htons(left_len);

  if (e_rtp) {
    line1_length = (line1_number + 1) * s->st20_bytes_in_line - offset;
    line2_length = s->st20_pkt_len - line1_length;
    rtp->row_length = htons(line1_length);
    e_rtp->row_length = htons(line2_length);
    e_rtp->row_offset = htons(0);
    e_rtp->row_number = htons((line1_number + 1) | field);
    rtp->row_offset = htons(line1_offset | ST20_SRD_OFFSET_CONTINUATION);
  }

  /* update mbuf */
  st_mbuf_init_ipv4(pkt);
  pkt->data_len = sizeof(struct st_rfc4175_video_hdr);
  if (e_rtp) pkt->data_len += sizeof(*e_rtp);
  pkt->pkt_len = pkt->data_len;

  if (!single_line && s->st20_linesize > s->st20_bytes_in_line)
    /* update offset with line padding for copying */
    offset = offset % s->st20_bytes_in_line + line1_number * s->st20_linesize;

  if (e_rtp && s->st20_linesize > s->st20_bytes_in_line) {
    /* cross lines with padding case */
    /* do not attach extbuf, copy to data room */
    void* payload = rte_pktmbuf_mtod(pkt_chain, void*);
    st_memcpy(payload, frame_info->addr + offset, line1_length);
    st_memcpy(payload + line1_length,
              frame_info->addr + s->st20_linesize * (line1_number + 1), line2_length);
  } else {
    /* attach payload to chainbuf */
    rte_pktmbuf_attach_extbuf(pkt_chain, frame_info->addr + offset,
                              frame_info->iova + offset, left_len, &frame_info->sh_info);
    rte_mbuf_ext_refcnt_update(&frame_info->sh_info, 1);
  }
  pkt_chain->data_len = pkt_chain->pkt_len = left_len;

  /* chain the pkt */
  rte_pktmbuf_chain(pkt, pkt_chain);
  if (!s->eth_has_chain[ST_SESSION_PORT_P]) {
    st_mbuf_chain_sw(pkt, pkt_chain);
  }

  udp->dgram_len = htons(pkt->pkt_len - pkt->l2_len - pkt->l3_len);
  ipv4->total_length = htons(pkt->pkt_len - pkt->l2_len);
  if (!s->eth_ipv4_cksum_offload[ST_SESSION_PORT_P]) {
    /* generate cksum if no offload */
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
  }

  return 0;
}

static int tv_build_rtp(struct st_main_impl* impl, struct st_tx_video_session_impl* s,
                        struct rte_mbuf* pkt, struct rte_mbuf* pkt_chain) {
  struct st_base_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  struct st_rfc3550_rtp_hdr* rtp;
  struct rte_udp_hdr* udp;

  hdr = rte_pktmbuf_mtod(pkt, struct st_base_hdr*);
  ipv4 = &hdr->ipv4;
  udp = &hdr->udp;
  rtp = rte_pktmbuf_mtod(pkt_chain, struct st_rfc3550_rtp_hdr*);

  /* copy the hdr: eth, ip, udp */
  rte_memcpy(&hdr->eth, &s->s_hdr[ST_SESSION_PORT_P].eth, sizeof(hdr->eth));
  rte_memcpy(ipv4, &s->s_hdr[ST_SESSION_PORT_P].ipv4, sizeof(hdr->ipv4));
  rte_memcpy(udp, &s->s_hdr[ST_SESSION_PORT_P].udp, sizeof(hdr->udp));

  /* update ipv4 hdr */
  ipv4->packet_id = htons(s->st20_ipv4_packet_id);
  s->st20_ipv4_packet_id++;

  if (rtp->tmstamp != s->st20_rtp_time) {
    /* start of a new frame */
    s->st20_pkt_idx = 0;
    rte_atomic32_inc(&s->stat_frame_cnt);
    s->st20_rtp_time = rtp->tmstamp;
    tv_sync_pacing(impl, s, false, 0);
    if (s->ops.flags & ST20_TX_FLAG_USER_TIMESTAMP) {
      s->pacing.rtp_time_stamp = ntohl(rtp->tmstamp);
    }
    dbg("%s(%d), rtp time stamp %u\n", __func__, s->idx, s->pacing.rtp_time_stamp);
  }
  /* update rtp time*/
  rtp->tmstamp = htonl(s->pacing.rtp_time_stamp);

  /* update mbuf */
  st_mbuf_init_ipv4(pkt);
  pkt->data_len = sizeof(struct st_base_hdr);
  pkt->pkt_len = pkt->data_len;

  /* chain the pkt */
  rte_pktmbuf_chain(pkt, pkt_chain);
  if (!s->eth_has_chain[ST_SESSION_PORT_P]) {
    st_mbuf_chain_sw(pkt, pkt_chain);
  }

  udp->dgram_len = htons(pkt->pkt_len - pkt->l2_len - pkt->l3_len);
  ipv4->total_length = htons(pkt->pkt_len - pkt->l2_len);
  if (!s->eth_ipv4_cksum_offload[ST_SESSION_PORT_P]) {
    /* generate cksum if no offload */
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
  }
  return 0;
}

static int tv_build_redundant_rtp(struct st_tx_video_session_impl* s,
                                  struct rte_mbuf* pkt_r, struct rte_mbuf* pkt_base,
                                  struct rte_mbuf* pkt_chain) {
  struct rte_ipv4_hdr *ipv4, *ipv4_base;
  struct st_base_hdr *hdr, *hdr_base;

  hdr = rte_pktmbuf_mtod(pkt_r, struct st_base_hdr*);
  ipv4 = &hdr->ipv4;
  hdr_base = rte_pktmbuf_mtod(pkt_base, struct st_base_hdr*);
  ipv4_base = &hdr_base->ipv4;

  /* copy the hdr: eth, ip, udp */
  rte_memcpy(&hdr->eth, &s->s_hdr[ST_SESSION_PORT_R].eth, sizeof(hdr->eth));
  rte_memcpy(ipv4, &s->s_hdr[ST_SESSION_PORT_R].ipv4, sizeof(hdr->ipv4));
  rte_memcpy(&hdr->udp, &s->s_hdr[ST_SESSION_PORT_R].udp, sizeof(hdr->udp));

  /* update ipv4 hdr */
  ipv4->packet_id = ipv4_base->packet_id;

  if (!s->eth_has_chain[ST_SESSION_PORT_R]) {
    st_mbuf_chain_sw_copy(pkt_r, pkt_chain);
  }

  /* update mbuf */
  pkt_r->data_len = pkt_base->data_len;
  pkt_r->pkt_len = pkt_base->pkt_len;
  pkt_r->l2_len = pkt_base->l2_len;
  pkt_r->l3_len = pkt_base->l3_len;
  pkt_r->ol_flags = pkt_base->ol_flags;
  pkt_r->nb_segs = 2;
  /* chain mbuf */
  pkt_r->next = pkt_chain;

  rte_mbuf_refcnt_update(pkt_chain, 1);
  hdr->udp.dgram_len = htons(pkt_r->pkt_len - pkt_r->l2_len - pkt_r->l3_len);
  ipv4->total_length = htons(pkt_r->pkt_len - pkt_r->l2_len);
  if (!s->eth_ipv4_cksum_offload[ST_SESSION_PORT_R]) {
    /* generate cksum if no offload */
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
  }

  return 0;
}

static int tv_build_redundant(struct st_tx_video_session_impl* s, struct rte_mbuf* pkt_r,
                              struct rte_mbuf* pkt_base, struct rte_mbuf* pkt_chain) {
  struct st_rfc4175_video_hdr* hdr;
  struct st_rfc4175_video_hdr* hdr_base;
  struct rte_ipv4_hdr* ipv4;
  struct rte_ipv4_hdr* ipv4_base;
  struct st20_rfc4175_rtp_hdr* rtp;
  struct st20_rfc4175_rtp_hdr* rtp_base;

  hdr = rte_pktmbuf_mtod(pkt_r, struct st_rfc4175_video_hdr*);
  ipv4 = &hdr->ipv4;
  rtp = &hdr->rtp;

  /* copy the hdr: eth, ip, udp, rtp */
  rte_memcpy(hdr, &s->s_hdr[ST_SESSION_PORT_R], sizeof(*hdr));

  /* update rtp */
  hdr_base = rte_pktmbuf_mtod(pkt_base, struct st_rfc4175_video_hdr*);
  ipv4_base = &hdr_base->ipv4;
  /* update ipv4 hdr */
  ipv4->packet_id = ipv4_base->packet_id;

  rtp_base = &hdr_base->rtp;
  rte_memcpy(rtp, rtp_base, sizeof(*rtp));

  /* copy extra if Continuation */
  uint16_t line1_offset = ntohs(rtp->row_offset);
  if (line1_offset & ST20_SRD_OFFSET_CONTINUATION) {
    rte_memcpy(&rtp[1], &rtp_base[1], sizeof(struct st20_rfc4175_extra_rtp_hdr));
  }

  if (!s->eth_has_chain[ST_SESSION_PORT_R]) {
    st_mbuf_chain_sw_copy(pkt_r, pkt_chain);
  }

  /* update mbuf */
  pkt_r->data_len = pkt_base->data_len;
  pkt_r->pkt_len = pkt_base->pkt_len;
  pkt_r->l2_len = pkt_base->l2_len;
  pkt_r->l3_len = pkt_base->l3_len;
  pkt_r->ol_flags = pkt_base->ol_flags;
  pkt_r->nb_segs = 2;
  /* chain mbuf */
  pkt_r->next = pkt_chain;

  rte_mbuf_refcnt_update(pkt_chain, 1);
  hdr->udp.dgram_len = htons(pkt_r->pkt_len - pkt_r->l2_len - pkt_r->l3_len);
  ipv4->total_length = htons(pkt_r->pkt_len - pkt_r->l2_len);
  if (!s->eth_ipv4_cksum_offload[ST_SESSION_PORT_R]) {
    /* generate cksum if no offload */
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
  }

  return 0;
}

static int tv_build_st22(struct st_main_impl* impl, struct st_tx_video_session_impl* s,
                         struct rte_mbuf* pkt, struct rte_mbuf* pkt_chain) {
  struct st22_rfc9134_video_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  struct rte_udp_hdr* udp;
  struct st22_rfc9134_rtp_hdr* rtp;
  struct st22_tx_video_info* st22_info = s->st22_info;

  hdr = rte_pktmbuf_mtod(pkt, struct st22_rfc9134_video_hdr*);
  ipv4 = &hdr->ipv4;
  rtp = &hdr->rtp;
  udp = &hdr->udp;

  /* copy the hdr: eth, ip, udp */
  rte_memcpy(&hdr->eth, &s->s_hdr[ST_SESSION_PORT_P].eth, sizeof(hdr->eth));
  rte_memcpy(ipv4, &s->s_hdr[ST_SESSION_PORT_P].ipv4, sizeof(*ipv4));
  rte_memcpy(udp, &s->s_hdr[ST_SESSION_PORT_P].udp, sizeof(*udp));
  /* copy rtp */
  rte_memcpy(rtp, &st22_info->rtp_hdr[ST_SESSION_PORT_P], sizeof(*rtp));

  /* update ipv4 hdr */
  ipv4->packet_id = htons(s->st20_ipv4_packet_id);
  s->st20_ipv4_packet_id++;

  /* update rtp */
  if (s->st20_pkt_idx >= (st22_info->st22_total_pkts - 1)) {
    rtp->base.marker = 1;
    rtp->last_packet = 1;
    dbg("%s(%d), maker on pkt %d(total %d)\n", __func__, s->idx, s->st20_pkt_idx,
        s->st22_total_pkts);
  }
  rtp->base.seq_number = htons((uint16_t)s->st20_seq_id);
  s->st20_seq_id++;
  rtp->base.tmstamp = htonl(s->pacing.rtp_time_stamp);
  uint16_t f_counter = st22_info->frame_idx % 32;
  uint16_t sep_counter = s->st20_pkt_idx / 2048;
  uint16_t p_counter = s->st20_pkt_idx % 2048;
  rtp->p_counter_lo = p_counter;
  rtp->p_counter_hi = p_counter >> 8;
  rtp->sep_counter_lo = sep_counter;
  rtp->sep_counter_hi = sep_counter >> 5;
  rtp->f_counter_lo = f_counter;
  rtp->f_counter_hi = f_counter >> 2;

  /* update mbuf */
  st_mbuf_init_ipv4(pkt);
  pkt->data_len = sizeof(*hdr);
  pkt->pkt_len = pkt->data_len;

  uint32_t offset = s->st20_pkt_idx * s->st20_pkt_len;
  uint16_t left_len = RTE_MIN(s->st20_pkt_len, st22_info->cur_frame_size - offset);
  dbg("%s(%d), data len %u on pkt %d(total %d)\n", __func__, s->idx, left_len,
      s->st20_pkt_idx, s->st22_total_pkts);

  /* attach payload to chainbuf */
  struct st_frame_trans* frame_info = &s->st20_frames[s->st20_frame_idx];
  rte_pktmbuf_attach_extbuf(pkt_chain, frame_info->addr + offset,
                            frame_info->iova + offset, left_len, &frame_info->sh_info);
  rte_mbuf_ext_refcnt_update(&frame_info->sh_info, 1);

  pkt_chain->data_len = pkt_chain->pkt_len = left_len;

  /* chain the pkt */
  rte_pktmbuf_chain(pkt, pkt_chain);
  if (!s->eth_has_chain[ST_SESSION_PORT_P]) {
    st_mbuf_chain_sw(pkt, pkt_chain);
  }

  udp->dgram_len = htons(pkt->pkt_len - pkt->l2_len - pkt->l3_len);
  ipv4->total_length = htons(pkt->pkt_len - pkt->l2_len);
  if (!s->eth_ipv4_cksum_offload[ST_SESSION_PORT_P]) {
    /* generate cksum if no offload */
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
  }

  return 0;
}

static int tv_build_st22_redundant(struct st_tx_video_session_impl* s,
                                   struct rte_mbuf* pkt_r, struct rte_mbuf* pkt_base,
                                   struct rte_mbuf* pkt_chain) {
  struct st22_rfc9134_video_hdr* hdr;
  struct st22_rfc9134_video_hdr* hdr_base;
  struct rte_ipv4_hdr* ipv4;
  struct rte_ipv4_hdr* ipv4_base;
  struct st22_rfc9134_rtp_hdr* rtp;
  struct st22_rfc9134_rtp_hdr* rtp_base;

  hdr = rte_pktmbuf_mtod(pkt_r, struct st22_rfc9134_video_hdr*);
  ipv4 = &hdr->ipv4;
  rtp = &hdr->rtp;

  /* copy the hdr: eth, ip, udp, rtp */
  rte_memcpy(hdr, &s->s_hdr[ST_SESSION_PORT_R], sizeof(*hdr));

  /* update rtp */
  hdr_base = rte_pktmbuf_mtod(pkt_base, struct st22_rfc9134_video_hdr*);
  ipv4_base = &hdr_base->ipv4;
  /* update ipv4 hdr */
  ipv4->packet_id = ipv4_base->packet_id;

  rtp_base = &hdr_base->rtp;
  rte_memcpy(rtp, rtp_base, sizeof(*rtp));

  /* update mbuf */
  pkt_r->data_len = pkt_base->data_len;
  pkt_r->pkt_len = pkt_base->pkt_len;
  pkt_r->l2_len = pkt_base->l2_len;
  pkt_r->l3_len = pkt_base->l3_len;
  pkt_r->ol_flags = pkt_base->ol_flags;
  pkt_r->nb_segs = 2;
  /* chain mbuf */
  pkt_r->next = pkt_chain;

  if (!s->eth_has_chain[ST_SESSION_PORT_R]) {
    st_mbuf_chain_sw(pkt_r, pkt_chain);
  }

  rte_mbuf_refcnt_update(pkt_chain, 1);
  hdr->udp.dgram_len = htons(pkt_r->pkt_len - pkt_r->l2_len - pkt_r->l3_len);
  ipv4->total_length = htons(pkt_r->pkt_len - pkt_r->l2_len);
  if (!s->eth_ipv4_cksum_offload[ST_SESSION_PORT_R]) {
    /* generate cksum if no offload */
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4);
  }

  return 0;
}

static uint64_t tv_pacing_required_tai(struct st_tx_video_session_impl* s,
                                       enum st10_timestamp_fmt tfmt, uint64_t timestamp) {
  uint64_t required_tai = 0;

  if (!(s->ops.flags & ST20_TX_FLAG_USER_PACING)) return 0;
  if (!timestamp) return 0;

  if (tfmt == ST10_TIMESTAMP_FMT_MEDIA_CLK) {
    if (timestamp > 0xFFFFFFFF) {
      err("%s(%d), invalid timestamp %" PRIu64 "\n", __func__, s->idx, timestamp);
    }
    required_tai = st10_media_clk_to_ns((uint32_t)timestamp, 90 * 1000);
  } else {
    required_tai = timestamp;
  }

  return required_tai;
}

static int tv_tasklet_pre_start(void* priv) { return 0; }

static int tv_tasklet_start(void* priv) {
  struct st_tx_video_sessions_mgr* mgr = priv;
  struct st_main_impl* impl = mgr->parnet;
  struct st_tx_video_session_impl* s;

  for (int sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = tx_video_session_try_get(mgr, sidx);
    if (!s) continue;
    /* re-calculate the vsync */
    st_vsync_calculate(impl, &s->vsync);
    tx_video_session_put(mgr, sidx);
  }

  return 0;
}

static int tv_tasklet_stop(void* priv) { return 0; }

static int tv_tasklet_frame(struct st_main_impl* impl,
                            struct st_tx_video_session_impl* s) {
  unsigned int bulk = s->bulk;
  unsigned int n;
  int idx = s->idx;
  struct st20_tx_ops* ops = &s->ops;
  struct st_tx_video_pacing* pacing = &s->pacing;
  int ret;
  bool send_r = false;
  struct rte_mempool* hdr_pool_p = s->mbuf_mempool_hdr[ST_SESSION_PORT_P];
  struct rte_mempool* hdr_pool_r = NULL;
  struct rte_mempool* chain_pool = s->mbuf_mempool_chain;
  struct rte_ring* ring_p = s->ring[ST_SESSION_PORT_P];
  struct rte_ring* ring_r = NULL;

  if (rte_ring_full(ring_p)) {
    s->stat_build_ret_code = -STI_FRAME_RING_FULL;
    return ST_TASKLET_ALL_DONE;
  }

  if (s->ops.num_port > 1) {
    send_r = true;
    hdr_pool_r = s->mbuf_mempool_hdr[ST_SESSION_PORT_R];
    ring_r = s->ring[ST_SESSION_PORT_R];
  }

  /* check if any inflight pkts */
  if (s->has_inflight[ST_SESSION_PORT_P]) {
    n = rte_ring_sp_enqueue_bulk(ring_p, (void**)&s->inflight[ST_SESSION_PORT_P][0], bulk,
                                 NULL);
    if (n > 0) {
      s->has_inflight[ST_SESSION_PORT_P] = false;
    } else {
      s->stat_build_ret_code = -STI_FRAME_INFLIGHT_ENQUEUE_FAIL;
      return ST_TASKLET_ALL_DONE;
    }
  }
  if (send_r && s->has_inflight[ST_SESSION_PORT_R]) {
    n = rte_ring_sp_enqueue_bulk(ring_r, (void**)&s->inflight[ST_SESSION_PORT_R][0], bulk,
                                 NULL);
    if (n > 0) {
      s->has_inflight[ST_SESSION_PORT_R] = false;
    } else {
      s->stat_build_ret_code = -STI_FRAME_INFLIGHT_R_ENQUEUE_FAIL;
      return ST_TASKLET_ALL_DONE;
    }
  }

  if (0 == s->st20_pkt_idx) {
    if (ST21_TX_STAT_WAIT_FRAME == s->st20_frame_stat) {
      uint16_t next_frame_idx = 0;
      struct st20_tx_frame_meta meta;
      memset(&meta, 0, sizeof(meta));
      meta.width = ops->width;
      meta.height = ops->height;
      meta.fps = ops->fps;
      meta.fmt = ops->fmt;

      /* Query next frame buffer idx */
      ret = ops->get_next_frame(ops->priv, &next_frame_idx, &meta);
      if (ret < 0) { /* no frame ready from app */
        if (s->stat_user_busy_first) {
          s->stat_user_busy++;
          s->stat_user_busy_first = false;
          dbg("%s(%d), get_next_frame fail %d\n", __func__, idx, ret);
        }
        s->stat_build_ret_code = -STI_FRAME_APP_GET_FRAME_BUSY;
        return ST_TASKLET_ALL_DONE;
      }
      /* check frame refcnt */
      struct st_frame_trans* frame = &s->st20_frames[next_frame_idx];
      int refcnt = rte_atomic32_read(&frame->refcnt);
      if (refcnt) {
        info("%s(%d), frame %u refcnt not zero %d\n", __func__, idx, next_frame_idx,
             refcnt);
        s->stat_build_ret_code = -STI_FRAME_APP_ERR_TX_FRAME;
        return ST_TASKLET_ALL_DONE;
      }
      frame->tv_meta = meta;

      s->stat_user_busy_first = true;
      /* all check fine */
      rte_atomic32_inc(&frame->refcnt);
      s->st20_frame_idx = next_frame_idx;
      s->st20_frame_lines_ready = 0;
      dbg("%s(%d), next_frame_idx %d start\n", __func__, idx, next_frame_idx);
      s->st20_frame_stat = ST21_TX_STAT_SENDING_PKTS;

      /* user timestamp control if any */
      uint64_t required_tai = tv_pacing_required_tai(s, meta.tfmt, meta.timestamp);
      tv_sync_pacing(impl, s, false, required_tai);
      if (ops->flags & ST20_TX_FLAG_USER_TIMESTAMP) {
        pacing->rtp_time_stamp = st10_get_media_clk(meta.tfmt, meta.timestamp, 90 * 1000);
      }
      dbg("%s(%d), rtp time stamp %u\n", __func__, idx, pacing->rtp_time_stamp);
      frame->tv_meta.tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
      frame->tv_meta.timestamp = pacing->rtp_time_stamp;
    }
  }

  if (ops->type == ST20_TYPE_SLICE_LEVEL) {
    uint16_t line_number = 0;
    if (ops->packing == ST20_PACKING_GPM_SL) {
      line_number = (s->st20_pkt_idx + bulk) / s->st20_pkts_in_line;
    } else {
      uint32_t offset = s->st20_pkt_len * (s->st20_pkt_idx + bulk);
      line_number = offset / s->st20_bytes_in_line + 1;
    }
    if (line_number >= ops->height) line_number = ops->height - 1;
    if (line_number >= s->st20_frame_lines_ready) {
      struct st20_tx_slice_meta slice_meta;
      memset(&slice_meta, 0, sizeof(slice_meta));
      ret = ops->query_frame_lines_ready(ops->priv, s->st20_frame_idx, &slice_meta);
      if (ret >= 0) s->st20_frame_lines_ready = slice_meta.lines_ready;
      dbg("%s(%d), need line %u, ready lines %u\n", __func__, s->idx, ops->height,
          s->st20_frame_lines_ready);
      if ((ret < 0) || (line_number >= s->st20_frame_lines_ready)) {
        dbg("%s(%d), line %u not ready, ready lines %u\n", __func__, s->idx, line_number,
            s->st20_frame_lines_ready);
        s->stat_lines_not_ready++;
        s->stat_build_ret_code = -STI_FRAME_APP_SLICE_NOT_READY;
        return ST_TASKLET_ALL_DONE;
      }
    }
  }

  struct rte_mbuf* pkts[bulk];
  struct rte_mbuf* pkts_r[bulk];
  struct rte_mbuf* pkts_chain[bulk];

  ret = rte_pktmbuf_alloc_bulk(chain_pool, pkts_chain, bulk);
  if (ret < 0) {
    dbg("%s(%d), pkts chain alloc fail %d\n", __func__, idx, ret);
    s->stat_build_ret_code = -STI_FRAME_PKT_ALLOC_FAIL;
    return ST_TASKLET_ALL_DONE;
  }

  ret = rte_pktmbuf_alloc_bulk(hdr_pool_p, pkts, bulk);
  if (ret < 0) {
    dbg("%s(%d), pkts alloc fail %d\n", __func__, idx, ret);
    rte_pktmbuf_free_bulk(pkts_chain, bulk);
    s->stat_build_ret_code = -STI_FRAME_PKT_ALLOC_FAIL;
    return ST_TASKLET_ALL_DONE;
  }

  if (send_r) {
    ret = rte_pktmbuf_alloc_bulk(hdr_pool_r, pkts_r, bulk);
    if (ret < 0) {
      dbg("%s(%d), pkts_r alloc fail %d\n", __func__, idx, ret);
      rte_pktmbuf_free_bulk(pkts, bulk);
      rte_pktmbuf_free_bulk(pkts_chain, bulk);
      s->stat_build_ret_code = -STI_FRAME_PKT_ALLOC_FAIL;
      return ST_TASKLET_ALL_DONE;
    }
  }

  for (unsigned int i = 0; i < bulk; i++) {
    if (s->st20_pkt_idx >= s->st20_total_pkts) {
      s->stat_pkts_dummy++;
      rte_pktmbuf_free(pkts_chain[i]);
      st_tx_mbuf_set_idx(pkts[i], ST_TX_DUMMY_PKT_IDX);
    } else {
      tv_build_pkt(impl, s, pkts[i], pkts_chain[i]);
      st_tx_mbuf_set_idx(pkts[i], s->st20_pkt_idx);
    }
    pacing_set_mbuf_time_stamp(pkts[i], pacing);

    if (send_r) {
      if (s->st20_pkt_idx >= s->st20_total_pkts) {
        st_tx_mbuf_set_idx(pkts_r[i], ST_TX_DUMMY_PKT_IDX);
      } else {
        tv_build_redundant(s, pkts_r[i], pkts[i], pkts_chain[i]);
        st_tx_mbuf_set_idx(pkts_r[i], s->st20_pkt_idx);
      }
      pacing_set_mbuf_time_stamp(pkts_r[i], pacing);
    }

    pacing_foward_cursor(pacing); /* pkt foward */
    s->st20_pkt_idx++;
    s->stat_pkts_build++;
  }

  bool done = false;
  n = rte_ring_sp_enqueue_bulk(ring_p, (void**)&pkts[0], bulk, NULL);
  if (n == 0) {
    for (unsigned int i = 0; i < bulk; i++) s->inflight[ST_SESSION_PORT_P][i] = pkts[i];
    s->has_inflight[ST_SESSION_PORT_P] = true;
    s->inflight_cnt[ST_SESSION_PORT_P]++;
    s->stat_build_ret_code = -STI_FRAME_PKT_ENQUEUE_FAIL;
    done = true;
  }
  if (send_r) {
    n = rte_ring_sp_enqueue_bulk(ring_r, (void**)&pkts_r[0], bulk, NULL);
    if (n == 0) {
      for (unsigned int i = 0; i < bulk; i++)
        s->inflight[ST_SESSION_PORT_R][i] = pkts_r[i];
      s->has_inflight[ST_SESSION_PORT_R] = true;
      s->inflight_cnt[ST_SESSION_PORT_R]++;
      s->stat_build_ret_code = -STI_FRAME_PKT_R_ENQUEUE_FAIL;
      done = true;
    }
  }

  if (s->st20_pkt_idx >= s->st20_total_pkts) {
    dbg("%s(%d), frame %d done with %d pkts\n", __func__, idx, s->st20_frame_idx,
        s->st20_pkt_idx);
    /* end of current frame */
    s->st20_frame_stat = ST21_TX_STAT_WAIT_FRAME;
    s->st20_pkt_idx = 0;
    rte_atomic32_inc(&s->stat_frame_cnt);

    uint64_t frame_end_time = st_get_tsc(impl);
    if (frame_end_time > pacing->tsc_time_cursor) {
      s->stat_exceed_frame_time++;
      dbg("%s(%d), frame %d build time out %fus\n", __func__, idx, s->st20_frame_idx,
          (frame_end_time - pacing->tsc_time_cursor) / NS_PER_US);
    }
  }

  return done ? ST_TASKLET_ALL_DONE : ST_TASKLET_HAS_PENDING;
}

static int tv_tasklet_rtp(struct st_main_impl* impl, struct st_tx_video_session_impl* s) {
  unsigned int bulk = s->bulk;
  unsigned int n;
#ifdef DEBUG
  int idx = s->idx;
#endif
  struct st_tx_video_pacing* pacing = &s->pacing;
  int ret;
  bool send_r = false;
  struct rte_mempool* hdr_pool_p = s->mbuf_mempool_hdr[ST_SESSION_PORT_P];
  struct rte_mempool* hdr_pool_r = NULL;
  struct rte_ring* ring_p = s->ring[ST_SESSION_PORT_P];
  struct rte_ring* ring_r = NULL;

  if (rte_ring_full(ring_p)) {
    s->stat_build_ret_code = -STI_RTP_RING_FULL;
    return ST_TASKLET_ALL_DONE;
  }

  if (s->ops.num_port > 1) {
    send_r = true;
    hdr_pool_r = s->mbuf_mempool_hdr[ST_SESSION_PORT_R];
    ring_r = s->ring[ST_SESSION_PORT_R];
  }

  /* check if any inflight pkts */
  if (s->has_inflight[ST_SESSION_PORT_P]) {
    n = rte_ring_sp_enqueue_bulk(ring_p, (void**)&s->inflight[ST_SESSION_PORT_P][0], bulk,
                                 NULL);
    if (n > 0) {
      s->has_inflight[ST_SESSION_PORT_P] = false;
    } else {
      s->stat_build_ret_code = -STI_RTP_INFLIGHT_ENQUEUE_FAIL;
      return ST_TASKLET_ALL_DONE;
    }
  }
  if (send_r && s->has_inflight[ST_SESSION_PORT_R]) {
    n = rte_ring_sp_enqueue_bulk(ring_r, (void**)&s->inflight[ST_SESSION_PORT_R][0], bulk,
                                 NULL);
    if (n > 0) {
      s->has_inflight[ST_SESSION_PORT_R] = false;
    } else {
      s->stat_build_ret_code = -STI_RTP_INFLIGHT_R_ENQUEUE_FAIL;
      return ST_TASKLET_ALL_DONE;
    }
  }

  struct rte_mbuf* pkts[bulk];
  struct rte_mbuf* pkts_r[bulk];
  struct rte_mbuf* pkts_chain[bulk];
  int pkts_remaining = s->st20_total_pkts - s->st20_pkt_idx;
  bool eof = (pkts_remaining > 0) && (pkts_remaining < bulk) ? true : false;
  unsigned int pkts_bulk = eof ? 1 : bulk; /* bulk one only at end of frame */

  if (eof)
    dbg("%s(%d), pkts_bulk %d pkt idx %d\n", __func__, idx, pkts_bulk, s->st20_pkt_idx);

  n = st_rte_ring_sc_dequeue_bulk(s->packet_ring, (void**)&pkts_chain, pkts_bulk, NULL);
  if (n == 0) {
    if (s->stat_user_busy_first) {
      s->stat_user_busy++;
      s->stat_user_busy_first = false;
      dbg("%s(%d), rtp pkts not ready %d, ring cnt %d\n", __func__, idx, ret,
          rte_ring_count(s->packet_ring));
    }
    s->stat_build_ret_code = -STI_RTP_APP_DEQUEUE_FAIL;
    return ST_TASKLET_ALL_DONE;
  }
  s->stat_user_busy_first = true;
  s->ops.notify_rtp_done(s->ops.priv);

  ret = rte_pktmbuf_alloc_bulk(hdr_pool_p, pkts, bulk);
  if (ret < 0) {
    dbg("%s(%d), pkts alloc fail %d\n", __func__, idx, ret);
    rte_pktmbuf_free_bulk(pkts_chain, bulk);
    s->stat_build_ret_code = -STI_RTP_PKT_ALLOC_FAIL;
    return ST_TASKLET_ALL_DONE;
  }

  if (send_r) {
    ret = rte_pktmbuf_alloc_bulk(hdr_pool_r, pkts_r, bulk);
    if (ret < 0) {
      dbg("%s(%d), pkts_r alloc fail %d\n", __func__, idx, ret);
      rte_pktmbuf_free_bulk(pkts, bulk);
      rte_pktmbuf_free_bulk(pkts_chain, bulk);
      s->stat_build_ret_code = -STI_RTP_PKT_ALLOC_FAIL;
      return ST_TASKLET_ALL_DONE;
    }
  }

  for (unsigned int i = 0; i < pkts_bulk; i++) {
    tv_build_rtp(impl, s, pkts[i], pkts_chain[i]);
    st_tx_mbuf_set_idx(pkts[i], s->st20_pkt_idx);
    pacing_set_mbuf_time_stamp(pkts[i], pacing);

    if (send_r) {
      tv_build_redundant_rtp(s, pkts_r[i], pkts[i], pkts_chain[i]);
      st_tx_mbuf_set_idx(pkts_r[i], s->st20_pkt_idx);
      pacing_set_mbuf_time_stamp(pkts_r[i], pacing);
    }

    pacing_foward_cursor(pacing); /* pkt foward */
    s->st20_pkt_idx++;
    s->stat_pkts_build++;
  }

  /* build dummy bulk pkts to satisfy video transmitter which is bulk based */
  if (eof) {
    for (unsigned int i = pkts_bulk; i < bulk; i++) {
      st_tx_mbuf_set_idx(pkts[i], ST_TX_DUMMY_PKT_IDX);
      pacing_set_mbuf_time_stamp(pkts[i], pacing);
      if (send_r) {
        st_tx_mbuf_set_idx(pkts_r[i], ST_TX_DUMMY_PKT_IDX);
        pacing_set_mbuf_time_stamp(pkts_r[i], pacing);
      }
      s->stat_pkts_dummy++;
    }
  }

  bool done = false;
  n = rte_ring_sp_enqueue_bulk(ring_p, (void**)&pkts[0], bulk, NULL);
  if (n == 0) {
    for (unsigned int i = 0; i < bulk; i++) s->inflight[ST_SESSION_PORT_P][i] = pkts[i];
    s->has_inflight[ST_SESSION_PORT_P] = true;
    s->inflight_cnt[ST_SESSION_PORT_P]++;
    s->stat_build_ret_code = -STI_RTP_PKT_ENQUEUE_FAIL;
    done = true;
  }
  if (send_r) {
    n = rte_ring_sp_enqueue_bulk(ring_r, (void**)&pkts_r[0], bulk, NULL);
    if (n == 0) {
      for (unsigned int i = 0; i < bulk; i++)
        s->inflight[ST_SESSION_PORT_R][i] = pkts_r[i];
      s->has_inflight[ST_SESSION_PORT_R] = true;
      s->inflight_cnt[ST_SESSION_PORT_R]++;
      s->stat_build_ret_code = -STI_RTP_PKT_R_ENQUEUE_FAIL;
      done = true;
    }
  }

  return done ? ST_TASKLET_ALL_DONE : ST_TASKLET_HAS_PENDING;
}

static int tv_tasklet_st22(struct st_main_impl* impl,
                           struct st_tx_video_session_impl* s) {
  unsigned int bulk = s->bulk;
  unsigned int n;
  int idx = s->idx;
  struct st20_tx_ops* ops = &s->ops;
  struct st22_tx_video_info* st22_info = s->st22_info;
  struct st_tx_video_pacing* pacing = &s->pacing;
  int ret;
  bool send_r = false;
  struct rte_mempool* hdr_pool_p = s->mbuf_mempool_hdr[ST_SESSION_PORT_P];
  struct rte_mempool* hdr_pool_r = NULL;
  struct rte_mempool* chain_pool = s->mbuf_mempool_chain;
  struct rte_ring* ring_p = s->ring[ST_SESSION_PORT_P];
  struct rte_ring* ring_r = NULL;

  if (rte_ring_full(ring_p)) {
    s->stat_build_ret_code = -STI_ST22_RING_FULL;
    return ST_TASKLET_ALL_DONE;
  }

  if (s->ops.num_port > 1) {
    send_r = true;
    hdr_pool_r = s->mbuf_mempool_hdr[ST_SESSION_PORT_R];
    ring_r = s->ring[ST_SESSION_PORT_R];
  }

  /* check if any inflight pkts */
  if (s->has_inflight[ST_SESSION_PORT_P]) {
    n = rte_ring_sp_enqueue_bulk(ring_p, (void**)&s->inflight[ST_SESSION_PORT_P][0], bulk,
                                 NULL);
    if (n > 0) {
      s->has_inflight[ST_SESSION_PORT_P] = false;
    } else {
      s->stat_build_ret_code = -STI_ST22_INFLIGHT_ENQUEUE_FAIL;
      return ST_TASKLET_ALL_DONE;
    }
  }
  if (send_r && s->has_inflight[ST_SESSION_PORT_R]) {
    n = rte_ring_sp_enqueue_bulk(ring_r, (void**)&s->inflight[ST_SESSION_PORT_R][0], bulk,
                                 NULL);
    if (n > 0) {
      s->has_inflight[ST_SESSION_PORT_R] = false;
    } else {
      s->stat_build_ret_code = -STI_ST22_INFLIGHT_R_ENQUEUE_FAIL;
      return ST_TASKLET_ALL_DONE;
    }
  }

  if (0 == s->st20_pkt_idx) {
    if (ST21_TX_STAT_WAIT_FRAME == s->st20_frame_stat) {
      uint16_t next_frame_idx;
      struct st22_tx_frame_meta meta;
      memset(&meta, 0, sizeof(meta));
      meta.width = ops->width;
      meta.height = ops->height;
      meta.fps = ops->fps;
      meta.codestream_size = s->st22_codestream_size;

      /* Query next frame buffer idx */
      ret = st22_info->get_next_frame(ops->priv, &next_frame_idx, &meta);
      if (ret < 0) { /* no frame ready from app */
        if (s->stat_user_busy_first) {
          s->stat_user_busy++;
          s->stat_user_busy_first = false;
          dbg("%s(%d), get_next_frame fail %d\n", __func__, idx, ret);
        }
        s->stat_build_ret_code = -STI_ST22_APP_GET_FRAME_BUSY;
        return ST_TASKLET_ALL_DONE;
      }
      /* check frame refcnt */
      struct st_frame_trans* frame = &s->st20_frames[next_frame_idx];
      int refcnt = rte_atomic32_read(&frame->refcnt);
      if (refcnt) {
        info("%s(%d), frame %u refcnt not zero %d\n", __func__, idx, next_frame_idx,
             refcnt);
        s->stat_build_ret_code = -STI_ST22_APP_ERR_TX_FRAME;
        return ST_TASKLET_ALL_DONE;
      }
      /* check code stream size */
      size_t codestream_size = meta.codestream_size;
      if ((codestream_size > s->st22_codestream_size) || !codestream_size) {
        err("%s(%d), invalid codestream size %" PRIu64 ", allowed %" PRIu64 "\n",
            __func__, idx, codestream_size, s->st22_codestream_size);
        tv_notify_frame_done(s, next_frame_idx);
        s->stat_build_ret_code = -STI_ST22_APP_GET_FRAME_ERR_SIZE;
        return ST_TASKLET_ALL_DONE;
      }

      s->stat_user_busy_first = true;
      /* all check fine */
      frame->tx_st22_meta = meta;
      rte_atomic32_inc(&frame->refcnt);
      size_t frame_size = codestream_size + s->st22_box_hdr_length;
      st22_info->st22_total_pkts = frame_size / s->st20_pkt_len;
      if (frame_size % s->st20_pkt_len) st22_info->st22_total_pkts++;
      s->st20_total_pkts = st22_info->st22_total_pkts;
      /* wa for attach_extbuf issue with too less pkts */
      if (s->st20_total_pkts < 40) s->st20_total_pkts = 40;
      st22_info->cur_frame_size = frame_size;
      s->st20_frame_idx = next_frame_idx;
      s->st20_frame_stat = ST21_TX_STAT_SENDING_PKTS;

      /* user timestamp control if any */
      uint64_t required_tai = tv_pacing_required_tai(s, meta.tfmt, meta.timestamp);
      tv_sync_pacing(impl, s, false, required_tai);
      if (ops->flags & ST20_TX_FLAG_USER_TIMESTAMP) {
        pacing->rtp_time_stamp = st10_get_media_clk(meta.tfmt, meta.timestamp, 90 * 1000);
      }
      dbg("%s(%d), rtp time stamp %u\n", __func__, idx, pacing->rtp_time_stamp);
      frame->tx_st22_meta.tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
      frame->tx_st22_meta.timestamp = pacing->rtp_time_stamp;
      dbg("%s(%d), next_frame_idx %d(%d pkts) start\n", __func__, idx, next_frame_idx,
          s->st20_total_pkts);
      dbg("%s(%d), codestream_size %ld time_stamp %u\n", __func__, idx, codestream_size,
          pacing->rtp_time_stamp);
    }
  }

  struct rte_mbuf* pkts[bulk];
  struct rte_mbuf* pkts_r[bulk];

  if (s->st20_pkt_idx >= st22_info->st22_total_pkts) { /* build pad */
    struct rte_mbuf* pad = s->pad[ST_SESSION_PORT_P][ST20_PKT_TYPE_NORMAL];
    struct rte_mbuf* pad_r = s->pad[ST_SESSION_PORT_R][ST20_PKT_TYPE_NORMAL];

    for (unsigned int i = 0; i < bulk; i++) {
      dbg("%s(%d), pad on pkt %d\n", __func__, s->idx, s->st20_pkt_idx);
      pkts[i] = pad;
      rte_mbuf_refcnt_update(pad, 1);
      st_tx_mbuf_set_idx(pkts[i], s->st20_pkt_idx);
      pacing_set_mbuf_time_stamp(pkts[i], pacing);

      if (send_r) {
        pkts_r[i] = pad_r;
        rte_mbuf_refcnt_update(pad_r, 1);
        st_tx_mbuf_set_idx(pkts_r[i], s->st20_pkt_idx);
      }

      pacing_foward_cursor(pacing); /* pkt foward */
      s->st20_pkt_idx++;
      s->stat_pkts_build++;
      s->stat_pkts_dummy++;
    }
  } else {
    struct rte_mbuf* pkts_chain[bulk];

    ret = rte_pktmbuf_alloc_bulk(chain_pool, pkts_chain, bulk);
    if (ret < 0) {
      dbg("%s(%d), pkts chain alloc fail %d\n", __func__, idx, ret);
      s->stat_build_ret_code = -STI_ST22_PKT_ALLOC_FAIL;
      return ST_TASKLET_ALL_DONE;
    }

    ret = rte_pktmbuf_alloc_bulk(hdr_pool_p, pkts, bulk);
    if (ret < 0) {
      dbg("%s(%d), pkts alloc fail %d\n", __func__, idx, ret);
      rte_pktmbuf_free_bulk(pkts_chain, bulk);
      s->stat_build_ret_code = -STI_ST22_PKT_ALLOC_FAIL;
      return ST_TASKLET_ALL_DONE;
    }

    if (send_r) {
      ret = rte_pktmbuf_alloc_bulk(hdr_pool_r, pkts_r, bulk);
      if (ret < 0) {
        dbg("%s(%d), pkts_r alloc fail %d\n", __func__, idx, ret);
        rte_pktmbuf_free_bulk(pkts, bulk);
        rte_pktmbuf_free_bulk(pkts_chain, bulk);
        s->stat_build_ret_code = -STI_ST22_PKT_ALLOC_FAIL;
        return ST_TASKLET_ALL_DONE;
      }
    }

    for (unsigned int i = 0; i < bulk; i++) {
      if (s->st20_pkt_idx >= st22_info->st22_total_pkts) {
        dbg("%s(%d), pad on pkt %d\n", __func__, s->idx, s->st20_pkt_idx);
        s->stat_pkts_dummy++;
        rte_pktmbuf_free(pkts_chain[i]);
        st_tx_mbuf_set_idx(pkts[i], ST_TX_DUMMY_PKT_IDX);
      } else {
        tv_build_st22(impl, s, pkts[i], pkts_chain[i]);
        st_tx_mbuf_set_idx(pkts[i], s->st20_pkt_idx);
      }
      pacing_set_mbuf_time_stamp(pkts[i], pacing);

      if (send_r) {
        if (s->st20_pkt_idx >= st22_info->st22_total_pkts) {
          st_tx_mbuf_set_idx(pkts_r[i], ST_TX_DUMMY_PKT_IDX);
        } else {
          tv_build_st22_redundant(s, pkts_r[i], pkts[i], pkts_chain[i]);
          st_tx_mbuf_set_idx(pkts_r[i], s->st20_pkt_idx);
        }
        pacing_set_mbuf_time_stamp(pkts_r[i], pacing);
      }

      pacing_foward_cursor(pacing); /* pkt foward */
      s->st20_pkt_idx++;
      s->stat_pkts_build++;
    }
  }

  bool done = false;
  n = rte_ring_sp_enqueue_bulk(ring_p, (void**)&pkts[0], bulk, NULL);
  if (n == 0) {
    for (unsigned int i = 0; i < bulk; i++) s->inflight[ST_SESSION_PORT_P][i] = pkts[i];
    s->has_inflight[ST_SESSION_PORT_P] = true;
    s->inflight_cnt[ST_SESSION_PORT_P]++;
    s->stat_build_ret_code = -STI_ST22_PKT_ENQUEUE_FAIL;
    done = true;
  }
  if (send_r) {
    n = rte_ring_sp_enqueue_bulk(ring_r, (void**)&pkts_r[0], bulk, NULL);
    if (n == 0) {
      for (unsigned int i = 0; i < bulk; i++)
        s->inflight[ST_SESSION_PORT_R][i] = pkts_r[i];
      s->has_inflight[ST_SESSION_PORT_R] = true;
      s->inflight_cnt[ST_SESSION_PORT_R]++;
      s->stat_build_ret_code = -STI_ST22_PKT_R_ENQUEUE_FAIL;
      done = true;
    }
  }

  if (s->st20_pkt_idx >= s->st20_total_pkts) {
    dbg("%s(%d), frame %d done with %d pkts\n", __func__, idx, s->st20_frame_idx,
        s->st20_pkt_idx);
    /* end of current frame */
    s->st20_frame_stat = ST21_TX_STAT_WAIT_FRAME;
    s->st20_pkt_idx = 0;
    rte_atomic32_inc(&s->stat_frame_cnt);
    st22_info->frame_idx++;

    uint64_t frame_end_time = st_get_tsc(impl);
    if (frame_end_time > pacing->tsc_time_cursor) {
      s->stat_exceed_frame_time++;
      dbg("%s(%d), frame %d build time out %fus\n", __func__, idx, s->st20_frame_idx,
          (frame_end_time - pacing->tsc_time_cursor) / NS_PER_US);
    }
  }

  return done ? ST_TASKLET_ALL_DONE : ST_TASKLET_HAS_PENDING;
}

static int tvs_tasklet_handler(void* priv) {
  struct st_tx_video_sessions_mgr* mgr = priv;
  struct st_main_impl* impl = mgr->parnet;
  struct st_tx_video_session_impl* s;
  int pending = ST_TASKLET_ALL_DONE;

  for (int sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = tx_video_session_try_get(mgr, sidx);
    if (!s) continue;

    /* check vsync if it has callback */
    if (s->ops.notify_event) tv_poll_vsync(impl, s);

    s->stat_build_ret_code = 0;
    if (s->st22_info)
      pending = tv_tasklet_st22(impl, s);
    else if (st20_is_frame_type(s->ops.type))
      pending = tv_tasklet_frame(impl, s);
    else
      pending = tv_tasklet_rtp(impl, s);

    tx_video_session_put(mgr, sidx);
  }

  return pending;
}

static int tv_uinit_hw(struct st_main_impl* impl, struct st_tx_video_session_impl* s) {
  enum st_port port;
  int num_port = s->ops.num_port;

  for (int i = 0; i < num_port; i++) {
    port = st_port_logic2phy(s->port_maps, i);

    if (s->ring[i]) {
      st_ring_dequeue_clean(s->ring[i]);
      rte_ring_free(s->ring[i]);
      s->ring[i] = NULL;
    }

    if (s->queue_active[i]) {
      struct rte_mbuf* pad = s->pad[i][ST20_PKT_TYPE_NORMAL];
      /* flush all the pkts in the tx ring desc */
      if (pad) st_dev_flush_tx_queue(impl, port, s->queue_id[i], pad);
      st_dev_free_tx_queue(impl, port, s->queue_id[i]);
      s->queue_active[i] = false;
    }

    for (int j = 0; j < ST20_PKT_TYPE_MAX; j++) {
      if (s->pad[i][j]) {
        rte_pktmbuf_free(s->pad[i][j]);
        s->pad[i][j] = NULL;
      }
    }
  }

  return 0;
}

static int tv_init_hw(struct st_main_impl* impl, struct st_tx_video_sessions_mgr* mgr,
                      struct st_tx_video_session_impl* s) {
  unsigned int flags, count;
  struct rte_ring* ring;
  char ring_name[32];
  int mgr_idx = mgr->idx, idx = s->idx, num_port = s->ops.num_port;
  int ret;
  uint16_t queue = 0;
  uint16_t port_id;
  struct rte_mempool* pad_mempool;
  struct rte_mbuf* pad;
  enum st_port port;

  for (int i = 0; i < num_port; i++) {
    port = st_port_logic2phy(s->port_maps, i);
    port_id = st_port_id(impl, port);

    ret = st_dev_request_tx_queue(impl, port, &queue, tv_rl_bps(s));
    if (ret < 0) {
      tv_uinit_hw(impl, s);
      return ret;
    }
    s->queue_id[i] = queue;
    s->queue_active[i] = true;
    s->port_id[i] = port_id;

    snprintf(ring_name, 32, "TX-VIDEO-RING-M%d-R%d-P%d", mgr_idx, idx, i);
    flags = RING_F_SP_ENQ | RING_F_SC_DEQ; /* single-producer and single-consumer */
    count = s->ring_count;
    ring = rte_ring_create(ring_name, count, st_socket_id(impl, i), flags);
    if (!ring) {
      err("%s(%d,%d), rte_ring_create fail for port %d\n", __func__, mgr_idx, idx, i);
      tv_uinit_hw(impl, s);
      return -ENOMEM;
    }
    s->ring[i] = ring;
    info("%s(%d,%d), port(l:%d,p:%d), queue %d, count %u\n", __func__, mgr_idx, idx, i,
         port, queue, count);

    if (st_pmd_is_kernel(impl, port) && s->mbuf_mempool_reuse_rx[i]) {
      if (s->mbuf_mempool_hdr[i]) {
        err("%s(%d,%d), fail to reuse rx, has mempool_hdr for port %d\n", __func__,
            mgr_idx, idx, i);
      } else {
        /* reuse rx mempool for zero copy */
        if (st_has_rx_mono_pool(impl))
          s->mbuf_mempool_hdr[i] = st_get_rx_mempool(impl, port);
        else
          s->mbuf_mempool_hdr[i] = st_if(impl, port)->rx_queues[queue].mbuf_pool;
        info("%s(%d,%d), reuse rx mempool(%p) for port %d\n", __func__, mgr_idx, idx,
             s->mbuf_mempool_hdr[i], i);
      }
    }

    if (false & st_pmd_is_kernel(impl, port)) {
      /* disable now, alwasy use no zc mempool for the flush pad */
      pad_mempool = s->mbuf_mempool_hdr[i];
    } else {
      pad_mempool = st_get_tx_mempool(impl, port);
    }
    for (int j = 0; j < ST20_PKT_TYPE_MAX; j++) {
      if (!s->st20_pkt_info[j].number) continue;
      pad = st_build_pad(impl, pad_mempool, port_id, RTE_ETHER_TYPE_IPV4,
                         s->st20_pkt_info[j].size);
      if (!pad) {
        tv_uinit_hw(impl, s);
        return -ENOMEM;
      }
      s->pad[i][j] = pad;
    }
  }

  return 0;
}

static int tv_mempool_free(struct st_tx_video_session_impl* s) {
  int ret;

  if (s->mbuf_mempool_chain && !s->tx_mono_pool) {
    ret = st_mempool_free(s->mbuf_mempool_chain);
    if (ret >= 0) s->mbuf_mempool_chain = NULL;
  }

  for (int i = 0; i < ST_SESSION_PORT_MAX; i++) {
    if (s->mbuf_mempool_hdr[i]) {
      if (!s->mbuf_mempool_reuse_rx[i] && !s->tx_mono_pool)
        ret = st_mempool_free(s->mbuf_mempool_hdr[i]);
      else
        ret = 0;
      if (ret >= 0) s->mbuf_mempool_hdr[i] = NULL;
    }
  }

  return 0;
}

static bool tv_has_chain_buf(struct st_tx_video_session_impl* s) {
  struct st20_tx_ops* ops = &s->ops;
  int num_ports = ops->num_port;

  for (int port = 0; port < num_ports; port++) {
    if (!s->eth_has_chain[port]) return false;
  }

  /* all ports capable chain */
  return true;
}

static int tv_mempool_init(struct st_main_impl* impl,
                           struct st_tx_video_sessions_mgr* mgr,
                           struct st_tx_video_session_impl* s) {
  struct st20_tx_ops* ops = &s->ops;
  int num_port = ops->num_port, idx = s->idx;
  enum st_port port;
  unsigned int n;
  uint16_t hdr_room_size = 0;
  uint16_t chain_room_size = 0;

  if (!tv_has_chain_buf(s)) {
    /* no chain buffer support in the driver */
    hdr_room_size = s->st20_pkt_size;
    if (ops->type == ST20_TYPE_RTP_LEVEL)
      chain_room_size = s->rtp_pkt_max_size;
    else
      chain_room_size = 0;
  } else if (s->st22_info) {
    hdr_room_size = sizeof(struct st22_rfc9134_video_hdr);
    /* attach extbuf used, only placeholder mbuf */
    chain_room_size = 0;
  } else if (ops->type == ST20_TYPE_RTP_LEVEL) {
    hdr_room_size = sizeof(struct st_base_hdr);
    chain_room_size = s->rtp_pkt_max_size;
  } else { /* frame level */
    hdr_room_size = sizeof(struct st_rfc4175_video_hdr);
    if (ops->packing != ST20_PACKING_GPM_SL)
      hdr_room_size += sizeof(struct st20_rfc4175_extra_rtp_hdr);
    /* attach extbuf used, only placeholder mbuf */
    chain_room_size = 0;
    if (s->st20_linesize > s->st20_bytes_in_line) { /* lines have padding */
      if (s->ops.packing != ST20_PACKING_GPM_SL) /* and there is packet acrossing lines */
        chain_room_size = s->st20_pkt_len;
    }
  }

  for (int i = 0; i < num_port; i++) {
    port = st_port_logic2phy(s->port_maps, i);
    if (s->mbuf_mempool_reuse_rx[i]) {
      s->mbuf_mempool_hdr[i] = NULL; /* reuse rx mempool for zero copy */
    } else if (s->tx_mono_pool) {
      s->mbuf_mempool_hdr[i] = st_get_tx_mempool(impl, port);
      info("%s(%d), use tx mono hdr mempool(%p) for port %d\n", __func__, idx,
           s->mbuf_mempool_hdr[i], i);
    } else {
      n = st_if_nb_tx_desc(impl, port) + s->ring_count;
      if (s->mbuf_mempool_hdr[i]) {
        warn("%s(%d), use previous hdr mempool for port %d\n", __func__, idx, i);
      } else {
        char pool_name[32];
        snprintf(pool_name, 32, "TXVIDEOHDR-M%d-R%d-P%d", mgr->idx, idx, i);
        struct rte_mempool* mbuf_pool =
            st_mempool_create(impl, port, pool_name, n, ST_MBUF_CACHE_SIZE,
                              sizeof(struct st_muf_priv_data), hdr_room_size);
        if (!mbuf_pool) {
          tv_mempool_free(s);
          return -ENOMEM;
        }
        s->mbuf_mempool_hdr[i] = mbuf_pool;
      }
    }
  }

  port = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_P);
  n = st_if_nb_tx_desc(impl, port) + s->ring_count;
  if (ops->type == ST20_TYPE_RTP_LEVEL) n += ops->rtp_ring_size;

  if (s->tx_mono_pool) {
    s->mbuf_mempool_chain = st_get_tx_mempool(impl, port);
    info("%s(%d), use tx mono chain mempool(%p)\n", __func__, idx, s->mbuf_mempool_chain);
  } else if (s->mbuf_mempool_chain) {
    warn("%s(%d), use previous chain mempool\n", __func__, idx);
  } else {
    char pool_name[32];
    snprintf(pool_name, 32, "TXVIDEOCHAIN-M%d-R%d", mgr->idx, idx);
    struct rte_mempool* mbuf_pool = st_mempool_create(
        impl, port, pool_name, n, ST_MBUF_CACHE_SIZE, 0, chain_room_size);
    if (!mbuf_pool) {
      tv_mempool_free(s);
      return -ENOMEM;
    }
    s->mbuf_mempool_chain = mbuf_pool;
  }

  return 0;
}

static int tv_init_packet_ring(struct st_main_impl* impl,
                               struct st_tx_video_sessions_mgr* mgr,
                               struct st_tx_video_session_impl* s) {
  char ring_name[32];
  struct rte_ring* ring;
  unsigned int flags, count = s->ops.rtp_ring_size;
  int mgr_idx = mgr->idx, idx = s->idx;
  enum st_port port = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_P);

  snprintf(ring_name, 32, "TX-VIDEO-PACKET-RING-M%d-R%d", mgr_idx, idx);
  flags = RING_F_SP_ENQ | RING_F_SC_DEQ; /* single-producer and single-consumer */
  ring = rte_ring_create(ring_name, count, st_socket_id(impl, port), flags);
  if (!ring) {
    err("%s(%d,%d), rte_ring_create fail\n", __func__, mgr_idx, idx);
    return -ENOMEM;
  }
  s->packet_ring = ring;
  info("%s(%d,%d), succ\n", __func__, mgr_idx, idx);
  return 0;
}

static int tv_uinit_sw(struct st_tx_video_session_impl* s) {
  int num_port = s->ops.num_port;

  for (int i = 0; i < num_port; i++) {
    /* free all inflight */
    if (s->has_inflight[i]) {
      rte_pktmbuf_free_bulk(&s->inflight[i][0], s->bulk);
      s->has_inflight[i] = false;
    }
    if (s->trs_inflight_num[i]) {
      rte_pktmbuf_free_bulk(&s->trs_inflight[i][s->trs_inflight_idx[i]],
                            s->trs_inflight_num[i]);
      s->trs_inflight_num[i] = 0;
    }
    if (s->trs_inflight_num2[i]) {
      rte_pktmbuf_free_bulk(&s->trs_inflight2[i][s->trs_inflight_idx2[i]],
                            s->trs_inflight_num2[i]);
      s->trs_inflight_num2[i] = 0;
    }
  }

  if (s->packet_ring) {
    st_ring_dequeue_clean(s->packet_ring);
    rte_ring_free(s->packet_ring);
    s->packet_ring = NULL;
  }

  tv_mempool_free(s);

  tv_free_frames(s);

  if (s->st22_info) {
    st_rte_free(s->st22_info);
    s->st22_info = NULL;
  }

  return 0;
}

static int tv_init_st22_frame(struct st_main_impl* impl,
                              struct st_tx_video_session_impl* s,
                              struct st22_tx_ops* st22_frame_ops) {
  struct st22_tx_video_info* st22_info;

  st22_info = st_rte_zmalloc_socket(sizeof(*st22_info), st_socket_id(impl, ST_PORT_P));
  if (!st22_info) return -ENOMEM;

  st22_info->get_next_frame = st22_frame_ops->get_next_frame;
  st22_info->notify_frame_done = st22_frame_ops->notify_frame_done;

  s->st22_info = st22_info;

  return 0;
}

static int tv_init_sw(struct st_main_impl* impl, struct st_tx_video_sessions_mgr* mgr,
                      struct st_tx_video_session_impl* s,
                      struct st22_tx_ops* st22_frame_ops) {
  int idx = s->idx, ret;
  enum st20_type type = s->ops.type;

  if (st22_frame_ops) {
    ret = tv_init_st22_frame(impl, s, st22_frame_ops);
    if (ret < 0) {
      err("%s(%d), tv_init_sw fail %d\n", __func__, idx, ret);
      tv_uinit_sw(s);
      return -EIO;
    }
    tv_init_st22_boxes(impl, s);
  }

  /* free the pool if any in previous session */
  tv_mempool_free(s);
  ret = tv_mempool_init(impl, mgr, s);
  if (ret < 0) {
    err("%s(%d), fail %d\n", __func__, idx, ret);
    tv_uinit_sw(s);
    return ret;
  }

  if (type == ST20_TYPE_RTP_LEVEL)
    ret = tv_init_packet_ring(impl, mgr, s);
  else
    ret = tv_alloc_frames(impl, s);
  if (ret < 0) {
    err("%s(%d), fail %d\n", __func__, idx, ret);
    tv_uinit_sw(s);
    return ret;
  }

  return 0;
}

static int tv_init_pkt(struct st_main_impl* impl, struct st_tx_video_session_impl* s,
                       struct st20_tx_ops* ops, enum st_session_type s_type,
                       struct st22_tx_ops* st22_frame_ops) {
  int idx = s->idx;
  uint32_t height = ops->interlaced ? (ops->height >> 1) : ops->height;
  enum st20_type type = ops->type;

  /* clear pkt info */
  memset(&s->st20_pkt_info[0], 0,
         sizeof(struct st20_packet_group_info) * ST20_PKT_TYPE_MAX);

  /* 4800 if 1080p yuv422 */
  s->st20_bytes_in_line = ops->width * s->st20_pg.size / s->st20_pg.coverage;
  /* rtp mode only  */
  s->rtp_pkt_max_size = ops->rtp_pkt_size;

  if (st22_frame_ops) { /* st22 frame mode */
    int max_data_len =
        impl->pkt_udp_suggest_max_size - sizeof(struct st22_rfc9134_rtp_hdr);
    uint32_t align = 128;
    max_data_len = max_data_len / align * align;
    s->st20_total_pkts = s->st20_frame_size / max_data_len;
    if (s->st20_frame_size % max_data_len) s->st20_total_pkts++;
    s->st20_pkt_len = max_data_len;
    s->st20_pkt_size = s->st20_pkt_len + sizeof(struct st22_rfc9134_rtp_hdr);
    /* assume all are normal */
    s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].size = s->st20_pkt_size;
    s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].number = s->st20_total_pkts;
  } else if (type == ST20_TYPE_RTP_LEVEL) { /* rtp path */
    s->st20_total_pkts = ops->rtp_frame_total_pkts;
    s->st20_pkt_size = ops->rtp_pkt_size + sizeof(struct st_base_hdr);
    s->st20_pkt_len = ops->rtp_pkt_size; /* not used in rtp, just set a value */
    /* assume all are normal */
    s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].size = s->st20_pkt_size;
    s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].number = s->st20_total_pkts;
  } else if (ops->packing == ST20_PACKING_GPM_SL) {
    /* calculate pkts in line */
    int bytes_in_pkt = ST_PKT_MAX_ETHER_BYTES - sizeof(struct st_rfc4175_video_hdr);
    s->st20_pkts_in_line = (s->st20_bytes_in_line / bytes_in_pkt) + 1;

    int pixel_in_pkt = (ops->width + s->st20_pkts_in_line - 1) / s->st20_pkts_in_line;
    s->st20_pkt_len =
        (pixel_in_pkt + s->st20_pg.coverage - 1) / s->st20_pg.coverage * s->st20_pg.size;
    s->st20_pkt_size = s->st20_pkt_len + sizeof(struct st_rfc4175_video_hdr);
    s->st20_total_pkts = height * s->st20_pkts_in_line;

    int line_last_len = s->st20_bytes_in_line % s->st20_pkt_len;
    if (line_last_len) {
      s->st20_pkt_info[ST20_PKT_TYPE_LINE_TAIL].number = height;
      s->st20_pkt_info[ST20_PKT_TYPE_LINE_TAIL].size =
          line_last_len + sizeof(struct st_rfc4175_video_hdr);
    }
    s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].size = s->st20_pkt_size;
    s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].number =
        s->st20_total_pkts - s->st20_pkt_info[ST20_PKT_TYPE_LINE_TAIL].number;
    info("%s(%d),  line_last_len: %d\n", __func__, idx, line_last_len);
  } else if (ops->packing == ST20_PACKING_BPM) {
    s->st20_pkt_len = ST_VIDEO_BPM_SIZE;
    int last_pkt_len = s->st20_frame_size % s->st20_pkt_len;
    s->st20_pkt_size = s->st20_pkt_len + sizeof(struct st_rfc4175_video_hdr);
    s->st20_total_pkts = ceil((double)s->st20_frame_size / s->st20_pkt_len);
    int bytes_per_pkt = s->st20_pkt_len;
    int temp = s->st20_bytes_in_line;
    while (temp % bytes_per_pkt != 0 && temp <= s->st20_frame_size) {
      temp += s->st20_bytes_in_line;
    }
    int none_extra_lines = ceil((double)s->st20_frame_size / temp);
    int extra_pkts = height - none_extra_lines;
    if (extra_pkts) {
      s->st20_pkt_info[ST20_PKT_TYPE_EXTRA].number = extra_pkts;
      s->st20_pkt_info[ST20_PKT_TYPE_EXTRA].size =
          s->st20_pkt_size + sizeof(struct st20_rfc4175_extra_rtp_hdr);
    }
    if (last_pkt_len) {
      s->st20_pkt_info[ST20_PKT_TYPE_FRAME_TAIL].number = 1;
      s->st20_pkt_info[ST20_PKT_TYPE_FRAME_TAIL].size =
          last_pkt_len + sizeof(struct st_rfc4175_video_hdr);
    }
    s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].size = s->st20_pkt_size;
    s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].number =
        s->st20_total_pkts - s->st20_pkt_info[ST20_PKT_TYPE_FRAME_TAIL].number -
        s->st20_pkt_info[ST20_PKT_TYPE_EXTRA].number;
    info("%s(%d),  extra_pkts: %d\n", __func__, idx, extra_pkts);
  } else if (ops->packing == ST20_PACKING_GPM) {
    int max_data_len = impl->pkt_udp_suggest_max_size -
                       sizeof(struct st20_rfc4175_rtp_hdr) -
                       sizeof(struct st20_rfc4175_extra_rtp_hdr);
    uint32_t align = s->st20_pg.size * 2;
    max_data_len = max_data_len / align * align;
    int pg_per_pkt = max_data_len / s->st20_pg.size;
    s->st20_total_pkts =
        (ceil)((double)ops->width * height / (s->st20_pg.coverage * pg_per_pkt));
    s->st20_pkt_len = pg_per_pkt * s->st20_pg.size;
    int last_pkt_len = s->st20_frame_size % s->st20_pkt_len;
    s->st20_pkt_size = s->st20_pkt_len + sizeof(struct st_rfc4175_video_hdr);
    int bytes_per_pkt = s->st20_pkt_len;
    int temp = s->st20_bytes_in_line;
    while (temp % bytes_per_pkt != 0 && temp <= s->st20_frame_size) {
      temp += s->st20_bytes_in_line;
    }
    int none_extra_lines = ceil((double)s->st20_frame_size / temp);
    int extra_pkts = height - none_extra_lines;
    if (extra_pkts) {
      s->st20_pkt_info[ST20_PKT_TYPE_EXTRA].number = extra_pkts;
      s->st20_pkt_info[ST20_PKT_TYPE_EXTRA].size =
          s->st20_pkt_size + sizeof(struct st20_rfc4175_extra_rtp_hdr);
    }
    if (last_pkt_len) {
      s->st20_pkt_info[ST20_PKT_TYPE_FRAME_TAIL].number = 1;
      s->st20_pkt_info[ST20_PKT_TYPE_FRAME_TAIL].size =
          last_pkt_len + sizeof(struct st_rfc4175_video_hdr);
    }
    s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].size = s->st20_pkt_size;
    s->st20_pkt_info[ST20_PKT_TYPE_NORMAL].number =
        s->st20_total_pkts - s->st20_pkt_info[ST20_PKT_TYPE_FRAME_TAIL].number -
        s->st20_pkt_info[ST20_PKT_TYPE_EXTRA].number;
    info("%s(%d),  extra_pkts: %d\n", __func__, idx, extra_pkts);
  } else {
    err("%s(%d), invalid packing mode %d\n", __func__, idx, ops->packing);
    return -EIO;
  }

  if (s->st20_pkt_size > ST_PKT_MAX_ETHER_BYTES) {
    err("%s(%d), invalid st20 pkt size %d\n", __func__, idx, s->st20_pkt_size);
    return -EIO;
  }

  return 0;
}

static int tv_attach(struct st_main_impl* impl, struct st_tx_video_sessions_mgr* mgr,
                     struct st_tx_video_session_impl* s, struct st20_tx_ops* ops,
                     enum st_session_type s_type, struct st22_tx_ops* st22_frame_ops) {
  int ret;
  int idx = s->idx, num_port = ops->num_port;
  char* ports[ST_SESSION_PORT_MAX];

  for (int i = 0; i < num_port; i++) ports[i] = ops->port[i];
  ret = st_build_port_map(impl, ports, s->port_maps, num_port);
  if (ret < 0) return ret;

  ret = st20_get_pgroup(ops->fmt, &s->st20_pg);
  if (ret < 0) {
    err("%s(%d), st20_get_pgroup fail %d\n", __func__, idx, ret);
    return ret;
  }

  ret = st_get_fps_timing(ops->fps, &s->fps_tm);
  if (ret < 0) {
    err("%s(%d), invalid fps %d\n", __func__, idx, ops->fps);
    return ret;
  }

  s->st20_linesize = ops->width * s->st20_pg.size / s->st20_pg.coverage;
  if (ops->linesize > s->st20_linesize)
    s->st20_linesize = ops->linesize;
  else if (ops->linesize) {
    err("%s(%d), invalid linesize %u\n", __func__, idx, ops->linesize);
    return -EINVAL;
  }

  uint32_t height = ops->interlaced ? (ops->height >> 1) : ops->height;
  if (st22_frame_ops) {
    if (st22_frame_ops->flags & ST22_TX_FLAG_DISABLE_BOXES)
      s->st22_box_hdr_length = 0;
    else
      s->st22_box_hdr_length = sizeof(struct st22_boxes);
    s->st22_codestream_size = st22_frame_ops->framebuff_max_size;
    s->st20_frame_size = s->st22_codestream_size + s->st22_box_hdr_length;
    s->st20_fb_size = s->st20_frame_size;
  } else {
    s->st20_frame_size = ops->width * height * s->st20_pg.size / s->st20_pg.coverage;
    s->st20_fb_size = s->st20_linesize * height;
  }
  s->st20_frames_cnt = ops->framebuff_cnt;

  ret = tv_init_pkt(impl, s, ops, s_type, st22_frame_ops);
  if (ret < 0) {
    err("%s(%d), pkt init fail %d\n", __func__, idx, ret);
    return ret;
  }

  double frame_time = (double)s->fps_tm.den / s->fps_tm.mul;
  s->st21_vrx_narrow = RTE_MAX(8, s->st20_total_pkts / (27000 * frame_time));
  s->st21_vrx_wide = RTE_MAX(720, s->st20_total_pkts / (300 * frame_time));

  info("%s(%d), st21_vrx_narrow: %d, st21_vrx_wide: %d\n", __func__, idx,
       s->st21_vrx_narrow, s->st21_vrx_wide);

  s->st20_pkt_idx = 0;
  s->st20_seq_id = 0;
  s->st20_rtp_time = UINT32_MAX;
  s->st20_frame_stat = ST21_TX_STAT_WAIT_FRAME;
  s->bulk = RTE_MIN(4, ST_SESSION_MAX_BULK);

  strncpy(s->ops_name, ops->name, ST_MAX_NAME_LEN - 1);
  s->ops = *ops;
  s->s_type = s_type;
  for (int i = 0; i < num_port; i++) {
    s->st20_src_port[i] = (ops->udp_port[i]) ? (ops->udp_port[i]) : (10000 + idx);
    s->st20_dst_port[i] = s->st20_src_port[i];
    enum st_port port = st_port_logic2phy(s->port_maps, i);
    s->eth_ipv4_cksum_offload[i] = st_if_has_offload_ipv4_cksum(impl, port);
    s->eth_has_chain[i] = st_if_has_chain_buff(impl, port);
    if (st_pmd_is_kernel(impl, port) && st_has_af_xdp_zc(impl)) {
      /* enable zero copy for tx */
      s->mbuf_mempool_reuse_rx[i] = true;
    } else {
      s->mbuf_mempool_reuse_rx[i] = false;
    }
  }
  s->tx_mono_pool = st_has_tx_mono_pool(impl);
  s->st20_ipv4_packet_id = 0;

  s->ring_count = ST_TX_VIDEO_SESSIONS_RING_SIZE;
  /* make sure the ring is smaller than total pkts */
  while (s->ring_count > s->st20_total_pkts) {
    s->ring_count /= 2;
  }

  ret = tv_init_sw(impl, mgr, s, st22_frame_ops);
  if (ret < 0) {
    err("%s(%d), tv_init_sw fail %d\n", __func__, idx, ret);
    return -EIO;
  }

  ret = tv_init_hw(impl, mgr, s);
  if (ret < 0) {
    err("%s(%d), tx_session_init_hw fail %d\n", __func__, idx, ret);
    tv_uinit_sw(s);
    return -EIO;
  }

  for (int i = 0; i < num_port; i++) {
    ret = tv_init_hdr(impl, s, i);
    if (ret < 0) {
      err("%s(%d), tx_session_init_hdr fail %d prot %d\n", __func__, idx, ret, i);
      tv_uinit_hw(impl, s);
      tv_uinit_sw(s);
      return ret;
    }
  }

  ret = tv_init_pacing(impl, s);
  if (ret < 0) {
    err("%s(%d), tx_session_init_pacing fail %d\n", __func__, idx, ret);
    tv_uinit_hw(impl, s);
    tv_uinit_sw(s);
    return ret;
  }

  /* init vsync */
  s->vsync.meta.frame_time = s->pacing.frame_time;
  st_vsync_calculate(impl, &s->vsync);
  s->vsync.init = true;
  /* init advice sleep us */
  double sleep_ns = s->pacing.trs * 128;
  s->advice_sleep_us = sleep_ns / NS_PER_US;
  if (st_tasklet_has_sleep(impl)) {
    info("%s(%d), advice sleep us %" PRIu64 "\n", __func__, idx, s->advice_sleep_us);
  }

  s->stat_lines_not_ready = 0;
  s->stat_user_busy = 0;
  s->stat_user_busy_first = true;
  s->stat_epoch_troffset_mismatch = 0;
  s->stat_trans_troffset_mismatch = 0;
  rte_atomic32_set(&s->stat_frame_cnt, 0);
  s->stat_last_time = st_get_monotonic_time();

  s->pri_nic_burst_cnt = 0;
  s->pri_nic_inflight_cnt = 0;
  rte_atomic32_set(&s->nic_burst_cnt, 0);
  rte_atomic32_set(&s->nic_inflight_cnt, 0);
  s->cpu_busy_score = 0;

  for (int i = 0; i < num_port; i++) {
    s->has_inflight[i] = false;
    s->inflight_cnt[i] = 0;
    s->trs_inflight_num[i] = 0;
    s->trs_inflight_num2[i] = 0;
    s->trs_pad_inflight_num[i] = 0;
    s->trs_target_tsc[i] = 0;
  }

  info("%s(%d), len %d(%d) total %d each line %d type %d flags 0x%x\n", __func__, idx,
       s->st20_pkt_len, s->st20_pkt_size, s->st20_total_pkts, s->st20_pkts_in_line,
       ops->type, ops->flags);
  info("%s(%d), w %u h %u fmt %d packing %d pt %d, pacing way: %s\n", __func__, idx,
       ops->width, ops->height, ops->fmt, ops->packing, ops->payload_type,
       st_tx_pacing_way_name(s->pacing_way[ST_SESSION_PORT_P]));
  return 0;
}

void tx_video_session_clear_cpu_busy(struct st_tx_video_session_impl* s) {
  rte_atomic32_set(&s->nic_burst_cnt, 0);
  rte_atomic32_set(&s->nic_inflight_cnt, 0);
  s->cpu_busy_score = 0;
}

void tx_video_session_cal_cpu_busy(struct st_tx_video_session_impl* s) {
  float nic_burst_cnt = rte_atomic32_read(&s->nic_burst_cnt);
  float nic_inflight_cnt = rte_atomic32_read(&s->nic_inflight_cnt);
  float cpu_busy_score = 0;

  tx_video_session_clear_cpu_busy(s);

  if (nic_burst_cnt) {
    cpu_busy_score = 100.0 * nic_inflight_cnt / nic_burst_cnt;
    cpu_busy_score = 100.0 - cpu_busy_score;
  }
  s->cpu_busy_score = cpu_busy_score;
}

static void tv_stat(struct st_tx_video_sessions_mgr* mgr,
                    struct st_tx_video_session_impl* s) {
  int m_idx = mgr->idx, idx = s->idx;
  uint64_t cur_time_ns = st_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->stat_last_time) / NS_PER_S;
  int frame_cnt = rte_atomic32_read(&s->stat_frame_cnt);
  double framerate = frame_cnt / time_sec;

  rte_atomic32_set(&s->stat_frame_cnt, 0);

  notice(
      "TX_VIDEO_SESSION(%d,%d:%s): fps %f, frame %d pkts %d:%d inflight %d:%d, cpu busy "
      "%f\n",
      m_idx, idx, s->ops_name, framerate, frame_cnt, s->stat_pkts_build,
      s->stat_pkts_burst, s->trs_inflight_cnt[0], s->inflight_cnt[0], s->cpu_busy_score);
  s->stat_last_time = cur_time_ns;
  s->stat_pkts_build = 0;
  s->stat_pkts_burst = 0;
  s->trs_inflight_cnt[0] = 0;
  s->inflight_cnt[0] = 0;
  s->stat_pkts_dummy = 0;
  s->stat_pkts_burst_dummy = 0;

  if (s->stat_epoch_troffset_mismatch) {
    notice("TX_VIDEO_SESSION(%d,%d): mismatch epoch troffset %u\n", m_idx, idx,
           s->stat_epoch_troffset_mismatch);
    s->stat_epoch_troffset_mismatch = 0;
  }
  if (s->stat_trans_troffset_mismatch) {
    notice("TX_VIDEO_SESSION(%d,%d): transmitter mismatch troffset %u\n", m_idx, idx,
           s->stat_trans_troffset_mismatch);
    s->stat_trans_troffset_mismatch = 0;
  }
  if (s->stat_epoch_drop) {
    notice("TX_VIDEO_SESSION(%d,%d): epoch drop %u\n", m_idx, idx, s->stat_epoch_drop);
    s->stat_epoch_drop = 0;
  }
  if (s->stat_exceed_frame_time) {
    notice("TX_VIDEO_SESSION(%d,%d): build timeout frames %u\n", m_idx, idx,
           s->stat_exceed_frame_time);
    s->stat_exceed_frame_time = 0;
  }
  if (s->stat_error_user_timestamp) {
    notice("TX_VIDEO_SESSION(%d,%d): error user timestamp %u\n", m_idx, idx,
           s->stat_error_user_timestamp);
    s->stat_error_user_timestamp = 0;
  }
  if (s->stat_user_busy) {
    notice("TX_VIDEO_SESSION(%d,%d): busy as no ready frame from user %u\n", m_idx, idx,
           s->stat_user_busy);
    s->stat_user_busy = 0;
  }
  if (s->stat_lines_not_ready) {
    notice("TX_VIDEO_SESSION(%d,%d): query new lines but app not ready %u\n", m_idx, idx,
           s->stat_lines_not_ready);
    s->stat_lines_not_ready = 0;
  }
  if (s->stat_vsync_mismatch) {
    notice("TX_VIDEO_SESSION(%d,%d): vsync mismatch cnt %u\n", m_idx, idx,
           s->stat_vsync_mismatch);
    s->stat_vsync_mismatch = 0;
  }
  if (frame_cnt <= 0) {
    /* error level */
    err("TX_VIDEO_SESSION(%d,%d:%s): build ret %d, trs ret %d:%d\n", m_idx, idx,
        s->ops_name, s->stat_build_ret_code, s->stat_trs_ret_code[ST_SESSION_PORT_P],
        s->stat_trs_ret_code[ST_SESSION_PORT_R]);
  }

  /* check frame busy stat */
  if (s->st20_frames) {
    struct st_frame_trans* frame_info;
    int frames_in_trans = 0;
    uint16_t framebuff_cnt = s->ops.framebuff_cnt;
    for (int i = 0; i < s->st20_frames_cnt; i++) {
      frame_info = &s->st20_frames[i];
      if (rte_atomic32_read(&frame_info->refcnt)) frames_in_trans++;
    }
    if ((frames_in_trans > 2) || (frames_in_trans >= framebuff_cnt)) {
      notice("TX_VIDEO_SESSION(%d,%d:%s): %d frames are in trans, total %u\n", m_idx, idx,
             s->ops_name, frames_in_trans, framebuff_cnt);
    }
  }
}

static int tv_detach(struct st_main_impl* impl, struct st_tx_video_sessions_mgr* mgr,
                     struct st_tx_video_session_impl* s) {
  tv_stat(mgr, s);
  /* must uinit hw firstly as frame use shared external buffer */
  tv_uinit_hw(impl, s);
  tv_uinit_sw(s);
  return 0;
}

static int tv_init(struct st_main_impl* impl, struct st_tx_video_sessions_mgr* mgr,
                   struct st_tx_video_session_impl* s, int idx) {
  s->idx = idx;
  return 0;
}

static struct st_tx_video_session_impl* tv_mgr_attach(
    struct st_tx_video_sessions_mgr* mgr, struct st20_tx_ops* ops,
    enum st_session_type s_type, struct st22_tx_ops* st22_frame_ops) {
  int midx = mgr->idx;
  struct st_main_impl* impl = mgr->parnet;
  int ret;
  struct st_tx_video_session_impl* s;

  /* find one empty slot in the mgr */
  for (int i = 0; i < ST_SCH_MAX_TX_VIDEO_SESSIONS; i++) {
    if (!tx_video_session_get_empty(mgr, i)) continue;

    s = st_rte_zmalloc_socket(sizeof(*s), st_socket_id(impl, ST_PORT_P));
    if (!s) {
      err("%s(%d), session malloc fail on %d\n", __func__, midx, i);
      tx_video_session_put(mgr, i);
      return NULL;
    }
    ret = tv_init(impl, mgr, s, i);
    if (ret < 0) {
      err("%s(%d), init fail on %d\n", __func__, midx, i);
      tx_video_session_put(mgr, i);
      st_rte_free(s);
      return NULL;
    }
    ret = tv_attach(impl, mgr, s, ops, s_type, st22_frame_ops);
    if (ret < 0) {
      err("%s(%d), attach fail on %d\n", __func__, midx, i);
      tx_video_session_put(mgr, i);
      st_rte_free(s);
      return NULL;
    }
    mgr->sessions[i] = s;
    mgr->max_idx = RTE_MAX(mgr->max_idx, i + 1);
    tx_video_session_put(mgr, i);
    return s;
  }

  err("%s(%d), fail\n", __func__, midx);
  return NULL;
}

static int tv_mgr_detach(struct st_tx_video_sessions_mgr* mgr,
                         struct st_tx_video_session_impl* s) {
  int midx = mgr->idx;
  int idx = s->idx;

  s = tx_video_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }

  tv_detach(mgr->parnet, mgr, s);
  mgr->sessions[idx] = NULL;
  st_rte_free(s);

  tx_video_session_put(mgr, idx);

  return 0;
}

static int tv_mgr_init(struct st_main_impl* impl, struct st_sch_impl* sch,
                       struct st_tx_video_sessions_mgr* mgr) {
  int idx = sch->idx;
  struct st_sch_tasklet_ops ops;
  int i;

  RTE_BUILD_BUG_ON(sizeof(struct st_rfc4175_video_hdr) != 62);
  RTE_BUILD_BUG_ON(sizeof(struct st_rfc3550_hdr) != 54);
  RTE_BUILD_BUG_ON(sizeof(struct st22_rfc9134_video_hdr) != 58);
  RTE_BUILD_BUG_ON(sizeof(struct st22_boxes) != 60);

  mgr->parnet = impl;
  mgr->idx = idx;

  for (i = 0; i < ST_SCH_MAX_TX_VIDEO_SESSIONS; i++) {
    rte_spinlock_init(&mgr->mutex[i]);
  }

  memset(&ops, 0x0, sizeof(ops));
  ops.priv = mgr;
  ops.name = "tx_video_sessions_mgr";
  ops.pre_start = tv_tasklet_pre_start;
  ops.start = tv_tasklet_start;
  ops.stop = tv_tasklet_stop;
  ops.handler = tvs_tasklet_handler;

  mgr->tasklet = st_sch_register_tasklet(sch, &ops);
  if (!mgr->tasklet) {
    err("%s(%d), st_sch_register_tasklet fail\n", __func__, idx);
    return -EIO;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int tv_mgr_uinit(struct st_tx_video_sessions_mgr* mgr) {
  int m_idx = mgr->idx;
  struct st_tx_video_session_impl* s;

  if (mgr->tasklet) {
    st_sch_unregister_tasklet(mgr->tasklet);
    mgr->tasklet = NULL;
  }

  for (int i = 0; i < ST_SCH_MAX_TX_VIDEO_SESSIONS; i++) {
    s = tx_video_session_get(mgr, i);
    if (!s) continue;

    warn("%s(%d), session %d still attached\n", __func__, m_idx, i);
    tv_mgr_detach(mgr, s);
    tx_video_session_put(mgr, i);
  }

  info("%s(%d), succ\n", __func__, m_idx);
  return 0;
}

static int tv_mgr_update(struct st_tx_video_sessions_mgr* mgr) {
  int max_idx = 0;
  struct st_main_impl* impl = mgr->parnet;
  uint64_t sleep_us = st_sch_default_sleep_us(impl);
  struct st_tx_video_session_impl* s;

  for (int i = 0; i < ST_SCH_MAX_TX_VIDEO_SESSIONS; i++) {
    s = mgr->sessions[i];
    if (!s) continue;
    max_idx = i + 1;
    sleep_us = RTE_MIN(s->advice_sleep_us, sleep_us);
  }
  dbg("%s(%d), sleep us %" PRIu64 ", max_idx %d\n", __func__, mgr->idx, sleep_us,
      max_idx);
  mgr->max_idx = max_idx;
  if (mgr->tasklet) st_tasklet_set_sleep(mgr->tasklet, sleep_us);
  return 0;
}

void st_tx_video_sessions_stat(struct st_main_impl* impl) {
  struct st_sch_impl* sch;
  struct st_tx_video_sessions_mgr* mgr;
  struct st_tx_video_session_impl* s;

  for (int sch_idx = 0; sch_idx < ST_MAX_SCH_NUM; sch_idx++) {
    sch = st_sch_instance(impl, sch_idx);
    if (!st_sch_started(sch)) continue;
    mgr = &sch->tx_video_mgr;
    for (int j = 0; j < mgr->max_idx; j++) {
      s = tx_video_session_get(mgr, j);
      if (!s) continue;
      tv_stat(mgr, s);
      tx_video_session_put(mgr, j);
    }
  }
}

int st_tx_video_sessions_sch_init(struct st_main_impl* impl, struct st_sch_impl* sch) {
  int ret, idx = sch->idx;

  if (sch->tx_video_init) return 0;

  /* create tx video context */
  struct st_tx_video_sessions_mgr* tx_video_mgr = &sch->tx_video_mgr;
  ret = tv_mgr_init(impl, sch, tx_video_mgr);
  if (ret < 0) {
    err("%s(%d), st_tv_mgr_init fail %d\n", __func__, idx, ret);
    return ret;
  }

  ret = st_video_transmitter_init(impl, sch, tx_video_mgr, &sch->video_transmitter);
  if (ret < 0) {
    tv_mgr_uinit(tx_video_mgr);
    err("%s(%d), st_video_transmitter_init fail %d\n", __func__, idx, ret);
    return ret;
  }

  sch->tx_video_init = true;
  return 0;
}

int st_tx_video_sessions_sch_uinit(struct st_main_impl* impl, struct st_sch_impl* sch) {
  if (!sch->tx_video_init) return 0;

  st_video_transmitter_uinit(&sch->video_transmitter);
  tv_mgr_uinit(&sch->tx_video_mgr);
  sch->tx_video_init = false;

  return 0;
}

int st_tx_video_session_migrate(struct st_main_impl* impl,
                                struct st_tx_video_sessions_mgr* mgr,
                                struct st_tx_video_session_impl* s, int idx) {
  tv_init(impl, mgr, s, idx);
  return 0;
}

static int tv_ops_check(struct st20_tx_ops* ops) {
  int num_ports = ops->num_port, ret;
  uint8_t* ip;

  if ((num_ports > ST_PORT_MAX) || (num_ports <= 0)) {
    err("%s, invalid num_ports %d\n", __func__, num_ports);
    return -EINVAL;
  }

  for (int i = 0; i < num_ports; i++) {
    ip = ops->dip_addr[i];
    ret = st_ip_addr_check(ip);
    if (ret < 0) {
      err("%s(%d), invalid ip %d.%d.%d.%d\n", __func__, i, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (num_ports > 1) {
    if (0 == memcmp(ops->dip_addr[0], ops->dip_addr[1], ST_IP_ADDR_LEN)) {
      err("%s, same %d.%d.%d.%d for both ip\n", __func__, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (st20_is_frame_type(ops->type)) {
    if ((ops->framebuff_cnt < 2) || (ops->framebuff_cnt > ST20_FB_MAX_COUNT)) {
      err("%s, invalid framebuff_cnt %d, should in range [2:%d]\n", __func__,
          ops->framebuff_cnt, ST20_FB_MAX_COUNT);
      return -EINVAL;
    }
    if (!ops->get_next_frame) {
      err("%s, pls set get_next_frame\n", __func__);
      return -EINVAL;
    }
    if (ops->type == ST20_TYPE_SLICE_LEVEL) {
      if (!ops->query_frame_lines_ready) {
        err("%s, pls set query_frame_lines_ready\n", __func__);
        return -EINVAL;
      }
    }
  } else if (ops->type == ST20_TYPE_RTP_LEVEL) {
    if (ops->rtp_ring_size <= 0) {
      err("%s, invalid rtp_ring_size %d\n", __func__, ops->rtp_ring_size);
      return -EINVAL;
    }
    if (ops->rtp_frame_total_pkts <= 0) {
      err("%s, invalid rtp_frame_total_pkts %d\n", __func__, ops->rtp_frame_total_pkts);
      return -EINVAL;
    }
    if (!st_rtp_len_valid(ops->rtp_pkt_size)) {
      err("%s, invalid rtp_pkt_size %d\n", __func__, ops->rtp_pkt_size);
      return -EINVAL;
    }
    if (!ops->notify_rtp_done) {
      err("%s, pls set notify_rtp_done\n", __func__);
      return -EINVAL;
    }
  }

  if (!st_is_valid_payload_type(ops->payload_type)) {
    err("%s, invalid payload_type %d\n", __func__, ops->payload_type);
    return -EINVAL;
  }

  return 0;
}

static int tv_st22_ops_check(struct st22_tx_ops* ops) {
  int num_ports = ops->num_port, ret;
  uint8_t* ip;

  if ((num_ports > ST_PORT_MAX) || (num_ports <= 0)) {
    err("%s, invalid num_ports %d\n", __func__, num_ports);
    return -EINVAL;
  }

  for (int i = 0; i < num_ports; i++) {
    ip = ops->dip_addr[i];
    ret = st_ip_addr_check(ip);
    if (ret < 0) {
      err("%s(%d), invalid ip %d.%d.%d.%d\n", __func__, i, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (num_ports > 1) {
    if (0 == memcmp(ops->dip_addr[0], ops->dip_addr[1], ST_IP_ADDR_LEN)) {
      err("%s, same %d.%d.%d.%d for both ip\n", __func__, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (ops->type == ST22_TYPE_FRAME_LEVEL) {
    if ((ops->framebuff_cnt < 2) || (ops->framebuff_cnt > ST22_FB_MAX_COUNT)) {
      err("%s, invalid framebuff_cnt %d, should in range [2:%d]\n", __func__,
          ops->framebuff_cnt, ST22_FB_MAX_COUNT);
      return -EINVAL;
    }
    if (ops->pack_type != ST22_PACK_CODESTREAM) {
      err("%s, invalid pack_type %d\n", __func__, ops->pack_type);
      return -EINVAL;
    }
    if (!ops->framebuff_max_size) {
      err("%s, pls set framebuff_max_size\n", __func__);
      return -EINVAL;
    }
    if (!ops->get_next_frame) {
      err("%s, pls set get_next_frame\n", __func__);
      return -EINVAL;
    }
  }

  if (ops->type == ST22_TYPE_RTP_LEVEL) {
    if (ops->rtp_ring_size <= 0) {
      err("%s, invalid rtp_ring_size %d\n", __func__, ops->rtp_ring_size);
      return -EINVAL;
    }
    if (!st_rtp_len_valid(ops->rtp_pkt_size)) {
      err("%s, invalid rtp_pkt_size %d\n", __func__, ops->rtp_pkt_size);
      return -EINVAL;
    }
    if (!ops->notify_rtp_done) {
      err("%s, pls set notify_rtp_done\n", __func__);
      return -EINVAL;
    }
  }

  if (!st_is_valid_payload_type(ops->payload_type)) {
    err("%s, invalid payload_type %d\n", __func__, ops->payload_type);
    return -EINVAL;
  }

  return 0;
}

st20_tx_handle st20_tx_create(st_handle st, struct st20_tx_ops* ops) {
  struct st_main_impl* impl = st;
  struct st_sch_impl* sch;
  struct st_tx_video_session_handle_impl* s_impl;
  struct st_tx_video_session_impl* s;
  int quota_mbs, ret;
  uint64_t bps;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  ret = tv_ops_check(ops);
  if (ret < 0) {
    err("%s, st_tv_ops_check fail %d\n", __func__, ret);
    return NULL;
  }
  int height = ops->interlaced ? (ops->height >> 1) : ops->height;
  ret = st20_get_bandwidth_bps(ops->width, height, ops->fmt, ops->fps, &bps);
  if (ret < 0) {
    err("%s, st20_get_bandwidth_bps fail\n", __func__);
    return NULL;
  }
  quota_mbs = bps / (1000 * 1000);
  quota_mbs *= ops->num_port;
  if (!st_has_user_quota(impl)) {
    if (ST20_TYPE_RTP_LEVEL == ops->type) {
      quota_mbs = quota_mbs * ST_QUOTA_TX1080P_PER_SCH / ST_QUOTA_TX1080P_RTP_PER_SCH;
    }
  }

  s_impl = st_rte_zmalloc_socket(sizeof(*s_impl), st_socket_id(impl, ST_PORT_P));
  if (!s_impl) {
    err("%s, s_impl malloc fail\n", __func__);
    return NULL;
  }

  sch = st_sch_get(impl, quota_mbs, ST_SCH_TYPE_DEFAULT, ST_SCH_MASK_ALL);
  if (!sch) {
    st_rte_free(s_impl);
    err("%s, get sch fail\n", __func__);
    return NULL;
  }

  st_pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  ret = st_tx_video_sessions_sch_init(impl, sch);
  st_pthread_mutex_unlock(&sch->tx_video_mgr_mutex);
  if (ret < 0) {
    err("%s, tx video sch init fail %d\n", __func__, ret);
    st_sch_put(sch, quota_mbs);
    st_rte_free(s_impl);
    return NULL;
  }

  st_pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  s = tv_mgr_attach(&sch->tx_video_mgr, ops, ST_SESSION_TYPE_TX_VIDEO, NULL);
  st_pthread_mutex_unlock(&sch->tx_video_mgr_mutex);
  if (!s) {
    err("%s(%d), st_tx_sessions_mgr_attach fail\n", __func__, sch->idx);
    st_sch_put(sch, quota_mbs);
    st_rte_free(s_impl);
    return NULL;
  }

  /* update mgr status */
  st_pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  tv_mgr_update(&sch->tx_video_mgr);
  st_pthread_mutex_unlock(&sch->tx_video_mgr_mutex);

  s_impl->parnet = impl;
  s_impl->type = ST_SESSION_TYPE_TX_VIDEO;
  s_impl->sch = sch;
  s_impl->impl = s;
  s_impl->quota_mbs = quota_mbs;

  s->st20_handle = s_impl;

  rte_atomic32_inc(&impl->st20_tx_sessions_cnt);
  info("%s, succ on sch %d session %p,%d num_port %d\n", __func__, sch->idx, s, s->idx,
       ops->num_port);
  return s_impl;
}

int st20_tx_set_ext_frame(st20_tx_handle handle, uint16_t idx,
                          struct st20_ext_frame* ext_frame) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  struct st_tx_video_session_impl* s;
  int s_idx;

  if (s_impl->type != ST_SESSION_TYPE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }

  if (!ext_frame) {
    err("%s, NULL ext frame\n", __func__);
    return -EIO;
  }

  s = s_impl->impl;
  s_idx = s->idx;

  if (ext_frame->buf_len < s->st20_fb_size) {
    err("%s(%d), ext framebuffer size %" PRIu64 " can not hold frame, need %" PRIu64 "\n",
        __func__, s_idx, ext_frame->buf_len, s->st20_fb_size);
    return -EIO;
  }
  void* addr = ext_frame->buf_addr;
  if (!addr) {
    err("%s(%d), invalid ext frame address\n", __func__, s_idx);
    return -EIO;
  }
  rte_iova_t iova_addr = ext_frame->buf_iova;
  if (iova_addr == ST_BAD_IOVA || iova_addr == 0) {
    err("%s(%d), invalid ext frame iova 0x%" PRIx64 "\n", __func__, s_idx, iova_addr);
    return -EIO;
  }

  if (idx >= s->st20_frames_cnt) {
    err("%s(%d), invalid idx %d, should be in range [0, %d]\n", __func__, s_idx, idx,
        s->st20_frames_cnt);
    return -EIO;
  }
  if (!s->st20_frames) {
    err("%s(%d), st20_frames not valid\n", __func__, s_idx);
    return -EINVAL;
  }
  struct st_frame_trans* frame = &s->st20_frames[idx];
  int refcnt = rte_atomic32_read(&frame->refcnt);
  if (refcnt) {
    err("%s(%d), frame %d are not free, refcnt %d\n", __func__, s_idx, idx, refcnt);
    return -EINVAL;
  }
  if (!(frame->flags & ST_FT_FLAG_EXT)) {
    err("%s(%d), frame %d are not ext enabled\n", __func__, s_idx, idx);
    return -EINVAL;
  }

  frame->addr = addr;
  frame->iova = iova_addr;
  return 0;
}

void* st20_tx_get_framebuffer(st20_tx_handle handle, uint16_t idx) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  struct st_tx_video_session_impl* s;

  if (s_impl->type != ST_SESSION_TYPE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;

  if (idx >= s->st20_frames_cnt) {
    err("%s, invalid idx %d, should be in range [0, %d]\n", __func__, idx,
        s->st20_frames_cnt);
    return NULL;
  }
  if (!s->st20_frames || !s->st20_frames[idx].addr) {
    err("%s, st20_frames not allocated\n", __func__);
    return NULL;
  }

  return s->st20_frames[idx].addr;
}

size_t st20_tx_get_framebuffer_size(st20_tx_handle handle) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  struct st_tx_video_session_impl* s;

  if (s_impl->type != ST_SESSION_TYPE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return 0;
  }

  s = s_impl->impl;
  return s->st20_fb_size;
}

int st20_tx_get_framebuffer_count(st20_tx_handle handle) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  struct st_tx_video_session_impl* s;

  if (s_impl->type != ST_SESSION_TYPE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }

  s = s_impl->impl;
  return s->st20_frames_cnt;
}

void* st20_tx_get_mbuf(st20_tx_handle handle, void** usrptr) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt;
  struct st_tx_video_session_impl* s;
  int idx;
  struct rte_ring* packet_ring;

  if (s_impl->type != ST_SESSION_TYPE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;
  idx = s->idx;
  packet_ring = s->packet_ring;
  if (!packet_ring) {
    err("%s(%d), packet ring is not created\n", __func__, idx);
    return NULL;
  }

  if (rte_ring_full(packet_ring)) {
    dbg("%s(%d), packet ring is full\n", __func__, idx);
    return NULL;
  }

  pkt = rte_pktmbuf_alloc(s->mbuf_mempool_chain);
  if (!pkt) {
    dbg("%s(%d), pkt alloc fail\n", __func__, idx);
    return NULL;
  }

  *usrptr = rte_pktmbuf_mtod(pkt, void*);
  return pkt;
}

int st20_tx_put_mbuf(st20_tx_handle handle, void* mbuf, uint16_t len) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = (struct rte_mbuf*)mbuf;
  struct st_tx_video_session_impl* s;
  struct rte_ring* packet_ring;
  int idx, ret;

  if (!st_rtp_len_valid(len)) {
    if (len) err("%s, invalid len %d\n", __func__, len);
    rte_pktmbuf_free(mbuf);
    return -EIO;
  }

  if (s_impl->type != ST_SESSION_TYPE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    rte_pktmbuf_free(mbuf);
    return -EIO;
  }

  s = s_impl->impl;
  idx = s->idx;
  packet_ring = s->packet_ring;
  if (!packet_ring) {
    err("%s(%d), packet ring is not created\n", __func__, idx);
    rte_pktmbuf_free(mbuf);
    return -EIO;
  }

  if (len > s->rtp_pkt_max_size) {
    err("%s(%d), invalid len %u, allowed %u\n", __func__, idx, len, s->rtp_pkt_max_size);
    rte_pktmbuf_free(mbuf);
    return -EIO;
  }

  pkt->data_len = pkt->pkt_len = len;
  ret = rte_ring_sp_enqueue(packet_ring, (void*)pkt);
  if (ret < 0) {
    err("%s(%d), can not enqueue to the rte ring\n", __func__, idx);
    rte_pktmbuf_free(mbuf);
    return -EBUSY;
  }

  return 0;
}

int st20_tx_get_sch_idx(st20_tx_handle handle) {
  struct st_tx_video_session_handle_impl* s_impl = handle;

  if (s_impl->type != ST_SESSION_TYPE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }

  return s_impl->sch->idx;
}

int st20_tx_free(st20_tx_handle handle) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  struct st_main_impl* impl;
  struct st_sch_impl* sch;
  struct st_tx_video_session_impl* s;
  int ret, sch_idx, idx;

  if (s_impl->type != ST_SESSION_TYPE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parnet;
  sch = s_impl->sch;
  s = s_impl->impl;
  idx = s->idx;
  sch_idx = sch->idx;

  st_pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  ret = tv_mgr_detach(&sch->tx_video_mgr, s);
  st_pthread_mutex_unlock(&sch->tx_video_mgr_mutex);
  if (ret < 0)
    err("%s(%d,%d), st_tx_sessions_mgr_deattach fail\n", __func__, sch_idx, idx);

  ret = st_sch_put(sch, s_impl->quota_mbs);
  if (ret < 0) err("%s(%d, %d), st_sch_put fail\n", __func__, sch_idx, idx);

  st_rte_free(s_impl);

  /* update mgr status */
  st_pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  tv_mgr_update(&sch->tx_video_mgr);
  st_pthread_mutex_unlock(&sch->tx_video_mgr_mutex);

  rte_atomic32_dec(&impl->st20_tx_sessions_cnt);
  info("%s, succ on sch %d session %d\n", __func__, sch_idx, idx);
  return 0;
}

st22_tx_handle st22_tx_create(st_handle st, struct st22_tx_ops* ops) {
  struct st_main_impl* impl = st;
  struct st_sch_impl* sch;
  struct st22_tx_video_session_handle_impl* s_impl;
  struct st_tx_video_session_impl* s;
  int quota_mbs, ret;
  uint64_t bps;
  struct st20_tx_ops st20_ops;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  ret = tv_st22_ops_check(ops);
  if (ret < 0) {
    err("%s, st_tv_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  if (ST22_TYPE_RTP_LEVEL == ops->type) {
    ret = st22_rtp_bandwidth_bps(ops->rtp_frame_total_pkts, ops->rtp_pkt_size, ops->fps,
                                 &bps);
    if (ret < 0) {
      err("%s, rtp_bandwidth_bps fail\n", __func__);
      return NULL;
    }
    quota_mbs = bps / (1000 * 1000);
    quota_mbs *= ops->num_port;
    if (!st_has_user_quota(impl)) {
      quota_mbs = quota_mbs * ST_QUOTA_TX1080P_PER_SCH / ST_QUOTA_TX1080P_RTP_PER_SCH;
    }
  } else {
    ret = st22_frame_bandwidth_bps(ops->framebuff_max_size, ops->fps, &bps);
    if (ret < 0) {
      err("%s, frame_bandwidth_bps fail\n", __func__);
      return NULL;
    }
    quota_mbs = bps / (1000 * 1000);
    quota_mbs *= ops->num_port;
  }

  s_impl = st_rte_zmalloc_socket(sizeof(*s_impl), st_socket_id(impl, ST_PORT_P));
  if (!s_impl) {
    err("%s, s_impl malloc fail\n", __func__);
    return NULL;
  }

  sch = st_sch_get(impl, quota_mbs, ST_SCH_TYPE_DEFAULT, ST_SCH_MASK_ALL);
  if (!sch) {
    st_rte_free(s_impl);
    err("%s, get sch fail\n", __func__);
    return NULL;
  }

  st_pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  ret = st_tx_video_sessions_sch_init(impl, sch);
  st_pthread_mutex_unlock(&sch->tx_video_mgr_mutex);
  if (ret < 0) {
    err("%s, tx video sch init fail fail %d\n", __func__, ret);
    st_sch_put(sch, quota_mbs);
    st_rte_free(s_impl);
    return NULL;
  }

  /* reuse st20 rtp type */
  memset(&st20_ops, 0, sizeof(st20_ops));
  st20_ops.name = ops->name;
  st20_ops.priv = ops->priv;
  st20_ops.num_port = ops->num_port;
  memcpy(st20_ops.dip_addr[ST_PORT_P], ops->dip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  strncpy(st20_ops.port[ST_PORT_P], ops->port[ST_PORT_P], ST_PORT_MAX_LEN);
  st20_ops.udp_port[ST_PORT_P] = ops->udp_port[ST_PORT_P];
  if (ops->flags & ST22_TX_FLAG_USER_P_MAC) {
    memcpy(&st20_ops.tx_dst_mac[ST_PORT_P][0], &ops->tx_dst_mac[ST_PORT_P][0], 6);
    st20_ops.flags |= ST20_TX_FLAG_USER_P_MAC;
  }
  if (ops->num_port > 1) {
    memcpy(st20_ops.dip_addr[ST_PORT_R], ops->dip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(st20_ops.port[ST_PORT_R], ops->port[ST_PORT_R], ST_PORT_MAX_LEN);
    st20_ops.udp_port[ST_PORT_R] = ops->udp_port[ST_PORT_R];
    if (ops->flags & ST22_TX_FLAG_USER_R_MAC) {
      memcpy(&st20_ops.tx_dst_mac[ST_PORT_R][0], &ops->tx_dst_mac[ST_PORT_R][0], 6);
      st20_ops.flags |= ST20_TX_FLAG_USER_R_MAC;
    }
  }
  if (ops->flags & ST22_TX_FLAG_USER_PACING) st20_ops.flags |= ST20_TX_FLAG_USER_PACING;
  if (ops->flags & ST22_TX_FLAG_USER_TIMESTAMP)
    st20_ops.flags |= ST20_TX_FLAG_USER_TIMESTAMP;
  st20_ops.pacing = ops->pacing;
  if (ST22_TYPE_RTP_LEVEL == ops->type)
    st20_ops.type = ST20_TYPE_RTP_LEVEL;
  else
    st20_ops.type = ST20_TYPE_FRAME_LEVEL;
  st20_ops.width = ops->width;
  st20_ops.height = ops->height;
  st20_ops.fps = ops->fps;
  st20_ops.fmt = ST20_FMT_YUV_422_10BIT;
  st20_ops.framebuff_cnt = ops->framebuff_cnt;
  st20_ops.payload_type = ops->payload_type;
  st20_ops.rtp_ring_size = ops->rtp_ring_size;
  st20_ops.rtp_frame_total_pkts = ops->rtp_frame_total_pkts;
  st20_ops.rtp_pkt_size = ops->rtp_pkt_size;
  st20_ops.notify_rtp_done = ops->notify_rtp_done;
  st20_ops.notify_event = ops->notify_event;
  st_pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  if (ST22_TYPE_RTP_LEVEL == ops->type) {
    s = tv_mgr_attach(&sch->tx_video_mgr, &st20_ops, ST22_SESSION_TYPE_TX_VIDEO, NULL);
  } else {
    s = tv_mgr_attach(&sch->tx_video_mgr, &st20_ops, ST22_SESSION_TYPE_TX_VIDEO, ops);
  }
  st_pthread_mutex_unlock(&sch->tx_video_mgr_mutex);
  if (!s) {
    err("%s(%d), st_tx_sessions_mgr_attach fail\n", __func__, sch->idx);
    st_sch_put(sch, quota_mbs);
    st_rte_free(s_impl);
    return NULL;
  }

  s_impl->parnet = impl;
  s_impl->type = ST22_SESSION_TYPE_TX_VIDEO;
  s_impl->sch = sch;
  s_impl->impl = s;
  s_impl->quota_mbs = quota_mbs;
  s->st22_handle = s_impl;

  rte_atomic32_inc(&impl->st22_tx_sessions_cnt);
  info("%s, succ on sch %d session %d num_port %d\n", __func__, sch->idx, s->idx,
       ops->num_port);
  return s_impl;
}

int st22_tx_free(st22_tx_handle handle) {
  struct st22_tx_video_session_handle_impl* s_impl = handle;
  struct st_main_impl* impl;
  struct st_sch_impl* sch;
  struct st_tx_video_session_impl* s;
  int ret, sch_idx, idx;

  if (s_impl->type != ST22_SESSION_TYPE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parnet;
  sch = s_impl->sch;
  s = s_impl->impl;
  idx = s->idx;
  sch_idx = sch->idx;

  st_pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  ret = tv_mgr_detach(&sch->tx_video_mgr, s);
  st_pthread_mutex_unlock(&sch->tx_video_mgr_mutex);
  if (ret < 0)
    err("%s(%d,%d), st_tx_sessions_mgr_deattach fail\n", __func__, sch_idx, idx);

  ret = st_sch_put(sch, s_impl->quota_mbs);
  if (ret < 0) err("%s(%d, %d), st_sch_put fail\n", __func__, sch_idx, idx);

  st_rte_free(s_impl);

  /* update mgr status */
  st_pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  tv_mgr_update(&sch->tx_video_mgr);
  st_pthread_mutex_unlock(&sch->tx_video_mgr_mutex);

  rte_atomic32_dec(&impl->st22_tx_sessions_cnt);
  info("%s, succ on sch %d session %d\n", __func__, sch_idx, idx);
  return 0;
}

void* st22_tx_get_mbuf(st22_tx_handle handle, void** usrptr) {
  struct st22_tx_video_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt;
  struct st_tx_video_session_impl* s;
  int idx;
  struct rte_ring* packet_ring;

  if (s_impl->type != ST22_SESSION_TYPE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;
  idx = s->idx;
  packet_ring = s->packet_ring;
  if (!packet_ring) {
    err("%s(%d), packet ring is not created\n", __func__, idx);
    return NULL;
  }

  if (rte_ring_full(packet_ring)) {
    dbg("%s(%d), packet ring is full\n", __func__, idx);
    return NULL;
  }

  pkt = rte_pktmbuf_alloc(s->mbuf_mempool_chain);
  if (!pkt) {
    dbg("%s(%d), pkt alloc fail\n", __func__, idx);
    return NULL;
  }

  *usrptr = rte_pktmbuf_mtod(pkt, void*);
  return pkt;
}

int st22_tx_put_mbuf(st22_tx_handle handle, void* mbuf, uint16_t len) {
  struct st22_tx_video_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = (struct rte_mbuf*)mbuf;
  struct st_tx_video_session_impl* s;
  struct rte_ring* packet_ring;
  int idx, ret;

  if (!st_rtp_len_valid(len)) {
    if (len) err("%s, invalid len %d\n", __func__, len);
    rte_pktmbuf_free(mbuf);
    return -EIO;
  }

  if (s_impl->type != ST22_SESSION_TYPE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    rte_pktmbuf_free(mbuf);
    return -EIO;
  }

  s = s_impl->impl;
  idx = s->idx;
  packet_ring = s->packet_ring;
  if (!packet_ring) {
    err("%s(%d), packet ring is not created\n", __func__, idx);
    rte_pktmbuf_free(mbuf);
    return -EIO;
  }

  if (len > s->rtp_pkt_max_size) {
    err("%s(%d), invalid len %u, allowed %u\n", __func__, idx, len, s->rtp_pkt_max_size);
    rte_pktmbuf_free(mbuf);
    return -EIO;
  }

  pkt->data_len = pkt->pkt_len = len;
  ret = rte_ring_sp_enqueue(packet_ring, (void*)pkt);
  if (ret < 0) {
    err("%s(%d), can not enqueue to the rte ring\n", __func__, idx);
    rte_pktmbuf_free(mbuf);
    return -EBUSY;
  }

  return 0;
}

int st22_tx_get_sch_idx(st22_tx_handle handle) {
  struct st22_tx_video_session_handle_impl* s_impl = handle;

  if (s_impl->type != ST22_SESSION_TYPE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }

  return s_impl->sch->idx;
}

void* st22_tx_get_fb_addr(st22_tx_handle handle, uint16_t idx) {
  struct st22_tx_video_session_handle_impl* s_impl = handle;
  struct st_tx_video_session_impl* s;

  if (s_impl->type != ST22_SESSION_TYPE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;

  if (idx >= s->st20_frames_cnt) {
    err("%s, invalid idx %d, should be in range [0, %d]\n", __func__, idx,
        s->st20_frames_cnt);
    return NULL;
  }
  if (!s->st20_frames || !s->st20_frames[idx].addr) {
    err("%s, st22_frames not allocated\n", __func__);
    return NULL;
  }

  if (s->st22_info) {
    return s->st20_frames[idx].addr + s->st22_box_hdr_length;
  } else {
    return s->st20_frames[idx].addr;
  }
}
