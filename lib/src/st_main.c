/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_main.h"

#include "pipeline/st_plugin.h"
#include "st_admin.h"
#include "st_ancillary_transmitter.h"
#include "st_arp.h"
#include "st_audio_transmitter.h"
#include "st_cni.h"
#include "st_config.h"
#include "st_dev.h"
#include "st_dma.h"
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
#include "st_util.h"
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

bool st_is_valid_socket(struct st_main_impl* impl, int soc_id) {
  int num_ports = st_num_ports(impl);
  int i;

  for (i = 0; i < num_ports; i++) {
    if (soc_id == st_socket_id(impl, i)) return true;
  }

  err("%s, invalid soc_id %d\n", __func__, soc_id);
  return false;
}

static int u64_cmp(const void* a, const void* b) {
  const uint64_t* ai = a;
  const uint64_t* bi = b;

  if (*ai < *bi) {
    return -1;
  } else if (*ai > *bi) {
    return 1;
  }
  return 0;
}

static void* st_calibrate_tsc(void* arg) {
  struct st_main_impl* impl = arg;
  int loop = 100;
  int trim = 10;
  uint64_t array[loop];
  uint64_t tsc_hz_sum = 0;

  for (int i = 0; i < loop; i++) {
    uint64_t start, start_tsc, end, end_tsc;

    start = st_get_monotonic_time();
    start_tsc = rte_get_tsc_cycles();

    st_sleep_ms(10);

    end = st_get_monotonic_time();
    end_tsc = rte_get_tsc_cycles();
    array[i] = NS_PER_S * (end_tsc - start_tsc) / (end - start);
  }

  qsort(array, loop, sizeof(uint64_t), u64_cmp);
  for (int i = trim; i < loop - trim; i++) {
    tsc_hz_sum += array[i];
  }
  impl->tsc_hz = tsc_hz_sum / (loop - trim * 2);

  info("%s, tscHz %" PRIu64 "\n", __func__, impl->tsc_hz);
  return NULL;
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

  /* create tx ancillary context */
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

  st_dma_init(impl);

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

  ret = st_admin_init(impl);
  if (ret < 0) {
    err("%s, st_admin_init fail %d\n", __func__, ret);
    return ret;
  }

  ret = st_plugins_init(impl);
  if (ret < 0) {
    err("%s, st_plugins_init fail %d\n", __func__, ret);
    return ret;
  }

  ret = st_config_init(impl);
  if (ret < 0) {
    err("%s, st_config_init fail %d\n", __func__, ret);
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

  st_config_uinit(impl);
  st_plugins_uinit(impl);
  st_admin_uinit(impl);
  st_cni_uinit(impl);
  st_ptp_uinit(impl);
  st_arp_uinit(impl);
  st_mcast_uinit(impl);

  st_dma_uinit(impl);

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
    if (!ops->get_next_frame) {
      err("%s, pls set get_next_frame\n", __func__);
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
    if (!ops->get_next_frame) {
      err("%s, pls set get_next_frame\n", __func__);
      return -EINVAL;
    }
  } else if (ops->type == ST40_TYPE_RTP_LEVEL) {
    if (ops->rtp_ring_size <= 0) {
      err("%s, invalid rtp_ring_size %d\n", __func__, ops->rtp_ring_size);
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

static int st_rx_video_ops_check(struct st20_rx_ops* ops) {
  int num_ports = ops->num_port, ret;
  uint8_t* ip;
  enum st20_type type = ops->type;

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

  if (st20_is_frame_type(type)) {
    if ((ops->framebuff_cnt < 2) || (ops->framebuff_cnt > ST20_FB_MAX_COUNT)) {
      err("%s, invalid framebuff_cnt %d, should in range [2:%d]\n", __func__,
          ops->framebuff_cnt, ST20_FB_MAX_COUNT);
      return -EINVAL;
    }
    if (!ops->notify_frame_ready) {
      err("%s, pls set notify_frame_ready\n", __func__);
      return -EINVAL;
    }
    if (ops->type == ST20_TYPE_SLICE_LEVEL) {
      if (!ops->notify_slice_ready) {
        err("%s, pls set notify_slice_ready\n", __func__);
        return -EINVAL;
      }
    }
    if (ops->flags & ST20_RX_FLAG_AUTO_DETECT) {
      if (!ops->notify_detected) {
        err("%s, pls set notify_detected\n", __func__);
        return -EINVAL;
      }
    }
  }

  if (ops->uframe_size) {
    if (!ops->uframe_pg_callback) {
      err("%s, pls set uframe_pg_callback\n", __func__);
      return -EINVAL;
    }
  }

  if (type == ST20_TYPE_RTP_LEVEL) {
    if (ops->rtp_ring_size <= 0) {
      err("%s, invalid rtp_ring_size %d\n", __func__, ops->rtp_ring_size);
      return -EINVAL;
    }
    if (!ops->notify_rtp_ready) {
      err("%s, pls set notify_rtp_ready\n", __func__);
      return -EINVAL;
    }
  }

  if (type == ST20_TYPE_SLICE_LEVEL) {
    if (!(ops->flags & ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME)) {
      err("%s, pls enable ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME for silce mode\n",
          __func__);
      return -EINVAL;
    }
  }

  if (!st_is_valid_payload_type(ops->payload_type)) {
    err("%s, invalid payload_type %d\n", __func__, ops->payload_type);
    return -EINVAL;
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
    if (!ops->notify_frame_ready) {
      err("%s, pls set notify_frame_ready\n", __func__);
      return -EINVAL;
    }
  }

  if (ops->type == ST22_TYPE_RTP_LEVEL) {
    if (ops->rtp_ring_size <= 0) {
      err("%s, invalid rtp_ring_size %d\n", __func__, ops->rtp_ring_size);
      return -EINVAL;
    }
    if (!ops->notify_rtp_ready) {
      err("%s, pls set notify_rtp_ready\n", __func__);
      return -EINVAL;
    }
  }

  if (!st_is_valid_payload_type(ops->payload_type)) {
    err("%s, invalid payload_type %d\n", __func__, ops->payload_type);
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
    if (!ops->notify_frame_ready) {
      err("%s, pls set notify_frame_ready\n", __func__);
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
    if (!ops->notify_rtp_ready) {
      err("%s, pls set notify_rtp_ready\n", __func__);
      return -EINVAL;
    }
  }

  if (!st_is_valid_payload_type(ops->payload_type)) {
    err("%s, invalid payload_type %d\n", __func__, ops->payload_type);
    return -EINVAL;
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

  if (!ops->notify_rtp_ready) {
    err("%s, pls set notify_rtp_ready\n", __func__);
    return -EINVAL;
  }

  if (!st_is_valid_payload_type(ops->payload_type)) {
    err("%s, invalid payload_type %d\n", __func__, ops->payload_type);
    return -EINVAL;
  }

  return 0;
}

static int st_rx_source_info_check(struct st_rx_source_info* src, int num_ports) {
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

static int _st_start(struct st_main_impl* impl) {
  int ret;

  if (rte_atomic32_read(&impl->started)) {
    dbg("%s, started already\n", __func__);
    return 0;
  }

  /* wait tsc calibrate done, pacing need fine tuned TSC */
  st_wait_tsc_stable(impl);

  ret = st_dev_start(impl);
  if (ret < 0) {
    err("%s, st_dev_start fail %d\n", __func__, ret);
    return ret;
  }

  rte_atomic32_set(&impl->started, 1);

  info("%s, succ, avail ports %d\n", __func__, rte_eth_dev_count_avail());
  return 0;
}

static int _st_stop(struct st_main_impl* impl) {
  if (!rte_atomic32_read(&impl->started)) {
    dbg("%s, not started\n", __func__);
    return 0;
  }

  st_dev_stop(impl);
  rte_atomic32_set(&impl->started, 0);
  info("%s, succ\n", __func__);
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
  impl->type = ST_SESSION_TYPE_MAIN;
  for (int i = 0; i < num_ports; i++) {
    impl->inf[i].socket_id = socket[i];
    info("%s(%d), socket_id %d\n", __func__, i, socket[i]);
  }
  rte_atomic32_set(&impl->started, 0);
  rte_atomic32_set(&impl->request_exit, 0);
  rte_atomic32_set(&impl->dev_in_reset, 0);
  impl->lcore_lock_fd = -1;
  impl->tx_sessions_cnt_max = RTE_MIN(180, p->tx_sessions_cnt_max);
  impl->rx_sessions_cnt_max = RTE_MIN(180, p->rx_sessions_cnt_max);
  info("%s, max sessions tx %d rx %d, flags 0x%" PRIx64 "\n", __func__,
       impl->tx_sessions_cnt_max, impl->rx_sessions_cnt_max,
       st_get_user_params(impl)->flags);
  impl->pkt_udp_suggest_max_size = ST_PKT_MAX_RTP_BYTES;
  if (p->pkt_udp_suggest_max_size) {
    if ((p->pkt_udp_suggest_max_size > 1000) &&
        (p->pkt_udp_suggest_max_size < (1460 - 8))) {
      impl->pkt_udp_suggest_max_size = p->pkt_udp_suggest_max_size;
      info("%s, new pkt_udp_suggest_max_size %u\n", __func__,
           impl->pkt_udp_suggest_max_size);
    } else {
      warn("%s, invalid pkt_udp_suggest_max_size %u\n", __func__,
           p->pkt_udp_suggest_max_size);
    }
  }

  /* init mgr lock for audio and anc */
  st_pthread_mutex_init(&impl->tx_a_mgr_mutex, NULL);
  st_pthread_mutex_init(&impl->rx_a_mgr_mutex, NULL);
  st_pthread_mutex_init(&impl->tx_anc_mgr_mutex, NULL);
  st_pthread_mutex_init(&impl->rx_anc_mgr_mutex, NULL);

  impl->tsc_hz = rte_get_tsc_hz();
  if (p->flags & ST_FLAG_TSC_PACING) {
    impl->tx_pacing_way = ST21_TX_PACING_WAY_TSC;
  } else {
    impl->tx_pacing_way = ST21_TX_PACING_WAY_AUTO;
  }

  /* init interface */
  ret = st_dev_if_init(impl);
  if (ret < 0) {
    err("%s, st dev if init fail %d\n", __func__, ret);
    goto err_exit;
  }

  ret = st_main_create(impl);
  if (ret < 0) {
    err("%s, st main create fail %d\n", __func__, ret);
    goto err_exit;
  }

  if (st_has_auto_start_stop(impl)) {
    ret = _st_start(impl);
    if (ret < 0) {
      err("%s, st start fail %d\n", __func__, ret);
      goto err_exit;
    }
  }

  info("%s, succ, tsc_hz %" PRIu64 "\n", __func__, impl->tsc_hz);
  info("%s, simd level %s\n", __func__, st_get_simd_level_name(st_get_simd_level()));
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

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  _st_stop(impl);

  st_tx_audio_uinit(impl);
  st_rx_audio_uinit(impl);
  st_tx_anc_uinit(impl);
  st_rx_anc_uinit(impl);

  st_main_free(impl);

  st_dev_if_uinit(impl);
  st_rte_free(impl);

  st_dev_uinit(p);

  info("%s, succ\n", __func__);
  return 0;
}

int st_start(st_handle st) {
  struct st_main_impl* impl = st;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  return _st_start(impl);
}

int st_stop(st_handle st) {
  struct st_main_impl* impl = st;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  if (st_has_auto_start_stop(impl)) return 0;

  return _st_stop(impl);
}

int st_get_lcore(st_handle st, unsigned int* lcore) {
  struct st_main_impl* impl = st;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  return st_dev_get_lcore(impl, lcore);
}

int st_put_lcore(st_handle st, unsigned int lcore) {
  struct st_main_impl* impl = st;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  return st_dev_put_lcore(impl, lcore);
}

int st_bind_to_lcore(st_handle st, pthread_t thread, unsigned int lcore) {
  struct st_main_impl* impl = st;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

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

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  ret = st_tx_video_ops_check(ops);
  if (ret < 0) {
    err("%s, st_tx_video_ops_check fail %d\n", __func__, ret);
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

  sch = st_sch_get(impl, quota_mbs, ST_SCH_TYPE_DEFAULT);
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
  s = st_tx_video_sessions_mgr_attach(&sch->tx_video_mgr, ops, ST_SESSION_TYPE_TX_VIDEO,
                                      NULL);
  st_pthread_mutex_unlock(&sch->tx_video_mgr_mutex);
  if (!s) {
    err("%s(%d), st_tx_sessions_mgr_attach fail\n", __func__, sch->idx);
    st_sch_put(sch, quota_mbs);
    st_rte_free(s_impl);
    return NULL;
  }

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

size_t st20_tx_get_framebuffer_size(st20_tx_handle handle) {
  struct st_tx_video_session_handle_impl* s_impl = handle;
  struct st_tx_video_session_impl* s;

  if (s_impl->type != ST_SESSION_TYPE_TX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return 0;
  }

  s = s_impl->impl;
  return s->st20_frame_size;
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
  ret = st_tx_video_sessions_mgr_detach(&sch->tx_video_mgr, s);
  st_pthread_mutex_unlock(&sch->tx_video_mgr_mutex);
  if (ret < 0)
    err("%s(%d,%d), st_tx_sessions_mgr_deattach fail\n", __func__, sch_idx, idx);

  ret = st_sch_put(sch, s_impl->quota_mbs);
  if (ret < 0) err("%s(%d, %d), st_sch_put fail\n", __func__, sch_idx, idx);

  st_rte_free(s_impl);

  /* update max idx */
  st_pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  st_tx_video_sessions_mgr_update(&sch->tx_video_mgr);
  st_pthread_mutex_unlock(&sch->tx_video_mgr_mutex);

  rte_atomic32_dec(&impl->st20_tx_sessions_cnt);
  info("%s, succ on sch %d session %d\n", __func__, sch_idx, idx);
  return 0;
}

st30_tx_handle st30_tx_create(st_handle st, struct st30_tx_ops* ops) {
  struct st_main_impl* impl = st;
  struct st_tx_audio_session_handle_impl* s_impl;
  struct st_tx_audio_session_impl* s;
  int ret;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  ret = st_tx_audio_ops_check(ops);
  if (ret < 0) {
    err("%s, st_tx_audio_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  st_pthread_mutex_lock(&impl->tx_a_mgr_mutex);
  ret = st_tx_audio_init(impl);
  st_pthread_mutex_unlock(&impl->tx_a_mgr_mutex);
  if (ret < 0) {
    err("%s, st_tx_audio_init fail %d\n", __func__, ret);
    return NULL;
  }

  s_impl = st_rte_zmalloc_socket(sizeof(*s_impl), st_socket_id(impl, ST_PORT_P));
  if (!s_impl) {
    err("%s, s_impl malloc fail\n", __func__);
    return NULL;
  }

  st_pthread_mutex_lock(&impl->tx_a_mgr_mutex);
  s = st_tx_audio_sessions_mgr_attach(&impl->tx_a_mgr, ops);
  st_pthread_mutex_unlock(&impl->tx_a_mgr_mutex);
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
  st_pthread_mutex_lock(&impl->tx_a_mgr_mutex);
  st_tx_audio_sessions_mgr_update(&impl->tx_a_mgr);
  st_pthread_mutex_unlock(&impl->tx_a_mgr_mutex);

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

  pkt = rte_pktmbuf_alloc(s->mbuf_mempool_chain);
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

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  ret = st_tx_ancillary_ops_check(ops);
  if (ret < 0) {
    err("%s, st_tx_ancillary_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  st_pthread_mutex_lock(&impl->tx_anc_mgr_mutex);
  ret = st_tx_anc_init(impl);
  st_pthread_mutex_unlock(&impl->tx_anc_mgr_mutex);
  if (ret < 0) {
    err("%s, st_tx_anc_init fail %d\n", __func__, ret);
    return NULL;
  }

  s_impl = st_rte_zmalloc_socket(sizeof(*s_impl), st_socket_id(impl, ST_PORT_P));
  if (!s_impl) {
    err("%s, s_impl malloc fail\n", __func__);
    return NULL;
  }

  st_pthread_mutex_lock(&impl->tx_anc_mgr_mutex);
  s = st_tx_ancillary_sessions_mgr_attach(&impl->tx_anc_mgr, ops);
  st_pthread_mutex_unlock(&impl->tx_anc_mgr_mutex);
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

  pkt = rte_pktmbuf_alloc(s->mbuf_mempool_chain);
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
  st_pthread_mutex_lock(&impl->tx_anc_mgr_mutex);
  st_tx_ancillary_sessions_mgr_update(&impl->tx_anc_mgr);
  st_pthread_mutex_unlock(&impl->tx_anc_mgr_mutex);

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
  int quota_mbs, ret, quota_mbs_wo_dma = 0;
  uint64_t bps;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

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
  quota_mbs *= ops->num_port;
  if (!st_has_user_quota(impl)) {
    if (ST20_TYPE_RTP_LEVEL == ops->type) {
      quota_mbs = quota_mbs * ST_QUOTA_TX1080P_PER_SCH / ST_QUOTA_RX1080P_RTP_PER_SCH;
    } else {
      quota_mbs_wo_dma =
          quota_mbs * ST_QUOTA_TX1080P_PER_SCH / ST_QUOTA_RX1080P_NO_DMA_PER_SCH;
      quota_mbs = quota_mbs * ST_QUOTA_TX1080P_PER_SCH / ST_QUOTA_RX1080P_PER_SCH;
    }
  }

  s_impl = st_rte_zmalloc_socket(sizeof(*s_impl), st_socket_id(impl, ST_PORT_P));
  if (!s_impl) {
    err("%s, s_impl malloc fail\n", __func__);
    return NULL;
  }

  sch = st_sch_get(
      impl, quota_mbs,
      st_rx_video_separate_sch(impl) ? ST_SCH_TYPE_RX_VIDEO_ONLY : ST_SCH_TYPE_DEFAULT);
  if (!sch) {
    st_rte_free(s_impl);
    err("%s, get sch fail\n", __func__);
    return NULL;
  }

  st_pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  ret = st_rx_video_sessions_sch_init(impl, sch);
  st_pthread_mutex_unlock(&sch->rx_video_mgr_mutex);
  if (ret < 0) {
    err("%s, st_rx_video_init fail %d\n", __func__, ret);
    st_sch_put(sch, quota_mbs);
    st_rte_free(s_impl);
    return NULL;
  }

  st_pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  s = st_rx_video_sessions_mgr_attach(&sch->rx_video_mgr, ops, NULL);
  st_pthread_mutex_unlock(&sch->rx_video_mgr_mutex);
  if (!s) {
    err("%s(%d), st_rx_video_sessions_mgr_attach fail\n", __func__, sch->idx);
    st_sch_put(sch, quota_mbs);
    st_rte_free(s_impl);
    return NULL;
  }

  if (!st_has_user_quota(impl) && st20_is_frame_type(ops->type) && !s->dma_dev) {
    int extra_quota_mbs = quota_mbs_wo_dma - quota_mbs;
    ret = st_sch_add_quota(sch, extra_quota_mbs);
    if (ret >= 0) quota_mbs += extra_quota_mbs;
  }

  s_impl->parnet = impl;
  s_impl->type = ST_SESSION_TYPE_RX_VIDEO;
  s_impl->sch = sch;
  s_impl->impl = s;
  s_impl->quota_mbs = quota_mbs;
  s->st20_handle = s_impl;

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

int st20_rx_pcapng_dump(st20_rx_handle handle, uint32_t max_dump_packets, bool sync,
                        struct st_pcap_dump_meta* meta) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s = s_impl->impl;
  struct st_main_impl* impl = s_impl->parnet;
  int ret;

  if (s_impl->type != ST_SESSION_TYPE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }

  ret = st_rx_video_session_start_pcapng(impl, s, max_dump_packets, sync, meta);

  return ret;
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
  sch_idx = sch->idx;

  /* no need to lock as session is located already */
  ret = st_rx_video_sessions_mgr_detach(&sch->rx_video_mgr, s);
  if (ret < 0)
    err("%s(%d,%d), st_rx_video_sessions_mgr_deattach fail\n", __func__, sch_idx, idx);

  ret = st_sch_put(sch, s_impl->quota_mbs);
  if (ret < 0) err("%s(%d,%d), st_sch_put fail\n", __func__, sch_idx, idx);

  st_rte_free(s_impl);

  /* update max idx */
  st_pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  st_rx_video_sessions_mgr_update(&sch->rx_video_mgr);
  st_pthread_mutex_unlock(&sch->rx_video_mgr_mutex);

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

size_t st20_rx_get_framebuffer_size(st20_rx_handle handle) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;

  if (s_impl->type != ST_SESSION_TYPE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return 0;
  }

  s = s_impl->impl;
  return s->st20_frame_size;
}

int st20_rx_get_framebuffer_count(st20_rx_handle handle) {
  struct st_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;

  if (s_impl->type != ST_SESSION_TYPE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }

  s = s_impl->impl;
  return s->st20_frames_cnt;
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

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  ret = st_rx_audio_ops_check(ops);
  if (ret < 0) {
    err("%s, st_rx_audio_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  st_pthread_mutex_lock(&impl->rx_a_mgr_mutex);
  ret = st_rx_audio_init(impl);
  st_pthread_mutex_unlock(&impl->rx_a_mgr_mutex);
  if (ret < 0) {
    err("%s, st_rx_audio_init fail %d\n", __func__, ret);
    return NULL;
  }

  s_impl = st_rte_zmalloc_socket(sizeof(*s_impl), st_socket_id(impl, ST_PORT_P));
  if (!s_impl) {
    err("%s, s_impl malloc fail\n", __func__);
    return NULL;
  }

  st_pthread_mutex_lock(&impl->rx_a_mgr_mutex);
  s = st_rx_audio_sessions_mgr_attach(&impl->rx_a_mgr, ops);
  st_pthread_mutex_unlock(&impl->rx_a_mgr_mutex);
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
  st_pthread_mutex_lock(&impl->rx_a_mgr_mutex);
  st_rx_audio_sessions_mgr_update(&impl->rx_a_mgr);
  st_pthread_mutex_unlock(&impl->rx_a_mgr_mutex);

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

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  ret = st_rx_ancillary_ops_check(ops);
  if (ret < 0) {
    err("%s, st_rx_audio_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  st_pthread_mutex_lock(&impl->rx_anc_mgr_mutex);
  ret = st_rx_anc_init(impl);
  st_pthread_mutex_unlock(&impl->rx_anc_mgr_mutex);
  if (ret < 0) {
    err("%s, st_rx_audio_init fail %d\n", __func__, ret);
    return NULL;
  }

  s_impl = st_rte_zmalloc_socket(sizeof(*s_impl), st_socket_id(impl, ST_PORT_P));
  if (!s_impl) {
    err("%s, s_impl malloc fail\n", __func__);
    return NULL;
  }

  st_pthread_mutex_lock(&impl->rx_anc_mgr_mutex);
  s = st_rx_ancillary_sessions_mgr_attach(&impl->rx_anc_mgr, ops);
  st_pthread_mutex_unlock(&impl->rx_anc_mgr_mutex);
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
  st_pthread_mutex_lock(&impl->rx_anc_mgr_mutex);
  st_rx_ancillary_sessions_mgr_update(&impl->rx_anc_mgr);
  st_pthread_mutex_unlock(&impl->rx_anc_mgr_mutex);

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

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  ret = st22_tx_video_ops_check(ops);
  if (ret < 0) {
    err("%s, st_tx_video_ops_check fail %d\n", __func__, ret);
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

  sch = st_sch_get(impl, quota_mbs, ST_SCH_TYPE_DEFAULT);
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
  if (ops->num_port > 1) {
    memcpy(st20_ops.dip_addr[ST_PORT_R], ops->dip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(st20_ops.port[ST_PORT_R], ops->port[ST_PORT_R], ST_PORT_MAX_LEN);
    st20_ops.udp_port[ST_PORT_R] = ops->udp_port[ST_PORT_R];
  }
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
  st20_ops.notify_frame_done = ops->notify_frame_done;
  st20_ops.notify_rtp_done = ops->notify_rtp_done;
  st_pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  if (ST22_TYPE_RTP_LEVEL == ops->type) {
    s = st_tx_video_sessions_mgr_attach(&sch->tx_video_mgr, &st20_ops,
                                        ST22_SESSION_TYPE_TX_VIDEO, NULL);
  } else {
    s = st_tx_video_sessions_mgr_attach(&sch->tx_video_mgr, &st20_ops,
                                        ST22_SESSION_TYPE_TX_VIDEO, ops);
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
  ret = st_tx_video_sessions_mgr_detach(&sch->tx_video_mgr, s);
  st_pthread_mutex_unlock(&sch->tx_video_mgr_mutex);
  if (ret < 0)
    err("%s(%d,%d), st_tx_sessions_mgr_deattach fail\n", __func__, sch_idx, idx);

  ret = st_sch_put(sch, s_impl->quota_mbs);
  if (ret < 0) err("%s(%d, %d), st_sch_put fail\n", __func__, sch_idx, idx);

  st_rte_free(s_impl);

  /* update max idx */
  st_pthread_mutex_lock(&sch->tx_video_mgr_mutex);
  st_tx_video_sessions_mgr_update(&sch->tx_video_mgr);
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

  if (idx < 0 || idx >= s->st20_frames_cnt) {
    err("%s, invalid idx %d, should be in range [0, %d]\n", __func__, idx,
        s->st20_frames_cnt);
    return NULL;
  }
  if (!s->st20_frames) {
    err("%s, st20_frames not allocated\n", __func__);
    return NULL;
  }

  if (s->st22_info) {
    return s->st20_frames[idx] + s->st22_box_hdr_length;
  } else {
    return s->st20_frames[idx];
  }
}

st22_rx_handle st22_rx_create(st_handle st, struct st22_rx_ops* ops) {
  struct st_main_impl* impl = st;
  struct st_sch_impl* sch;
  struct st22_rx_video_session_handle_impl* s_impl;
  struct st_rx_video_session_impl* s;
  int quota_mbs, ret;
  uint64_t bps;
  struct st20_rx_ops st20_ops;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  ret = st22_rx_video_ops_check(ops);
  if (ret < 0) {
    err("%s, st_rx_video_ops_check fail %d\n", __func__, ret);
    return NULL;
  }

  if (ST22_TYPE_RTP_LEVEL == ops->type) {
    ret = st20_get_bandwidth_bps(ops->width, ops->height, ST20_FMT_YUV_422_10BIT,
                                 ops->fps, &bps);
    if (ret < 0) {
      err("%s, get_bandwidth_bps fail\n", __func__);
      return NULL;
    }
    bps /= 4; /* default compress ratio 1/4 */
    quota_mbs = bps / (1000 * 1000);
    quota_mbs *= ops->num_port;
    quota_mbs *= 2; /* double quota for RTP path */
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

  sch = st_sch_get(
      impl, quota_mbs,
      st_rx_video_separate_sch(impl) ? ST_SCH_TYPE_RX_VIDEO_ONLY : ST_SCH_TYPE_DEFAULT);
  if (!sch) {
    st_rte_free(s_impl);
    err("%s, get sch fail\n", __func__);
    return NULL;
  }

  st_pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  ret = st_rx_video_sessions_sch_init(impl, sch);
  st_pthread_mutex_unlock(&sch->rx_video_mgr_mutex);
  if (ret < 0) {
    err("%s, st_rx_video_init fail %d\n", __func__, ret);
    st_sch_put(sch, quota_mbs);
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
  if (ops->type == ST22_TYPE_RTP_LEVEL)
    st20_ops.type = ST20_TYPE_RTP_LEVEL;
  else
    st20_ops.type = ST20_TYPE_FRAME_LEVEL;
  st20_ops.width = ops->width;
  st20_ops.height = ops->height;
  st20_ops.fps = ops->fps;
  st20_ops.fmt = ST20_FMT_YUV_422_10BIT;
  st20_ops.payload_type = ops->payload_type;
  st20_ops.rtp_ring_size = ops->rtp_ring_size;
  st20_ops.notify_rtp_ready = ops->notify_rtp_ready;
  st20_ops.framebuff_cnt = ops->framebuff_cnt;
  st_pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  s = st_rx_video_sessions_mgr_attach(&sch->rx_video_mgr, &st20_ops, ops);
  st_pthread_mutex_unlock(&sch->rx_video_mgr_mutex);
  if (!s) {
    err("%s(%d), st_rx_video_sessions_mgr_attach fail\n", __func__, sch->idx);
    st_sch_put(sch, quota_mbs);
    st_rte_free(s_impl);
    return NULL;
  }

  s_impl->parnet = impl;
  s_impl->type = ST22_SESSION_TYPE_RX_VIDEO;
  s_impl->sch = sch;
  s_impl->impl = s;
  s_impl->quota_mbs = quota_mbs;
  s->st22_handle = s_impl;

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

int st22_rx_pcapng_dump(st22_rx_handle handle, uint32_t max_dump_packets, bool sync,
                        struct st_pcap_dump_meta* meta) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s = s_impl->impl;
  struct st_main_impl* impl = s_impl->parnet;
  int ret;

  if (s_impl->type != ST22_SESSION_TYPE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EINVAL;
  }

  ret = st_rx_video_session_start_pcapng(impl, s, max_dump_packets, sync, meta);

  return ret;
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
  sch_idx = sch->idx;

  /* no need to lock as session is located already */
  ret = st_rx_video_sessions_mgr_detach(&sch->rx_video_mgr, s);
  if (ret < 0)
    err("%s(%d,%d), st_rx_video_sessions_mgr_deattach fail\n", __func__, sch_idx, idx);

  ret = st_sch_put(sch, s_impl->quota_mbs);
  if (ret < 0) err("%s(%d,%d), st_sch_put fail\n", __func__, sch_idx, idx);

  st_rte_free(s_impl);

  /* update max idx */
  st_pthread_mutex_lock(&sch->rx_video_mgr_mutex);
  st_rx_video_sessions_mgr_update(&sch->rx_video_mgr);
  st_pthread_mutex_unlock(&sch->rx_video_mgr_mutex);

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

int st22_rx_put_framebuff(st22_rx_handle handle, void* frame) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;

  if (s_impl->type != ST22_SESSION_TYPE_RX_VIDEO) {
    err("%s, invalid type %d\n", __func__, s_impl->type);
    return -EIO;
  }

  s = s_impl->impl;

  return st_rx_video_session_put_frame(s, frame);
}

void* st22_rx_get_fb_addr(st22_rx_handle handle, uint16_t idx) {
  struct st22_rx_video_session_handle_impl* s_impl = handle;
  struct st_rx_video_session_impl* s;

  if (s_impl->type != ST22_SESSION_TYPE_RX_VIDEO) {
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

int st_request_exit(st_handle st) {
  struct st_main_impl* impl = st;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  rte_atomic32_set(&impl->request_exit, 1);

  return 0;
}

void* st_memcpy(void* dest, const void* src, size_t n) {
  return rte_memcpy(dest, src, n);
}

void* st_hp_malloc(st_handle st, size_t size, enum st_port port) {
  struct st_main_impl* impl = st;
  int num_ports = st_num_ports(impl);

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  if (port < 0 || port >= num_ports) {
    err("%s, invalid port %d\n", __func__, port);
    return NULL;
  }

  return st_rte_malloc_socket(size, st_socket_id(impl, port));
}

void* st_hp_zmalloc(st_handle st, size_t size, enum st_port port) {
  struct st_main_impl* impl = st;
  int num_ports = st_num_ports(impl);

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  if (port < 0 || port >= num_ports) {
    err("%s, invalid port %d\n", __func__, port);
    return NULL;
  }

  return st_rte_zmalloc_socket(size, st_socket_id(impl, port));
}

void st_hp_free(st_handle st, void* ptr) { return st_rte_free(ptr); }

st_iova_t st_hp_virt2iova(st_handle st, const void* addr) {
  return rte_malloc_virt2iova(addr);
}

const char* st_version(void) {
  static char version[64];
  if (version[0] != 0) return version;

  snprintf(version, sizeof(version), "%d.%d.%d %s %s", ST_VERSION_MAJOR, ST_VERSION_MINOR,
           ST_VERSION_LAST, __TIMESTAMP__, __ST_GIT__);

  return version;
}

int st_get_cap(st_handle st, struct st_cap* cap) {
  struct st_main_impl* impl = st;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  cap->tx_sessions_cnt_max = impl->tx_sessions_cnt_max;
  cap->rx_sessions_cnt_max = impl->rx_sessions_cnt_max;
  cap->dma_dev_cnt_max = impl->dma_mgr.num_dma_dev;
  return 0;
}

int st_get_stats(st_handle st, struct st_stats* stats) {
  struct st_main_impl* impl = st;
  struct st_dma_mgr* mgr = st_get_dma_mgr(impl);

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return -EIO;
  }

  stats->st20_tx_sessions_cnt = rte_atomic32_read(&impl->st20_tx_sessions_cnt);
  stats->st22_tx_sessions_cnt = rte_atomic32_read(&impl->st22_tx_sessions_cnt);
  stats->st30_tx_sessions_cnt = rte_atomic32_read(&impl->st30_tx_sessions_cnt);
  stats->st40_tx_sessions_cnt = rte_atomic32_read(&impl->st40_tx_sessions_cnt);
  stats->st20_rx_sessions_cnt = rte_atomic32_read(&impl->st20_rx_sessions_cnt);
  stats->st22_rx_sessions_cnt = rte_atomic32_read(&impl->st22_rx_sessions_cnt);
  stats->st30_rx_sessions_cnt = rte_atomic32_read(&impl->st30_rx_sessions_cnt);
  stats->st40_rx_sessions_cnt = rte_atomic32_read(&impl->st40_rx_sessions_cnt);
  stats->sch_cnt = rte_atomic32_read(&st_sch_get_mgr(impl)->sch_cnt);
  stats->lcore_cnt = rte_atomic32_read(&impl->lcore_cnt);
  stats->dma_dev_cnt = rte_atomic32_read(&mgr->num_dma_dev_active);
  if (rte_atomic32_read(&impl->started))
    stats->dev_started = 1;
  else
    stats->dev_started = 0;
  return 0;
}

uint64_t st_ptp_read_time(st_handle st) {
  struct st_main_impl* impl = st;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return 0;
  }

  return st_get_ptp_time(impl, ST_PORT_P);
}

st_udma_handle st_udma_create(st_handle st, uint16_t nb_desc, enum st_port port) {
  struct st_main_impl* impl = st;
  struct st_dma_request_req req;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  req.nb_desc = nb_desc;
  req.max_shared = 1;
  req.sch_idx = 0;
  req.socket_id = st_socket_id(impl, port);
  req.priv = impl;
  req.drop_mbuf_cb = NULL;
  struct st_dma_lender_dev* dev = st_dma_request_dev(impl, &req);
  if (dev) dev->type = ST_SESSION_TYPE_UDMA;
  return dev;
}

int st_udma_free(st_udma_handle handle) {
  struct st_dma_lender_dev* dev = handle;
  struct st_main_impl* impl = dev->priv;

  if (dev->type != ST_SESSION_TYPE_UDMA) {
    err("%s, invalid type %d\n", __func__, dev->type);
    return -EIO;
  }

  return st_dma_free_dev(impl, dev);
}

int st_udma_copy(st_udma_handle handle, st_iova_t dst, st_iova_t src, uint32_t length) {
  struct st_dma_lender_dev* dev = handle;

  if (dev->type != ST_SESSION_TYPE_UDMA) {
    err("%s, invalid type %d\n", __func__, dev->type);
    return -EIO;
  }

  return st_dma_copy(dev, dst, src, length);
}

int st_udma_fill(st_udma_handle handle, st_iova_t dst, uint64_t pattern,
                 uint32_t length) {
  struct st_dma_lender_dev* dev = handle;

  if (dev->type != ST_SESSION_TYPE_UDMA) {
    err("%s, invalid type %d\n", __func__, dev->type);
    return -EIO;
  }

  return st_dma_fill(dev, dst, pattern, length);
}

int st_udma_submit(st_udma_handle handle) {
  struct st_dma_lender_dev* dev = handle;

  if (dev->type != ST_SESSION_TYPE_UDMA) {
    err("%s, invalid type %d\n", __func__, dev->type);
    return -EIO;
  }

  return st_dma_submit(dev);
}

uint16_t st_udma_completed(st_udma_handle handle, const uint16_t nb_cpls) {
  struct st_dma_lender_dev* dev = handle;

  if (dev->type != ST_SESSION_TYPE_UDMA) {
    err("%s, invalid type %d\n", __func__, dev->type);
    return -EIO;
  }

  return st_dma_completed(dev, nb_cpls, NULL, NULL);
}

enum st_simd_level st_get_simd_level(void) {
  if (rte_cpu_get_flag_enabled(RTE_CPUFLAG_AVX512VBMI2))
    return ST_SIMD_LEVEL_AVX512_VBMI2;
  if (rte_cpu_get_flag_enabled(RTE_CPUFLAG_AVX512VL)) return ST_SIMD_LEVEL_AVX512;
  if (rte_cpu_get_flag_enabled(RTE_CPUFLAG_AVX2)) return ST_SIMD_LEVEL_AVX2;
  /* no simd */
  return ST_SIMD_LEVEL_NONE;
}

static const char* st_simd_level_names[ST_SIMD_LEVEL_MAX] = {
    "none",
    "avx2",
    "avx512",
    "avx512_vbmi",
};

const char* st_get_simd_level_name(enum st_simd_level level) {
  if ((level >= ST_SIMD_LEVEL_MAX) || (level < 0)) {
    err("%s, invalid level %d\n", __func__, level);
    return "unknown";
  }

  return st_simd_level_names[level];
}
