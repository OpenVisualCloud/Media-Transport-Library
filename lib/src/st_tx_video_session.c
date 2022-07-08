/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
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

static inline uint64_t tx_video_session_rl_bps(struct st_tx_video_session_impl* s) {
  double ractive = 1.0;
  if (s->ops.interlaced && s->ops.height <= 576) {
    ractive = (s->ops.height == 480) ? 487.0 / 525.0 : 576.0 / 625.0;
  }
  return (uint64_t)(s->st20_pkt_size * s->st20_total_pkts * 1.0 * s->fps_tm.mul /
                    s->fps_tm.den / ractive);
}

static int tx_video_session_free_frame(struct st_tx_video_session_impl* s, int idx) {
  if (s->st20_frames_sh_info[idx]) {
    uint16_t sh_info_refcnt = rte_mbuf_ext_refcnt_read(s->st20_frames_sh_info[idx]);

    if (sh_info_refcnt > 0)
      err("%s(%d), sh_info still active, refcnt %d\n", __func__, idx, sh_info_refcnt);
  }

  if (s->st20_frames_sh_info[idx]) {
    st_rte_free(s->st20_frames_sh_info[idx]);
    s->st20_frames_sh_info[idx] = NULL;
  }

  if (s->st20_frames[idx]) {
    st_rte_free(s->st20_frames[idx]);
    s->st20_frames[idx] = NULL;
  }
  s->st20_frames_iova[idx] = 0;

  dbg("%s(%d), succ\n", __func__, s->idx);
  return 0;
}

static void tx_video_session_frames_free_cb(void* addr, void* opaque) {
  struct st_tx_video_session_impl* s = opaque;
  uint16_t frame_idx;
  int idx = s->idx;

  for (frame_idx = 0; frame_idx < s->st20_frames_cnt; ++frame_idx) {
    if (addr >= s->st20_frames[frame_idx] &&
        addr < s->st20_frames[frame_idx] + s->st20_frame_size)
      break;
  }
  if (frame_idx >= s->st20_frames_cnt) {
    err("%s(%d), addr %p do not belong to the session\n", __func__, idx, addr);
    return;
  }

  if (s->st22_info) {
    if (s->st22_info->notify_frame_done)
      s->st22_info->notify_frame_done(s->ops.priv, frame_idx);
  } else {
    if (s->ops.notify_frame_done) s->ops.notify_frame_done(s->ops.priv, frame_idx);
  }

  dbg("%s(%d), succ\n", __func__, idx);
}

static int tx_video_session_alloc_frames(struct st_main_impl* impl,
                                         struct st_tx_video_session_impl* s) {
  enum st_port port = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_P);
  int soc_id = st_socket_id(impl, port);
  int idx = s->idx;
  size_t size = s->st20_frame_size;
  void* frame;
  struct rte_mbuf_ext_shared_info* sh_info;

  s->st20_frames = st_rte_zmalloc_socket(sizeof(void*) * s->st20_frames_cnt, soc_id);

  if (!s->st20_frames) {
    err("%s(%d), st20_frames not alloc\n", __func__, idx);
    return -ENOMEM;
  }

  s->st20_frames_iova =
      st_rte_zmalloc_socket(sizeof(rte_iova_t) * s->st20_frames_cnt, soc_id);

  if (!s->st20_frames_iova) {
    err("%s(%d), st20_frames_iova not alloc\n", __func__, idx);
    return -ENOMEM;
  }

  s->st20_frames_sh_info = st_rte_zmalloc_socket(
      sizeof(struct rte_mbuf_ext_shared_info*) * s->st20_frames_cnt, soc_id);

  if (!s->st20_frames_sh_info) {
    err("%s(%d), st20_frames_iova not alloc\n", __func__, idx);
    return -ENOMEM;
  }

  for (int i = 0; i < s->st20_frames_cnt; i++) {
    frame = st_rte_zmalloc_socket(size, soc_id);
    if (!frame) {
      err("%s(%d), rte_malloc %" PRIu64 " fail at %d\n", __func__, idx, size, i);
      return -ENOMEM;
    }

    if (s->st22_info) { /* copy boxes */
      st_memcpy(frame, &s->st22_boxes, s->st22_box_hdr_length);
    }

    sh_info = st_rte_zmalloc_socket(sizeof(struct rte_mbuf_ext_shared_info), soc_id);
    if (!sh_info) {
      st_rte_free(frame);
      err("%s(%d), sh_info rte_malloc fail\n", __func__, idx);
      return -ENOMEM;
    }
    sh_info->free_cb = tx_video_session_frames_free_cb;
    sh_info->fcb_opaque = s;
    rte_mbuf_ext_refcnt_set(sh_info, 0);

    s->st20_frames_iova[i] = rte_mem_virt2iova(frame);
    s->st20_frames_sh_info[i] = sh_info;
    s->st20_frames[i] = frame;
  }

  dbg("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int tx_video_session_free_frames(struct st_tx_video_session_impl* s) {
  if (s->st20_frames) {
    for (int i = 0; i < s->st20_frames_cnt; i++) {
      tx_video_session_free_frame(s, i);
    }
  }

  if (s->st20_frames_iova) {
    st_rte_free(s->st20_frames_iova);
    s->st20_frames_iova = NULL;
  }
  if (s->st20_frames) {
    st_rte_free(s->st20_frames);
    s->st20_frames = NULL;
  }
  if (s->st20_frames_sh_info) {
    st_rte_free(s->st20_frames_sh_info);
    s->st20_frames_sh_info = NULL;
  }
  s->st20_frames_cnt = 0;

  dbg("%s(%d), succ\n", __func__, s->idx);
  return 0;
}

static int tx_video_session_init_pacing(struct st_main_impl* impl,
                                        struct st_tx_video_session_impl* s) {
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
  return 0;
}

static int tx_video_session_sync_pacing(struct st_main_impl* impl,
                                        struct st_tx_video_session_impl* s, bool sync) {
  int idx = s->idx;
  struct st_tx_video_pacing* pacing = &s->pacing;
  double frame_time = pacing->frame_time;
  /* always use ST_PORT_P for ptp now */
  uint64_t ptp_time = st_get_ptp_time(impl, ST_PORT_P);
  uint64_t epochs = ptp_time / frame_time;
  double to_epoch_tr_offset;

  dbg("%s(%d), epochs %" PRIu64 " %" PRIu64 ", ptp_time %" PRIu64 "\n", __func__, idx,
      epochs, pacing->cur_epochs, ptp_time);
  if (epochs == pacing->cur_epochs) {
    /* likely most previous frame can enqueue within previous timing */
    epochs++;
  }

  if ((epochs + 1) == pacing->cur_epochs) {
    /* corner case for rtp packet way */
    epochs = pacing->cur_epochs + 1;
  }

  to_epoch_tr_offset = pacing_tr_offset_time(pacing, epochs) - ptp_time;
  if (to_epoch_tr_offset < 0) {
    /* current time run out of tr offset already, sync to next epochs */
    s->st20_epoch_mismatch++;
    epochs++;
    to_epoch_tr_offset = pacing_tr_offset_time(pacing, epochs) - ptp_time;
  }

  if (to_epoch_tr_offset < 0) {
    /* should never happen */
    err("%s(%d), error to_epoch_tr_offset %f, ptp_time %" PRIu64 ", epochs %" PRIu64
        " %" PRIu64 "\n",
        __func__, idx, to_epoch_tr_offset, ptp_time, epochs, pacing->cur_epochs);
    to_epoch_tr_offset = 0;
  }

  pacing->cur_epochs = epochs;
  pacing->cur_time_stamp = pacing_time_stamp(pacing, epochs);
  pacing->tsc_time_cursor = (double)st_get_tsc(impl) + to_epoch_tr_offset;
  dbg("%s(%d), epochs %lu time_stamp %u time_cursor %f\n", __func__, idx,
      pacing->cur_epochs, pacing->cur_time_stamp, pacing->tsc_time_cursor);

  if (sync) {
    dbg("%s(%d), delay to epoch_time %f, cur %" PRIu64 "\n", __func__, idx,
        pacing->tsc_time_cursor, st_get_tsc(impl));
    st_tsc_delay_to(impl, pacing->tsc_time_cursor);
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

static int _tx_video_session_train_pacing(struct st_main_impl* impl,
                                          struct st_tx_video_session_impl* s,
                                          enum st_session_port s_port) {
  enum st_port port = st_port_logic2phy(s->port_maps, s_port);
  struct rte_mbuf* pad = s->pad[s_port][ST20_PKT_TYPE_NORMAL];
  int idx = s->idx;
  uint16_t port_id = s->port_id[s_port];
  uint16_t queue_id = s->queue_id[s_port];
  int pad_pkts, ret;
  int loop_cnt = 30;
  int trim = 5;
  double array[loop_cnt];
  double pkts_per_sec_sum = 0;
  float pad_interval;
  uint64_t rl_bps = tx_video_session_rl_bps(s);
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
      for (int j = 0; j < pkts; j++) {
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
  info("%s(%d), trained pad_interval %f pkts_per_frame %f with time %fs\n", __func__, idx,
       pad_interval, pkts_per_frame,
       (double)(train_end_time - train_start_time) / NS_PER_S);
  return 0;
}

static int tx_video_session_train_pacing(struct st_main_impl* impl,
                                         struct st_tx_video_session_impl* s) {
  int num_port = s->ops.num_port;
  struct st_tx_video_pacing* pacing = &s->pacing;
  int ret;

  if (pacing->trained) return 0;

  if (ST21_TX_PACING_WAY_TSC != impl->tx_pacing_way) {
    for (int i = 0; i < num_port; i++) {
      ret = _tx_video_session_train_pacing(impl, s, i);
      if (ret < 0) return ret;
    }
  } else {
    /* revert to default vrx */
    pacing->tr_offset_vrx = s->st21_vrx_narrow;
  }

  pacing->trained = true;

  return 0;
}

static int tx_video_session_init_st22_boxes(struct st_main_impl* impl,
                                            struct st_tx_video_session_impl* s) {
  struct st22_jpvs* jpvs = &s->st22_boxes.jpvs;
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

  struct st22_colr* colr = &s->st22_boxes.colr;
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

static int tx_video_session_init_hdr(struct st_main_impl* impl,
                                     struct st_tx_video_session_impl* s,
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

  /* ether hdr */
  ret = st_dev_dst_ip_mac(impl, dip, st_eth_d_addr(eth), port);
  if (ret < 0) {
    err("%s(%d), st_dev_dst_ip_mac fail %d for %d.%d.%d.%d\n", __func__, idx, ret, dip[0],
        dip[1], dip[2], dip[3]);
    return ret;
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
  /* rtp size + ipv4 + udp */
  ipv4->total_length = htons(s->st20_pkt_size + sizeof(*ipv4) + sizeof(*udp));
  ipv4->next_proto_id = 17;
  st_memcpy(&ipv4->src_addr, sip, ST_IP_ADDR_LEN);
  st_memcpy(&ipv4->dst_addr, dip, ST_IP_ADDR_LEN);

  /* udp hdr */
  udp->src_port = htons(s->st20_src_port[s_port]);
  udp->dst_port = htons(s->st20_dst_port[s_port]);
  /* rtp size + udp */
  udp->dgram_len = htons(ipv4->total_length - sizeof(*ipv4));
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
  return 0;
}

static int tx_video_session_build_single(struct st_main_impl* impl,
                                         struct st_tx_video_session_impl* s,
                                         struct rte_mbuf* pkt,
                                         struct rte_mbuf* pkt_chain) {
  struct st_rfc4175_video_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  struct rte_udp_hdr* udp;
  struct st20_rfc4175_rtp_hdr* rtp;
  struct st20_rfc4175_extra_rtp_hdr* e_rtp = NULL;
  struct st20_tx_ops* ops = &s->ops;
  uint32_t offset;
  uint16_t line1_number, line1_offset;
  bool single_line = (ops->packing == ST20_PACKING_GPM_SL);

  if (s->st20_pkt_idx >= s->st20_total_pkts) {
    s->st20_stat_pkts_dummy++;
    rte_pktmbuf_free(pkt_chain);
    return 0;
  }

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
    int pkts_in_line = s->st20_pkts_in_line;
    line1_number = s->st20_pkt_idx / pkts_in_line;
    int pixel_in_pkt = s->st20_pkt_len / s->st20_pg.size * s->st20_pg.coverage;
    line1_offset = pixel_in_pkt * (s->st20_pkt_idx % pkts_in_line);
    offset = (line1_number * ops->width + line1_offset) / s->st20_pg.coverage *
             s->st20_pg.size;
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
  uint16_t field = s->st20_second_field ? ST20_SECOND_FIELD : 0x0000;
  rtp->row_number = htons(line1_number | field);
  rtp->row_offset = htons(line1_offset);
  rtp->base.tmstamp = htonl(s->pacing.cur_time_stamp);

  uint32_t temp =
      single_line ? ((ops->width - line1_offset) / s->st20_pg.coverage * s->st20_pg.size)
                  : (s->st20_frame_size - offset);
  uint16_t left_len = RTE_MIN(s->st20_pkt_len, temp);
  rtp->row_length = htons(left_len);

  if (e_rtp) {
    uint16_t line1_length = (line1_number + 1) * s->st20_bytes_in_line - offset;
    uint16_t line2_length = s->st20_pkt_len - line1_length;
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

  /* attach payload to chainbuf */
  int idx = s->st20_frame_idx;

  rte_pktmbuf_attach_extbuf(pkt_chain, s->st20_frames[idx] + offset,
                            s->st20_frames_iova[idx] + offset, left_len,
                            s->st20_frames_sh_info[idx]);
  rte_mbuf_ext_refcnt_update(s->st20_frames_sh_info[idx], 1);
  pkt_chain->data_len = pkt_chain->pkt_len = left_len;
  /* chain the pkt */
  rte_pktmbuf_chain(pkt, pkt_chain);

  udp->dgram_len = htons(pkt->pkt_len - pkt->l2_len - pkt->l3_len);
  ipv4->total_length = htons(pkt->pkt_len - pkt->l2_len);

  return 0;
}

static int tx_video_session_build_single_rtp(struct st_main_impl* impl,
                                             struct st_tx_video_session_impl* s,
                                             struct rte_mbuf* pkt,
                                             struct rte_mbuf* pkt_chain) {
  struct st_rfc3550_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  struct st_rfc3550_rtp_hdr* rtp;
  struct rte_udp_hdr* udp;

  hdr = rte_pktmbuf_mtod(pkt, struct st_rfc3550_hdr*);
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
    rte_atomic32_inc(&s->st20_stat_frame_cnt);
    s->st20_rtp_time = rtp->tmstamp;
    tx_video_session_sync_pacing(impl, s, false);
  }
  /* update rtp time*/
  rtp->tmstamp = htonl(s->pacing.cur_time_stamp);

  /* update mbuf */
  st_mbuf_init_ipv4(pkt);
  pkt->data_len = sizeof(struct st_base_hdr);
  pkt->pkt_len = pkt->data_len;
  /* chain the pkt */
  rte_pktmbuf_chain(pkt, pkt_chain);
  udp->dgram_len = htons(pkt->pkt_len - pkt->l2_len - pkt->l3_len);
  ipv4->total_length = htons(pkt->pkt_len - pkt->l2_len);
  return 0;
}

static int tx_video_session_build_redundant_rtp(struct st_tx_video_session_impl* s,
                                                struct rte_mbuf* pkt_r,
                                                struct rte_mbuf* pkt_base,
                                                struct rte_mbuf* pkt_chain) {
  struct rte_ipv4_hdr *ipv4, *ipv4_base;
  struct st_rfc4175_video_hdr *hdr, *hdr_base;
  hdr = rte_pktmbuf_mtod(pkt_r, struct st_rfc4175_video_hdr*);
  ipv4 = &hdr->ipv4;
  hdr_base = rte_pktmbuf_mtod(pkt_base, struct st_rfc4175_video_hdr*);
  ipv4_base = &hdr_base->ipv4;

  /* copy the hdr: eth, ip, udp */
  rte_memcpy(&hdr->eth, &s->s_hdr[ST_SESSION_PORT_R].eth, sizeof(hdr->eth));
  rte_memcpy(ipv4, &s->s_hdr[ST_SESSION_PORT_R].ipv4, sizeof(hdr->ipv4));
  rte_memcpy(&hdr->udp, &s->s_hdr[ST_SESSION_PORT_R].udp, sizeof(hdr->udp));

  /* update ipv4 hdr */
  ipv4->packet_id = ipv4_base->packet_id;

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

  return 0;
}

static int tx_video_session_build_redundant(struct st_tx_video_session_impl* s,
                                            struct rte_mbuf* pkt_r,
                                            struct rte_mbuf* pkt_base,
                                            struct rte_mbuf* pkt_chain) {
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

  return 0;
}

static int tx_video_session_build_st22(struct st_main_impl* impl,
                                       struct st_tx_video_session_impl* s,
                                       struct rte_mbuf* pkt, struct rte_mbuf* pkt_chain) {
  struct st22_rfc9134_video_hdr* hdr;
  struct rte_ipv4_hdr* ipv4;
  struct rte_udp_hdr* udp;
  struct st22_rfc9134_rtp_hdr* rtp;
  struct st22_tx_video_info* st22_info = s->st22_info;

  if (s->st20_pkt_idx >= s->st20_total_pkts) {
    s->st20_stat_pkts_dummy++;
    rte_pktmbuf_free(pkt_chain);
    return 0;
  }

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
  if (s->st20_pkt_idx >= (s->st20_total_pkts - 1)) {
    rtp->base.marker = 1;
    rtp->last_packet = 1;
  }
  rtp->base.seq_number = htons((uint16_t)s->st20_seq_id);
  s->st20_seq_id++;
  rtp->base.tmstamp = htonl(s->pacing.cur_time_stamp);
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

  /* attach payload to chainbuf */
  int idx = s->st20_frame_idx;

  uint32_t offset = s->st20_pkt_idx * s->st20_pkt_len;
  uint16_t left_len = RTE_MIN(s->st20_pkt_len, st22_info->cur_frame_size - offset);
  rte_pktmbuf_attach_extbuf(pkt_chain, s->st20_frames[idx] + offset,
                            s->st20_frames_iova[idx] + offset, left_len,
                            s->st20_frames_sh_info[idx]);
  rte_mbuf_ext_refcnt_update(s->st20_frames_sh_info[idx], 1);
  pkt_chain->data_len = pkt_chain->pkt_len = left_len;
  /* chain the pkt */
  rte_pktmbuf_chain(pkt, pkt_chain);

  udp->dgram_len = htons(pkt->pkt_len - pkt->l2_len - pkt->l3_len);
  ipv4->total_length = htons(pkt->pkt_len - pkt->l2_len);

  return 0;
}

static int tx_video_sessions_tasklet_pre_start(void* priv) {
  struct st_tx_video_sessions_mgr* mgr = priv;
  struct st_main_impl* impl = mgr->parnet;
  struct st_tx_video_session_impl* s;

  for (int sid = 0; sid < ST_SCH_MAX_TX_VIDEO_SESSIONS; sid++) {
    s = tx_video_session_get(mgr, sid);
    if (!s) continue;

    /* make sure all pacing are trained, for vf */
    tx_video_session_train_pacing(impl, s);
    tx_video_session_put(mgr, sid);
  }

  return 0;
}

static int tx_video_sessions_tasklet_start(void* priv) { return 0; }

static int tx_video_sessions_tasklet_stop(void* priv) { return 0; }

static int tx_video_session_tasklet_frame(struct st_main_impl* impl,
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

  if (rte_ring_full(ring_p)) return -STI_FRAME_RING_FULL;

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
      return 0;
    } else {
      return -STI_FRAME_INFLIGHT_ENQUEUE_FAIL;
    }
  }
  if (send_r && s->has_inflight[ST_SESSION_PORT_R]) {
    n = rte_ring_sp_enqueue_bulk(ring_r, (void**)&s->inflight[ST_SESSION_PORT_R][0], bulk,
                                 NULL);
    if (n > 0) {
      s->has_inflight[ST_SESSION_PORT_R] = false;
      return 0;
    } else {
      return -STI_FRAME_INFLIGHT_R_ENQUEUE_FAIL;
    }
  }

  if (0 == s->st20_pkt_idx) {
    if (ST21_TX_STAT_WAIT_FRAME == s->st20_frame_stat) {
      uint16_t next_frame_idx;
      bool second_field = false;

      /* Query next frame buffer idx */
      ret = ops->get_next_frame(ops->priv, &next_frame_idx, &second_field);
      if (ret < 0) { /* no frame ready from app */
        s->st20_user_busy++;
        dbg("%s(%d), get_next_frame fail %d\n", __func__, idx, ret);
        return -STI_FRAME_APP_GET_FRAME_BUSY;
      }
      s->st20_frame_idx = next_frame_idx;
      s->st20_second_field = second_field;
      s->st20_frame_lines_ready = 0;
      dbg("%s(%d), next_frame_idx %d start\n", __func__, idx, next_frame_idx);
      s->st20_frame_stat = ST21_TX_STAT_SENDING_PKTS;

      tx_video_session_sync_pacing(impl, s, false);
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
      ops->query_frame_lines_ready(ops->priv, s->st20_frame_idx,
                                   &s->st20_frame_lines_ready);
      dbg("%s(%d), need line %u, ready lines %u\n", __func__, s->idx, ops->height,
          s->st20_frame_lines_ready);
      if (line_number >= s->st20_frame_lines_ready) {
        dbg("%s(%d), line %u not ready, ready lines %u\n", __func__, s->idx, line_number,
            s->st20_frame_lines_ready);
        s->st20_lines_not_ready++;
        return -STI_FRAME_APP_SLICE_NOT_READY;
      }
    }
  }

  struct rte_mbuf* pkts[bulk];
  struct rte_mbuf* pkts_r[bulk];
  struct rte_mbuf* pkts_chain[bulk];

  ret = rte_pktmbuf_alloc_bulk(chain_pool, pkts_chain, bulk);
  if (ret < 0) {
    err("%s(%d), pkts chain alloc fail %d\n", __func__, idx, ret);
    return -STI_FRAME_PKT_ALLOC_FAIL;
  }

  ret = rte_pktmbuf_alloc_bulk(hdr_pool_p, pkts, bulk);
  if (ret < 0) {
    err("%s(%d), pkts alloc fail %d\n", __func__, idx, ret);
    rte_pktmbuf_free_bulk(pkts_chain, bulk);
    return -STI_FRAME_PKT_ALLOC_FAIL;
  }

  if (send_r) {
    ret = rte_pktmbuf_alloc_bulk(hdr_pool_r, pkts_r, bulk);
    if (ret < 0) {
      err("%s(%d), pkts_r alloc fail %d\n", __func__, idx, ret);
      rte_pktmbuf_free_bulk(pkts, bulk);
      rte_pktmbuf_free_bulk(pkts_chain, bulk);
      return -STI_FRAME_PKT_ALLOC_FAIL;
    }
  }

  for (unsigned int i = 0; i < bulk; i++) {
    tx_video_session_build_single(impl, s, pkts[i], pkts_chain[i]);
    st_tx_mbuf_set_idx(pkts[i], s->st20_pkt_idx);
    st_tx_mbuf_set_time_stamp(pkts[i], pacing->tsc_time_cursor);

    if (send_r) {
      tx_video_session_build_redundant(s, pkts_r[i], pkts[i], pkts_chain[i]);

      st_tx_mbuf_set_idx(pkts_r[i], s->st20_pkt_idx);
      st_tx_mbuf_set_time_stamp(pkts_r[i], pacing->tsc_time_cursor);
    }

    pacing->tsc_time_cursor += pacing->trs; /* pkt foward */
    s->st20_pkt_idx++;
    s->st20_stat_pkts_build++;
  }

  ret = 0;
  n = rte_ring_sp_enqueue_bulk(ring_p, (void**)&pkts[0], bulk, NULL);
  if (n == 0) {
    for (unsigned int i = 0; i < bulk; i++) s->inflight[ST_SESSION_PORT_P][i] = pkts[i];
    s->has_inflight[ST_SESSION_PORT_P] = true;
    s->inflight_cnt[ST_SESSION_PORT_P]++;
    ret = -STI_FRAME_PKT_ENQUEUE_FAIL;
  }
  if (send_r) {
    n = rte_ring_sp_enqueue_bulk(ring_r, (void**)&pkts_r[0], bulk, NULL);
    if (n == 0) {
      for (unsigned int i = 0; i < bulk; i++)
        s->inflight[ST_SESSION_PORT_R][i] = pkts_r[i];
      s->has_inflight[ST_SESSION_PORT_R] = true;
      s->inflight_cnt[ST_SESSION_PORT_R]++;
      ret = -STI_FRAME_PKT_R_ENQUEUE_FAIL;
    }
  }

  if (s->st20_pkt_idx >= s->st20_total_pkts) {
    dbg("%s(%d), frame %d done\n", __func__, idx, s->st20_frame_idx);
    /* end of current frame */
    s->st20_frame_stat = ST21_TX_STAT_WAIT_FRAME;
    s->st20_pkt_idx = 0;
    rte_atomic32_inc(&s->st20_stat_frame_cnt);
  }

  return ret;
}

static int tx_video_session_tasklet_rtp(struct st_main_impl* impl,
                                        struct st_tx_video_session_impl* s) {
  unsigned int bulk = s->bulk;
  unsigned int n;
  int idx = s->idx;
  struct st_tx_video_pacing* pacing = &s->pacing;
  int ret;
  bool send_r = false;
  struct rte_mempool* hdr_pool_p = s->mbuf_mempool_hdr[ST_SESSION_PORT_P];
  struct rte_mempool* hdr_pool_r = NULL;
  struct rte_ring* ring_p = s->ring[ST_SESSION_PORT_P];
  struct rte_ring* ring_r = NULL;

  if (rte_ring_full(ring_p)) return -STI_RTP_RING_FULL;

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
      return 0;
    } else {
      return -STI_RTP_INFLIGHT_ENQUEUE_FAIL;
    }
  }
  if (send_r && s->has_inflight[ST_SESSION_PORT_R]) {
    n = rte_ring_sp_enqueue_bulk(ring_r, (void**)&s->inflight[ST_SESSION_PORT_R][0], bulk,
                                 NULL);
    if (n > 0) {
      s->has_inflight[ST_SESSION_PORT_R] = false;
      return 0;
    } else {
      return -STI_RTP_INFLIGHT_R_ENQUEUE_FAIL;
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

  n = rte_ring_sc_dequeue_bulk(s->packet_ring, (void**)&pkts_chain, pkts_bulk, NULL);
  if (n == 0) {
    s->st20_user_busy++;
    dbg("%s(%d), rtp pkts not ready %d, ring cnt %d\n", __func__, idx, ret,
        rte_ring_count(s->packet_ring));
    return -STI_RTP_APP_DEQUEUE_FAIL;
  }
  s->ops.notify_rtp_done(s->ops.priv);

  ret = rte_pktmbuf_alloc_bulk(hdr_pool_p, pkts, bulk);
  if (ret < 0) {
    err("%s(%d), pkts alloc fail %d\n", __func__, idx, ret);
    rte_pktmbuf_free_bulk(pkts_chain, bulk);
    return -STI_RTP_PKT_ALLOC_FAIL;
  }

  if (send_r) {
    ret = rte_pktmbuf_alloc_bulk(hdr_pool_r, pkts_r, bulk);
    if (ret < 0) {
      err("%s(%d), pkts_r alloc fail %d\n", __func__, idx, ret);
      rte_pktmbuf_free_bulk(pkts, bulk);
      rte_pktmbuf_free_bulk(pkts_chain, bulk);
      return -STI_RTP_PKT_ALLOC_FAIL;
    }
  }

  for (unsigned int i = 0; i < pkts_bulk; i++) {
    tx_video_session_build_single_rtp(impl, s, pkts[i], pkts_chain[i]);
    st_tx_mbuf_set_idx(pkts[i], s->st20_pkt_idx);
    st_tx_mbuf_set_time_stamp(pkts[i], pacing->tsc_time_cursor);

    if (send_r) {
      tx_video_session_build_redundant_rtp(s, pkts_r[i], pkts[i], pkts_chain[i]);
      st_tx_mbuf_set_idx(pkts_r[i], s->st20_pkt_idx);
      st_tx_mbuf_set_time_stamp(pkts_r[i], pacing->tsc_time_cursor);
    }

    pacing->tsc_time_cursor += pacing->trs; /* pkt foward */
    s->st20_pkt_idx++;
    s->st20_stat_pkts_build++;
  }

  /* build dummy bulk pkts to satisfy video transmitter which is bulk based */
  if (eof) {
    for (unsigned int i = pkts_bulk; i < bulk; i++) {
      st_tx_mbuf_set_idx(pkts[i], s->st20_total_pkts);
      st_tx_mbuf_set_time_stamp(pkts[i], pacing->tsc_time_cursor);
      if (send_r) {
        st_tx_mbuf_set_idx(pkts_r[i], s->st20_total_pkts);
        st_tx_mbuf_set_time_stamp(pkts_r[i], pacing->tsc_time_cursor);
      }
      s->st20_stat_pkts_dummy++;
    }
  }

  ret = 0;
  n = rte_ring_sp_enqueue_bulk(ring_p, (void**)&pkts[0], bulk, NULL);
  if (n == 0) {
    for (unsigned int i = 0; i < bulk; i++) s->inflight[ST_SESSION_PORT_P][i] = pkts[i];
    s->has_inflight[ST_SESSION_PORT_P] = true;
    s->inflight_cnt[ST_SESSION_PORT_P]++;
    ret = -STI_RTP_PKT_ENQUEUE_FAIL;
  }
  if (send_r) {
    n = rte_ring_sp_enqueue_bulk(ring_r, (void**)&pkts_r[0], bulk, NULL);
    if (n == 0) {
      for (unsigned int i = 0; i < bulk; i++)
        s->inflight[ST_SESSION_PORT_R][i] = pkts_r[i];
      s->has_inflight[ST_SESSION_PORT_R] = true;
      s->inflight_cnt[ST_SESSION_PORT_R]++;
      ret = -STI_RTP_PKT_R_ENQUEUE_FAIL;
    }
  }
  return ret;
}

static int tx_video_session_tasklet_st22(struct st_main_impl* impl,
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

  if (rte_ring_full(ring_p)) return -STI_ST22_RING_FULL;

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
      return 0;
    } else {
      return -STI_ST22_INFLIGHT_ENQUEUE_FAIL;
    }
  }
  if (send_r && s->has_inflight[ST_SESSION_PORT_R]) {
    n = rte_ring_sp_enqueue_bulk(ring_r, (void**)&s->inflight[ST_SESSION_PORT_R][0], bulk,
                                 NULL);
    if (n > 0) {
      s->has_inflight[ST_SESSION_PORT_R] = false;
      return 0;
    } else {
      return -STI_ST22_INFLIGHT_R_ENQUEUE_FAIL;
    }
  }

  if (0 == s->st20_pkt_idx) {
    if (ST21_TX_STAT_WAIT_FRAME == s->st20_frame_stat) {
      uint16_t next_frame_idx;
      size_t codestream_size = s->st22_codestream_size;

      /* Query next frame buffer idx */
      ret = st22_info->get_next_frame(ops->priv, &next_frame_idx, &codestream_size);
      if (ret < 0) { /* no frame ready from app */
        s->st20_user_busy++;
        dbg("%s(%d), get_next_frame fail %d\n", __func__, idx, ret);
        return -STI_ST22_APP_GET_FRAME_BUSY;
      }
      if ((codestream_size > s->st22_codestream_size) || !codestream_size) {
        err("%s(%d), invalid codestream_size %ld\n", __func__, idx, codestream_size);
        return -STI_ST22_APP_GET_FRAME_ERR_SIZE;
      }
      size_t frame_size = codestream_size + s->st22_box_hdr_length;
      s->st20_total_pkts = frame_size / s->st20_pkt_len;
      if (frame_size % s->st20_pkt_len) s->st20_total_pkts++;
      st22_info->cur_frame_size = frame_size;
      s->st20_frame_idx = next_frame_idx;
      dbg("%s(%d), next_frame_idx %d(%d pkts) start\n", __func__, idx, next_frame_idx,
          s->st20_total_pkts);
      s->st20_frame_stat = ST21_TX_STAT_SENDING_PKTS;

      tx_video_session_sync_pacing(impl, s, false);
    }
  }

  struct rte_mbuf* pkts[bulk];
  struct rte_mbuf* pkts_r[bulk];
  struct rte_mbuf* pkts_chain[bulk];

  ret = rte_pktmbuf_alloc_bulk(chain_pool, pkts_chain, bulk);
  if (ret < 0) {
    err("%s(%d), pkts chain alloc fail %d\n", __func__, idx, ret);
    return -STI_ST22_PKT_ALLOC_FAIL;
  }

  ret = rte_pktmbuf_alloc_bulk(hdr_pool_p, pkts, bulk);
  if (ret < 0) {
    err("%s(%d), pkts alloc fail %d\n", __func__, idx, ret);
    rte_pktmbuf_free_bulk(pkts_chain, bulk);
    return -STI_ST22_PKT_ALLOC_FAIL;
  }

  if (send_r) {
    ret = rte_pktmbuf_alloc_bulk(hdr_pool_r, pkts_r, bulk);
    if (ret < 0) {
      err("%s(%d), pkts_r alloc fail %d\n", __func__, idx, ret);
      rte_pktmbuf_free_bulk(pkts, bulk);
      rte_pktmbuf_free_bulk(pkts_chain, bulk);
      return -STI_ST22_PKT_ALLOC_FAIL;
    }
  }

  for (unsigned int i = 0; i < bulk; i++) {
    tx_video_session_build_st22(impl, s, pkts[i], pkts_chain[i]);
    st_tx_mbuf_set_idx(pkts[i], s->st20_pkt_idx);
    st_tx_mbuf_set_time_stamp(pkts[i], pacing->tsc_time_cursor);

    if (send_r) {
      tx_video_session_build_redundant(s, pkts_r[i], pkts[i], pkts_chain[i]);

      st_tx_mbuf_set_idx(pkts_r[i], s->st20_pkt_idx);
      st_tx_mbuf_set_time_stamp(pkts_r[i], pacing->tsc_time_cursor);
    }

    pacing->tsc_time_cursor += pacing->trs; /* pkt foward */
    s->st20_pkt_idx++;
    s->st20_stat_pkts_build++;
  }

  ret = 0;
  n = rte_ring_sp_enqueue_bulk(ring_p, (void**)&pkts[0], bulk, NULL);
  if (n == 0) {
    for (unsigned int i = 0; i < bulk; i++) s->inflight[ST_SESSION_PORT_P][i] = pkts[i];
    s->has_inflight[ST_SESSION_PORT_P] = true;
    s->inflight_cnt[ST_SESSION_PORT_P]++;
    ret = -STI_ST22_PKT_ENQUEUE_FAIL;
  }
  if (send_r) {
    n = rte_ring_sp_enqueue_bulk(ring_r, (void**)&pkts_r[0], bulk, NULL);
    if (n == 0) {
      for (unsigned int i = 0; i < bulk; i++)
        s->inflight[ST_SESSION_PORT_R][i] = pkts_r[i];
      s->has_inflight[ST_SESSION_PORT_R] = true;
      s->inflight_cnt[ST_SESSION_PORT_R]++;
      ret = -STI_ST22_PKT_R_ENQUEUE_FAIL;
    }
  }

  if (s->st20_pkt_idx >= s->st20_total_pkts) {
    dbg("%s(%d), frame %d done\n", __func__, idx, s->st20_frame_idx);
    /* end of current frame */
    s->st20_frame_stat = ST21_TX_STAT_WAIT_FRAME;
    s->st20_pkt_idx = 0;
    rte_atomic32_inc(&s->st20_stat_frame_cnt);
    st22_info->frame_idx++;
  }

  return ret;
}

static int tx_video_sessions_tasklet_handler(void* priv) {
  struct st_tx_video_sessions_mgr* mgr = priv;
  struct st_main_impl* impl = mgr->parnet;
  struct st_tx_video_session_impl* s;
  int ret;

  for (int sidx = 0; sidx < mgr->max_idx; sidx++) {
    s = tx_video_session_try_get(mgr, sidx);
    if (!s) continue;

    if (s->st22_info)
      ret = tx_video_session_tasklet_st22(impl, s);
    else if (st20_is_frame_type(s->ops.type))
      ret = tx_video_session_tasklet_frame(impl, s);
    else
      ret = tx_video_session_tasklet_rtp(impl, s);
    s->stat_build_ret_code = ret;

    tx_video_session_put(mgr, sidx);
  }

  return 0;
}

static int tx_video_session_uinit_hw(struct st_main_impl* impl,
                                     struct st_tx_video_session_impl* s) {
  enum st_port port;
  int num_port = s->ops.num_port;

  for (int i = 0; i < num_port; i++) {
    port = st_port_logic2phy(s->port_maps, i);

    if (s->ring[i]) {
      st_ring_dequeue_clean(s->ring[i]);
      rte_ring_free(s->ring[i]);
      s->ring[i] = NULL;
    }

    for (int j = 0; j < ST20_PKT_TYPE_MAX; j++) {
      if (s->pad[i][j]) {
        rte_pktmbuf_free(s->pad[i][j]);
        s->pad[i][j] = NULL;
      }
    }

    if (s->queue_active[i]) {
      /* flush all the pkts in the tx ring desc */
      st_dev_flush_tx_queue(impl, port, s->queue_id[i]);
      st_dev_free_tx_queue(impl, port, s->queue_id[i]);
      s->queue_active[i] = false;
    }
  }

  return 0;
}

static int tx_video_session_init_hw(struct st_main_impl* impl,
                                    struct st_tx_video_sessions_mgr* mgr,
                                    struct st_tx_video_session_impl* s) {
  unsigned int flags, count;
  struct rte_ring* ring;
  char ring_name[32];
  int mgr_idx = mgr->idx, idx = s->idx, num_port = s->ops.num_port;
  int ret;
  uint16_t queue = 0;
  uint16_t port_id;
  struct rte_mbuf* pad;
  enum st_port port;

  for (int i = 0; i < num_port; i++) {
    port = st_port_logic2phy(s->port_maps, i);

    port_id = st_port_id(impl, port);
    for (int j = 0; j < ST20_PKT_TYPE_MAX; j++) {
      if (!s->st20_pkt_info[j].number) continue;
      pad = st_build_pad(impl, port, port_id, RTE_ETHER_TYPE_IPV4,
                         s->st20_pkt_info[j].size);
      if (!pad) {
        tx_video_session_uinit_hw(impl, s);
        return -ENOMEM;
      }
      s->pad[i][j] = pad;
    }

    ret = st_dev_request_tx_queue(impl, port, &queue, tx_video_session_rl_bps(s));
    if (ret < 0) {
      tx_video_session_uinit_hw(impl, s);
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
      tx_video_session_uinit_hw(impl, s);
      return -ENOMEM;
    }
    s->ring[i] = ring;
    info("%s(%d,%d), port(l:%d,p:%d), queue %d, count %u\n", __func__, mgr_idx, idx, i,
         port, queue, count);
  }

  return 0;
}

static int tx_video_session_mempool_free(struct st_tx_video_session_impl* s) {
  int ret;

  if (s->mbuf_mempool_chain) {
    ret = st_mempool_free(s->mbuf_mempool_chain);
    if (ret >= 0) s->mbuf_mempool_chain = NULL;
  }

  for (int i = 0; i < ST_SESSION_PORT_MAX; i++) {
    if (s->mbuf_mempool_hdr[i]) {
      ret = st_mempool_free(s->mbuf_mempool_hdr[i]);
      if (ret >= 0) s->mbuf_mempool_hdr[i] = NULL;
    }
  }

  return 0;
}

static int tx_video_session_mempool_init(struct st_main_impl* impl,
                                         struct st_tx_video_sessions_mgr* mgr,
                                         struct st_tx_video_session_impl* s) {
  struct st20_tx_ops* ops = &s->ops;
  int num_port = ops->num_port, idx = s->idx;
  enum st_port port;
  unsigned int n;
  uint16_t hdr_room_size = 0;
  uint16_t chain_room_size = 0;

  if (s->st22_info) {
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
  }

  for (int i = 0; i < num_port; i++) {
    port = st_port_logic2phy(s->port_maps, i);
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
        tx_video_session_mempool_free(s);
        return -ENOMEM;
      }
      s->mbuf_mempool_hdr[i] = mbuf_pool;
    }
  }

  port = st_port_logic2phy(s->port_maps, ST_SESSION_PORT_P);
  n = st_if_nb_tx_desc(impl, port) + s->ring_count;
  if (ops->type == ST20_TYPE_RTP_LEVEL) n += ops->rtp_ring_size;
  if (s->mbuf_mempool_chain) {
    warn("%s(%d), use previous chain mempool\n", __func__, idx);
  } else {
    char pool_name[32];
    snprintf(pool_name, 32, "TXVIDEOCHAIN-M%d-R%d", mgr->idx, idx);
    struct rte_mempool* mbuf_pool = st_mempool_create(
        impl, port, pool_name, n, ST_MBUF_CACHE_SIZE, 0, chain_room_size);
    if (!mbuf_pool) {
      tx_video_session_mempool_free(s);
      return -ENOMEM;
    }
    s->mbuf_mempool_chain = mbuf_pool;
  }

  return 0;
}

static int tx_video_session_init_packet_ring(struct st_main_impl* impl,
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

static int tx_video_session_uinit_sw(struct st_tx_video_session_impl* s) {
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

  tx_video_session_mempool_free(s);

  tx_video_session_free_frames(s);

  if (s->st22_info) {
    st_rte_free(s->st22_info);
    s->st22_info = NULL;
  }

  return 0;
}

static int tx_video_session_init_st22_frame(struct st_main_impl* impl,
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

static int tx_video_session_init_sw(struct st_main_impl* impl,
                                    struct st_tx_video_sessions_mgr* mgr,
                                    struct st_tx_video_session_impl* s,
                                    struct st22_tx_ops* st22_frame_ops) {
  int idx = s->idx, ret;
  enum st20_type type = s->ops.type;

  if (st22_frame_ops) {
    ret = tx_video_session_init_st22_frame(impl, s, st22_frame_ops);
    if (ret < 0) {
      err("%s(%d), tx_video_session_init_sw fail %d\n", __func__, idx, ret);
      tx_video_session_uinit_sw(s);
      return -EIO;
    }
    tx_video_session_init_st22_boxes(impl, s);
  }

  /* free the pool if any in previous session */
  tx_video_session_mempool_free(s);
  ret = tx_video_session_mempool_init(impl, mgr, s);
  if (ret < 0) {
    err("%s(%d), fail %d\n", __func__, idx, ret);
    tx_video_session_uinit_sw(s);
    return ret;
  }

  if (type == ST20_TYPE_RTP_LEVEL)
    ret = tx_video_session_init_packet_ring(impl, mgr, s);
  else
    ret = tx_video_session_alloc_frames(impl, s);
  if (ret < 0) {
    err("%s(%d), fail %d\n", __func__, idx, ret);
    tx_video_session_uinit_sw(s);
    return ret;
  }

  return 0;
}

static int tx_video_session_init_pkt(struct st_main_impl* impl,
                                     struct st_tx_video_session_impl* s,
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
    s->st20_total_pkts = st22_frame_ops->framebuff_max_size / max_data_len;
    if (st22_frame_ops->framebuff_max_size % max_data_len) s->st20_total_pkts++;
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
    s->st20_pkt_len = 1260;
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

static int tx_video_session_attach(struct st_main_impl* impl,
                                   struct st_tx_video_sessions_mgr* mgr,
                                   struct st_tx_video_session_impl* s,
                                   struct st20_tx_ops* ops, enum st_session_type s_type,
                                   struct st22_tx_ops* st22_frame_ops) {
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

  uint32_t height = ops->interlaced ? (ops->height >> 1) : ops->height;
  if (st22_frame_ops) {
    s->st22_box_hdr_length = sizeof(s->st22_boxes);
    s->st22_codestream_size = st22_frame_ops->framebuff_max_size;
    s->st20_frame_size = s->st22_codestream_size + s->st22_box_hdr_length;
  } else {
    s->st20_frame_size = ops->width * height * s->st20_pg.size / s->st20_pg.coverage;
  }
  s->st20_frames_cnt = ops->framebuff_cnt;

  ret = tx_video_session_init_pkt(impl, s, ops, s_type, st22_frame_ops);
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
  }
  s->st20_ipv4_packet_id = 0;

  s->ring_count = ST_TX_VIDEO_SESSIONS_RING_SIZE;
  /* make sure the ring is smaller than total pkts */
  while (s->ring_count > s->st20_total_pkts) {
    s->ring_count /= 2;
  }

  ret = tx_video_session_init_sw(impl, mgr, s, st22_frame_ops);
  if (ret < 0) {
    err("%s(%d), tx_video_session_init_sw fail %d\n", __func__, idx, ret);
    return -EIO;
  }

  ret = tx_video_session_init_hw(impl, mgr, s);
  if (ret < 0) {
    err("%s(%d), tx_session_init_hw fail %d\n", __func__, idx, ret);
    tx_video_session_uinit_sw(s);
    return -EIO;
  }

  for (int i = 0; i < num_port; i++) {
    ret = tx_video_session_init_hdr(impl, s, i);
    if (ret < 0) {
      err("%s(%d), tx_session_init_hdr fail %d prot %d\n", __func__, idx, ret, i);
      tx_video_session_uinit_hw(impl, s);
      tx_video_session_uinit_sw(s);
      return ret;
    }
  }

  ret = tx_video_session_init_pacing(impl, s);
  if (ret < 0) {
    err("%s(%d), tx_session_init_pacing fail %d\n", __func__, idx, ret);
    tx_video_session_uinit_hw(impl, s);
    tx_video_session_uinit_sw(s);
    return ret;
  }

  s->st20_lines_not_ready = 0;
  s->st20_user_busy = 0;
  s->st20_epoch_mismatch = 0;
  s->st20_troffset_mismatch = 0;
  rte_atomic32_set(&s->st20_stat_frame_cnt, 0);
  s->st20_stat_last_time = st_get_monotonic_time();

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

  tx_video_session_train_pacing(impl, s);

  info("%s(%d), len %d(%d) total %d each line %d type %d\n", __func__, idx,
       s->st20_pkt_len, s->st20_pkt_size, s->st20_total_pkts, s->st20_pkts_in_line,
       s->ops.type);
  info("%s(%d), ops info, w %u h %u fmt %d packing %d pt %d\n", __func__, idx, ops->width,
       ops->height, ops->fmt, ops->packing, ops->payload_type);
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

static void tx_video_session_stat(struct st_tx_video_sessions_mgr* mgr,
                                  struct st_tx_video_session_impl* s) {
  int m_idx = mgr->idx, idx = s->idx;
  uint64_t cur_time_ns = st_get_monotonic_time();
  double time_sec = (double)(cur_time_ns - s->st20_stat_last_time) / NS_PER_S;
  int frame_cnt = rte_atomic32_read(&s->st20_stat_frame_cnt);
  double framerate = frame_cnt / time_sec;

  rte_atomic32_set(&s->st20_stat_frame_cnt, 0);

  info(
      "TX_VIDEO_SESSION(%d,%d:%s): fps %f, frame %d pkts %d:%d inflight %d:%d, cpu busy "
      "%f\n",
      m_idx, idx, s->ops_name, framerate, frame_cnt, s->st20_stat_pkts_build,
      s->st20_stat_pkts_burst, s->trs_inflight_cnt[0], s->inflight_cnt[0],
      s->cpu_busy_score);
  s->st20_stat_last_time = cur_time_ns;
  s->st20_stat_pkts_build = 0;
  s->st20_stat_pkts_burst = 0;
  s->trs_inflight_cnt[0] = 0;
  s->inflight_cnt[0] = 0;
  s->st20_stat_pkts_dummy = 0;
  s->st20_stat_pkts_burst_dummy = 0;

  if (s->st20_epoch_mismatch || s->st20_troffset_mismatch) {
    info("TX_VIDEO_SESSION(%d,%d): mismatch error epoch %u troffset %u\n", m_idx, idx,
         s->st20_epoch_mismatch, s->st20_troffset_mismatch);
    s->st20_epoch_mismatch = 0;
    s->st20_troffset_mismatch = 0;
  }
  if (s->st20_user_busy) {
    info("TX_VIDEO_SESSION(%d,%d): busy as no ready buffer from user %u\n", m_idx, idx,
         s->st20_user_busy);
    s->st20_user_busy = 0;
  }
  if (s->st20_lines_not_ready) {
    info("TX_VIDEO_SESSION(%d,%d): query new lines but app not ready %u\n", m_idx, idx,
         s->st20_lines_not_ready);
    s->st20_lines_not_ready = 0;
  }
  if (frame_cnt <= 0) {
    err("TX_VIDEO_SESSION(%d,%d:%s): build ret %d, trs ret %d:%d\n", m_idx, idx,
        s->ops_name, s->stat_build_ret_code, s->stat_trs_ret_code[0],
        s->stat_trs_ret_code[0]);
  }
}

static int tx_video_session_detach(struct st_main_impl* impl,
                                   struct st_tx_video_sessions_mgr* mgr,
                                   struct st_tx_video_session_impl* s) {
  tx_video_session_stat(mgr, s);
  /* must uinit hw firstly as frame use shared external buffer */
  tx_video_session_uinit_hw(impl, s);
  tx_video_session_uinit_sw(s);
  return 0;
}

static int tx_video_session_init(struct st_main_impl* impl,
                                 struct st_tx_video_sessions_mgr* mgr,
                                 struct st_tx_video_session_impl* s, int idx) {
  s->idx = idx;
  return 0;
}

struct st_tx_video_session_impl* st_tx_video_sessions_mgr_attach(
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
    ret = tx_video_session_init(impl, mgr, s, i);
    if (ret < 0) {
      err("%s(%d), init fail on %d\n", __func__, midx, i);
      tx_video_session_put(mgr, i);
      st_rte_free(s);
      return NULL;
    }
    ret = tx_video_session_attach(impl, mgr, s, ops, s_type, st22_frame_ops);
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

static int tx_video_sessions_mgr_detach(struct st_tx_video_sessions_mgr* mgr,
                                        struct st_tx_video_session_impl* s, int idx) {
  tx_video_session_detach(mgr->parnet, mgr, s);
  mgr->sessions[idx] = NULL;
  st_rte_free(s);
  return 0;
}

int st_tx_video_sessions_mgr_detach(struct st_tx_video_sessions_mgr* mgr,
                                    struct st_tx_video_session_impl* s) {
  int midx = mgr->idx;
  int idx = s->idx;

  s = tx_video_session_get(mgr, idx); /* get the lock */
  if (!s) {
    err("%s(%d,%d), get session fail\n", __func__, midx, idx);
    return -EIO;
  }

  tx_video_sessions_mgr_detach(mgr, s, idx);

  tx_video_session_put(mgr, idx);

  return 0;
}

static int tx_video_sessions_mgr_init(struct st_main_impl* impl, struct st_sch_impl* sch,
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
  ops.pre_start = tx_video_sessions_tasklet_pre_start;
  ops.start = tx_video_sessions_tasklet_start;
  ops.stop = tx_video_sessions_tasklet_stop;
  ops.handler = tx_video_sessions_tasklet_handler;

  mgr->tasklet = st_sch_register_tasklet(sch, &ops);
  if (!mgr->tasklet) {
    err("%s(%d), st_sch_register_tasklet fail\n", __func__, idx);
    return -EIO;
  }

  info("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int tx_video_sessions_mgr_uinit(struct st_tx_video_sessions_mgr* mgr) {
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
    tx_video_sessions_mgr_detach(mgr, s, i);
    tx_video_session_put(mgr, i);
  }

  info("%s(%d), succ\n", __func__, m_idx);
  return 0;
}

int st_tx_video_sessions_mgr_update(struct st_tx_video_sessions_mgr* mgr) {
  int max_idx = 0;

  for (int i = 0; i < ST_SCH_MAX_TX_VIDEO_SESSIONS; i++) {
    if (mgr->sessions[i]) max_idx = i + 1;
  }
  mgr->max_idx = max_idx;
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
      tx_video_session_stat(mgr, s);
      tx_video_session_put(mgr, j);
    }
  }
}

int st_tx_video_sessions_sch_init(struct st_main_impl* impl, struct st_sch_impl* sch) {
  int ret, idx = sch->idx;

  if (sch->tx_video_init) return 0;

  /* create tx video context */
  struct st_tx_video_sessions_mgr* tx_video_mgr = &sch->tx_video_mgr;
  ret = tx_video_sessions_mgr_init(impl, sch, tx_video_mgr);
  if (ret < 0) {
    err("%s(%d), st_tx_video_sessions_mgr_init fail %d\n", __func__, idx, ret);
    return ret;
  }

  ret = st_video_transmitter_init(impl, sch, tx_video_mgr, &sch->video_transmitter);
  if (ret < 0) {
    tx_video_sessions_mgr_uinit(tx_video_mgr);
    err("%s(%d), st_video_transmitter_init fail %d\n", __func__, idx, ret);
    return ret;
  }

  sch->tx_video_init = true;
  return 0;
}

int st_tx_video_sessions_sch_uinit(struct st_main_impl* impl, struct st_sch_impl* sch) {
  if (!sch->tx_video_init) return 0;

  st_video_transmitter_uinit(&sch->video_transmitter);
  tx_video_sessions_mgr_uinit(&sch->tx_video_mgr);
  sch->tx_video_init = false;

  return 0;
}

int st_tx_video_session_migrate(struct st_main_impl* impl,
                                struct st_tx_video_sessions_mgr* mgr,
                                struct st_tx_video_session_impl* s, int idx) {
  tx_video_session_init(impl, mgr, s, idx);
  return 0;
}
