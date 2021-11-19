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

#include "st_ptp.h"

#include "st_cni.h"
#include "st_dev.h"
//#define DEBUG
#include "st_log.h"
#include "st_mcast.h"
#include "st_sch.h"

static char* ptp_mode_strs[ST_PTP_MAX_MODE] = {
    "l2",
    "l4",
};

static inline char* ptp_mode_str(enum st_ptp_l_mode mode) { return ptp_mode_strs[mode]; }

static inline uint64_t ptp_net_tmstamp_to_ns(struct st_ptp_tmstamp* ts) {
  uint64_t sec = (uint64_t)ntohl(ts->sec_lsb) + ((uint64_t)ntohs(ts->sec_msb) << 32);
  return (sec * NS_PER_S) + ntohl(ts->ns);
}

static inline void ptp_timesync_lock(struct st_ptp_impl* ptp) { /* todo */
}

static inline void ptp_timesync_unlock(struct st_ptp_impl* ptp) { /* todo */
}

static inline int ptp_get_time_spec(struct st_ptp_impl* ptp, struct timespec* spec) {
  enum st_port port = ptp->port;
  uint16_t port_id = ptp->port_id;
  int ret;

  ptp_timesync_lock(ptp);
  ret = rte_eth_timesync_read_time(port_id, spec);
  ptp_timesync_unlock(ptp);

  if (ret < 0) err("%s(%d), err %d\n", __func__, port, ret);
  return ret;
}

static inline uint64_t ptp_get_raw_time(struct st_ptp_impl* ptp) {
  struct timespec spec;

  ptp_get_time_spec(ptp, &spec);
  return st_timespec_to_ns(&spec);
}

static uint64_t ptp_get_time(struct st_ptp_impl* ptp) {
  struct timespec spec;

  ptp_get_time_spec(ptp, &spec);
  spec.tv_sec -= ptp->master_utc_offset;
  return st_timespec_to_ns(&spec);
}

static uint64_t ptp_from_eth(struct st_main_impl* impl, enum st_port port) {
  return ptp_get_time(st_get_ptp(impl, port));
}

static void ptp_print_port_id(enum st_port port, struct st_ptp_port_id* pid) {
  uint8_t* id = &pid->clock_identity.id[0];
  info(
      "st_ptp_port_id(%d), port_number: %04x, clk_id: "
      "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
      port, pid->port_number, id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
}

static inline bool ptp_port_id_equal(struct st_ptp_port_id* s, struct st_ptp_port_id* t) {
  if (!memcmp(s, t, sizeof(struct st_ptp_port_id)))
    return true;
  else
    return false;
}

static struct rte_ether_addr ptp_l4_multicast_eaddr = {
    {0x01, 0x00, 0x5e, 0x00, 0x01, 0x81}}; /* 224.0.1.129 */

static struct rte_ether_addr ptp_l2_multicast_eaddr = {
    {0x01, 0x1b, 0x19, 0x00, 0x00, 0x00}};

static inline void ptp_set_master_addr(struct st_ptp_impl* ptp,
                                       struct rte_ether_addr* d_addr) {
  if (ptp->master_addr_mode == ST_PTP_MULTICAST_ADDR) {
    if (ptp->t2_mode == ST_PTP_L4)
      rte_ether_addr_copy(&ptp_l4_multicast_eaddr, d_addr);
    else
      rte_ether_addr_copy(&ptp_l2_multicast_eaddr, d_addr);
  } else {
    rte_ether_addr_copy(&ptp->master_addr, d_addr);
  }
}

static int ptp_parse_result(struct st_ptp_impl* ptp) {
  uint16_t port_id = ptp->port_id;
  int64_t delta = ((int64_t)ptp->t4 - ptp->t3) - ((int64_t)ptp->t2 - ptp->t1);
  uint64_t abs_delta, expect_delta;

  delta /= 2;
  abs_delta = abs(delta);

  if (ptp->delta_result_cnt) {
    expect_delta = ptp->delta_result_sum / ptp->delta_result_cnt * 2;
    expect_delta = RTE_MAX(expect_delta, 100 * 1000); /* min 100us */
    if (abs_delta > expect_delta) {
      dbg("%s(%d), error abs_delta %" PRIu64 "\n", __func__, ptp->port, abs_delta);
      ptp->delta_result_err++;
      ptp->stat_result_err++;
      if (ptp->delta_result_err > 5) {
        dbg("%s(%d), reset the delta result as too many errors\n", __func__, ptp->port);
        ptp->delta_result_err = 0;
        ptp->delta_result_cnt = 0;
        ptp->delta_result_sum = 0;
      }
      return -EIO;
    }
  }
  ptp->delta_result_err = 0;

  ptp_timesync_lock(ptp);
  rte_eth_timesync_adjust_time(port_id, delta);
  ptp_timesync_unlock(ptp);
  dbg("%s(%d), delta %" PRId64 ", ptp %" PRIu64 "\n", __func__, ptp->port, delta,
      ptp_get_raw_time(ptp));
  ptp->ptp_delta += delta;

  if (1 == ptp->delta_result_cnt) ptp->delta_result_sum = abs_delta;
  ptp->delta_result_sum += abs_delta;

  ptp->delta_result_cnt++;
  /* update status */
  ptp->stat_delta_min = RTE_MIN(delta, ptp->stat_delta_min);
  ptp->stat_delta_max = RTE_MAX(delta, ptp->stat_delta_max);
  ptp->stat_delta_cnt++;
  ptp->stat_delta_sum += abs_delta;
  return 0;
}

static void ptp_delay_req_handler(void* param) {
  struct st_ptp_impl* ptp = param;
  enum st_port port = ptp->port;
  uint16_t port_id = ptp->port_id;
  size_t hdr_offset;
  struct st_ptp_sync_msg* msg;

  struct rte_mbuf* m = rte_pktmbuf_alloc(ptp->mbuf_pool);
  if (!m) {
    err("%s(%d), rte_pktmbuf_alloc fail\n", __func__, port);
    return;
  }

  if (ptp->t2_vlan) {
    err("%s(%d), todo for vlan\n", __func__, port);
  } else {
    m->ol_flags |= PKT_TX_IEEE1588_TMST;
  }

  struct rte_ether_hdr* hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr*);
  hdr_offset = sizeof(struct rte_ether_hdr);

  if (ptp->t2_mode == ST_PTP_L4) {
    struct st_ptp_ipv4_udp* ipv4_hdr =
        rte_pktmbuf_mtod_offset(m, struct st_ptp_ipv4_udp*, hdr_offset);
    hdr_offset += sizeof(struct st_ptp_ipv4_udp);
    rte_memcpy(ipv4_hdr, &ptp->dst_udp, sizeof(*ipv4_hdr));
    ipv4_hdr->udp.src_port = htons(ST_PTP_UDP_EVENT_PORT);
    ipv4_hdr->udp.dst_port = ipv4_hdr->udp.src_port;
    ipv4_hdr->ip.time_to_live = 255;
    ipv4_hdr->ip.packet_id = htons(ptp->t3_sequence_id);
    st_mbuf_init_ipv4(m);
    hdr->ether_type = htons(RTE_ETHER_TYPE_IPV4);
  } else {
    hdr->ether_type = htons(RTE_ETHER_TYPE_1588);
  }

  msg = rte_pktmbuf_mtod_offset(m, struct st_ptp_sync_msg*, hdr_offset);
  memset(msg, 0x0, sizeof(*msg));
  msg->hdr.message_type = PTP_DELAY_REQ;
  msg->hdr.version = 2;
  msg->hdr.message_length = htons(sizeof(struct st_ptp_sync_msg));
  msg->hdr.domain_number = ptp->t1_domain_number;
  rte_memcpy(&msg->hdr.source_port_identity, &ptp->our_port_id,
             sizeof(struct st_ptp_port_id));
  ptp->t3_sequence_id++;
  msg->hdr.sequence_id = htons(ptp->t3_sequence_id);

  rte_eth_macaddr_get(port_id, &hdr->s_addr);
  ptp_set_master_addr(ptp, &hdr->d_addr);
  m->pkt_len = hdr_offset + sizeof(struct st_ptp_sync_msg);
  m->data_len = m->pkt_len;

  struct timespec ts;
  ptp_timesync_lock(ptp);
  rte_eth_timesync_read_tx_timestamp(port_id, &ts);
  ptp_timesync_unlock(ptp);
  // st_mbuf_dump(port, 0, "PTP_DELAY_REQ", m);
  uint16_t tx = rte_eth_tx_burst(port_id, ptp->tx_queue_id, &m, 1);
  if (tx < 1) {
    rte_pktmbuf_free(m);
    err("%s(%d), rte_eth_tx_burst fail\n", __func__, port);
    return;
  }

  /* Wait at least 50 us to read TX timestamp. */
  int max_retry = 50;
  int ret;
  while (max_retry > 0) {
    ptp_timesync_lock(ptp);
    ret = rte_eth_timesync_read_tx_timestamp(port_id, &ts);
    ptp_timesync_unlock(ptp);
    if (ret >= 0) break;
    st_delay_us(1);
    max_retry--;
  }
  if (max_retry <= 0) {
    ptp_timesync_lock(ptp);
    rte_eth_timesync_read_time(port_id, &ts);
    ptp_timesync_unlock(ptp);
  }
  ptp->t3 = st_timespec_to_ns(&ts);
  dbg("%s(%d), t3 %" PRIu64 ", seq %d, max_retry %d, ptp %" PRIu64 "\n", __func__, port,
      ptp->t3, ptp->t3_sequence_id, max_retry, ptp_get_raw_time(ptp));

  /* all time get */
  if (ptp->t4 && ptp->t2 && ptp->t1) {
    ptp_parse_result(ptp);
  }
}

static int ptp_parse_sync(struct st_ptp_impl* ptp, struct st_ptp_sync_msg* msg, bool vlan,
                          enum st_ptp_l_mode mode, uint16_t timesync) {
  struct timespec timestamp;
  int ret;
  uint16_t port_id = ptp->port_id;
  uint64_t rx_ns = 0, ptp_ns = 0, delta;
#define RX_MAX_DELTA (1 * 1000 * 1000) /* 1ms */

  ptp_timesync_lock(ptp);
  ret = rte_eth_timesync_read_rx_timestamp(port_id, &timestamp, timesync);
  if (ret >= 0) rx_ns = st_timespec_to_ns(&timestamp);
  ret = rte_eth_timesync_read_time(port_id, &timestamp);
  if (ret >= 0) ptp_ns = st_timespec_to_ns(&timestamp);
  ptp_timesync_unlock(ptp);
  delta = ptp_ns - rx_ns;
  if (unlikely(delta > RX_MAX_DELTA)) {
    dbg("%s(%d), rx_ns %" PRIu64 ", delta %" PRIu64 "\n", __func__, ptp->port, rx_ns,
        delta);
    rx_ns = ptp_ns; /* use ptp instead, timesync unstable */
    ptp->stat_rx_sync_err++;
  }

  rte_eal_alarm_cancel(ptp_delay_req_handler, ptp);
  ptp->t1 = 0;
  ptp->t3 = 0;
  ptp->t4 = 0;
  ptp->t2 = rx_ns;
  ptp->t2_sequence_id = msg->hdr.sequence_id;
  ptp->t2_vlan = vlan;
  ptp->t2_mode = mode;
  dbg("%s(%d), t2 %" PRIu64 ", seq %d, ptp %" PRIu64 "\n", __func__, ptp->port, ptp->t2,
      ptp->t2_sequence_id, ptp_get_raw_time(ptp));
  return 0;
}

static int ptp_parse_follow_up(struct st_ptp_impl* ptp,
                               struct st_ptp_follow_up_msg* msg) {
  enum st_port port = ptp->port;

  if (msg->hdr.sequence_id != ptp->t2_sequence_id) {
    dbg("%s(%d), error sequence id %d %d\n", __func__, port, msg->hdr.sequence_id,
        ptp->t2_sequence_id);
    return -EINVAL;
  }

  ptp->t1 = ptp_net_tmstamp_to_ns(&msg->precise_origin_timestamp);
  ptp->t1_domain_number = msg->hdr.domain_number;
  dbg("%s(%d), t1 %" PRIu64 ", ptp %" PRIu64 "\n", __func__, port, ptp->t1,
      ptp_get_raw_time(ptp));

  rte_eal_alarm_set(ST_PTP_DELAY_REQ_US + (port * ST_PTP_DELAY_STEP_US),
                    ptp_delay_req_handler, ptp);

  return 0;
}

static int ptp_parse_annouce(struct st_ptp_impl* ptp, struct st_ptp_announce_msg* msg,
                             enum st_ptp_l_mode mode, struct st_ptp_ipv4_udp* ipv4_hdr) {
  enum st_port port = ptp->port;

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
    if (mode == ST_PTP_L4) {
      struct st_ptp_ipv4_udp* dst_udp = &ptp->dst_udp;

      rte_memcpy(dst_udp, ipv4_hdr, sizeof(*dst_udp));
      rte_memcpy(&dst_udp->ip.src_addr, &ptp->sip_addr[0], ST_IP_ADDR_LEN);
      dst_udp->ip.total_length =
          htons(sizeof(struct st_ptp_ipv4_udp) + sizeof(struct st_ptp_sync_msg));
      dst_udp->ip.hdr_checksum = 0;
      dst_udp->udp.dgram_len =
          htons(sizeof(struct rte_udp_hdr) + sizeof(struct st_ptp_sync_msg));
    }

    /* point ptp fn to eth if no user assigned ptp source */
    if (st_has_user_ptp(ptp->impl))
      warn("%s(%d), skip as user provide ptp source already\n", __func__, port);
    else
      st_if(ptp->impl, port)->ptp_get_time_fn = ptp_from_eth;
  }

  return 0;
}

static int ptp_parse_delay_resp(struct st_ptp_impl* ptp,
                                struct st_ptp_delay_resp_msg* msg) {
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

static void ptp_stat_clear(struct st_ptp_impl* ptp) {
  ptp->stat_delta_cnt = 0;
  ptp->stat_delta_sum = 0;
  ptp->stat_delta_min = INT_MAX;
  ptp->stat_delta_max = INT_MIN;
  ptp->stat_rx_sync_err = 0;
  ptp->stat_result_err = 0;
}

static int ptp_init(struct st_main_impl* impl, struct st_ptp_impl* ptp,
                    enum st_port port) {
  uint16_t port_id = st_port_id(impl, port);
  struct rte_ether_addr mac;
  int ret;
  uint8_t* ip = &ptp->sip_addr[0];

  if (!st_if_has_timesync(impl, port)) return 0;

  ret = rte_eth_macaddr_get(port_id, &mac);
  if (ret < 0) {
    err("%s(%d), succ rte_eth_macaddr_get fail %d\n", __func__, port, ret);
    return ret;
  }

  uint16_t magic = ST_PTP_CLOCK_IDENTITY_MAGIC;
  struct st_ptp_port_id* our_port_id = &ptp->our_port_id;
  uint8_t* id = &our_port_id->clock_identity.id[0];
  memcpy(&id[0], &mac.addr_bytes[0], 3);
  memcpy(&id[3], &magic, 2);
  memcpy(&id[5], &mac.addr_bytes[3], 3);
  our_port_id->port_number = htons(port_id);  // now allways
  ptp_print_port_id(port_id, our_port_id);

  rte_memcpy(ip, st_sip_addr(impl, port), ST_IP_ADDR_LEN);

  ptp->impl = impl;
  ptp->port = port;
  ptp->port_id = port_id;
  ptp->mbuf_pool = st_get_mempool(impl, port);
  ptp->master_initialized = false;
  ptp->t3_sequence_id = 0x1000 * port;

  /* todo: check why we can't support both mode with same config */
  struct st_init_params* p = st_get_user_params(impl);
  if (p->flags & ST_FLAG_PTP_UNICAST_ADDR) {
    ptp->master_addr_mode = ST_PTP_UNICAST_ADDR;
    info("%s(%d), ST_PTP_UNICAST_ADDR\n", __func__, port);
  } else {
    ptp->master_addr_mode = ST_PTP_MULTICAST_ADDR;
  }

  ptp_stat_clear(ptp);

  /* create tx queue */
  ret = st_dev_request_tx_queue(impl, port, &ptp->tx_queue_id, 0);
  if (ret < 0) {
    err("%s(%d), ptp_tx_q create fail\n", __func__, port);
    return ret;
  }
  ptp->tx_queue_active = true;

  inet_pton(AF_INET, "224.0.1.129", ptp->mcast_group_addr);

  /* create rx queue */
  struct st_dev_flow flow;
  memset(&flow, 0xff, sizeof(flow));
  rte_memcpy(flow.dip_addr, ptp->mcast_group_addr, ST_IP_ADDR_LEN);
  rte_memcpy(flow.sip_addr, st_sip_addr(impl, port), ST_IP_ADDR_LEN);
  flow.port_flow = false;
  flow.src_port = ST_PTP_UDP_GEN_PORT;
  flow.dst_port = ST_PTP_UDP_GEN_PORT;
  ret = st_dev_request_rx_queue(impl, port, &ptp->rx_queue_id, &flow);
  if (ret < 0) {
    err("%s(%d), ptp_rx_q create fail\n", __func__, port);
    return ret;
  }
  ptp->rx_queue_active = true;

  /* join mcast */
  ret = st_mcast_join(impl, *(uint32_t*)ptp->mcast_group_addr, port);
  if (ret < 0) {
    err("%s(%d), join ptp multicast group fail\n", __func__, port);
    return ret;
  }
  st_mcast_l2_join(impl, &ptp_l2_multicast_eaddr, port);

  info("%s(%d), queue %d %d, sip: %d.%d.%d.%d\n", __func__, port, ptp->tx_queue_id,
       ptp->rx_queue_id, ip[0], ip[1], ip[2], ip[3]);
  return 0;
}

static int ptp_uinit(struct st_main_impl* impl, struct st_ptp_impl* ptp) {
  enum st_port port = ptp->port;
  int ret;

  if (!st_if_has_timesync(impl, port)) return 0;

  ret = rte_eal_alarm_cancel(ptp_delay_req_handler, ptp);
  if (ret < 0) err("%s(%d), st_delay_req_handler cancel fail %d\n", __func__, port, ret);

  st_mcast_l2_leave(impl, &ptp_l2_multicast_eaddr, port);
  st_mcast_leave(impl, *(uint32_t*)ptp->mcast_group_addr, port);

  if (ptp->tx_queue_active) {
    st_dev_free_tx_queue(impl, port, ptp->tx_queue_id);
    ptp->tx_queue_active = false;
  }
  if (ptp->rx_queue_active) {
    st_dev_free_rx_queue(impl, port, ptp->rx_queue_id);
    ptp->rx_queue_active = false;
  }

  info("%s(%d), succ\n", __func__, port);
  return 0;
}

int st_ptp_parse(struct st_ptp_impl* ptp, struct st_ptp_header* hdr, bool vlan,
                 enum st_ptp_l_mode mode, uint16_t timesync,
                 struct st_ptp_ipv4_udp* ipv4_hdr) {
  enum st_port port = ptp->port;

  if (!st_if_has_timesync(ptp->impl, port)) return 0;

  // dbg("%s(%d), message_type %d\n", __func__, port, hdr->message_type);
  // st_ptp_print_port_id(port, &hdr->source_port_identity);

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
      ptp_parse_sync(ptp, (struct st_ptp_sync_msg*)hdr, vlan, mode, timesync);
      break;
    case PTP_FOLLOW_UP:
      ptp_parse_follow_up(ptp, (struct st_ptp_follow_up_msg*)hdr);
      break;
    case PTP_DELAY_RESP:
      ptp_parse_delay_resp(ptp, (struct st_ptp_delay_resp_msg*)hdr);
      break;
    case PTP_ANNOUNCE:
      ptp_parse_annouce(ptp, (struct st_ptp_announce_msg*)hdr, mode, ipv4_hdr);
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
  struct timespec timestamp;
  rte_eth_timesync_read_rx_timestamp(ptp->port_id, &timestamp, timesync);
  return 0;
}

void st_ptp_stat(struct st_main_impl* impl) {
  int num_ports = st_num_ports(impl);
  struct st_ptp_impl* ptp;
  char date_time[64];
  struct timespec spec;
  struct tm t;
  uint64_t ns;

  for (int i = 0; i < num_ports; i++) {
    ns = st_get_ptp_time(impl, i);
    st_ns_to_timespec(ns, &spec);
    localtime_r(&spec.tv_sec, &t);
    strftime(date_time, sizeof(date_time), "%Y-%m-%d %H:%M:%S", &t);
    info("PTP(%d), time %" PRIu64 ", %s\n", i, ns, date_time);

    if (!st_if_has_timesync(impl, i)) continue;

    ptp = st_get_ptp(impl, i);
    if (ptp->stat_delta_cnt)
      info("PTP(%d), mode %s, delta: avr %" PRId64 ", min %" PRId64 ", max %" PRId64
           ", cnt %" PRId64 ", avg %" PRId64 "\n",
           i, ptp_mode_str(ptp->t2_mode), ptp->stat_delta_sum / ptp->stat_delta_cnt,
           ptp->stat_delta_min, ptp->stat_delta_max, ptp->stat_delta_cnt,
           ptp->delta_result_cnt ? (ptp->delta_result_sum / ptp->delta_result_cnt) : 0);
    else
      info("PTP(%d): not connected\n", i);
    if (ptp->stat_rx_sync_err || ptp->stat_result_err)
      info("PTP(%d): rx time sync error %" PRId64 ", delta result error %" PRId64 "\n", i,
           ptp->stat_rx_sync_err, ptp->stat_result_err);
    ptp_stat_clear(ptp);
  }
}

int st_ptp_init(struct st_main_impl* impl) {
  int num_ports = st_num_ports(impl);
  int ret;
  struct st_ptp_impl* ptp;

  for (int i = 0; i < num_ports; i++) {
    ptp = st_get_ptp(impl, i);
    ret = ptp_init(impl, ptp, i);
    if (ret < 0) {
      err("%s(%d), ptp_init fail %d\n", __func__, i, ret);
      st_ptp_uinit(impl);
      return ret;
    }
  }

  return 0;
}

int st_ptp_uinit(struct st_main_impl* impl) {
  int num_ports = st_num_ports(impl);
  struct st_ptp_impl* ptp;

  for (int i = 0; i < num_ports; i++) {
    ptp = st_get_ptp(impl, i);
    ptp_uinit(impl, ptp);
  }

  return 0;
}
