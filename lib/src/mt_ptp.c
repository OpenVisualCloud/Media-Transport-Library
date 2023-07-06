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
#define MT_PTP_USE_TX_TIMER (0)
#define MT_PTP_CHECK_TX_TIME_STAMP (1)
#define MT_PTP_CHECK_RX_TIME_STAMP (1)
#define MT_PTP_PRINT_ERR_RESULT (0)

#define MT_PTP_EBU_SYNC_MS (10)

#define MT_PTP_DEFAULT_KP 5e-10 /* to be tuned */
#define MT_PTP_DEFAULT_KI 1e-10 /* to be tuned */

#define KP 0.7
#define KI 0.3

static char* ptp_mode_strs[MT_PTP_MAX_MODE] = {
    "l2",
    "l4",
};

enum servo_state {
	UNLOCKED,
	JUMP,
	LOCKED,
};

enum controller_mode {
	NONE,
	PI,
	ALL
} mode;

static inline char* ptp_mode_str(enum mt_ptp_l_mode mode) { return ptp_mode_strs[mode]; }

static inline uint64_t ptp_net_tmstamp_to_ns(struct mt_ptp_tmstamp* ts) {
  uint64_t sec = (uint64_t)ntohl(ts->sec_lsb) + ((uint64_t)ntohs(ts->sec_msb) << 32);
  return (sec * NS_PER_S) + ntohl(ts->ns);
}

static inline void ptp_timesync_lock(struct mt_ptp_impl* ptp) { /* todo */
}

static inline void ptp_timesync_unlock(struct mt_ptp_impl* ptp) { /* todo */
}

static inline double pi_sample(struct mt_pi_servo *s, double offset, double local_ts,
	  enum servo_state *state)
{
	double ppb = 0.0;

	switch (s->count) {
	case 0:
		s->offset[0] = offset;
		s->local[0] = local_ts;
		*state = UNLOCKED;
		s->count = 1;
		break;
	case 1:
		s->offset[1] = offset;
		s->local[1] = local_ts;
		*state = UNLOCKED;
		s->count = 2;
		break;
	case 2:
		s->drift += (s->offset[1] - s->offset[0]) /
			(s->local[1] - s->local[0]);
		*state = UNLOCKED;
		s->count = 3;
		break;
	case 3:
		*state = JUMP;
		s->count = 4;
		break;
	case 4:
		s->drift += KI * offset;
		ppb = KP * offset + s->drift;
		*state = LOCKED;
		break;
	}

	return ppb;
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
    {0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E}};

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

void ptp_adj_system_clock_time(struct mt_ptp_impl* ptp, int64_t delta) {
  struct timex adjtime;
  int sign = 1;
  
  if (delta < 0) {
    sign = -1;
    delta *= -1;
  }

  memset(&adjtime, 0, sizeof(adjtime));
  adjtime.modes = ADJ_SETOFFSET | ADJ_NANO;
  adjtime.time.tv_sec = sign * (delta / NS_PER_S);
  adjtime.time.tv_usec = sign * (delta % NS_PER_S);
  if (adjtime.time.tv_usec < 0) {
    adjtime.time.tv_sec -= 1;
    adjtime.time.tv_usec += 1000000000;
  }
  
  clock_adjtime(CLOCK_REALTIME, &adjtime);
}

void ptp_adj_system_clock_freq(struct mt_ptp_impl* ptp, double freq) {
  struct timex adjfreq;
  memset(&adjfreq, 0, sizeof(adjfreq));

  if (ptp->phc2sys.realtime_nominal_tick) {
    adjfreq.modes |= ADJ_TICK;
    adjfreq.tick =
        round(freq / 1e3 / ptp->phc2sys.realtime_hz) + ptp->phc2sys.realtime_nominal_tick;
    freq -=
        1e3 * ptp->phc2sys.realtime_hz * (adjfreq.tick - ptp->phc2sys.realtime_nominal_tick);
  }

  adjfreq.modes |= ADJ_FREQUENCY;
  adjfreq.freq = (long)(freq * 65.536);
  clock_adjtime(CLOCK_REALTIME, &adjfreq);
}

static void phc2sys_adjust(struct mt_ptp_impl* ptp) {
  enum servo_state state = UNLOCKED;
  double ppb;
  struct timespec ts1_sys, ts2_sys, ts_phc;
  uint64_t t_phc, t1_sys, t2_sys, t_sys, shortest_delay, delay;
  int64_t offset;
  int ret;

  ptp_timesync_lock(ptp);
  shortest_delay = UINT64_MAX;
  offset = 0;
  t_sys = 0;
  for (uint8_t i = 0; i < 10; i++) {
    clock_gettime(CLOCK_REALTIME, &ts1_sys);
    ret = rte_eth_timesync_read_time(ptp->port_id, &ts_phc);
    clock_gettime(CLOCK_REALTIME, &ts2_sys);
    if (ret >= 0) {
      t1_sys = mt_timespec_to_ns(&ts1_sys);
      ;
      t2_sys = mt_timespec_to_ns(&ts2_sys);
      t_phc = mt_timespec_to_ns(&ts_phc);

      delay = t2_sys - t1_sys;
      if (shortest_delay > delay) {
        t_sys = (t1_sys + t2_sys) / 2;
        offset = t_sys - t_phc;
        shortest_delay = delay;
      }
    }
  }
  ptp_timesync_unlock(ptp);
  if (ret >= 0) {
    ppb = pi_sample(&ptp->phc2sys.servo, offset, t_sys, &state);

    switch (state) {
      case UNLOCKED:
        break;
      case JUMP:
        ptp_adj_system_clock_time(ptp, -offset);
        dbg("%s(%d), CLOCK_REALTIME offset %" PRId64 ", delay %" PRIu64 " adjust time.\n",
            __func__, ptp->port_id, offset, shortest_delay);
        break;
      case LOCKED:
        ptp_adj_system_clock_freq(ptp, -ppb);
        dbg("%s(%d), CLOCK_REALTIME offset %" PRId64 ", delay %" PRIu64 " adjust freq.\n",
            __func__, ptp->port_id, offset, shortest_delay);
        break;
    }

    ptp->phc2sys.stat_delta_max = RTE_MAX(abs(offset), ptp->phc2sys.stat_delta_max);
  } else {
    err("%s(%d), PHC time retrieving failed. Err %d\n", __func__, ptp->port_id, ret);
  }
}

static void ptp_adjust(struct mt_ptp_impl* ptp) {
  double adj_freq;
  enum servo_state state = UNLOCKED;

  if (!ptp->path_delay || !ptp->t1 || !ptp->t2) return;

  int64_t delta = ptp->t2 - ptp->t1 - ptp->path_delay;

  adj_freq = pi_sample(&ptp->servo, delta, ptp->t2, &state);

  switch (state) {
    case UNLOCKED:
      break;
    case JUMP:
      ptp_timesync_lock(ptp);
      rte_eth_timesync_adjust_time(ptp->port_id, -1 * delta);
      dbg("%s(%d), master offset: %" PRId64 " path delay: %" PRId64 " adjust time.\n",
          __func__, ptp->port_id, delta, ptp->path_delay);

      ptp_timesync_unlock(ptp);
      break;
    case LOCKED:
      ptp_timesync_lock(ptp);
      rte_eth_timesync_adjust_freq(ptp->port_id, -1 * (long)(adj_freq * 65.536));
      dbg("%s(%d), master offset: %" PRId64 " path delay: %" PRId64 " adjust freq.\n",
          __func__, ptp->port_id, delta, ptp->path_delay);
      ptp_timesync_unlock(ptp);
      break;
  }

  phc2sys_adjust(ptp);

  dbg("%s(%d), delta %" PRId64 ", ptp %" PRIu64 "\n", __func__, ptp->port, delta,
      ptp_get_raw_time(ptp));
  ptp->ptp_delta += delta;

  /* update status */
  ptp->stat_delta_min = RTE_MIN(abs(delta), ptp->stat_delta_min);
  ptp->stat_delta_max = RTE_MAX(abs(delta), ptp->stat_delta_max);
  ptp->stat_delta_cnt++;
  ptp->stat_delta_sum += abs(delta);
}

static void ptp_coefficient_result_reset(struct mt_ptp_impl* ptp) {
  ptp->coefficient_result_sum = 0.0;
  ptp->coefficient_result_min = 2.0;
  ptp->coefficient_result_max = 0.0;
  ptp->coefficient_result_cnt = 0;
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
#if MT_PTP_USE_TX_TIME_STAMP
static void ptp_delay_req_read_tx_time_handler(void* param) {
  struct mt_ptp_impl* ptp = param;
  struct timespec ts;
  uint16_t port_id = ptp->port_id;
  uint64_t tx_ns = 0;
  int ret;
  ptp_timesync_lock(ptp);
  ret = rte_eth_timesync_read_tx_timestamp(port_id, &ts);
  ptp_timesync_unlock(ptp);
  if (ret >= 0) {
    tx_ns = mt_timespec_to_ns(&ts);
    ptp->t3 = tx_ns;
  } else {
    if (!ptp->t4) rte_eal_alarm_set(5, ptp_delay_req_read_tx_time_handler, ptp);
  }
}
#endif

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
  ptp->skip_sync_cnt = 0;
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

static void ptp_delay_req_task(struct mt_ptp_impl* ptp) {
  enum mtl_port port = ptp->port;
  uint16_t port_id = ptp->port_id;
  size_t hdr_offset;
  struct mt_ptp_follow_up_msg* msg;

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
    ipv4_hdr->udp.dgram_cksum = 0;
    ipv4_hdr->ip.time_to_live = 255;
    ipv4_hdr->ip.packet_id = htons(ptp->t3_sequence_id);
    ipv4_hdr->ip.next_proto_id = 17;
    ipv4_hdr->ip.hdr_checksum = 0;
    mt_mbuf_init_ipv4(m);
    hdr->ether_type = htons(RTE_ETHER_TYPE_IPV4);
  } else {
    hdr->ether_type = htons(RTE_ETHER_TYPE_1588);
  }

  msg = rte_pktmbuf_mtod_offset(m, struct mt_ptp_follow_up_msg*, hdr_offset);
  memset(msg, 0x0, sizeof(*msg));
  msg->hdr.message_type = PTP_DELAY_REQ;
  msg->hdr.version = 2;
  msg->hdr.message_length = htons(sizeof(struct mt_ptp_follow_up_msg));
  msg->hdr.domain_number = ptp->t1_domain_number;
  msg->hdr.control_field = CTL_DELAY_REQ;
  msg->hdr.log_message_interval = 0x7f;
  rte_memcpy(&msg->hdr.source_port_identity, &ptp->our_port_id,
             sizeof(struct mt_ptp_port_id));
  ptp->t3_sequence_id++;
  msg->hdr.sequence_id = htons(ptp->t3_sequence_id);

  rte_eth_macaddr_get(port_id, mt_eth_s_addr(hdr));
  ptp_set_master_addr(ptp, mt_eth_d_addr(hdr));
  m->pkt_len = hdr_offset + sizeof(struct mt_ptp_follow_up_msg);
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
  /*
   * The DELAY_REQ packet will be blocked by Qbv scheduler. The Tx timestamp
   * will not be created immediately. So, start an alarm task to poll the Tx
   * timestamp.
   */
  rte_eal_alarm_set(5, ptp_delay_req_read_tx_time_handler, ptp);
#else
  ptp->t3 = ptp_get_raw_time(ptp);
#endif
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
  uint8_t is_valid_ts = 1;

  if (ptp->t2 > 0) {
    /*
     * New sync message arrives before current PTP transaction complete.
     * Maybe the DELAY_REQ is blocked by Qbv scheduler. So, skip
     * one new sync message to wait for current transaction complete.
     */
    ptp->skip_sync_cnt++;
    dbg("%s(%d), t2 already get\n", __func__, ptp->port);
    if (ptp->skip_sync_cnt < 2) return -EIO;
  }

#define RX_MAX_DELTA (100 * 1000 * 1000) /* 100ms */

  ptp->stat_sync_cnt++;

  ptp_timesync_lock(ptp);
  ret = rte_eth_timesync_read_rx_timestamp(port_id, &timestamp, timesync);
  if (ret >= 0) rx_ns = mt_timespec_to_ns(&timestamp);
  ptp_timesync_unlock(ptp);

#if MT_PTP_CHECK_TX_TIME_STAMP
  uint64_t ptp_ns = 0, delta;
  ret = rte_eth_timesync_read_time(port_id, &timestamp);
  if (ret >= 0) ptp_ns = mt_timespec_to_ns(&timestamp);
  delta = abs(ptp_ns - rx_ns);
  if (unlikely(delta > RX_MAX_DELTA)) {
    err("%s(%d), rx_ns %" PRIu64 ", delta %" PRIu64 "\n", __func__, ptp->port, rx_ns,
        delta);
    ptp->stat_rx_sync_err++;
    is_valid_ts = 0;
  }
#endif

#if MT_PTP_USE_TX_TIMER
  rte_eal_alarm_cancel(ptp_delay_req_handler, ptp);
#endif
  ptp_t_result_clear(ptp);
  if (is_valid_ts) {
    ptp->t2 = rx_ns;
    ptp->t2_sequence_id = msg->hdr.sequence_id;
    ptp->t2_vlan = vlan;
    ptp->t2_mode = mode;
    dbg("%s(%d), t2 %" PRIu64 ", seq %d, ptp %" PRIu64 "\n", __func__, ptp->port, ptp->t2,
        ptp->t2_sequence_id, ptp_get_raw_time(ptp));
  }

  return 0;
}

static int ptp_parse_follow_up(struct mt_ptp_impl* ptp,
                               struct mt_ptp_follow_up_msg* msg) {
  if (msg->hdr.sequence_id != ptp->t2_sequence_id) {
    dbg("%s(%d), error sequence id %d %d\n", __func__, ptp->port, msg->hdr.sequence_id,
        ptp->t2_sequence_id);
    return -EINVAL;
  }

  ptp->t1 = ptp_net_tmstamp_to_ns(&msg->precise_origin_timestamp) +
            (be64toh(msg->hdr.correction_field) >> 16);
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
      rte_memcpy(&dst_udp->ip.dst_addr, &ptp->mcast_group_addr[0], MTL_IP_ADDR_LEN);
      dst_udp->ip.total_length =
          htons(sizeof(struct mt_ipv4_udp) + sizeof(struct mt_ptp_follow_up_msg));
      dst_udp->ip.hdr_checksum = 0;
      dst_udp->udp.dgram_len =
          htons(sizeof(struct rte_udp_hdr) + sizeof(struct mt_ptp_follow_up_msg));
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
  if (ptp->t3_sequence_id != ntohs(msg->hdr.sequence_id)) {
    dbg("%s(%d), mismatch sequence_id %d %d\n", __func__, ptp->port, msg->hdr.sequence_id,
        ptp->t3_sequence_id);
    return -EIO;
  }

  ptp->t4 = ptp_net_tmstamp_to_ns(&msg->receive_timestamp) -
            (be64toh(msg->hdr.correction_field) >> 16);
  dbg("%s(%d), t4 %" PRIu64 ", seq %d, ptp %" PRIu64 "\n", __func__, ptp->port, ptp->t4,
      ptp->t3_sequence_id, ptp_get_raw_time(ptp));

  if (ptp->t4 && ptp->t3 && ptp->t2 && ptp->t1) {
    ptp->path_delay = ((ptp->t2 - ptp->t1) + (ptp->t4 - ptp->t3)) / 2;

    ptp_adjust(ptp);
    ptp_t_result_clear(ptp);
  }

  return 0;
}

static void ptp_stat_clear(struct mt_ptp_impl* ptp) {
  ptp->stat_delta_cnt = 0;
  ptp->stat_delta_sum = 0;
  ptp->stat_delta_min = INT_MAX;
  ptp->stat_delta_max = 0;
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
  ptp->phc2sys.stat_delta_max = 0;
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

  memset(&ptp->servo, 0, sizeof(struct mt_pi_servo));
  ptp->path_delay = 0;

  memset(&ptp->phc2sys.servo, 0, sizeof(struct mt_pi_servo));
  ptp->phc2sys.realtime_hz = sysconf(_SC_CLK_TCK);
  ptp->phc2sys.realtime_nominal_tick = 0;
  if (ptp->phc2sys.realtime_hz > 0) {
    ptp->phc2sys.realtime_nominal_tick =
        (1000000 + ptp->phc2sys.realtime_hz / 2) / ptp->phc2sys.realtime_hz;
  }
  ptp->phc2sys.stat_sync = false;

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
  ptp_t_result_clear(ptp);
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

  /* create rx queue */
  struct mt_rxq_flow flow;
  memset(&flow, 0, sizeof(flow));
  rte_memcpy(flow.dip_addr, ptp->mcast_group_addr, MTL_IP_ADDR_LEN);
  rte_memcpy(flow.sip_addr, mt_sip_addr(impl, port), MTL_IP_ADDR_LEN);
  flow.no_port_flow = true;
  flow.dst_port = MT_PTP_UDP_GEN_PORT;
  ptp->rxq = mt_rxq_get(impl, port, &flow);
  if (ptp->rxq) {
    info("%s(%d), rx queue %d\n", __func__, port, mt_rxq_queue_id(ptp->rxq));
  } else {
    warn("%s(%d), fall back to cni path\n", __func__, port);
  }
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

  if (ptp->rxq) {
    mt_rxq_put(ptp->rxq);
    ptp->rxq = NULL;
  }

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

  if (ptp->stat_delta_cnt) {
    if (ptp->phc2sys.stat_delta_max < 300) {
      ptp->phc2sys.stat_sync = true;
    }
    
    notice("PTP(%d): mode %s, delta avr %" PRId64 ", min %" PRId64 ", max %" PRId64
           ", max sys-off %" PRId64 ", cnt %d.\n",
           port, ptp_mode_str(ptp->t2_mode), ptp->stat_delta_sum / ptp->stat_delta_cnt,
           ptp->stat_delta_min, ptp->stat_delta_max, ptp->phc2sys.stat_delta_max,
           ptp->stat_delta_cnt);
  } else
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