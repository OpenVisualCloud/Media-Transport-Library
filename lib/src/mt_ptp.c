/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_ptp.h"

#include "mt_cni.h"
#include "mt_queue.h"
// #define DEBUG
#include "mt_log.h"
#include "mt_mcast.h"
#include "mt_sch.h"
#include "mt_stat.h"
#include "mt_util.h"

#define MT_PTP_USE_TX_TIME_STAMP (1)
#define MT_PTP_USE_TX_TIMER (1)
#define MT_PTP_CHECK_TX_TIME_STAMP (0)
#define MT_PTP_CHECK_RX_TIME_STAMP (0)
#define MT_PTP_PRINT_ERR_RESULT (0)

#define MT_PTP_EBU_SYNC_MS (10)

#define MT_PTP_DEFAULT_KP 5e-10 /* to be tuned */
#define MT_PTP_DEFAULT_KI 1e-10 /* to be tuned */

static char* ptp_mode_strs[MT_PTP_MAX_MODE] = {
    "l2",
    "l4",
};

static inline char* ptp_mode_str(enum mt_ptp_l_mode mode) { return ptp_mode_strs[mode]; }

static inline uint64_t ptp_net_tmstamp_to_ns(struct mt_ptp_tmstamp* ts) {
  uint64_t sec = (uint64_t)ntohl(ts->sec_lsb) + ((uint64_t)ntohs(ts->sec_msb) << 32);
  return (sec * NS_PER_S) + ntohl(ts->ns);
}

static inline void ptp_timesync_lock(struct mt_ptp_impl* ptp) { /* todo */
}

static inline void ptp_timesync_unlock(struct mt_ptp_impl* ptp) { /* todo */
}

static inline uint64_t ptp_correct_ts(struct mt_ptp_impl* ptp, uint64_t ts) {
  int64_t ts_local_advanced = ts - ptp->last_sync_ts;
  int64_t ts_ptp_advanced = ptp->coefficient * ts_local_advanced;
  return ptp->last_sync_ts + ts_ptp_advanced;
}

static inline int ptp_get_time_spec(struct mt_ptp_impl* ptp, struct timespec* spec) {
  enum mtl_port port = ptp->port;
  uint16_t port_id = ptp->port_id;
  int ret;

  ptp_timesync_lock(ptp);
  ret = rte_eth_timesync_read_time(port_id, spec);
  ptp_timesync_unlock(ptp);

  if (ret < 0) err("%s(%d), err %d\n", __func__, port, ret);
  return ret;
}

static inline uint64_t ptp_get_raw_time(struct mt_ptp_impl* ptp) {
  struct timespec spec;

  ptp_get_time_spec(ptp, &spec);
  return mt_timespec_to_ns(&spec);
}

static uint64_t ptp_get_correct_time(struct mt_ptp_impl* ptp) {
  struct timespec spec;

  ptp_get_time_spec(ptp, &spec);
  return ptp_correct_ts(ptp, mt_timespec_to_ns(&spec));
}

static uint64_t ptp_from_eth(struct mtl_main_impl* impl, enum mtl_port port) {
  return ptp_get_correct_time(mt_get_ptp(impl, port));
}

static void ptp_print_port_id(enum mtl_port port, struct mt_ptp_port_id* pid) {
  uint8_t* id = &pid->clock_identity.id[0];
  info(
      "mt_ptp_port_id(%d), port_number: %04x, clk_id: "
      "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
      port, pid->port_number, id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
}

static inline bool ptp_port_id_equal(struct mt_ptp_port_id* s, struct mt_ptp_port_id* t) {
  if (!memcmp(s, t, sizeof(struct mt_ptp_port_id)))
    return true;
  else
    return false;
}

static struct rte_ether_addr ptp_l4_multicast_eaddr = {
    {0x01, 0x00, 0x5e, 0x00, 0x01, 0x81}}; /* 224.0.1.129 */

static struct rte_ether_addr ptp_l2_multicast_eaddr = {
    {0x01, 0x1b, 0x19, 0x00, 0x00, 0x00}};

static inline void ptp_set_master_addr(struct mt_ptp_impl* ptp,
                                       struct rte_ether_addr* d_addr) {
  if (ptp->master_addr_mode == MT_PTP_MULTICAST_ADDR) {
    if (ptp->t2_mode == MT_PTP_L4)
      rte_ether_addr_copy(&ptp_l4_multicast_eaddr, d_addr);
    else
      rte_ether_addr_copy(&ptp_l2_multicast_eaddr, d_addr);
  } else {
    rte_ether_addr_copy(&ptp->master_addr, d_addr);
  }
}

static void ptp_coefficient_result_reset(struct mt_ptp_impl* ptp) {
  ptp->coefficient_result_sum = 0.0;
  ptp->coefficient_result_min = 2.0;
  ptp->coefficient_result_max = 0.0;
  ptp->coefficient_result_cnt = 0;
}

static void ptp_update_coefficient(struct mt_ptp_impl* ptp, int64_t error) {
  ptp->integral += (error + ptp->prev_error) / 2;
  ptp->prev_error = error;
  double offset = ptp->kp * error + ptp->ki * ptp->integral;
  if (ptp->t2_mode == MT_PTP_L4) offset /= 4; /* where sync interval is 0.25s for l4 */
  ptp->coefficient += RTE_MIN(RTE_MAX(offset, -1e-7), 1e-7);
  dbg("%s(%d), error %" PRId64 ", offset %.15lf\n", __func__, ptp->port, error, offset);
}

static void ptp_calculate_coefficient(struct mt_ptp_impl* ptp, int64_t delta) {
  if (delta > 1000 * 1000) return;
  uint64_t ts_s = ptp_get_raw_time(ptp);
  uint64_t ts_m = ts_s + delta;
  double coefficient = (double)(ts_m - ptp->last_sync_ts) / (ts_s - ptp->last_sync_ts);
  ptp->coefficient_result_sum += coefficient;
  ptp->coefficient_result_min = RTE_MIN(coefficient, ptp->coefficient_result_min);
  ptp->coefficient_result_max = RTE_MAX(coefficient, ptp->coefficient_result_max);
  ptp->coefficient_result_cnt++;
  if (ptp->coefficient - 1.0 < 1e-15) /* store first result */
    ptp->coefficient = coefficient;
  if (ptp->coefficient_result_cnt == 10) {
    /* get every 10 results' average */
    ptp->coefficient_result_sum -= ptp->coefficient_result_min;
    ptp->coefficient_result_sum -= ptp->coefficient_result_max;
    ptp->coefficient = ptp->coefficient_result_sum / 8;
    ptp_coefficient_result_reset(ptp);
  }
  ptp->last_sync_ts = ts_m;
  dbg("%s(%d), delta %" PRId64 ", co %.15lf, ptp %" PRIu64 "\n", __func__, ptp->port,
      delta, ptp->coefficient, ts_m);
}

static void ptp_adjust_delta(struct mt_ptp_impl* ptp, int64_t delta) {
  ptp_timesync_lock(ptp);
  rte_eth_timesync_adjust_time(ptp->port_id, delta);
  ptp_timesync_unlock(ptp);

  dbg("%s(%d), delta %" PRId64 ", ptp %" PRIu64 "\n", __func__, ptp->port, delta,
      ptp_get_raw_time(ptp));
  ptp->ptp_delta += delta;

  if (5 == ptp->delta_result_cnt) /* clear the first 5 results */
    ptp->delta_result_sum = labs(delta) * ptp->delta_result_cnt;
  else
    ptp->delta_result_sum += labs(delta);

  ptp->delta_result_cnt++;
  /* update status */
  ptp->stat_delta_min = RTE_MIN(delta, ptp->stat_delta_min);
  ptp->stat_delta_max = RTE_MAX(delta, ptp->stat_delta_max);
  ptp->stat_delta_cnt++;
  ptp->stat_delta_sum += labs(delta);
}

static void ptp_expect_result_clear(struct mt_ptp_impl* ptp) {
  ptp->expect_result_cnt = 0;
  ptp->expect_result_sum = 0;
  ptp->expect_result_start_ns = 0;
}

static void ptp_t_result_clear(struct mt_ptp_impl* ptp) {
  ptp->t1 = 0;
  ptp->t2 = 0;
  ptp->t3 = 0;
  ptp->t4 = 0;
}

static void ptp_result_reset(struct mt_ptp_impl* ptp) {
  ptp->delta_result_err = 0;
  ptp->delta_result_cnt = 0;
  ptp->delta_result_sum = 0;
  ptp->expect_result_avg = 0;
}

static void ptp_monitor_handler(void* param) {
  struct mt_ptp_impl* ptp = param;
  uint64_t expect_result_period_us = ptp->expect_result_period_ns / 1000;

  ptp->stat_sync_timeout_err++;
  if (ptp->expect_result_avg && expect_result_period_us) {
    ptp_adjust_delta(ptp, ptp->expect_result_avg);
    dbg("%s(%d), next timer %" PRIu64 "\n", __func__, ptp->port, expect_result_period_us);
    rte_eal_alarm_set(expect_result_period_us, ptp_monitor_handler, ptp);
  }
}

static void ptp_sync_timeout_handler(void* param) {
  struct mt_ptp_impl* ptp = param;
  uint64_t expect_result_period_us = ptp->expect_result_period_ns / 1000;

  ptp_expect_result_clear(ptp);
  ptp_t_result_clear(ptp);
  ptp->stat_sync_timeout_err++;
  if (ptp->expect_result_avg) {
    ptp_adjust_delta(ptp, ptp->expect_result_avg);
    dbg("%s(%d), next timer %" PRIu64 "\n", __func__, ptp->port, expect_result_period_us);
    if (expect_result_period_us) {
      rte_eal_alarm_set(expect_result_period_us, ptp_monitor_handler, ptp);
    }
  }
}

static int ptp_parse_result(struct mt_ptp_impl* ptp) {
  int64_t delta = ((int64_t)ptp->t4 - ptp->t3) - ((int64_t)ptp->t2 - ptp->t1);
  int64_t path_delay = ((int64_t)ptp->t2 - ptp->t1) + ((int64_t)ptp->t4 - ptp->t3);
  uint64_t abs_delta, expect_delta;

  dbg("%s(%d), t1 %" PRIu64 " t2 %" PRIu64 " t3 %" PRIu64 " t4 %" PRIu64 "\n", __func__,
      ptp->port, ptp->t1, ptp->t2, ptp->t3, ptp->t4);

  delta /= 2;
  path_delay /= 2;
  abs_delta = labs(delta);
  /* measure frequency corrected delta */
  int64_t correct_delta = ((int64_t)ptp->t4 - ptp_correct_ts(ptp, ptp->t3)) -
                          ((int64_t)ptp_correct_ts(ptp, ptp->t2) - ptp->t1);
  correct_delta /= 2;
  dbg("%s(%d), correct_delta %" PRId64 "\n", __func__, ptp->port, correct_delta);
  /* update correct delta and path delay result */
  ptp->stat_correct_delta_min = RTE_MIN(correct_delta, ptp->stat_correct_delta_min);
  ptp->stat_correct_delta_max = RTE_MAX(correct_delta, ptp->stat_correct_delta_max);
  ptp->stat_correct_delta_cnt++;
  ptp->stat_correct_delta_sum += labs(correct_delta);
  ptp->stat_path_delay_min = RTE_MIN(path_delay, ptp->stat_path_delay_min);
  ptp->stat_path_delay_max = RTE_MAX(path_delay, ptp->stat_path_delay_max);
  ptp->stat_path_delay_cnt++;
  ptp->stat_path_delay_sum += labs(path_delay);

  /* cancel the monitor */
  rte_eal_alarm_cancel(ptp_sync_timeout_handler, ptp);
  rte_eal_alarm_cancel(ptp_monitor_handler, ptp);

  if (ptp->delta_result_cnt) {
    expect_delta = abs(ptp->expect_result_avg) * (RTE_MIN(ptp->delta_result_err + 2, 5));
    if (!expect_delta) {
      expect_delta = ptp->delta_result_sum / ptp->delta_result_cnt * 2;
      expect_delta = RTE_MAX(expect_delta, 100 * 1000); /* min 100us */
    }
    if (abs_delta > expect_delta) {
#if MT_PTP_PRINT_ERR_RESULT
      err("%s(%d), error abs_delta %" PRIu64 "\n", __func__, ptp->port, abs_delta);
      err("%s(%d), t1 %" PRIu64 " t2 %" PRIu64 " t3 %" PRIu64 " t4 %" PRIu64 "\n",
          __func__, ptp->port, ptp->t1, ptp->t2, ptp->t3, ptp->t4);
#endif
      ptp_t_result_clear(ptp);
      ptp_expect_result_clear(ptp);
      ptp->delta_result_err++;
      ptp->stat_result_err++;
      if (ptp->delta_result_err > 10) {
        dbg("%s(%d), reset the result as too many errors\n", __func__, ptp->port);
        ptp_result_reset(ptp);
      }
      if (ptp->expect_result_avg) {
        ptp_adjust_delta(ptp, ptp->expect_result_avg);
      }
      return -EIO;
    }
  }
  ptp->delta_result_err = 0;

  if (ptp->use_pi && labs(correct_delta) < 1000) {
    /* fine tune coefficient */
    ptp_update_coefficient(ptp, correct_delta);
    ptp->last_sync_ts = ptp_get_raw_time(ptp) + delta; /* approximation */
  } else {
    /* re-calculate coefficient */
    ptp_calculate_coefficient(ptp, delta);
  }

  ptp_adjust_delta(ptp, delta);
  ptp_t_result_clear(ptp);

  if (ptp->delta_result_cnt > 10) {
    if (labs(delta) < 10000) {
      ptp->expect_result_cnt++;
      if (!ptp->expect_result_start_ns)
        ptp->expect_result_start_ns = mt_get_monotonic_time();
      ptp->expect_result_sum += delta;
      if (ptp->expect_result_cnt >= 10) {
        ptp->expect_result_avg = ptp->expect_result_sum / ptp->expect_result_cnt;
        ptp->expect_result_period_ns =
            (mt_get_monotonic_time() - ptp->expect_result_start_ns) /
            (ptp->expect_result_cnt - 1);
        dbg("%s(%d), expect result avg %u, period %fs\n", __func__, ptp->port,
            ptp->expect_result_avg, (float)ptp->expect_result_period_ns / NS_PER_S);
        ptp_expect_result_clear(ptp);
      }
    } else {
      ptp_expect_result_clear(ptp);
    }
  }

  return 0;
}

static void ptp_delay_req_task(struct mt_ptp_impl* ptp) {
  enum mtl_port port = ptp->port;
  uint16_t port_id = ptp->port_id;
  size_t hdr_offset;
  struct mt_ptp_sync_msg* msg;

  if (ptp->t3) return; /* t3 already sent */

  struct rte_mbuf* m = rte_pktmbuf_alloc(ptp->mbuf_pool);
  if (!m) {
    err("%s(%d), rte_pktmbuf_alloc fail\n", __func__, port);
    return;
  }

  if (ptp->t2_vlan) {
    err("%s(%d), todo for vlan\n", __func__, port);
  } else {
#if RTE_VERSION >= RTE_VERSION_NUM(21, 11, 0, 0)
    m->ol_flags |= RTE_MBUF_F_TX_IEEE1588_TMST;
#else
    m->ol_flags |= PKT_TX_IEEE1588_TMST;
#endif
  }

  struct rte_ether_hdr* hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr*);
  hdr_offset = sizeof(struct rte_ether_hdr);

  if (ptp->t2_mode == MT_PTP_L4) {
    struct mt_ipv4_udp* ipv4_hdr =
        rte_pktmbuf_mtod_offset(m, struct mt_ipv4_udp*, hdr_offset);
    hdr_offset += sizeof(struct mt_ipv4_udp);
    rte_memcpy(ipv4_hdr, &ptp->dst_udp, sizeof(*ipv4_hdr));
    ipv4_hdr->udp.src_port = htons(MT_PTP_UDP_EVENT_PORT);
    ipv4_hdr->udp.dst_port = ipv4_hdr->udp.src_port;
    ipv4_hdr->ip.time_to_live = 255;
    ipv4_hdr->ip.packet_id = htons(ptp->t3_sequence_id);
    mt_mbuf_init_ipv4(m);
    hdr->ether_type = htons(RTE_ETHER_TYPE_IPV4);
  } else {
    hdr->ether_type = htons(RTE_ETHER_TYPE_1588);
  }

  msg = rte_pktmbuf_mtod_offset(m, struct mt_ptp_sync_msg*, hdr_offset);
  memset(msg, 0x0, sizeof(*msg));
  msg->hdr.message_type = PTP_DELAY_REQ;
  msg->hdr.version = 2;
  msg->hdr.message_length = htons(sizeof(struct mt_ptp_sync_msg));
  msg->hdr.domain_number = ptp->t1_domain_number;
  rte_memcpy(&msg->hdr.source_port_identity, &ptp->our_port_id,
             sizeof(struct mt_ptp_port_id));
  ptp->t3_sequence_id++;
  msg->hdr.sequence_id = htons(ptp->t3_sequence_id);

  rte_eth_macaddr_get(port_id, mt_eth_s_addr(hdr));
  ptp_set_master_addr(ptp, mt_eth_d_addr(hdr));
  m->pkt_len = hdr_offset + sizeof(struct mt_ptp_sync_msg);
  m->data_len = m->pkt_len;

#if MT_PTP_USE_TX_TIME_STAMP
  struct timespec ts;
  ptp_timesync_lock(ptp);
  rte_eth_timesync_read_tx_timestamp(port_id, &ts);
  ptp_timesync_unlock(ptp);
#endif
  // mt_mbuf_dump(port, 0, "PTP_DELAY_REQ", m);
  uint16_t tx = mt_dev_tx_sys_queue_burst(ptp->impl, port, &m, 1);
  if (tx < 1) {
    rte_pktmbuf_free(m);
    err("%s(%d), tx fail\n", __func__, port);
    return;
  }

#if MT_PTP_USE_TX_TIME_STAMP
  /* Wait max 50 us to read TX timestamp. */
  int max_retry = 50;
  int ret;
  uint64_t tx_ns = 0;
  while (max_retry > 0) {
    ptp_timesync_lock(ptp);
    ret = rte_eth_timesync_read_tx_timestamp(port_id, &ts);
    ptp_timesync_unlock(ptp);
    if (ret >= 0) {
      tx_ns = mt_timespec_to_ns(&ts);
      break;
    }
    mt_delay_us(1);
    max_retry--;
  }

  if (max_retry <= 0) {
    err("%s(%d), read tx reach max retry\n", __func__, port);
  }

#if MT_PTP_CHECK_TX_TIME_STAMP
  ptp_timesync_lock(ptp);
  rte_eth_timesync_read_time(port_id, &ts);
  ptp_timesync_unlock(ptp);

  uint64_t ptp_ns = mt_timespec_to_ns(&ts);
  uint64_t delta = ptp_ns - tx_ns;
#define TX_MAX_DELTA (1 * 1000 * 1000) /* 1ms */
  if (unlikely(delta > TX_MAX_DELTA)) {
    err("%s(%d), tx_ns %" PRIu64 ", delta %" PRIu64 "\n", __func__, ptp->port, tx_ns,
        delta);
    ptp->stat_tx_sync_err++;
  }
#endif

  ptp->t3 = tx_ns;
#else
  ptp->t3 = ptp_get_raw_time(ptp);
#endif
  dbg("%s(%d), t3 %" PRIu64 ", seq %d, max_retry %d, ptp %" PRIu64 "\n", __func__, port,
      ptp->t3, ptp->t3_sequence_id, max_retry, ptp_get_raw_time(ptp));

  /* all time get */
  if (ptp->t4 && ptp->t2 && ptp->t1) {
    ptp_parse_result(ptp);
  }
}

#if MT_PTP_USE_TX_TIMER
static void ptp_delay_req_handler(void* param) {
  struct mt_ptp_impl* ptp = param;
  return ptp_delay_req_task(ptp);
}
#endif

static int ptp_parse_sync(struct mt_ptp_impl* ptp, struct mt_ptp_sync_msg* msg, bool vlan,
                          enum mt_ptp_l_mode mode, uint16_t timesync) {
  struct timespec timestamp;
  int ret;
  uint16_t port_id = ptp->port_id;
  uint64_t rx_ns = 0;
#define RX_MAX_DELTA (1 * 1000 * 1000) /* 1ms */

  ptp->stat_sync_cnt++;

  uint64_t monitor_period_us = ptp->expect_result_period_ns / 1000 / 2;
  if (monitor_period_us) {
    monitor_period_us = RTE_MAX(monitor_period_us, 100 * 1000 * 1000); /* min 100ms */
    if (ptp->t2) { /* already has a pending t2 */
      ptp_expect_result_clear(ptp);
      ptp_t_result_clear(ptp);
      ptp->stat_sync_timeout_err++;
      if (ptp->expect_result_avg) ptp_adjust_delta(ptp, ptp->expect_result_avg);
    }
    rte_eal_alarm_cancel(ptp_monitor_handler, ptp);
    rte_eal_alarm_cancel(ptp_sync_timeout_handler, ptp);
    rte_eal_alarm_set(monitor_period_us, ptp_sync_timeout_handler, ptp);
  }

  ptp_timesync_lock(ptp);
  ret = rte_eth_timesync_read_rx_timestamp(port_id, &timestamp, timesync);
  if (ret >= 0) rx_ns = mt_timespec_to_ns(&timestamp);
  ptp_timesync_unlock(ptp);

#if MT_PTP_CHECK_TX_TIME_STAMP
  uint64_t ptp_ns = 0, delta;
  ret = rte_eth_timesync_read_time(port_id, &timestamp);
  if (ret >= 0) ptp_ns = mt_timespec_to_ns(&timestamp);
  delta = ptp_ns - rx_ns;
  if (unlikely(delta > RX_MAX_DELTA)) {
    err("%s(%d), rx_ns %" PRIu64 ", delta %" PRIu64 "\n", __func__, ptp->port, rx_ns,
        delta);
    ptp->stat_rx_sync_err++;
  }
#endif

#if MT_PTP_USE_TX_TIMER
  rte_eal_alarm_cancel(ptp_delay_req_handler, ptp);
#endif
  ptp_t_result_clear(ptp);
  ptp->t2 = rx_ns;
  ptp->t2_sequence_id = msg->hdr.sequence_id;
  ptp->t2_vlan = vlan;
  ptp->t2_mode = mode;
  dbg("%s(%d), t2 %" PRIu64 ", seq %d, ptp %" PRIu64 "\n", __func__, ptp->port, ptp->t2,
      ptp->t2_sequence_id, ptp_get_raw_time(ptp));
  return 0;
}

static int ptp_parse_follow_up(struct mt_ptp_impl* ptp,
                               struct mt_ptp_follow_up_msg* msg) {
  if (msg->hdr.sequence_id != ptp->t2_sequence_id) {
    dbg("%s(%d), error sequence id %d %d\n", __func__, ptp->port, msg->hdr.sequence_id,
        ptp->t2_sequence_id);
    return -EINVAL;
  }

  ptp->t1 = ptp_net_tmstamp_to_ns(&msg->precise_origin_timestamp);
  ptp->t1_domain_number = msg->hdr.domain_number;
  dbg("%s(%d), t1 %" PRIu64 ", ptp %" PRIu64 "\n", __func__, ptp->port, ptp->t1,
      ptp_get_raw_time(ptp));

#if MT_PTP_USE_TX_TIMER
  rte_eal_alarm_set(MT_PTP_DELAY_REQ_US + (ptp->port * MT_PTP_DELAY_STEP_US),
                    ptp_delay_req_handler, ptp);
#else
  ptp_delay_req_task(ptp);
#endif

  return 0;
}

static int ptp_parse_announce(struct mt_ptp_impl* ptp, struct mt_ptp_announce_msg* msg,
                              enum mt_ptp_l_mode mode, struct mt_ipv4_udp* ipv4_hdr) {
  enum mtl_port port = ptp->port;

  if (!ptp->master_initialized) {
    ptp->master_initialized = true;
    ptp->master_utc_offset = ntohs(msg->current_utc_offset);
    rte_memcpy(&ptp->master_port_id, &msg->hdr.source_port_identity,
               sizeof(ptp->master_port_id));
    rte_memcpy(&ptp->master_addr.addr_bytes[0], &ptp->master_port_id.clock_identity.id[0],
               3);
    rte_memcpy(&ptp->master_addr.addr_bytes[3], &ptp->master_port_id.clock_identity.id[5],
               3);
    info("%s(%d), master initialized, mode %s utc_offset %d domain_number %d\n", __func__,
         port, ptp_mode_str(mode), ptp->master_utc_offset, msg->hdr.domain_number);
    ptp_print_port_id(port, &ptp->master_port_id);
    if (mode == MT_PTP_L4) {
      struct mt_ipv4_udp* dst_udp = &ptp->dst_udp;

      rte_memcpy(dst_udp, ipv4_hdr, sizeof(*dst_udp));
      rte_memcpy(&dst_udp->ip.src_addr, &ptp->sip_addr[0], MTL_IP_ADDR_LEN);
      dst_udp->ip.total_length =
          htons(sizeof(struct mt_ipv4_udp) + sizeof(struct mt_ptp_sync_msg));
      dst_udp->ip.hdr_checksum = 0;
      dst_udp->udp.dgram_len =
          htons(sizeof(struct rte_udp_hdr) + sizeof(struct mt_ptp_sync_msg));
    }

    /* point ptp fn to eth phc */
    if (mt_ptp_tsc_source(ptp->impl))
      warn("%s(%d), skip as ptp force to tsc\n", __func__, port);
    else if (mt_has_user_ptp(ptp->impl))
      warn("%s(%d), skip as user provide ptp source already\n", __func__, port);
    else
      mt_if(ptp->impl, port)->ptp_get_time_fn = ptp_from_eth;
  }

  return 0;
}

static int ptp_parse_delay_resp(struct mt_ptp_impl* ptp,
                                struct mt_ptp_delay_resp_msg* msg) {
  if (ptp->t4) {
    dbg("%s(%d), t4 already get\n", __func__, ptp->port);
    return -EIO;
  }

  if (ptp->t3_sequence_id != ntohs(msg->hdr.sequence_id)) {
    dbg("%s(%d), mismatch sequence_id %d %d\n", __func__, ptp->port, msg->hdr.sequence_id,
        ptp->t3_sequence_id);
    return -EIO;
  }

  ptp->t4 = ptp_net_tmstamp_to_ns(&msg->receive_timestamp);
  dbg("%s(%d), t4 %" PRIu64 ", seq %d, ptp %" PRIu64 "\n", __func__, ptp->port, ptp->t4,
      ptp->t3_sequence_id, ptp_get_raw_time(ptp));

  /* all time get */
  if (ptp->t3 && ptp->t2 && ptp->t1) {
    ptp_parse_result(ptp);
  }

  return 0;
}

static void ptp_stat_clear(struct mt_ptp_impl* ptp) {
  ptp->stat_delta_cnt = 0;
  ptp->stat_delta_sum = 0;
  ptp->stat_delta_min = INT_MAX;
  ptp->stat_delta_max = INT_MIN;
  ptp->stat_correct_delta_cnt = 0;
  ptp->stat_correct_delta_sum = 0;
  ptp->stat_correct_delta_min = INT_MAX;
  ptp->stat_correct_delta_max = INT_MIN;
  ptp->stat_path_delay_cnt = 0;
  ptp->stat_path_delay_sum = 0;
  ptp->stat_path_delay_min = INT_MAX;
  ptp->stat_path_delay_max = INT_MIN;
  ptp->stat_rx_sync_err = 0;
  ptp->stat_tx_sync_err = 0;
  ptp->stat_result_err = 0;
  ptp->stat_sync_timeout_err = 0;
  ptp->stat_sync_cnt = 0;
}

static void ptp_sync_from_user(struct mtl_main_impl* impl, struct mt_ptp_impl* ptp) {
  enum mtl_port port = ptp->port;
  uint64_t target_ns = mt_get_ptp_time(impl, port);
  uint64_t raw_ns = ptp_get_raw_time(ptp);
  int64_t delta = (int64_t)target_ns - raw_ns;
  uint64_t abs_delta = labs(delta);
  uint64_t expect_abs_delta = abs(ptp->expect_result_avg) * 2;

  if (expect_abs_delta) {
    if (abs_delta > expect_abs_delta) delta = ptp->expect_result_avg;
  } else {
    if (abs_delta < 10000) {
      ptp->expect_result_sum += delta;
      ptp->expect_result_cnt++;
      if (ptp->expect_result_cnt > 1000) {
        ptp->expect_result_avg = ptp->expect_result_sum / ptp->expect_result_cnt;
        info("%s(%d), expect delta %d, sum %d\n", __func__, port, ptp->expect_result_avg,
             ptp->expect_result_sum);
      }
    }
  }

  ptp->delta_result_cnt++;
  ptp_timesync_lock(ptp);
  rte_eth_timesync_adjust_time(ptp->port_id, delta);
  ptp_timesync_unlock(ptp);
  ptp->ptp_delta += delta;
  dbg("%s(%d), delta %" PRId64 "\n", __func__, port, delta);

  /* update status */
  ptp->stat_delta_min = RTE_MIN(delta, ptp->stat_delta_min);
  ptp->stat_delta_max = RTE_MAX(delta, ptp->stat_delta_max);
  ptp->stat_delta_cnt++;
  ptp->stat_delta_sum += labs(delta);
}

static void ptp_sync_from_user_handler(void* param) {
  struct mt_ptp_impl* ptp = param;

  ptp_sync_from_user(ptp->impl, ptp);
  rte_eal_alarm_set(MT_PTP_EBU_SYNC_MS * 1000, ptp_sync_from_user_handler, ptp);
}

static int ptp_init(struct mtl_main_impl* impl, struct mt_ptp_impl* ptp,
                    enum mtl_port port) {
  uint16_t port_id = mt_port_id(impl, port);
  struct rte_ether_addr mac;
  int ret;
  uint8_t* ip = &ptp->sip_addr[0];

  ret = rte_eth_macaddr_get(port_id, &mac);
  if (ret < 0) {
    err("%s(%d), succ rte_eth_macaddr_get fail %d\n", __func__, port, ret);
    return ret;
  }

  uint16_t magic = MT_PTP_CLOCK_IDENTITY_MAGIC;
  struct mt_ptp_port_id* our_port_id = &ptp->our_port_id;
  uint8_t* id = &our_port_id->clock_identity.id[0];
  memcpy(&id[0], &mac.addr_bytes[0], 3);
  memcpy(&id[3], &magic, 2);
  memcpy(&id[5], &mac.addr_bytes[3], 3);
  our_port_id->port_number = htons(port_id);  // now allways
  // ptp_print_port_id(port_id, our_port_id);

  rte_memcpy(ip, mt_sip_addr(impl, port), MTL_IP_ADDR_LEN);

  ptp->impl = impl;
  ptp->port = port;
  ptp->port_id = port_id;
  ptp->mbuf_pool = mt_get_tx_mempool(impl, port);
  ptp->master_initialized = false;
  ptp->t3_sequence_id = 0x1000 * port;
  ptp->coefficient = 1.0;
  ptp->kp = impl->user_para.kp < 1e-15 ? MT_PTP_DEFAULT_KP : impl->user_para.kp;
  ptp->ki = impl->user_para.ki < 1e-15 ? MT_PTP_DEFAULT_KI : impl->user_para.ki;
  ptp->use_pi = (impl->user_para.flags & MTL_FLAG_PTP_PI);
  if (ptp->use_pi)
    info("%s(%d), use pi controller, kp %e, ki %e\n", __func__, port, ptp->kp, ptp->ki);

  struct mtl_init_params* p = mt_get_user_params(impl);
  if (p->flags & MTL_FLAG_PTP_UNICAST_ADDR) {
    ptp->master_addr_mode = MT_PTP_UNICAST_ADDR;
    info("%s(%d), MT_PTP_UNICAST_ADDR\n", __func__, port);
  } else {
    ptp->master_addr_mode = MT_PTP_MULTICAST_ADDR;
  }

  ptp_stat_clear(ptp);
  ptp_coefficient_result_reset(ptp);

  if (!mt_if_has_ptp(impl, port)) {
    if (mt_has_ebu(impl)) {
      ptp_sync_from_user(impl, ptp);
      rte_eal_alarm_set(MT_PTP_EBU_SYNC_MS * 1000, ptp_sync_from_user_handler, ptp);
    }
    return 0;
  }

  if (mt_no_system_rxq(impl)) {
    warn("%s(%d), disabled as no system rx queues\n", __func__, port);
    return 0;
  }

  inet_pton(AF_INET, "224.0.1.129", ptp->mcast_group_addr);

  /* no need to create rx queue, always use CNI path */

  /* join mcast */
  ret = mt_mcast_join(impl, mt_ip_to_u32(ptp->mcast_group_addr), port);
  if (ret < 0) {
    err("%s(%d), join ptp multicast group fail\n", __func__, port);
    return ret;
  }
  mt_mcast_l2_join(impl, &ptp_l2_multicast_eaddr, port);

  info("%s(%d), sip: %d.%d.%d.%d\n", __func__, port, ip[0], ip[1], ip[2], ip[3]);
  return 0;
}

static int ptp_uinit(struct mtl_main_impl* impl, struct mt_ptp_impl* ptp) {
  enum mtl_port port = ptp->port;

  rte_eal_alarm_cancel(ptp_sync_from_user_handler, ptp);
#if MT_PTP_USE_TX_TIMER
  rte_eal_alarm_cancel(ptp_delay_req_handler, ptp);
#endif
  rte_eal_alarm_cancel(ptp_sync_timeout_handler, ptp);
  rte_eal_alarm_cancel(ptp_monitor_handler, ptp);

  if (!mt_if_has_ptp(impl, port)) return 0;

  mt_mcast_l2_leave(impl, &ptp_l2_multicast_eaddr, port);
  mt_mcast_leave(impl, mt_ip_to_u32(ptp->mcast_group_addr), port);

  info("%s(%d), succ\n", __func__, port);
  return 0;
}

int mt_ptp_parse(struct mt_ptp_impl* ptp, struct mt_ptp_header* hdr, bool vlan,
                 enum mt_ptp_l_mode mode, uint16_t timesync,
                 struct mt_ipv4_udp* ipv4_hdr) {
  enum mtl_port port = ptp->port;

  if (!mt_if_has_ptp(ptp->impl, port)) return 0;

  // dbg("%s(%d), message_type %d\n", __func__, port, hdr->message_type);
  // mt_ptp_print_port_id(port, &hdr->source_port_identity);

  if (hdr->message_type != PTP_ANNOUNCE) {
    if (!ptp->master_initialized) {
      dbg("%s(%d), master not initialized, message_type %d, mode %s\n", __func__, port,
          hdr->message_type, ptp_mode_str(mode));
      return -EINVAL;
    }
    if (!ptp_port_id_equal(&hdr->source_port_identity, &ptp->master_port_id)) {
      dbg("%s(%d), source_port_identity not our master, message_type %d, mode %s\n",
          __func__, port, hdr->message_type, ptp_mode_str(mode));
#ifdef DEBUG
      ptp_print_port_id(port, &hdr->source_port_identity);
#endif
      return -EINVAL;
    }
  }

  switch (hdr->message_type) {
    case PTP_SYNC:
      ptp_parse_sync(ptp, (struct mt_ptp_sync_msg*)hdr, vlan, mode, timesync);
      break;
    case PTP_FOLLOW_UP:
      ptp_parse_follow_up(ptp, (struct mt_ptp_follow_up_msg*)hdr);
      break;
    case PTP_DELAY_RESP:
      ptp_parse_delay_resp(ptp, (struct mt_ptp_delay_resp_msg*)hdr);
      break;
    case PTP_ANNOUNCE:
      ptp_parse_announce(ptp, (struct mt_ptp_announce_msg*)hdr, mode, ipv4_hdr);
      break;
    case PTP_DELAY_REQ:
      break;
    case PTP_PDELAY_REQ:
      break;
    default:
      err("%s(%d), unknown message_type %d\n", __func__, port, hdr->message_type);
      return -EINVAL;
  }

  /* read to clear rx timesync status */
  // struct timespec timestamp;
  // rte_eth_timesync_read_rx_timestamp(ptp->port_id, &timestamp, timesync);
  return 0;
}

static int ptp_stat(void* priv) {
  struct mt_ptp_impl* ptp = priv;
  struct mtl_main_impl* impl = ptp->impl;
  enum mtl_port port = ptp->port;
  char date_time[64];
  struct timespec spec;
  struct tm t;
  uint64_t ns;

  ns = mt_get_ptp_time(ptp->impl, port);
  mt_ns_to_timespec(ns, &spec);
  spec.tv_sec -= ptp->master_utc_offset; /* display with utc offset */
  localtime_r(&spec.tv_sec, &t);
  strftime(date_time, sizeof(date_time), "%Y-%m-%d %H:%M:%S", &t);
  notice("PTP(%d): time %" PRIu64 ", %s\n", port, ns, date_time);

  if (!mt_if_has_ptp(impl, port)) {
    if (mt_has_ebu(impl)) {
      notice("PTP(%d): raw ptp %" PRIu64 "\n", port, ptp_get_raw_time(ptp));
      if (ptp->stat_delta_cnt) {
        notice("PTP(%d): delta avr %" PRId64 ", min %" PRId64 ", max %" PRId64
               ", cnt %d, expect %d\n",
               port, ptp->stat_delta_sum / ptp->stat_delta_cnt, ptp->stat_delta_min,
               ptp->stat_delta_max, ptp->stat_delta_cnt, ptp->expect_result_avg);
      }
      ptp_stat_clear(ptp);
    }
    return 0;
  }

  if (ptp->stat_delta_cnt)
    notice("PTP(%d): delta avg %" PRId64 ", min %" PRId64 ", max %" PRId64 ", cnt %d\n",
           port, ptp->stat_delta_sum / ptp->stat_delta_cnt, ptp->stat_delta_min,
           ptp->stat_delta_max, ptp->stat_delta_cnt);
  else
    notice("PTP(%d): not connected\n", port);
  if (ptp->stat_correct_delta_cnt)
    notice("PTP(%d): correct_delta avg %" PRId64 ", min %" PRId64 ", max %" PRId64
           ", cnt %d\n",
           port, ptp->stat_correct_delta_sum / ptp->stat_correct_delta_cnt,
           ptp->stat_correct_delta_min, ptp->stat_correct_delta_max,
           ptp->stat_correct_delta_cnt);
  if (ptp->stat_path_delay_cnt)
    notice("PTP(%d): path_delay avg %" PRId64 ", min %" PRId64 ", max %" PRId64
           ", cnt %d\n",
           port, ptp->stat_path_delay_sum / ptp->stat_path_delay_cnt,
           ptp->stat_path_delay_min, ptp->stat_path_delay_max, ptp->stat_path_delay_cnt);
  notice("PTP(%d): mode %s, sync cnt %d, expect avg %d@%fs\n", port,
         ptp_mode_str(ptp->t2_mode), ptp->stat_sync_cnt, ptp->expect_result_avg,
         (float)ptp->expect_result_period_ns / NS_PER_S);
  if (ptp->stat_rx_sync_err || ptp->stat_result_err || ptp->stat_tx_sync_err)
    notice("PTP(%d): rx time error %d, tx time error %d, delta result error %d\n", port,
           ptp->stat_rx_sync_err, ptp->stat_tx_sync_err, ptp->stat_result_err);
  if (ptp->stat_sync_timeout_err)
    notice("PTP(%d): sync timeout %d\n", port, ptp->stat_sync_timeout_err);
  ptp_stat_clear(ptp);

  return 0;
}

int mt_ptp_init(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  int socket = mt_socket_id(impl, MTL_PORT_P);
  int ret;

  for (int i = 0; i < num_ports; i++) {
    /* no ptp for kernel based pmd */
    if (mt_pmd_is_kernel(impl, i)) continue;

    struct mt_ptp_impl* ptp = mt_rte_zmalloc_socket(sizeof(*ptp), socket);
    if (!ptp) {
      err("%s(%d), ptp malloc fail\n", __func__, i);
      mt_ptp_uinit(impl);
      return -ENOMEM;
    }

    ret = ptp_init(impl, ptp, i);
    if (ret < 0) {
      err("%s(%d), ptp_init fail %d\n", __func__, i, ret);
      mt_rte_free(ptp);
      mt_ptp_uinit(impl);
      return ret;
    }

    mt_stat_register(impl, ptp_stat, ptp, "ptp");

    /* assign arp instance */
    impl->ptp[i] = ptp;
  }

  return 0;
}

int mt_ptp_uinit(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  struct mt_ptp_impl* ptp;

  for (int i = 0; i < num_ports; i++) {
    ptp = mt_get_ptp(impl, i);
    if (!ptp) continue;

    mt_stat_unregister(impl, ptp_stat, ptp);

    ptp_uinit(impl, ptp);

    /* free the memory */
    mt_rte_free(ptp);
    impl->ptp[i] = NULL;
  }

  return 0;
}

uint64_t mt_get_raw_ptp_time(struct mtl_main_impl* impl, enum mtl_port port) {
  return ptp_get_raw_time(mt_get_ptp(impl, port));
}

uint64_t mt_mbuf_hw_time_stamp(struct mtl_main_impl* impl, struct rte_mbuf* mbuf,
                               enum mtl_port port) {
  struct mt_ptp_impl* ptp = mt_get_ptp(impl, port);
  uint64_t time_stamp =
      *RTE_MBUF_DYNFIELD(mbuf, impl->dynfield_offset, rte_mbuf_timestamp_t*);
  time_stamp += ptp->ptp_delta;
  return ptp_correct_ts(ptp, time_stamp);
}