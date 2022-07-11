/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_mcast.h"

#include "st_dev.h"
#include "st_log.h"
#include "st_util.h"

/* Computing the Internet Checksum based on rfc1071 */
static uint16_t mcast_msg_checksum(enum mcast_msg_type type, void* msg, uint16_t num) {
  size_t size = 0;

  switch (type) {
    case MEMBERSHIP_QUERY:
      size = sizeof(struct mcast_mb_query_v3);
      break;
    case MEMBERSHIP_REPORT_V3:
      size = (sizeof(struct mcast_mb_report_v3_wo_gr) +
              num * sizeof(struct mcast_group_record));
      break;
    default:
      err("%s, wrong mcast msg type: %d\n", __func__, type);
      break;
  }

  return st_rf1071_check_sum(msg, size, true);
}

/* group record shaping, refer to RFC3376 - 4.2.4 */
static inline void mcast_create_group_record(uint32_t group_addr,
                                             enum mcast_group_record_type type,
                                             struct mcast_group_record* group_record) {
  group_record->record_type = type;
  group_record->aux_data_len = 0;
  group_record->num_sources = 0;  // not specify source
  group_record->multicast_addr = group_addr;
  // group_record.source_addr = 0;
  // group_record.auxiliary_data = 0;
}

/* 224.0.0.22 */
static struct rte_ether_addr const mcast_mac_dst = {{0x01, 0x00, 0x5e, 0x00, 0x00, 0x16}};

/* 224.0.0.1 */
static struct rte_ether_addr const mcast_mac_query = {
    {0x01, 0x00, 0x5e, 0x00, 0x00, 0x01}};

int mcast_membership_general_query(struct st_main_impl* impl, enum st_port port) {
  struct st_mcast_impl* mcast = &impl->mcast;
  struct rte_mbuf* pkt;
  struct rte_ether_hdr* eth_hdr;
  struct rte_ipv4_hdr* ip_hdr;
  struct mcast_mb_query_v3* mb_query;
  size_t hdr_offset = 0;
  size_t mb_query_len = sizeof(struct mcast_mb_query_v3);

  pkt = rte_pktmbuf_alloc(st_get_mempool(impl, port));
  if (!pkt) {
    err("%s, report packet alloc failed\n", __func__);
    return -ENOMEM;
  }

  eth_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ether_hdr*, hdr_offset);
  rte_eth_macaddr_get(st_port_id(impl, port), st_eth_s_addr(eth_hdr));
  rte_ether_addr_copy(&mcast_mac_query, st_eth_d_addr(eth_hdr));
  eth_hdr->ether_type = htons(RTE_ETHER_TYPE_IPV4);
  hdr_offset += sizeof(*eth_hdr);

  ip_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ipv4_hdr*, hdr_offset);
  ip_hdr->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
  ip_hdr->time_to_live = 1;
  ip_hdr->type_of_service = IP_IGMP_DSCP_VALUE;
  ip_hdr->fragment_offset = ST_IP_DONT_FRAGMENT_FLAG;
  ip_hdr->hdr_checksum = 0;
  ip_hdr->total_length = htons(sizeof(struct rte_ipv4_hdr) + mb_query_len);
  ip_hdr->next_proto_id = IGMP_PROTOCOL;
  ip_hdr->src_addr = *(uint32_t*)st_sip_addr(impl, port);
  inet_pton(AF_INET, IGMP_QUERY_IP, &ip_hdr->dst_addr);
  hdr_offset += sizeof(*ip_hdr);

  mb_query = rte_pktmbuf_mtod_offset(pkt, struct mcast_mb_query_v3*, hdr_offset);
  mb_query->type = MEMBERSHIP_QUERY;
  mb_query->max_resp_code = 100;
  mb_query->checksum = 0x00;
  mb_query->resv = 0x00;
  mb_query->s = 0x00;
  mb_query->qrv = 0x00;
  mb_query->qqic = 0x08;
  mb_query->group_addr = 0;
  mb_query->num_sources = 0;
  mb_query->source_addr = 0;

  uint16_t checksum = mcast_msg_checksum(MEMBERSHIP_QUERY, mb_query, 0);
  if (checksum <= 0) {
    err("%s, err checksum %d\n", __func__, checksum);
    return -EIO;
  }
  mb_query->checksum = htons(checksum);

  st_mbuf_init_ipv4(pkt);
  pkt->pkt_len = pkt->l2_len + pkt->l3_len + mb_query_len;
  pkt->data_len = pkt->pkt_len;

  uint16_t tx = rte_eth_tx_burst(st_port_id(impl, port), mcast->tx_q_id[port], &pkt, 1);
  if (tx < 1) {
    err("%s, send pkt fail\n", __func__);
    rte_pktmbuf_free(pkt);
    return -EIO;
  }

  return 0;
}

/* membership report shaping, refer to RFC3376 - 4.2 */
static int mcast_membership_report(struct st_main_impl* impl,
                                   enum mcast_group_record_type type, enum st_port port) {
  struct st_mcast_impl* mcast = &impl->mcast;
  uint16_t group_num = mcast->group_num[port];
  struct rte_mbuf* pkt;
  struct rte_ether_hdr* eth_hdr;
  struct rte_ipv4_hdr* ip_hdr;
  struct mcast_mb_report_v3_wo_gr* mb_report;
  struct mcast_group_record* group_records;
  size_t hdr_offset = 0;
  size_t mb_report_len = sizeof(struct mcast_mb_report_v3_wo_gr) +
                         group_num * sizeof(struct mcast_group_record);

  if (group_num <= 0) {
    dbg("%s(%d), no group to join\n", __func__, port);
    return 0;
  }

  dbg("%s(%d), group_num: %d\n", __func__, port, group_num);

  pkt = rte_pktmbuf_alloc(st_get_mempool(impl, port));
  if (!pkt) {
    err("%s, report packet alloc failed\n", __func__);
    return -ENOMEM;
  }

  eth_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ether_hdr*, hdr_offset);
  rte_eth_macaddr_get(st_port_id(impl, port), st_eth_s_addr(eth_hdr));
  rte_ether_addr_copy(&mcast_mac_dst, st_eth_d_addr(eth_hdr));
  eth_hdr->ether_type = htons(RTE_ETHER_TYPE_IPV4);
  hdr_offset += sizeof(*eth_hdr);

  ip_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ipv4_hdr*, hdr_offset);
  ip_hdr->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
  ip_hdr->time_to_live = 1;
  ip_hdr->type_of_service = IP_IGMP_DSCP_VALUE;
  ip_hdr->fragment_offset = ST_IP_DONT_FRAGMENT_FLAG;
  ip_hdr->hdr_checksum = 0;
  ip_hdr->total_length = htons(sizeof(struct rte_ipv4_hdr) + mb_report_len);
  ip_hdr->next_proto_id = IGMP_PROTOCOL;
  ip_hdr->src_addr = *(uint32_t*)st_sip_addr(impl, port);
  inet_pton(AF_INET, IGMP_REPORT_IP, &ip_hdr->dst_addr);
  hdr_offset += sizeof(*ip_hdr);

  mb_report = rte_pktmbuf_mtod_offset(pkt, struct mcast_mb_report_v3_wo_gr*, hdr_offset);
  mb_report->type = MEMBERSHIP_REPORT_V3;  // type: IGMPv3 membership report
  mb_report->reserved_1 = 0x00;
  mb_report->checksum = 0x00;
  mb_report->reserved_2 = 0x00;
  mb_report->num_group_records = htons(group_num);
  hdr_offset += sizeof(struct mcast_mb_report_v3_wo_gr);
  group_records = rte_pktmbuf_mtod_offset(pkt, struct mcast_group_record*, hdr_offset);
  for (int i = 0; i < group_num; i++) {
    mcast_create_group_record(mcast->group_ip[port][i], type, &group_records[i]);
  }
  uint16_t checksum = mcast_msg_checksum(MEMBERSHIP_REPORT_V3, mb_report, group_num);
  if (checksum <= 0) {
    err("%s, err checksum %d\n", __func__, checksum);
    return -EIO;
  }
  dbg("%s, checksum %d\n", __func__, checksum);
  mb_report->checksum = htons(checksum);

  st_mbuf_init_ipv4(pkt);
  pkt->pkt_len = pkt->l2_len + pkt->l3_len + mb_report_len;
  pkt->data_len = pkt->pkt_len;

#ifdef MCAST_DEBUG
  /* send packet to kni for capturing */
  struct st_kni_impl* kni = &impl->kni;
  struct rte_kni* rkni = kni->rkni[port];
  if (rkni) rte_kni_tx_burst(rkni, (struct rte_mbuf**)&report_pkt, 1);
#endif

  uint16_t tx = rte_eth_tx_burst(st_port_id(impl, port), mcast->tx_q_id[port], &pkt, 1);
  if (tx < 1) {
    err("%s, send pkt fail\n", __func__);
    rte_pktmbuf_free(pkt);
    return -EIO;
  }

  return 0;
}

static void mcast_membership_report_cb(void* param) {
  struct st_main_impl* impl = (struct st_main_impl*)param;
  int num_ports = st_num_ports(impl);
  int ret;

  for (int port = 0; port < num_ports; port++) {
    ret = mcast_membership_report(impl, MCAST_MODE_IS_EXCLUDE, port);
    if (ret < 0) err("%s(%d), mcast_membership_report fail %d\n", __func__, port, ret);
  }

  ret = rte_eal_alarm_set(IGMP_JOIN_GROUP_PERIOD_US, mcast_membership_report_cb, impl);
  if (ret < 0) err("%s, set igmp alarm fail %d\n", __func__, ret);
}

static int mcast_queues_uinit(struct st_main_impl* impl) {
  int num_ports = st_num_ports(impl);
  struct st_mcast_impl* mcast = &impl->mcast;

  for (int i = 0; i < num_ports; i++) {
    if (mcast->tx_q_active[i]) {
      st_dev_free_tx_queue(impl, i, mcast->tx_q_id[i]);
      mcast->tx_q_active[i] = false;
    }
  }

  return 0;
}

static int mcast_queues_init(struct st_main_impl* impl) {
  int num_ports = st_num_ports(impl);
  struct st_mcast_impl* mcast = &impl->mcast;
  int ret;

  for (int i = 0; i < num_ports; i++) {
    ret = st_dev_request_tx_queue(impl, i, &mcast->tx_q_id[i], 0);
    if (ret < 0) {
      err("%s(%d), tx_q create fail\n", __func__, i);
      mcast_queues_uinit(impl);
      return ret;
    }
    mcast->tx_q_active[i] = true;
    info("%s(%d), tx q %d\n", __func__, i, mcast->tx_q_id[i]);
  }

  return 0;
}

static int mcast_addr_pool_extend(struct st_interface* inf) {
  struct rte_ether_addr* mc_list;
  size_t mc_list_size;

  if ((inf->mcast_nb % ST_MCAST_POOL_INC) != 0) {
    inf->mcast_nb++;
    return 0;
  }

  /*
   * [re]allocate a pool with MCAST_POOL_INC more entries.
   * The previous test guarantees that port->mc_addr_nb is a multiple
   * of MCAST_POOL_INC.
   */
  mc_list_size = sizeof(struct rte_ether_addr) * (inf->mcast_nb + ST_MCAST_POOL_INC);
  mc_list = (struct rte_ether_addr*)realloc(inf->mcast_mac_lists, mc_list_size);
  if (mc_list == NULL) {
    return -ENOMEM;
  }

  inf->mcast_mac_lists = mc_list;
  inf->mcast_nb++;
  return 0;
}

static int mcast_addr_pool_append(struct st_interface* inf,
                                  struct rte_ether_addr* mc_addr) {
  if (mcast_addr_pool_extend(inf) != 0) return -1;
  rte_ether_addr_copy(mc_addr, &inf->mcast_mac_lists[inf->mcast_nb - 1]);
  return 0;
}

static void mcast_addr_pool_remove(struct st_interface* inf, uint32_t addr_idx) {
  inf->mcast_nb--;
  if (addr_idx == inf->mcast_nb) {
    /* No need to recompact the set of multicast addressses. */
    if (inf->mcast_nb == 0) {
      /* free the pool of multicast addresses. */
      free(inf->mcast_mac_lists);
      inf->mcast_mac_lists = NULL;
    }
    return;
  }
  memmove(&inf->mcast_mac_lists[addr_idx], &inf->mcast_mac_lists[addr_idx + 1],
          sizeof(struct rte_ether_addr) * (inf->mcast_nb - addr_idx));
}

static int mcast_inf_add_mac(struct st_interface* inf, struct rte_ether_addr* mcast_mac) {
  uint16_t port_id = inf->port_id;
  uint32_t i;

  /*
   * Check that the added multicast MAC address is not already recorded
   * in the pool of multicast addresses.
   */
  for (i = 0; i < inf->mcast_nb; i++) {
    if (rte_is_same_ether_addr(mcast_mac, &inf->mcast_mac_lists[i])) {
      return 0;
    }
  }

  mcast_addr_pool_append(inf, mcast_mac);
  if (ST_PORT_VF == inf->port_type)
    return rte_eth_dev_set_mc_addr_list(port_id, inf->mcast_mac_lists, inf->mcast_nb);
  else
    return rte_eth_dev_mac_addr_add(port_id, mcast_mac, 0);
}

static int mcast_inf_remove_mac(struct st_interface* inf,
                                struct rte_ether_addr* mcast_mac) {
  uint32_t i;
  uint16_t port_id = inf->port_id;

  /*
   * Search the pool of multicast MAC addresses for the removed address.
   */
  for (i = 0; i < inf->mcast_nb; i++) {
    if (rte_is_same_ether_addr(mcast_mac, &inf->mcast_mac_lists[i])) break;
  }
  if (i == inf->mcast_nb) {
    return 0;
  }

  mcast_addr_pool_remove(inf, i);
  if (ST_PORT_VF == inf->port_type)
    return rte_eth_dev_set_mc_addr_list(port_id, inf->mcast_mac_lists, inf->mcast_nb);
  else
    return rte_eth_dev_mac_addr_remove(port_id, mcast_mac);
}

int st_mcast_init(struct st_main_impl* impl) {
  struct st_mcast_impl* mcast = &impl->mcast;
  int ret;

  for (int port = 0; port < ST_PORT_MAX; ++port) {
    st_pthread_mutex_init(&mcast->group_mutex[port], NULL);
  }

  ret = rte_eal_alarm_set(IGMP_JOIN_GROUP_PERIOD_US, mcast_membership_report_cb, impl);
  if (ret < 0) err("%s, set igmp alarm fail %d\n", __func__, ret);

  ret = mcast_queues_init(impl);
  if (ret < 0) return ret;

  info("%s, report every %d seconds\n", __func__, IGMP_JOIN_GROUP_PERIOD_S);
  return 0;
}

int st_mcast_uinit(struct st_main_impl* impl) {
  struct st_mcast_impl* mcast = &impl->mcast;
  int ret;

  mcast_queues_uinit(impl);

  for (int port = 0; port < ST_PORT_MAX; ++port) {
    st_pthread_mutex_destroy(&mcast->group_mutex[port]);
  }

  ret = rte_eal_alarm_cancel(mcast_membership_report_cb, impl);
  if (ret < 0) err("%s, alarm cancel fail %d\n", __func__, ret);

  info("%s, succ\n", __func__);
  return 0;
}

/* add a group address to the group ip list */
int st_mcast_join(struct st_main_impl* impl, uint32_t group_addr, enum st_port port) {
  struct st_mcast_impl* mcast = &impl->mcast;
  struct rte_ether_addr mcast_mac;
  struct st_interface* inf = st_if(impl, port);
  int group_num = mcast->group_num[port];
  uint8_t* ip = (uint8_t*)&group_addr;

  if (group_num >= ST_MCAST_GROUP_MAX) {
    err("%s, reach max multicast group number!\n", __func__);
    return -EIO;
  }

  st_pthread_mutex_lock(&mcast->group_mutex[port]);
  for (int i = 0; i < group_num; ++i) {
    if (mcast->group_ip[port][i] == group_addr) {
      st_pthread_mutex_unlock(&mcast->group_mutex[port]);
      info("%s(%d), group %d.%d.%d.%d already in\n", __func__, port, ip[0], ip[1], ip[2],
           ip[3]);
      return 0;
    }
  }
  mcast->group_ip[port][group_num] = group_addr;
  mcast->group_num[port]++;
  st_pthread_mutex_unlock(&mcast->group_mutex[port]);

  /* add mcast mac to interface */
  st_mcast_ip_to_mac(ip, &mcast_mac);
  mcast_inf_add_mac(inf, &mcast_mac);
  /* report to switch */
  mcast_membership_report(impl, MCAST_MODE_IS_EXCLUDE, port);

  info("%s(%d), succ, group %d.%d.%d.%d\n", __func__, port, ip[0], ip[1], ip[2], ip[3]);
  return 0;
}

/* not implement fast leave report for IGMPv3, just stop sending join report, */
/* after a while the switch will delete the port in the multicast group */
int st_mcast_leave(struct st_main_impl* impl, uint32_t group_addr, enum st_port port) {
  struct st_mcast_impl* mcast = &impl->mcast;
  int group_num = mcast->group_num[port];
  struct st_interface* inf = st_if(impl, port);
  uint8_t* ip = (uint8_t*)&group_addr;
  struct rte_ether_addr mcast_mac;

  st_pthread_mutex_lock(&mcast->group_mutex[port]);
  /* search the group ip list and delete the addr */
  for (int i = 0; i < group_num; ++i) {
    if (mcast->group_ip[port][i] == group_addr) {
      dbg("%s, found group ip in the group list, delete it\n", __func__);
      mcast->group_ip[port][i] = mcast->group_ip[port][group_num - 1];
      mcast->group_num[port]--;
      st_pthread_mutex_unlock(&mcast->group_mutex[port]);
      /* remove mcast mac from interface */
      st_mcast_ip_to_mac(ip, &mcast_mac);
      mcast_inf_remove_mac(inf, &mcast_mac);
      return 0;
    }
  }
  st_pthread_mutex_unlock(&mcast->group_mutex[port]);
  dbg("%s, group ip not found, nothing to delete\n", __func__);
  return 0;
}

int st_mcast_restore(struct st_main_impl* impl, enum st_port port) {
  struct st_interface* inf = st_if(impl, port);
  uint16_t port_id = inf->port_id;

  if (ST_PORT_VF == inf->port_type) {
    rte_eth_dev_set_mc_addr_list(port_id, inf->mcast_mac_lists, inf->mcast_nb);
  } else {
    for (uint32_t i = 0; i < inf->mcast_nb; i++)
      rte_eth_dev_mac_addr_add(port_id, &inf->mcast_mac_lists[i], 0);
  }
  mcast_membership_report(impl, MCAST_MODE_IS_EXCLUDE, port);
  return 0;
}

int st_mcast_l2_join(struct st_main_impl* impl, struct rte_ether_addr* addr,
                     enum st_port port) {
  struct st_interface* inf = st_if(impl, port);
  return mcast_inf_add_mac(inf, addr);
}

int st_mcast_l2_leave(struct st_main_impl* impl, struct rte_ether_addr* addr,
                      enum st_port port) {
  struct st_interface* inf = st_if(impl, port);
  return mcast_inf_remove_mac(inf, addr);
}
