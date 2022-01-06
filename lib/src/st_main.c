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

#include "st_main.h"

#include "st_ancillary_transmitter.h"
#include "st_arp.h"
#include "st_audio_transmitter.h"
#include "st_cni.h"
#include "st_dev.h"
#include "st_fmt.h"
#include "st_log.h"
#include "st_mcast.h"
#include "st_ptp.h"
#include "st_rx_ancillary_session.h"
#include "st_rx_audio_session.h"
#include "st_rx_video_session.h"
#include "st_sch.h"
#include "st_tx_ancillary_session.h"
#include "st_tx_audio_session.h"
#include "st_tx_video_session.h"
#include "st_video_transmitter.h"

enum st_port st_port_by_id(struct st_main_impl* impl, uint16_t port_id) {
  int num_ports = st_num_ports(impl);
  int i;

  for (i = 0; i < num_ports; i++) {
    if (port_id == st_port_id(impl, i)) return i;
  }

  err("%s, invalid port_id %d\n", __func__, port_id);
  return ST_PORT_MAX;
}

static void* st_calibrate_tsc(void* arg) {
  struct st_main_impl* impl = arg;
  int loop = 100;
  uint64_t tsc_hz_sum = 0;

  for (int i = 0; i < loop; i++) {
    uint64_t start, start_tsc, end, end_tsc;

    start = st_get_monotonic_time();
    start_tsc = rte_get_tsc_cycles();

    st_sleep_ms(10);

    end = st_get_monotonic_time();
    end_tsc = rte_get_tsc_cycles();
    tsc_hz_sum += NS_PER_S * (end_tsc - start_tsc) / (end - start);
  }

  impl->tsc_hz = tsc_hz_sum / loop;
  info("%s, tscHz %" PRIu64 "\n", __func__, impl->tsc_hz);
  return NULL;
}

static int st_tx_video_init(struct st_main_impl* impl, struct st_sch_impl* sch) {
  int ret, idx = sch->idx;

  if (sch->tx_video_init) return 0;

  /* create tx video context */
  struct st_tx_video_sessions_mgr* tx_video_mgr = &sch->tx_video_mgr;
  ret = st_tx_video_sessions_mgr_init(impl, sch, tx_video_mgr);
  if (ret < 0) {
    err("%s(%d), st_tx_video_sessions_mgr_init fail %d\n", __func__, idx, ret);
    return ret;
  }

  ret = st_video_transmitter_init(impl, sch, tx_video_mgr, &sch->video_transmitter);
  if (ret < 0) {
    st_tx_video_sessions_mgr_uinit(tx_video_mgr);
    err("%s(%d), st_video_transmitter_init fail %d\n", __func__, idx, ret);
    return ret;
  }

  sch->tx_video_init = true;
  return 0;
}

static int st_tx_video_uinit(struct st_main_impl* impl) {
  struct st_sch_impl* sch;

  for (int sch_idx = 0; sch_idx < ST_MAX_SCH_NUM; sch_idx++) {
    sch = st_get_sch(impl, sch_idx);
    if (!sch->tx_video_init) continue;

    st_video_transmitter_uinit(&sch->video_transmitter);
    st_tx_video_sessions_mgr_uinit(&sch->tx_video_mgr);
    sch->tx_video_init = false;
  }

  return 0;
}

static int st_rx_video_init(struct st_main_impl* impl, struct st_sch_impl* sch) {
  int ret, idx = sch->idx;

  if (sch->rx_video_init) return 0;

  /* create tx video context */
  ret = st_rx_video_sessions_mgr_init(impl, sch, &sch->rx_video_mgr);
  if (ret < 0) {
    err("%s(%d), st_rx_video_sessions_mgr_init fail %d\n", __func__, idx, ret);
    return ret;
  }

  sch->rx_video_init = true;
  return 0;
}

static int st_rx_video_uinit(struct st_main_impl* impl) {
  struct st_sch_impl* sch;

  for (int sch_idx = 0; sch_idx < ST_MAX_SCH_NUM; sch_idx++) {
    sch = st_get_sch(impl, sch_idx);
    if (!sch->rx_video_init) continue;

    st_rx_video_sessions_mgr_uinit(&sch->rx_video_mgr);
    sch->rx_video_init = false;
  }

  return 0;
}

static int st_tx_audio_init(struct st_main_impl* impl) {
  int ret;

  if (impl->tx_a_init) return 0;

  /* create tx audio context */
  ret = st_tx_audio_sessions_mgr_init(impl, impl->main_sch, &impl->tx_a_mgr);
  if (ret < 0) {
    err("%s, st_tx_audio_sessions_mgr_init fail\n", __func__);
    return ret;
  }
  ret = st_audio_transmitter_init(impl, impl->main_sch, &impl->tx_a_mgr, &impl->a_trs);
  if (ret < 0) {
    st_tx_audio_sessions_mgr_uinit(&impl->tx_a_mgr);
    err("%s, st_audio_transmitter_init fail %d\n", __func__, ret);
    return ret;
  }

  impl->tx_a_init = true;
  return 0;
}

static int st_tx_audio_uinit(struct st_main_impl* impl) {
  if (!impl->tx_a_init) return 0;

  /* free tx audio context */
  st_audio_transmitter_uinit(&impl->a_trs);
  st_tx_audio_sessions_mgr_uinit(&impl->tx_a_mgr);

  impl->tx_a_init = false;
  return 0;
}

static int st_rx_audio_init(struct st_main_impl* impl) {
  int ret;

  if (impl->rx_a_init) return 0;

  /* create rx audio context */
  ret = st_rx_audio_sessions_mgr_init(impl, impl->main_sch, &impl->rx_a_mgr);
  if (ret < 0) {
    err("%s, st_tx_audio_sessions_mgr_init fail\n", __func__);
    return ret;
  }

  impl->rx_a_init = true;
  return 0;
}

static int st_rx_audio_uinit(struct st_main_impl* impl) {
  if (!impl->rx_a_init) return 0;

  st_rx_audio_sessions_mgr_uinit(&impl->rx_a_mgr);

  impl->rx_a_init = false;
  return 0;
}

static int st_tx_anc_init(struct st_main_impl* impl) {
  int ret;

  if (impl->tx_anc_init) return 0;

  /* create tx ancillary context, todo: creat only when it has ancillary */
  ret = st_tx_ancillary_sessions_mgr_init(impl, impl->main_sch, &impl->tx_anc_mgr);
  if (ret < 0) {
    err("%s, st_tx_ancillary_sessions_mgr_init fail\n", __func__);
    return ret;
  }
  ret = st_ancillary_transmitter_init(impl, impl->main_sch, &impl->tx_anc_mgr,
                                      &impl->anc_trs);
  if (ret < 0) {
    st_tx_ancillary_sessions_mgr_uinit(&impl->tx_anc_mgr);
    err("%s, st_ancillary_transmitter_init fail %d\n", __func__, ret);
    return ret;
  }

  impl->tx_anc_init = true;
  return 0;
}

static int st_tx_anc_uinit(struct st_main_impl* impl) {
  if (!impl->tx_anc_init) return 0;

  /* free tx ancillary context */
  st_ancillary_transmitter_uinit(&impl->anc_trs);
  st_tx_ancillary_sessions_mgr_uinit(&impl->tx_anc_mgr);

  impl->tx_anc_init = false;
  return 0;
}

static int st_rx_anc_init(struct st_main_impl* impl) {
  int ret;

  if (impl->rx_anc_init) return 0;

  /* create rx ancillary context */
  ret = st_rx_ancillary_sessions_mgr_init(impl, impl->main_sch, &impl->rx_anc_mgr);
  if (ret < 0) {
    err("%s, st_tx_audio_sessions_mgr_init fail\n", __func__);
    return ret;
  }

  impl->rx_anc_init = true;
  return 0;
}

static int st_rx_anc_uinit(struct st_main_impl* impl) {
  if (!impl->rx_anc_init) return 0;

  st_rx_ancillary_sessions_mgr_uinit(&impl->rx_anc_mgr);

  impl->rx_anc_init = false;
  return 0;
}

static int st_main_create(struct st_main_impl* impl) {
  int ret;

  ret = st_dev_create(impl);
  if (ret < 0) {
    err("%s, st_dev_create fail %d\n", __func__, ret);
    return ret;
  }

  ret = st_arp_init(impl);
  if (ret < 0) {
    err("%s, st_arp_init fail %d\n", __func__, ret);
    return ret;
  }

  ret = st_mcast_init(impl);
  if (ret < 0) {
    err("%s, st_mcast_init fail %d\n", __func__, ret);
    return ret;
  }

  ret = st_ptp_init(impl);
  if (ret < 0) {
    err("%s, st_ptp_init fail %d\n", __func__, ret);
    return ret;
  }

  ret = st_cni_init(impl);
  if (ret < 0) {
    err("%s, st_cni_init fail %d\n", __func__, ret);
    return ret;
  }

  rte_ctrl_thread_create(&impl->tsc_cal_tid, "tsc_calibrate", NULL, st_calibrate_tsc,
                         impl);

  info("%s, succ\n", __func__);
  return 0;
}

static int st_main_free(struct st_main_impl* impl) {
  if (impl->tsc_cal_tid) {
    pthread_join(impl->tsc_cal_tid, NULL);
    impl->tsc_cal_tid = 0;
  }

  st_cni_uinit(impl);
  st_ptp_uinit(impl);
  st_arp_uinit(impl);
  st_mcast_uinit(impl);

  st_dev_free(impl);
  info("%s, succ\n", __func__);
  return 0;
}

static int st_ip_addr_check(uint8_t* ip) {
  for (int i = 0; i < ST_IP_ADDR_LEN; i++) {
    if (ip[i]) return 0;
  }

  return -EINVAL;
}

static int st_user_params_check(struct st_init_params* p) {
  int num_ports = p->num_ports, ret;
  uint8_t* ip;

  if ((num_ports > ST_PORT_MAX) || (num_ports <= 0)) {
    err("%s, invalid num_ports %d\n", __func__, num_ports);
    return -EINVAL;
  }

  if (p->tx_sessions_cnt_max < 0) {
    err("%s, invalid tx_sessions_cnt_max %d\n", __func__, p->tx_sessions_cnt_max);
    return -EINVAL;
  }

  if (p->rx_sessions_cnt_max < 0) {
    err("%s, invalid rx_sessions_cnt_max %d\n", __func__, p->rx_sessions_cnt_max);
    return -EINVAL;
  }

  if (num_ports > 1) {
    if (0 == strncmp(st_p_port(p), st_r_port(p), ST_PORT_MAX_LEN)) {
      err("%s, same %s for both port\n", __func__, st_p_port(p));
      return -EINVAL;
    }
  }

  for (int i = 0; i < num_ports; i++) {
    ip = p->sip_addr[i];
    ret = st_ip_addr_check(ip);
    if (ret < 0) {
      err("%s(%d), invalid ip %d.%d.%d.%d\n", __func__, i, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (num_ports > 1) {
    if (0 == memcmp(p->sip_addr[0], p->sip_addr[1], ST_IP_ADDR_LEN)) {
      err("%s, same %d.%d.%d.%d for both ip\n", __func__, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  return 0;
}

static int st_tx_video_ops_check(struct st20_tx_ops* ops) {
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

  if (ops->type == ST20_TYPE_FRAME_LEVEL) {
    if ((ops->framebuff_cnt < 2) || (ops->framebuff_cnt > ST20_FB_MAX_COUNT)) {
      err("%s, invalid framebuff_cnt %d, should in range [2:%d]\n", __func__,
          ops->framebuff_cnt, ST20_FB_MAX_COUNT);
      return -EINVAL;
    }
  } else if (ops->type == ST20_TYPE_RTP_LEVEL) {
    if (ops->rtp_ring_size <= 0) {
      err("%s, invalid rtp_ring_size %d\n", __func__, ops->rtp_ring_size);
      return -EINVAL;
    }
    if (!st_rtp_len_valid(ops->rtp_pkt_size)) {
      err("%s, invalid rtp_pkt_size %d\n", __func__, ops->rtp_pkt_size);
      return -EINVAL;
    }
  }

  return 0;
}

static int st22_tx_video_ops_check(struct st22_tx_ops* ops) {
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

  if (ops->rtp_ring_size <= 0) {
    err("%s, invalid rtp_ring_size %d\n", __func__, ops->rtp_ring_size);
    return -EINVAL;
  }
  if (!st_rtp_len_valid(ops->rtp_pkt_size)) {
    err("%s, invalid rtp_pkt_size %d\n", __func__, ops->rtp_pkt_size);
    return -EINVAL;
  }

  return 0;
}

static int st_tx_audio_ops_check(struct st30_tx_ops* ops) {
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

  if (ops->type == ST30_TYPE_FRAME_LEVEL) {
    if (ops->framebuff_cnt < 1) {
      err("%s, invalid framebuff_cnt %d\n", __func__, ops->framebuff_cnt);
      return -EINVAL;
    }
  } else if (ops->type == ST30_TYPE_RTP_LEVEL) {
    if (ops->rtp_ring_size <= 0) {
      err("%s, invalid rtp_ring_size %d\n", __func__, ops->rtp_ring_size);
      return -EINVAL;
    }
    if ((ops->sample_size <= 0) || (ops->sample_size > ST_PKT_MAX_RTP_BYTES)) {
      err("%s, invalid sample_size %d\n", __func__, ops->sample_size);
      return -EINVAL;
    }
  }

  return 0;
}

static int st_tx_ancillary_ops_check(struct st40_tx_ops* ops) {
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

  if (ops->type == ST40_TYPE_FRAME_LEVEL) {
    if (ops->framebuff_cnt < 1) {
      err("%s, invalid framebuff_cnt %d\n", __func__, ops->framebuff_cnt);
      return -EINVAL;
    }
  } else if (ops->type == ST40_TYPE_RTP_LEVEL) {
    if (ops->rtp_ring_size <= 0) {
      err("%s, invalid rtp_ring_size %d\n", __func__, ops->rtp_ring_size);
      return -EINVAL;
    }
  }

  return 0;
}

static int st_rx_video_ops_check(struct st20_rx_ops* ops) {
  int num_ports = ops->num_port, ret;
  uint8_t* ip;

  if ((num_ports > ST_PORT_MAX) || (num_ports <= 0)) {
    err("%s, invalid num_ports %d\n", __func__, num_ports);
    return -EINVAL;
  }

  for (int i = 0; i < num_ports; i++) {
    ip = ops->sip_addr[i];
    ret = st_ip_addr_check(ip);
    if (ret < 0) {
      err("%s(%d), invalid ip %d.%d.%d.%d\n", __func__, i, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (num_ports > 1) {
    if (0 == memcmp(ops->sip_addr[0], ops->sip_addr[1], ST_IP_ADDR_LEN)) {
      err("%s, same %d.%d.%d.%d for both ip\n", __func__, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (ops->type == ST20_TYPE_FRAME_LEVEL) {
    if ((ops->framebuff_cnt < 2) || (ops->framebuff_cnt > ST20_FB_MAX_COUNT)) {
      err("%s, invalid framebuff_cnt %d, should in range [2:%d]\n", __func__,
          ops->framebuff_cnt, ST20_FB_MAX_COUNT);
      return -EINVAL;
    }
  } else if (ops->type == ST20_TYPE_RTP_LEVEL) {
    if (ops->rtp_ring_size <= 0) {
      err("%s, invalid rtp_ring_size %d\n", __func__, ops->rtp_ring_size);
      return -EINVAL;
    }
  }

  return 0;
}

static int st22_rx_video_ops_check(struct st22_rx_ops* ops) {
  int num_ports = ops->num_port, ret;
  uint8_t* ip;

  if ((num_ports > ST_PORT_MAX) || (num_ports <= 0)) {
    err("%s, invalid num_ports %d\n", __func__, num_ports);
    return -EINVAL;
  }

  for (int i = 0; i < num_ports; i++) {
    ip = ops->sip_addr[i];
    ret = st_ip_addr_check(ip);
    if (ret < 0) {
      err("%s(%d), invalid ip %d.%d.%d.%d\n", __func__, i, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (num_ports > 1) {
    if (0 == memcmp(ops->sip_addr[0], ops->sip_addr[1], ST_IP_ADDR_LEN)) {
      err("%s, same %d.%d.%d.%d for both ip\n", __func__, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (ops->rtp_ring_size <= 0) {
    err("%s, invalid rtp_ring_size %d\n", __func__, ops->rtp_ring_size);
    return -EINVAL;
  }

  return 0;
}

static int st_rx_audio_ops_check(struct st30_rx_ops* ops) {
  int num_ports = ops->num_port, ret;
  uint8_t* ip;

  if ((num_ports > ST_PORT_MAX) || (num_ports <= 0)) {
    err("%s, invalid num_ports %d\n", __func__, num_ports);
    return -EINVAL;
  }

  for (int i = 0; i < num_ports; i++) {
    ip = ops->sip_addr[i];
    ret = st_ip_addr_check(ip);
    if (ret < 0) {
      err("%s(%d), invalid ip %d.%d.%d.%d\n", __func__, i, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (num_ports > 1) {
    if (0 == memcmp(ops->sip_addr[0], ops->sip_addr[1], ST_IP_ADDR_LEN)) {
      err("%s, same %d.%d.%d.%d for both ip\n", __func__, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (ops->type == ST30_TYPE_FRAME_LEVEL) {
    if (ops->framebuff_cnt < 1) {
      err("%s, invalid framebuff_cnt %d\n", __func__, ops->framebuff_cnt);
      return -EINVAL;
    }
  } else if (ops->type == ST30_TYPE_RTP_LEVEL) {
    if (ops->rtp_ring_size <= 0) {
      err("%s, invalid rtp_ring_size %d\n", __func__, ops->rtp_ring_size);
      return -EINVAL;
    }
    if ((ops->sample_size < 0) || (ops->sample_size > ST_PKT_MAX_RTP_BYTES)) {
      err("%s, invalid sample_size %d\n", __func__, ops->sample_size);
      return -EINVAL;
    }
  }

  return 0;
}

static int st_rx_ancillary_ops_check(struct st40_rx_ops* ops) {
  int num_ports = ops->num_port, ret;
  uint8_t* ip;

  if ((num_ports > ST_PORT_MAX) || (num_ports <= 0)) {
    err("%s, invalid num_ports %d\n", __func__, num_ports);
    return -EINVAL;
  }

  for (int i = 0; i < num_ports; i++) {
    ip = ops->sip_addr[i];
    ret = st_ip_addr_check(ip);
    if (ret < 0) {
      err("%s(%d), invalid ip %d.%d.%d.%d\n", __func__, i, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (num_ports > 1) {
    if (0 == memcmp(ops->sip_addr[0], ops->sip_addr[1], ST_IP_ADDR_LEN)) {
      err("%s, same %d.%d.%d.%d for both ip\n", __func__, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (ops->rtp_ring_size <= 0) {
    err("%s, invalid rtp_ring_size %d\n", __func__, ops->rtp_ring_size);
    return -EINVAL;
  }

  return 0;
}

int st_rx_source_info_check(struct st_rx_source_info* src, int num_ports) {
  uint8_t* ip;
  int ret;

  for (int i = 0; i < num_ports; i++) {
    ip = src->sip_addr[i];
    ret = st_ip_addr_check(ip);
    if (ret < 0) {
      err("%s(%d), invalid ip %d.%d.%d.%d\n", __func__, i, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  if (num_ports > 1) {
    if (0 == memcmp(src->sip_addr[0], src->sip_addr[1], ST_IP_ADDR_LEN)) {
      err("%s, same %d.%d.%d.%d for both ip\n", __func__, ip[0], ip[1], ip[2], ip[3]);
      return -EINVAL;
    }
  }

  return 0;
}

st_handle st_init(struct st_init_params* p) {
  struct st_main_impl* impl = NULL;
  int socket[ST_PORT_MAX], ret;
  int num_ports = p->num_ports;

  RTE_BUILD_BUG_ON(ST_SESSION_PORT_MAX > (int)ST_PORT_MAX);

  ret = st_user_params_check(p);
  if (ret < 0) {
    err("%s, st_user_params_check fail %d\n", __func__, ret);
    return NULL;
  }

  ret = st_dev_init(p);
  if (ret < 0) {
    err("%s, st_dev_eal_init fail %d\n", __func__, ret);
    return NULL;
  }
  info("st version: %s, dpdk version: %s\n", st_version(), rte_version());

  for (int i = 0; i < num_ports; i++) {
    socket[i] = st_dev_get_socket(st_p_port(p));
    if (socket[i] < 0) {
      err("%s, get socket fail %d\n", __func__, socket[i]);
      goto err_exit;
    }
  }

#ifndef WINDOWSENV
  int numa_nodes = 0;
  if (numa_available() >= 0) numa_nodes = numa_max_node() + 1;
  if ((p->flags & ST_FLAG_BIND_NUMA) && (numa_nodes > 1)) {
    /* bind current thread and its children to socket node */
    struct bitmask* mask = numa_bitmask_alloc(numa_nodes);

    info("%s, bind to socket %d, numa_nodes %d\n", __func__, socket[ST_PORT_P],
         numa_nodes);
    numa_bitmask_setbit(mask, socket[ST_PORT_P]);
    numa_bind(mask);
    numa_bitmask_free(mask);
  }
#endif

  impl = st_rte_zmalloc_socket(sizeof(*impl), socket[ST_PORT_P]);
  if (!impl) goto err_exit;

  rte_memcpy(&impl->user_para, p, sizeof(*p));
  for (int i = 0; i < num_ports; i++) {
    impl->inf[i].socket_id = socket[i];
    info("%s(%d), socket_id %d\n", __func__, i, socket[i]);
  }
  rte_atomic32_set(&impl->started, 0);
  rte_atomic32_set(&impl->request_exit, 0);
  rte_atomic32_set(&impl->dev_in_reset, 0);
  impl->lcore_lock_fd = -1;
  impl->tx_sessions_cnt_max = RTE_MIN(60, p->tx_sessions_cnt_max);
  impl->rx_sessions_cnt_max = RTE_MIN(60, p->rx_sessions_cnt_max);
  info("%s, max sessions tx %d rx %d\n", __func__, impl->tx_sessions_cnt_max,
       impl->rx_sessions_cnt_max);

  /* init mgr lock for audio and anc */
  pthread_mutex_init(&impl->tx_a_mgr_mutex, NULL);
  pthread_mutex_init(&impl->rx_a_mgr_mutex, NULL);
  pthread_mutex_init(&impl->tx_anc_mgr_mutex, NULL);
  pthread_mutex_init(&impl->rx_anc_mgr_mutex, NULL);

  impl->tsc_hz = rte_get_tsc_hz();

  /* init interface */
  ret = st_dev_if_init(impl);
  if (ret < 0) {
    err("%s, st_if_init fail\n", __func__);
    goto err_exit;
  }

  ret = st_main_create(impl);
  if (ret < 0) {
    err("%s, st_main_create fail\n", __func__);
    goto err_exit;
  }

  info("%s, succ, tsc_hz %" PRIu64 "\n", __func__, impl->tsc_hz);
  return impl;

err_exit:
  if (impl) {
    st_dev_if_uinit(impl);
    st_rte_free(impl);
  }
  st_dev_uinit(p);
  return NULL;
}

int st_uninit(st_handle st) {
  struct st_main_impl* impl = st;
  struct st_init_params* p = st_get_user_params(impl);

  st_tx_audio_uinit(impl);
  st_rx_audio_uinit(impl);
  st_tx_anc_uinit(impl);
  st_rx_anc_uinit(impl);
  st_tx_video_uinit(impl);
  st_rx_video_uinit(impl);

  st_main_free(impl);

  st_dev_if_uinit(impl);
  st_rte_free(impl);

  st_dev_uinit(p);

  info("%s, succ\n", __func__);
  return 0;
}

int st_start(st_handle st) {
  struct st_main_impl* impl = st;
  int ret;

  if (rte_atomic32_read(&impl->started)) {
    err("%s, started already\n", __func__);
    return -EIO;
  }

  /* wait tsc calibrate done, pacing need fine tuned TSC */
  if (impl->tsc_cal_tid) {
    pthread_join(impl->tsc_cal_tid, NULL);
    impl->tsc_cal_tid = 0;
  }

  ret = st_dev_start(impl);
  if (ret < 0) {
    err("%s, st_dev_start fail %d\n", __func__, ret);
    return ret;
  }

  rte_atomic32_set(&impl->started, 1);

  info("%s, succ, avail ports %d\n", __func__, rte_eth_dev_count_avail());
  return 0;
}

int st_stop(st_handle st) {
  struct st_main_impl* impl = st;

  if (!rte_atomic32_read(&impl->started)) {
    info("%s, not started\n", __func__);
    return -EIO;
  }

  st_dev_stop(impl);
  rte_atomic32_set(&impl->started, 0);
  info("%s, succ\n", __func__);
  return 0;
}

int st_get_lcore(st_handle st, unsigned int* lcore) {
  struct st_main_impl* impl = st;
  if (!impl) return -EIO;
  return st_dev_get_lcore(impl, lcore);
}

int st_put_lcore(st_handle st, unsigned int lcore) {
  struct st_main_impl* impl = st;
  if (!impl) return -EIO;
  return st_dev_put_lcore(impl, lcore);
}

int st_bind_to_lcore(st_handle st, pthread_t thread, unsigned int lcore) {
  struct st_main_impl* impl = st;
  if (!impl) return -EIO;

  if (!st_dev_lcore_valid(impl, lcore)) {
    err("%s, invalid lcore %d\n", __func__, lcore);
    return -EINVAL;
  }

  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(lcore, &mask);
  pthread_setaffinity_np(thread, sizeof(mask), &mask);

  return 0;
}

st20_tx_handle st20_tx_create(st_handle st, struct st20_tx_ops* ops) {
  struct st_main_impl* impl = st;
  struct st_sch_impl* sch;
  struct st_tx_video_session_handle_impl* s_impl;
  struct st_tx_video_session_impl* s;
  int quota_mbs, ret;
  uint64_t bps;

  if (rte_atomic32_read(&impl->started)) {
    err("%s, only allowed when dev is in stop state\n", __func__);
    return NULL;
  }

  ret = st_tx_video_ops_check(ops);
  if (ret < 0) {
    err("%s, st_tx_video_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  ret = st20_get_bandwidth_bps(ops->width, ops->height, ops->fmt, ops->fps, &bps);
  if (ret < 0) {
    err("%s, st20_get_bandwidth_bps fail\n", __func__);
    return NULL;
  }
  quota_mbs = bps / (1000 * 1000);
  quota_mbs *= ops->num_port;

  s_impl = st_rte_zmalloc_socket(sizeof(*s_impl), st_socket_id(impl, ST_PORT_P));
  if (!s_impl) {
    err("%s, s_impl malloc fail\n", __func__);
    return NULL;
  }

  sch = st_dev_get_sch(impl, quota_mbs);
  if (!sch) {
    st_rte_free(s_impl);
    err("%s, st_dev_get_sch fail\n", __func__);
    return NULL;
  }

  pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  ret = st_tx_video_init(impl, sch);
  pthread_mutex_unlock(&sch->tx_video_mgr_mutex);
  if (ret < 0) {
    err("%s, st_tx_video_init fail %d\n", __func__, ret);
    st_dev_put_sch(sch, quota_mbs);
    st_rte_free(s_impl);
    return NULL;
  }

  pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  s = st_tx_video_sessions_mgr_attach(&sch->tx_video_mgr, ops, ST_SESSION_TYPE_TX_VIDEO);
  pthread_mutex_unlock(&sch->tx_video_mgr_mutex);
  if (!s) {
    err("%s(%d), st_tx_sessions_mgr_attach fail\n", __func__, sch->idx);
    st_dev_put_sch(sch, quota_mbs);
    st_rte_free(s_impl);
    return NULL;
  }

  s_impl->parnet = impl;
  s_impl->type = ST_SESSION_TYPE_TX_VIDEO;
  s_impl->sch = sch;
  s_impl->impl = s;
  s_impl->quota_mbs = quota_mbs;

  rte_atomic32_inc(&impl->st20_tx_sessions_cnt);
  info("%s, succ on sch %d session %p,%d num_port %d\n", __func__, sch->idx, s, s->idx,
       ops->num_port);
  return s_impl;
}

void* st20_tx_get_framebuffer(st20_tx_handle handle, uint16_t idx) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  struct st_tx_video_session_impl* s;

  if (s_impl->type != ST_SESSION_TYPE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;

  if (idx < 0 || idx >= s->st20_frames_cnt) {
    err("%s, invalid idx %d, should be in range [0, %d]\n", __func__, idx,
        s->st20_frames_cnt);
    return NULL;
  }
  if (!s->st20_frames) {
    err("%s, st20_frames not allocated\n", __func__);
    return NULL;
  }

  return s->st20_frames[idx];
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

  pkt = rte_pktmbuf_alloc(s->packet_mempool);
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
  sch_idx = s->idx;

  if (rte_atomic32_read(&impl->started)) {
    err("%s(%d,%d), only allowed when dev is in stop state\n", __func__, sch_idx, idx);
    return -EIO;
  }

  /* no need to lock as session is located already */
  ret = st_tx_video_sessions_mgr_detach(&sch->tx_video_mgr, s);
  if (ret < 0)
    err("%s(%d,%d), st_tx_sessions_mgr_deattach fail\n", __func__, sch_idx, idx);

  ret = st_dev_put_sch(sch, s_impl->quota_mbs);
  if (ret < 0) err("%s(%d, %d), st_dev_put_sch fail\n", __func__, sch_idx, idx);

  st_rte_free(s_impl);

  /* update max idx */
  pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  st_tx_video_sessions_mgr_update(&sch->tx_video_mgr);
  pthread_mutex_unlock(&sch->tx_video_mgr_mutex);

  rte_atomic32_dec(&impl->st20_tx_sessions_cnt);
  info("%s, succ on sch %d session %d\n", __func__, sch_idx, idx);
  return 0;
}

st30_tx_handle st30_tx_create(st_handle st, struct st30_tx_ops* ops) {
  struct st_main_impl* impl = st;
  struct st_tx_audio_session_handle_impl* s_impl;
  struct st_tx_audio_session_impl* s;
  int ret;

  ret = st_tx_audio_ops_check(ops);
  if (ret < 0) {
    err("%s, st_tx_audio_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  pthread_mutex_lock(&impl->tx_a_mgr_mutex);
  ret = st_tx_audio_init(impl);
  pthread_mutex_unlock(&impl->tx_a_mgr_mutex);
  if (ret < 0) {
    err("%s, st_tx_audio_init fail %d\n", __func__, ret);
    return NULL;
  }

  s_impl = st_rte_zmalloc_socket(sizeof(*s_impl), st_socket_id(impl, ST_PORT_P));
  if (!s_impl) {
    err("%s, s_impl malloc fail\n", __func__);
    return NULL;
  }

  pthread_mutex_lock(&impl->tx_a_mgr_mutex);
  s = st_tx_audio_sessions_mgr_attach(&impl->tx_a_mgr, ops);
  pthread_mutex_unlock(&impl->tx_a_mgr_mutex);
  if (!s) {
    err("%s, st_tx_audio_sessions_mgr_attach fail\n", __func__);
    st_rte_free(s_impl);
    return NULL;
  }

  s_impl->parnet = impl;
  s_impl->type = ST_SESSION_TYPE_TX_AUDIO;
  s_impl->impl = s;

  rte_atomic32_inc(&impl->st30_tx_sessions_cnt);
  info("%s, succ on session %d\n", __func__, s->idx);
  return s_impl;
}

int st30_tx_free(st30_tx_handle handle) {
  struct st_tx_audio_session_handle_impl* s_impl = handle;
  struct st_main_impl* impl;
  struct st_tx_audio_session_impl* s;
  int ret, idx;

  if (s_impl->type != ST_SESSION_TYPE_TX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parnet;
  s = s_impl->impl;
  idx = s->idx;

  /* no need to lock as session is located already */
  ret = st_tx_audio_sessions_mgr_detach(&impl->tx_a_mgr, s);
  if (ret < 0) err("%s(%d), st_tx_audio_sessions_mgr_deattach fail\n", __func__, idx);

  st_rte_free(s_impl);

  /* update max idx */
  pthread_mutex_lock(&impl->tx_a_mgr_mutex);
  st_tx_audio_sessions_mgr_update(&impl->tx_a_mgr);
  pthread_mutex_unlock(&impl->tx_a_mgr_mutex);

  rte_atomic32_dec(&impl->st30_tx_sessions_cnt);
  info("%s, succ on session %d\n", __func__, idx);
  return 0;
}

void* st30_tx_get_framebuffer(st30_tx_handle handle, uint16_t idx) {
  struct st_tx_audio_session_handle_impl* s_impl = handle;
  struct st_tx_audio_session_impl* s;

  if (s_impl->type != ST_SESSION_TYPE_TX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;

  if (idx < 0 || idx >= s->ops.framebuff_cnt) {
    err("%s, invalid idx %d, should be in range [0, %d]\n", __func__, idx,
        s->ops.framebuff_cnt);
    return NULL;
  }
  if (!s->st30_frames) {
    err("%s, st30_frames not allocated\n", __func__);
    return NULL;
  }

  return s->st30_frames + s->st30_frame_size * idx;
}

void* st30_tx_get_mbuf(st30_tx_handle handle, void** usrptr) {
  struct st_tx_audio_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt;
  struct st_tx_audio_session_impl* s;
  int idx;
  struct rte_ring* packet_ring;

  if (s_impl->type != ST_SESSION_TYPE_TX_AUDIO) {
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

  pkt = rte_pktmbuf_alloc(s->packet_mempool);
  if (!pkt) {
    dbg("%s(%d), pkt alloc fail\n", __func__, idx);
    return NULL;
  }

  *usrptr = rte_pktmbuf_mtod(pkt, void*);
  return pkt;
}

int st30_tx_put_mbuf(st30_tx_handle handle, void* mbuf, uint16_t len) {
  struct st_tx_audio_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = (struct rte_mbuf*)mbuf;
  struct st_tx_audio_session_impl* s;
  int idx, ret;
  struct rte_ring* packet_ring;

  if (s_impl->type != ST_SESSION_TYPE_TX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    rte_pktmbuf_free(mbuf);
    return -EIO;
  }

  if (!st_rtp_len_valid(len)) {
    if (len) err("%s, invalid len %d\n", __func__, len);
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

  pkt->data_len = pkt->pkt_len = len;
  ret = rte_ring_sp_enqueue(packet_ring, (void*)pkt);
  if (ret < 0) {
    err("%s(%d), can not enqueue to the rte ring\n", __func__, idx);
    rte_pktmbuf_free(mbuf);
    return -EBUSY;
  }

  return 0;
}

st40_tx_handle st40_tx_create(st_handle st, struct st40_tx_ops* ops) {
  struct st_main_impl* impl = st;
  struct st_tx_ancillary_session_handle_impl* s_impl;
  struct st_tx_ancillary_session_impl* s;
  int ret;

  ret = st_tx_ancillary_ops_check(ops);
  if (ret < 0) {
    err("%s, st_tx_ancillary_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  pthread_mutex_lock(&impl->tx_anc_mgr_mutex);
  ret = st_tx_anc_init(impl);
  pthread_mutex_unlock(&impl->tx_anc_mgr_mutex);
  if (ret < 0) {
    err("%s, st_tx_anc_init fail %d\n", __func__, ret);
    return NULL;
  }

  s_impl = st_rte_zmalloc_socket(sizeof(*s_impl), st_socket_id(impl, ST_PORT_P));
  if (!s_impl) {
    err("%s, s_impl malloc fail\n", __func__);
    return NULL;
  }

  pthread_mutex_lock(&impl->tx_anc_mgr_mutex);
  s = st_tx_ancillary_sessions_mgr_attach(&impl->tx_anc_mgr, ops);
  pthread_mutex_unlock(&impl->tx_anc_mgr_mutex);
  if (!s) {
    err("%s, st_tx_ancillary_sessions_mgr_attach fail\n", __func__);
    st_rte_free(s_impl);
    return NULL;
  }

  s_impl->parnet = impl;
  s_impl->type = ST_SESSION_TYPE_TX_ANC;
  s_impl->impl = s;

  rte_atomic32_inc(&impl->st40_tx_sessions_cnt);
  info("%s, succ on session %d\n", __func__, s->idx);
  return s_impl;
}

void* st40_tx_get_mbuf(st40_tx_handle handle, void** usrptr) {
  struct st_tx_ancillary_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = NULL;
  struct st_tx_ancillary_session_impl* s;
  int idx;
  struct rte_ring* packet_ring;

  if (s_impl->type != ST_SESSION_TYPE_TX_ANC) {
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

  pkt = rte_pktmbuf_alloc(s->packet_mempool);
  if (!pkt) {
    dbg("%s(%d), pkt alloc fail\n", __func__, idx);
    return NULL;
  }

  *usrptr = rte_pktmbuf_mtod(pkt, void*);
  return pkt;
}

int st40_tx_put_mbuf(st40_tx_handle handle, void* mbuf, uint16_t len) {
  struct st_tx_ancillary_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = (struct rte_mbuf*)mbuf;
  struct st_tx_ancillary_session_impl* s;
  struct rte_ring* packet_ring;
  int idx, ret;

  if (s_impl->type != ST_SESSION_TYPE_TX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  if (!st_rtp_len_valid(len)) {
    if (len) err("%s, invalid len %d\n", __func__, len);
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

  pkt->data_len = pkt->pkt_len = len;
  ret = rte_ring_sp_enqueue(packet_ring, (void*)pkt);
  if (ret < 0) {
    err("%s(%d), can not enqueue to the rte ring\n", __func__, idx);
    rte_pktmbuf_free(mbuf);
    return -EBUSY;
  }

  return 0;
}

int st40_tx_free(st40_tx_handle handle) {
  struct st_tx_ancillary_session_handle_impl* s_impl = handle;
  struct st_tx_ancillary_session_impl* s;
  struct st_main_impl* impl;
  int ret, idx;

  if (s_impl->type != ST_SESSION_TYPE_TX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parnet;
  s = s_impl->impl;
  idx = s->idx;

  /* no need to lock as session is located already */
  ret = st_tx_ancillary_sessions_mgr_detach(&impl->tx_anc_mgr, s);
  if (ret < 0) err("%s(%d), st_tx_ancillary_sessions_mgr_detach fail\n", __func__, idx);

  st_rte_free(s_impl);

  /* update max idx */
  pthread_mutex_lock(&impl->tx_anc_mgr_mutex);
  st_tx_ancillary_sessions_mgr_update(&impl->tx_anc_mgr);
  pthread_mutex_unlock(&impl->tx_anc_mgr_mutex);

  rte_atomic32_dec(&impl->st40_tx_sessions_cnt);
  info("%s, succ on session %d\n", __func__, idx);
  return 0;
}

void* st40_tx_get_framebuffer(st40_tx_handle handle, uint16_t idx) {
  struct st_tx_ancillary_session_handle_impl* s_impl = handle;
  struct st_tx_ancillary_session_impl* s;

  if (s_impl->type != ST_SESSION_TYPE_TX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;
  if (idx < 0 || idx >= s->ops.framebuff_cnt) {
    err("%s, invalid idx %d, should be in range [0, %d]\n", __func__, idx,
        s->ops.framebuff_cnt);
    return NULL;
  }
  if (!s->st40_frames) {
    err("%s, st40_frames not allocated\n", __func__);
    return NULL;
  }

  return s->st40_frames + sizeof(struct st40_frame) * idx;
}

st20_rx_handle st20_rx_create(st_handle st, struct st20_rx_ops* ops) {
  struct st_main_impl* impl = st;
  struct st_sch_impl* sch;
  struct st_rx_video_session_handle_impl* s_impl;
  struct st_rx_video_session_impl* s;
  int quota_mbs, ret;
  uint64_t bps;

  ret = st_rx_video_ops_check(ops);
  if (ret < 0) {
    err("%s, st_rx_video_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  ret = st20_get_bandwidth_bps(ops->width, ops->height, ops->fmt, ops->fps, &bps);
  if (ret < 0) {
    err("%s, st20_get_bandwidth_bps fail\n", __func__);
    return NULL;
  }
  quota_mbs = bps / (1000 * 1000);

  s_impl = st_rte_zmalloc_socket(sizeof(*s_impl), st_socket_id(impl, ST_PORT_P));
  if (!s_impl) {
    err("%s, s_impl malloc fail\n", __func__);
    return NULL;
  }

  sch = st_dev_get_sch(impl, quota_mbs);
  if (!sch) {
    st_rte_free(s_impl);
    err("%s, st_dev_get_sch fail\n", __func__);
    return NULL;
  }

  pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  ret = st_rx_video_init(impl, sch);
  pthread_mutex_unlock(&sch->rx_video_mgr_mutex);
  if (ret < 0) {
    err("%s, st_rx_video_init fail %d\n", __func__, ret);
    st_dev_put_sch(sch, quota_mbs);
    st_rte_free(s_impl);
    return NULL;
  }

  pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  s = st_rx_video_sessions_mgr_attach(&sch->rx_video_mgr, ops);
  pthread_mutex_unlock(&sch->rx_video_mgr_mutex);
  if (!s) {
    err("%s(%d), st_rx_video_sessions_mgr_attach fail\n", __func__, sch->idx);
    st_dev_put_sch(sch, quota_mbs);
    st_rte_free(s_impl);
    return NULL;
  }

  s_impl->parnet = impl;
  s_impl->type = ST_SESSION_TYPE_RX_VIDEO;
  s_impl->sch = sch;
  s_impl->impl = s;
  s_impl->quota_mbs = quota_mbs;

  rte_atomic32_inc(&impl->st20_rx_sessions_cnt);
  info("%s, succ on sch %d session %d\n", __func__, sch->idx, s->idx);
  return s_impl;
}

int st20_rx_update_source(st20_rx_handle handle, struct st_rx_source_info* src) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;
  int idx, ret;

  if (s_impl->type != ST_SESSION_TYPE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;
  idx = s->idx;

  ret = st_rx_source_info_check(src, s->ops.num_port);
  if (ret < 0) return ret;

  ret = st_rx_video_sessions_mgr_update_src(&s_impl->sch->rx_video_mgr, s, src);
  if (ret < 0) {
    err("%s(%d), online update fail %d\n", __func__, idx, ret);
    return ret;
  }

  info("%s, succ on session %d\n", __func__, idx);
  return 0;
}

int st20_rx_get_sch_idx(st20_rx_handle handle) {
  struct st_rx_video_session_handle_impl* s_impl = handle;

  if (s_impl->type != ST_SESSION_TYPE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }

  return s_impl->sch->idx;
}

int st20_rx_free(st20_rx_handle handle) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct st_sch_impl* sch;
  struct st_rx_video_session_impl* s;
  struct st_main_impl* impl;
  int ret, sch_idx, idx;

  if (s_impl->type != ST_SESSION_TYPE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parnet;
  sch = s_impl->sch;
  s = s_impl->impl;
  idx = s->idx;
  sch_idx = s->idx;

  /* no need to lock as session is located already */
  ret = st_rx_video_sessions_mgr_detach(&sch->rx_video_mgr, s);
  if (ret < 0)
    err("%s(%d,%d), st_rx_video_sessions_mgr_deattach fail\n", __func__, sch_idx, idx);

  ret = st_dev_put_sch(sch, s_impl->quota_mbs);
  if (ret < 0) err("%s(%d,%d), st_dev_put_sch fail\n", __func__, sch_idx, idx);

  st_rte_free(s_impl);

  /* update max idx */
  pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  st_rx_video_sessions_mgr_update(&sch->rx_video_mgr);
  pthread_mutex_unlock(&sch->rx_video_mgr_mutex);

  rte_atomic32_dec(&impl->st20_rx_sessions_cnt);
  info("%s, succ on sch %d session %d\n", __func__, sch_idx, idx);
  return 0;
}

int st20_rx_put_framebuff(st20_rx_handle handle, void* frame) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;

  if (s_impl->type != ST_SESSION_TYPE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;

  return st_rx_video_session_put_frame(s, frame);
}

void* st20_rx_get_mbuf(st20_rx_handle handle, void** usrptr, uint16_t* len) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;
  struct rte_mbuf* pkt;
  int idx, ret;
  struct rte_ring* rtps_ring;

  if (s_impl->type != ST_SESSION_TYPE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;
  idx = s->idx;
  rtps_ring = s->st20_rtps_ring;
  if (!rtps_ring) {
    err("%s(%d), rtp ring is not created\n", __func__, idx);
    return NULL;
  }

  ret = rte_ring_sc_dequeue(rtps_ring, (void**)&pkt);
  if (ret < 0) {
    dbg("%s(%d), rtp ring is empty\n", __func__, idx);
    return NULL;
  }

  size_t hdr_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                   sizeof(struct rte_udp_hdr);
  *len = pkt->data_len - hdr_len;
  *usrptr = rte_pktmbuf_mtod_offset(pkt, void*, hdr_len);
  return pkt;
}

void st20_rx_put_mbuf(st20_rx_handle handle, void* mbuf) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = (struct rte_mbuf*)mbuf;

  if (s_impl->type != ST_SESSION_TYPE_RX_VIDEO)
    err("%s, invalid type %d\n", __func__, s_impl->type);

  if (pkt) rte_pktmbuf_free(pkt);
}

st30_rx_handle st30_rx_create(st_handle st, struct st30_rx_ops* ops) {
  struct st_main_impl* impl = st;
  struct st_sch_impl* sch = impl->main_sch;
  struct st_rx_audio_session_handle_impl* s_impl;
  struct st_rx_audio_session_impl* s;
  int ret;

  ret = st_rx_audio_ops_check(ops);
  if (ret < 0) {
    err("%s, st_rx_audio_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  pthread_mutex_lock(&impl->rx_a_mgr_mutex);
  ret = st_rx_audio_init(impl);
  pthread_mutex_unlock(&impl->rx_a_mgr_mutex);
  if (ret < 0) {
    err("%s, st_rx_audio_init fail %d\n", __func__, ret);
    return NULL;
  }

  s_impl = st_rte_zmalloc_socket(sizeof(*s_impl), st_socket_id(impl, ST_PORT_P));
  if (!s_impl) {
    err("%s, s_impl malloc fail\n", __func__);
    return NULL;
  }

  pthread_mutex_lock(&impl->rx_a_mgr_mutex);
  s = st_rx_audio_sessions_mgr_attach(&impl->rx_a_mgr, ops);
  pthread_mutex_unlock(&impl->rx_a_mgr_mutex);
  if (!s) {
    err("%s(%d), st_rx_audio_sessions_mgr_attach fail\n", __func__, sch->idx);
    st_rte_free(s_impl);
    return NULL;
  }

  s_impl->parnet = impl;
  s_impl->type = ST_SESSION_TYPE_RX_AUDIO;
  s_impl->impl = s;

  rte_atomic32_inc(&impl->st30_rx_sessions_cnt);
  info("%s, succ on sch %d session %d\n", __func__, sch->idx, s->idx);
  return s_impl;
}

int st30_rx_update_source(st30_rx_handle handle, struct st_rx_source_info* src) {
  struct st_rx_audio_session_handle_impl* s_impl = handle;
  struct st_main_impl* impl;
  struct st_rx_audio_session_impl* s;
  int idx, ret;

  if (s_impl->type != ST_SESSION_TYPE_RX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parnet;
  s = s_impl->impl;
  idx = s->idx;

  ret = st_rx_source_info_check(src, s->ops.num_port);
  if (ret < 0) return ret;

  ret = st_rx_audio_sessions_mgr_update_src(&impl->rx_a_mgr, s, src);
  if (ret < 0) {
    err("%s(%d), online update fail %d\n", __func__, idx, ret);
    return ret;
  }

  info("%s, succ on session %d\n", __func__, idx);
  return 0;
}

int st30_rx_free(st30_rx_handle handle) {
  struct st_rx_audio_session_handle_impl* s_impl = handle;
  struct st_main_impl* impl;
  struct st_rx_audio_session_impl* s;
  int ret, idx;

  if (s_impl->type != ST_SESSION_TYPE_RX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parnet;
  s = s_impl->impl;
  idx = s->idx;

  /* no need to lock as session is located already */
  ret = st_rx_audio_sessions_mgr_detach(&impl->rx_a_mgr, s);
  if (ret < 0) err("%s(%d), st_rx_audio_sessions_mgr_deattach fail\n", __func__, idx);

  st_rte_free(s_impl);

  /* update max idx */
  pthread_mutex_lock(&impl->rx_a_mgr_mutex);
  st_rx_audio_sessions_mgr_update(&impl->rx_a_mgr);
  pthread_mutex_unlock(&impl->rx_a_mgr_mutex);

  rte_atomic32_dec(&impl->st30_rx_sessions_cnt);
  info("%s, succ on session %d\n", __func__, idx);
  return 0;
}

int st30_rx_put_framebuff(st30_rx_handle handle, void* frame) {
  struct st_rx_audio_session_handle_impl* s_impl = handle;
  struct st_rx_audio_session_impl* s;

  if (s_impl->type != ST_SESSION_TYPE_RX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;

  return st_rx_audio_session_put_frame(s, frame);
}

void* st30_rx_get_mbuf(st30_rx_handle handle, void** usrptr, uint16_t* len) {
  struct st_rx_audio_session_handle_impl* s_impl = handle;
  struct st_rx_audio_session_impl* s;
  struct rte_mbuf* pkt;
  struct rte_ring* rtps_ring;
  int idx, ret;

  if (s_impl->type != ST_SESSION_TYPE_RX_AUDIO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;
  idx = s->idx;
  rtps_ring = s->st30_rtps_ring;
  if (!rtps_ring) {
    err("%s(%d), rtp ring is not created\n", __func__, idx);
    return NULL;
  }

  ret = rte_ring_sc_dequeue(rtps_ring, (void**)&pkt);
  if (ret < 0) {
    dbg("%s(%d), rtp ring is empty\n", __func__, idx);
    return NULL;
  }

  size_t hdr_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                   sizeof(struct rte_udp_hdr);
  *len = pkt->data_len - hdr_len;
  *usrptr = rte_pktmbuf_mtod_offset(pkt, void*, hdr_len);
  return pkt;
}

void st30_rx_put_mbuf(st30_rx_handle handle, void* mbuf) {
  struct st_rx_audio_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = (struct rte_mbuf*)mbuf;

  if (s_impl->type != ST_SESSION_TYPE_RX_AUDIO)
    err("%s, invalid type %d\n", __func__, s_impl->type);

  if (pkt) rte_pktmbuf_free(pkt);
}

st40_rx_handle st40_rx_create(st_handle st, struct st40_rx_ops* ops) {
  struct st_main_impl* impl = st;
  struct st_rx_ancillary_session_handle_impl* s_impl;
  struct st_rx_ancillary_session_impl* s;
  int ret;

  ret = st_rx_ancillary_ops_check(ops);
  if (ret < 0) {
    err("%s, st_rx_audio_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  pthread_mutex_lock(&impl->rx_anc_mgr_mutex);
  ret = st_rx_anc_init(impl);
  pthread_mutex_unlock(&impl->rx_anc_mgr_mutex);
  if (ret < 0) {
    err("%s, st_rx_audio_init fail %d\n", __func__, ret);
    return NULL;
  }

  s_impl = st_rte_zmalloc_socket(sizeof(*s_impl), st_socket_id(impl, ST_PORT_P));
  if (!s_impl) {
    err("%s, s_impl malloc fail\n", __func__);
    return NULL;
  }

  pthread_mutex_lock(&impl->rx_anc_mgr_mutex);
  s = st_rx_ancillary_sessions_mgr_attach(&impl->rx_anc_mgr, ops);
  pthread_mutex_unlock(&impl->rx_anc_mgr_mutex);
  if (!s) {
    err("%s, st_rx_ancillary_sessions_mgr_attach fail\n", __func__);
    st_rte_free(s_impl);
    return NULL;
  }

  s_impl->parnet = impl;
  s_impl->type = ST_SESSION_TYPE_RX_ANC;
  s_impl->impl = s;

  rte_atomic32_inc(&impl->st40_rx_sessions_cnt);
  info("%s, succ on session %d\n", __func__, s->idx);
  return s_impl;
}

int st40_rx_update_source(st40_rx_handle handle, struct st_rx_source_info* src) {
  struct st_rx_ancillary_session_handle_impl* s_impl = handle;
  struct st_main_impl* impl;
  struct st_rx_ancillary_session_impl* s;
  int idx, ret;

  if (s_impl->type != ST_SESSION_TYPE_RX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parnet;
  s = s_impl->impl;
  idx = s->idx;

  ret = st_rx_source_info_check(src, s->ops.num_port);
  if (ret < 0) return ret;

  ret = st_rx_ancillary_sessions_mgr_update_src(&impl->rx_anc_mgr, s, src);
  if (ret < 0) {
    err("%s(%d), online update fail %d\n", __func__, idx, ret);
    return ret;
  }

  info("%s, succ on session %d\n", __func__, idx);
  return 0;
}

int st40_rx_free(st40_rx_handle handle) {
  struct st_rx_ancillary_session_handle_impl* s_impl = handle;
  struct st_main_impl* impl;
  struct st_rx_ancillary_session_impl* s;
  int ret, idx;

  if (s_impl->type != ST_SESSION_TYPE_RX_ANC) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parnet;
  s = s_impl->impl;
  idx = s->idx;

  /* no need to lock as session is located already */
  ret = st_rx_ancillary_sessions_mgr_detach(&impl->rx_anc_mgr, s);
  if (ret < 0) err("%s(%d), st_rx_ancillary_sessions_mgr_detach fail\n", __func__, idx);

  st_rte_free(s_impl);

  /* update max idx */
  pthread_mutex_lock(&impl->rx_anc_mgr_mutex);
  st_rx_ancillary_sessions_mgr_update(&impl->rx_anc_mgr);
  pthread_mutex_unlock(&impl->rx_anc_mgr_mutex);

  rte_atomic32_dec(&impl->st40_rx_sessions_cnt);
  info("%s, succ on session %d\n", __func__, idx);
  return 0;
}

void* st40_rx_get_mbuf(st40_rx_handle handle, void** usrptr, uint16_t* len) {
  struct st_rx_ancillary_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt;
  struct st_rx_ancillary_session_impl* s;
  struct rte_ring* packet_ring;
  int idx, ret;

  if (s_impl->type != ST_SESSION_TYPE_RX_ANC) {
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

  ret = rte_ring_sc_dequeue(packet_ring, (void**)&pkt);
  if (ret == 0) {
    int header_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                     sizeof(struct rte_udp_hdr);
    *len = pkt->data_len - header_len;
    *usrptr = rte_pktmbuf_mtod_offset(pkt, void*, header_len);
    return (void*)pkt;
  }

  return NULL;
}

void st40_rx_put_mbuf(st40_rx_handle handle, void* mbuf) {
  struct st_rx_ancillary_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = (struct rte_mbuf*)mbuf;

  if (s_impl->type != ST_SESSION_TYPE_RX_ANC)
    err("%s, invalid type %d\n", __func__, s_impl->type);

  if (pkt) rte_pktmbuf_free(pkt);
}

st22_tx_handle st22_tx_create(st_handle st, struct st22_tx_ops* ops) {
  struct st_main_impl* impl = st;
  struct st_sch_impl* sch;
  struct st22_tx_video_session_handle_impl* s_impl;
  struct st_tx_video_session_impl* s;
  int quota_mbs, ret;
  uint64_t bps;
  struct st20_tx_ops st20_ops;

  if (rte_atomic32_read(&impl->started)) {
    err("%s, only allowed when dev is in stop state\n", __func__);
    return NULL;
  }

  ret = st22_tx_video_ops_check(ops);
  if (ret < 0) {
    err("%s, st_tx_video_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  /* todo: calculate from total pkts in ops */
  ret = st20_get_bandwidth_bps(ops->width, ops->height, ops->fmt, ops->fps, &bps);
  if (ret < 0) {
    err("%s, st20_get_bandwidth_bps fail\n", __func__);
    return NULL;
  }
  quota_mbs = bps / (1000 * 1000);
  quota_mbs *= ops->num_port;

  s_impl = st_rte_zmalloc_socket(sizeof(*s_impl), st_socket_id(impl, ST_PORT_P));
  if (!s_impl) {
    err("%s, s_impl malloc fail\n", __func__);
    return NULL;
  }

  sch = st_dev_get_sch(impl, quota_mbs);
  if (!sch) {
    st_rte_free(s_impl);
    err("%s, st_dev_get_sch fail\n", __func__);
    return NULL;
  }

  pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  ret = st_tx_video_init(impl, sch);
  pthread_mutex_unlock(&sch->tx_video_mgr_mutex);
  if (ret < 0) {
    err("%s, st_tx_video_init fail %d\n", __func__, ret);
    st_dev_put_sch(sch, quota_mbs);
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
  if (ops->num_port > 1) {
    memcpy(st20_ops.dip_addr[ST_PORT_R], ops->dip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(st20_ops.port[ST_PORT_R], ops->port[ST_PORT_R], ST_PORT_MAX_LEN);
    st20_ops.udp_port[ST_PORT_R] = ops->udp_port[ST_PORT_R];
  }
  st20_ops.pacing = ops->pacing;
  st20_ops.type = ST20_TYPE_RTP_LEVEL;
  st20_ops.width = ops->width;
  st20_ops.height = ops->height;
  st20_ops.fps = ops->fps;
  st20_ops.fmt = ops->fmt;
  st20_ops.rtp_ring_size = ops->rtp_ring_size;
  st20_ops.rtp_frame_total_pkts = ops->rtp_frame_total_pkts;
  st20_ops.rtp_pkt_size = ops->rtp_pkt_size;
  st20_ops.notify_rtp_done = ops->notify_rtp_done;
  pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  s = st_tx_video_sessions_mgr_attach(&sch->tx_video_mgr, &st20_ops,
                                      ST22_SESSION_TYPE_TX_VIDEO);
  pthread_mutex_unlock(&sch->tx_video_mgr_mutex);
  if (!s) {
    err("%s(%d), st_tx_sessions_mgr_attach fail\n", __func__, sch->idx);
    st_dev_put_sch(sch, quota_mbs);
    st_rte_free(s_impl);
    return NULL;
  }

  s_impl->parnet = impl;
  s_impl->type = ST22_SESSION_TYPE_TX_VIDEO;
  s_impl->sch = sch;
  s_impl->impl = s;
  s_impl->quota_mbs = quota_mbs;

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
  sch_idx = s->idx;

  if (rte_atomic32_read(&impl->started)) {
    err("%s(%d,%d), only allowed when dev is in stop state\n", __func__, sch_idx, idx);
    return -EIO;
  }

  /* no need to lock as session is located already */
  ret = st_tx_video_sessions_mgr_detach(&sch->tx_video_mgr, s);
  if (ret < 0)
    err("%s(%d,%d), st_tx_sessions_mgr_deattach fail\n", __func__, sch_idx, idx);

  ret = st_dev_put_sch(sch, s_impl->quota_mbs);
  if (ret < 0) err("%s(%d, %d), st_dev_put_sch fail\n", __func__, sch_idx, idx);

  st_rte_free(s_impl);

  /* update max idx */
  pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  st_tx_video_sessions_mgr_update(&sch->tx_video_mgr);
  pthread_mutex_unlock(&sch->tx_video_mgr_mutex);

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

  pkt = rte_pktmbuf_alloc(s->packet_mempool);
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

st22_rx_handle st22_rx_create(st_handle st, struct st22_rx_ops* ops) {
  struct st_main_impl* impl = st;
  struct st_sch_impl* sch;
  struct st22_rx_video_session_handle_impl* s_impl;
  struct st_rx_video_session_impl* s;
  int quota_mbs, ret;
  uint64_t bps;
  struct st20_rx_ops st20_ops;

  ret = st22_rx_video_ops_check(ops);
  if (ret < 0) {
    err("%s, st_rx_video_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  /* todo: calculate from total pkts in ops */
  ret = st20_get_bandwidth_bps(ops->width, ops->height, ops->fmt, ops->fps, &bps);
  if (ret < 0) {
    err("%s, st20_get_bandwidth_bps fail\n", __func__);
    return NULL;
  }
  quota_mbs = bps / (1000 * 1000);

  s_impl = st_rte_zmalloc_socket(sizeof(*s_impl), st_socket_id(impl, ST_PORT_P));
  if (!s_impl) {
    err("%s, s_impl malloc fail\n", __func__);
    return NULL;
  }

  sch = st_dev_get_sch(impl, quota_mbs);
  if (!sch) {
    st_rte_free(s_impl);
    err("%s, st_dev_get_sch fail\n", __func__);
    return NULL;
  }

  pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  ret = st_rx_video_init(impl, sch);
  pthread_mutex_unlock(&sch->rx_video_mgr_mutex);
  if (ret < 0) {
    err("%s, st_rx_video_init fail %d\n", __func__, ret);
    st_dev_put_sch(sch, quota_mbs);
    st_rte_free(s_impl);
    return NULL;
  }

  /* reuse st20 rtp type */
  memset(&st20_ops, 0, sizeof(st20_ops));
  st20_ops.name = ops->name;
  st20_ops.priv = ops->priv;
  st20_ops.num_port = ops->num_port;
  memcpy(st20_ops.sip_addr[ST_PORT_P], ops->sip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  strncpy(st20_ops.port[ST_PORT_P], ops->port[ST_PORT_P], ST_PORT_MAX_LEN);
  st20_ops.udp_port[ST_PORT_P] = ops->udp_port[ST_PORT_P];
  if (ops->num_port > 1) {
    memcpy(st20_ops.sip_addr[ST_PORT_R], ops->sip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(st20_ops.port[ST_PORT_R], ops->port[ST_PORT_R], ST_PORT_MAX_LEN);
    st20_ops.udp_port[ST_PORT_R] = ops->udp_port[ST_PORT_R];
  }
  st20_ops.pacing = ops->pacing;
  st20_ops.type = ST20_TYPE_RTP_LEVEL;
  st20_ops.width = ops->width;
  st20_ops.height = ops->height;
  st20_ops.fps = ops->fps;
  st20_ops.fmt = ops->fmt;
  st20_ops.rtp_ring_size = ops->rtp_ring_size;
  st20_ops.notify_rtp_ready = ops->notify_rtp_ready;
  pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  s = st_rx_video_sessions_mgr_attach(&sch->rx_video_mgr, &st20_ops);
  pthread_mutex_unlock(&sch->rx_video_mgr_mutex);
  if (!s) {
    err("%s(%d), st_rx_video_sessions_mgr_attach fail\n", __func__, sch->idx);
    st_dev_put_sch(sch, quota_mbs);
    st_rte_free(s_impl);
    return NULL;
  }

  s_impl->parnet = impl;
  s_impl->type = ST22_SESSION_TYPE_RX_VIDEO;
  s_impl->sch = sch;
  s_impl->impl = s;
  s_impl->quota_mbs = quota_mbs;

  rte_atomic32_inc(&impl->st22_rx_sessions_cnt);
  info("%s, succ on sch %d session %d\n", __func__, sch->idx, s->idx);
  return s_impl;
}

int st22_rx_update_source(st22_rx_handle handle, struct st_rx_source_info* src) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;
  int idx, ret;

  if (s_impl->type != ST22_SESSION_TYPE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;
  idx = s->idx;

  ret = st_rx_source_info_check(src, s->ops.num_port);
  if (ret < 0) return ret;

  ret = st_rx_video_sessions_mgr_update_src(&s_impl->sch->rx_video_mgr, s, src);
  if (ret < 0) {
    err("%s(%d), online update fail %d\n", __func__, idx, ret);
    return ret;
  }

  info("%s, succ on session %d\n", __func__, idx);
  return 0;
}

int st22_rx_get_sch_idx(st22_rx_handle handle) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;

  if (s_impl->type != ST22_SESSION_TYPE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }

  return s_impl->sch->idx;
}

int st22_rx_free(st22_rx_handle handle) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;
  struct st_sch_impl* sch;
  struct st_rx_video_session_impl* s;
  struct st_main_impl* impl;
  int ret, sch_idx, idx;

  if (s_impl->type != ST22_SESSION_TYPE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  impl = s_impl->parnet;
  sch = s_impl->sch;
  s = s_impl->impl;
  idx = s->idx;
  sch_idx = s->idx;

  /* no need to lock as session is located already */
  ret = st_rx_video_sessions_mgr_detach(&sch->rx_video_mgr, s);
  if (ret < 0)
    err("%s(%d,%d), st_rx_video_sessions_mgr_deattach fail\n", __func__, sch_idx, idx);

  ret = st_dev_put_sch(sch, s_impl->quota_mbs);
  if (ret < 0) err("%s(%d,%d), st_dev_put_sch fail\n", __func__, sch_idx, idx);

  st_rte_free(s_impl);

  /* update max idx */
  pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  st_rx_video_sessions_mgr_update(&sch->rx_video_mgr);
  pthread_mutex_unlock(&sch->rx_video_mgr_mutex);

  rte_atomic32_dec(&impl->st22_rx_sessions_cnt);
  info("%s, succ on sch %d session %d\n", __func__, sch_idx, idx);
  return 0;
}

void* st22_rx_get_mbuf(st22_rx_handle handle, void** usrptr, uint16_t* len) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;
  struct rte_mbuf* pkt;
  int idx, ret;
  struct rte_ring* rtps_ring;

  if (s_impl->type != ST22_SESSION_TYPE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return NULL;
  }

  s = s_impl->impl;
  idx = s->idx;
  rtps_ring = s->st20_rtps_ring;
  if (!rtps_ring) {
    err("%s(%d), rtp ring is not created\n", __func__, idx);
    return NULL;
  }

  ret = rte_ring_sc_dequeue(rtps_ring, (void**)&pkt);
  if (ret < 0) {
    dbg("%s(%d), rtp ring is empty\n", __func__, idx);
    return NULL;
  }

  size_t hdr_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                   sizeof(struct rte_udp_hdr);
  *len = pkt->data_len - hdr_len;
  *usrptr = rte_pktmbuf_mtod_offset(pkt, void*, hdr_len);
  return pkt;
}

void st22_rx_put_mbuf(st22_rx_handle handle, void* mbuf) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;
  struct rte_mbuf* pkt = (struct rte_mbuf*)mbuf;

  if (s_impl->type != ST22_SESSION_TYPE_RX_VIDEO)
    err("%s, invalid type %d\n", __func__, s_impl->type);

  if (pkt) rte_pktmbuf_free(pkt);
}

int st_request_exit(st_handle st) {
  struct st_main_impl* impl = st;

  rte_atomic32_set(&impl->request_exit, 1);

  return 0;
}

void* st_memcpy(void* dest, const void* src, size_t n) {
  return rte_memcpy(dest, src, n);
}

void* st_hp_malloc(st_handle st, size_t size, enum st_port port) {
  struct st_main_impl* impl = st;
  int num_ports = st_num_ports(impl);

  if (port < 0 || port >= num_ports) {
    err("%s, invalid port %d\n", __func__, port);
    return NULL;
  }

  return st_rte_malloc_socket(size, st_socket_id(impl, port));
}

void* st_hp_zmalloc(st_handle st, size_t size, enum st_port port) {
  struct st_main_impl* impl = st;
  int num_ports = st_num_ports(impl);

  if (port < 0 || port >= num_ports) {
    err("%s, invalid port %d\n", __func__, port);
    return NULL;
  }

  return st_rte_zmalloc_socket(size, st_socket_id(impl, port));
}

void st_hp_free(st_handle st, void* ptr) { return st_rte_free(ptr); }

const char* st_version(void) {
  static char version[64];
  if (version[0] != 0) return version;

  snprintf(version, sizeof(version), "%d.%d.%d %s %s", ST_VERSION_MAJOR, ST_VERSION_MINOR,
           ST_VERSION_LAST, __TIMESTAMP__, __ST_GIT__);

  return version;
}

int st_get_cap(st_handle st, struct st_cap* cap) {
  struct st_main_impl* impl = st;

  cap->tx_sessions_cnt_max = impl->tx_sessions_cnt_max;
  cap->rx_sessions_cnt_max = impl->rx_sessions_cnt_max;
  return 0;
}

int st_get_stats(st_handle st, struct st_stats* stats) {
  struct st_main_impl* impl = st;

  stats->st20_tx_sessions_cnt = rte_atomic32_read(&impl->st20_tx_sessions_cnt);
  stats->st22_tx_sessions_cnt = rte_atomic32_read(&impl->st22_tx_sessions_cnt);
  stats->st30_tx_sessions_cnt = rte_atomic32_read(&impl->st30_tx_sessions_cnt);
  stats->st40_tx_sessions_cnt = rte_atomic32_read(&impl->st40_tx_sessions_cnt);
  stats->st20_rx_sessions_cnt = rte_atomic32_read(&impl->st20_rx_sessions_cnt);
  stats->st22_rx_sessions_cnt = rte_atomic32_read(&impl->st22_rx_sessions_cnt);
  stats->st30_rx_sessions_cnt = rte_atomic32_read(&impl->st30_rx_sessions_cnt);
  stats->st40_rx_sessions_cnt = rte_atomic32_read(&impl->st40_rx_sessions_cnt);
  stats->sch_cnt = rte_atomic32_read(&impl->sch_cnt);
  stats->lcore_cnt = rte_atomic32_read(&impl->lcore_cnt);
  if (rte_atomic32_read(&impl->started))
    stats->dev_started = 1;
  else
    stats->dev_started = 0;
  return 0;
}

uint64_t st_ptp_read_time(st_handle st) {
  struct st_main_impl* impl = st;
  return st_get_ptp_time(impl, ST_PORT_P);
}
