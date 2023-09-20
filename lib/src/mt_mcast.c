/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_mcast.h"

#include "mt_dev.h"
#include "mt_log.h"
#include "mt_socket.h"
#include "mt_util.h"

#define MT_MCAST_POOL_INC (32)

static inline struct mt_mcast_impl* get_mcast(struct mtl_main_impl* impl,
                                              enum mtl_port port) {
  return impl->mcast[port];
}

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

  return mt_rf1071_check_sum(msg, size, true);
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

int mcast_membership_general_query(struct mtl_main_impl* impl, enum mtl_port port) {
  struct rte_mbuf* pkt;
  struct rte_ether_hdr* eth_hdr;
  struct rte_ipv4_hdr* ip_hdr;
  struct mcast_mb_query_v3* mb_query;
  size_t hdr_offset = 0;
  size_t mb_query_len = sizeof(struct mcast_mb_query_v3);

  pkt = rte_pktmbuf_alloc(mt_get_tx_mempool(impl, port));
  if (!pkt) {
    err("%s, report packet alloc failed\n", __func__);
    return -ENOMEM;
  }

  eth_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ether_hdr*, hdr_offset);
  mt_macaddr_get(impl, port, mt_eth_s_addr(eth_hdr));
  rte_ether_addr_copy(&mcast_mac_query, mt_eth_d_addr(eth_hdr));
  eth_hdr->ether_type = htons(RTE_ETHER_TYPE_IPV4);
  hdr_offset += sizeof(*eth_hdr);

  ip_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ipv4_hdr*, hdr_offset);
  ip_hdr->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
  ip_hdr->time_to_live = 1;
  ip_hdr->type_of_service = IP_IGMP_DSCP_VALUE;
  ip_hdr->fragment_offset = MT_IP_DONT_FRAGMENT_FLAG;
  ip_hdr->hdr_checksum = 0;
  ip_hdr->total_length = htons(sizeof(struct rte_ipv4_hdr) + mb_query_len);
  ip_hdr->next_proto_id = IGMP_PROTOCOL;
  ip_hdr->src_addr = *(uint32_t*)mt_sip_addr(impl, port);
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

  mt_mbuf_init_ipv4(pkt);
  pkt->pkt_len = pkt->l2_len + pkt->l3_len + mb_query_len;
  pkt->data_len = pkt->pkt_len;

  uint16_t tx = mt_dev_tx_sys_queue_burst(impl, port, &pkt, 1);
  if (tx < 1) {
    err("%s, send pkt fail\n", __func__);
    rte_pktmbuf_free(pkt);
    return -EIO;
  }

  return 0;
}

/* membership report shaping, refer to RFC3376 - 4.2 */
static int mcast_membership_report(struct mtl_main_impl* impl,
                                   enum mcast_group_record_type type,
                                   enum mtl_port port) {
  struct mt_mcast_impl* mcast = get_mcast(impl, port);
  uint16_t group_num = mcast->group_num;
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

  pkt = rte_pktmbuf_alloc(mt_get_tx_mempool(impl, port));
  if (!pkt) {
    err("%s, report packet alloc failed\n", __func__);
    return -ENOMEM;
  }

  eth_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ether_hdr*, hdr_offset);
  mt_macaddr_get(impl, port, mt_eth_s_addr(eth_hdr));
  rte_ether_addr_copy(&mcast_mac_dst, mt_eth_d_addr(eth_hdr));
  eth_hdr->ether_type = htons(RTE_ETHER_TYPE_IPV4);
  hdr_offset += sizeof(*eth_hdr);

  ip_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ipv4_hdr*, hdr_offset);
  ip_hdr->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
  ip_hdr->time_to_live = 1;
  ip_hdr->type_of_service = IP_IGMP_DSCP_VALUE;
  ip_hdr->fragment_offset = MT_IP_DONT_FRAGMENT_FLAG;
  ip_hdr->hdr_checksum = 0;
  ip_hdr->total_length = htons(sizeof(struct rte_ipv4_hdr) + mb_report_len);
  ip_hdr->next_proto_id = IGMP_PROTOCOL;
  ip_hdr->src_addr = *(uint32_t*)mt_sip_addr(impl, port);
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
    mcast_create_group_record(mcast->group_ip[i], type, &group_records[i]);
  }
  uint16_t checksum = mcast_msg_checksum(MEMBERSHIP_REPORT_V3, mb_report, group_num);
  if (checksum <= 0) {
    err("%s, err checksum %d\n", __func__, checksum);
    return -EIO;
  }
  dbg("%s, checksum %d\n", __func__, checksum);
  mb_report->checksum = htons(checksum);

  mt_mbuf_init_ipv4(pkt);
  pkt->pkt_len = pkt->l2_len + pkt->l3_len + mb_report_len;
  pkt->data_len = pkt->pkt_len;

#ifdef MCAST_DEBUG
  /* send packet to kernel for capturing */
  if (mt_has_virtio_user(impl, port)) {
    struct mt_interface* inf = mt_if(impl, port);
    rte_eth_tx_burst(inf->virtio_port_id, 0, (struct rte_mbuf**)&report_pkt, 1);
  }
#endif

  uint16_t tx = mt_dev_tx_sys_queue_burst(impl, port, &pkt, 1);
  if (tx < 1) {
    err("%s, send pkt fail\n", __func__);
    rte_pktmbuf_free(pkt);
    return -EIO;
  }

  return 0;
}

static void mcast_membership_report_cb(void* param) {
  struct mtl_main_impl* impl = (struct mtl_main_impl*)param;
  int num_ports = mt_num_ports(impl);
  int ret;

  for (int port = 0; port < num_ports; port++) {
    if (!mt_drv_use_kernel_ctl(impl, port)) {
      ret = mcast_membership_report(impl, MCAST_MODE_IS_EXCLUDE, port);
      if (ret < 0) {
        err("%s(%d), mcast_membership_report fail %d\n", __func__, port, ret);
      }
    }
  }

  ret = rte_eal_alarm_set(IGMP_JOIN_GROUP_PERIOD_US, mcast_membership_report_cb, impl);
  if (ret < 0) err("%s, set igmp alarm fail %d\n", __func__, ret);
}

static int mcast_addr_pool_extend(struct mt_interface* inf) {
  struct rte_ether_addr* mc_list;
  size_t mc_list_size;

  if ((inf->mcast_nb % MT_MCAST_POOL_INC) != 0) {
    inf->mcast_nb++;
    return 0;
  }

  /*
   * [re]allocate a pool with MCAST_POOL_INC more entries.
   * The previous test guarantees that port->mc_addr_nb is a multiple
   * of MCAST_POOL_INC.
   */
  mc_list_size = sizeof(struct rte_ether_addr) * (inf->mcast_nb + MT_MCAST_POOL_INC);
  mc_list = (struct rte_ether_addr*)realloc(inf->mcast_mac_lists, mc_list_size);
  if (mc_list == NULL) {
    return -ENOMEM;
  }

  inf->mcast_mac_lists = mc_list;
  inf->mcast_nb++;
  return 0;
}

static int mcast_addr_pool_append(struct mt_interface* inf,
                                  struct rte_ether_addr* mc_addr) {
  if (mcast_addr_pool_extend(inf) != 0) return -1;
  rte_ether_addr_copy(mc_addr, &inf->mcast_mac_lists[inf->mcast_nb - 1]);
  return 0;
}

static void mcast_addr_pool_remove(struct mt_interface* inf, uint32_t addr_idx) {
  inf->mcast_nb--;
  if (addr_idx == inf->mcast_nb) {
    /* No need to recompose the set of multicast address. */
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

static int mcast_inf_add_mac(struct mt_interface* inf, struct rte_ether_addr* mcast_mac) {
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
  if (inf->drv_info.flags & MT_DRV_F_USE_MC_ADDR_LIST)
    return rte_eth_dev_set_mc_addr_list(port_id, inf->mcast_mac_lists, inf->mcast_nb);
  else
    return rte_eth_dev_mac_addr_add(port_id, mcast_mac, 0);
}

static int mcast_inf_remove_mac(struct mt_interface* inf,
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
  if (inf->drv_info.flags & MT_DRV_F_USE_MC_ADDR_LIST)
    return rte_eth_dev_set_mc_addr_list(port_id, inf->mcast_mac_lists, inf->mcast_nb);
  else
    return rte_eth_dev_mac_addr_remove(port_id, mcast_mac);
}

int mt_mcast_init(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);
  int socket = mt_socket_id(impl, MTL_PORT_P);

  for (int i = 0; i < num_ports; i++) {
    struct mt_mcast_impl* mcast = mt_rte_zmalloc_socket(sizeof(*mcast), socket);
    if (!mcast) {
      err("%s(%d), mcast malloc fail\n", __func__, i);
      mt_mcast_uinit(impl);
      return -ENOMEM;
    }

    mt_pthread_mutex_init(&mcast->group_mutex, NULL);

    /* assign arp instance */
    impl->mcast[i] = mcast;
  }

  int ret =
      rte_eal_alarm_set(IGMP_JOIN_GROUP_PERIOD_US, mcast_membership_report_cb, impl);
  if (ret < 0) err("%s, set igmp alarm fail %d\n", __func__, ret);

  info("%s, report every %d seconds\n", __func__, IGMP_JOIN_GROUP_PERIOD_S);
  return 0;
}

int mt_mcast_uinit(struct mtl_main_impl* impl) {
  int num_ports = mt_num_ports(impl);

  for (int i = 0; i < num_ports; i++) {
    struct mt_mcast_impl* mcast = get_mcast(impl, i);
    if (!mcast) continue;

    mt_pthread_mutex_destroy(&mcast->group_mutex);

    /* free the memory */
    mt_rte_free(mcast);
    impl->mcast[i] = NULL;
  }

  int ret = rte_eal_alarm_cancel(mcast_membership_report_cb, impl);
  if (ret < 0) err("%s, alarm cancel fail %d\n", __func__, ret);

  dbg("%s, succ\n", __func__);
  return 0;
}

/* add a group address to the group ip list */
int mt_mcast_join(struct mtl_main_impl* impl, uint32_t group_addr, enum mtl_port port) {
  struct mt_mcast_impl* mcast = get_mcast(impl, port);
  struct rte_ether_addr mcast_mac;
  struct mt_interface* inf = mt_if(impl, port);
  int group_num = mcast->group_num;
  uint8_t* ip = (uint8_t*)&group_addr;
  int ret;

  if (group_num >= MT_MCAST_GROUP_MAX) {
    err("%s, reach max multicast group number!\n", __func__);
    return -EIO;
  }

  mt_pthread_mutex_lock(&mcast->group_mutex);
  for (int i = 0; i < group_num; ++i) {
    if (mcast->group_ip[i] == group_addr) {
      mcast->group_ref_cnt[i]++;
      mt_pthread_mutex_unlock(&mcast->group_mutex);
      info("%s(%d), group %d.%d.%d.%d ref cnt %u\n", __func__, port, ip[0], ip[1], ip[2],
           ip[3], mcast->group_ref_cnt[i]);
      return 0;
    }
  }
  if (mt_drv_use_kernel_ctl(impl, port)) {
    ret = mt_socket_join_mcast(impl, port, group_addr);
    if (ret < 0) {
      mt_pthread_mutex_unlock(&mcast->group_mutex);
      err("%s(%d), fail(%d) to join socket group %d.%d.%d.%d\n", __func__, port, ret,
          ip[0], ip[1], ip[2], ip[3]);
      return ret;
    }
  }
  mcast->group_ip[group_num] = group_addr;
  mcast->group_ref_cnt[group_num] = 1;
  mcast->group_num++;

  if (!mt_drv_use_kernel_ctl(impl, port)) {
    /* add mcast mac to interface */
    mt_mcast_ip_to_mac(ip, &mcast_mac);
    mcast_inf_add_mac(inf, &mcast_mac);
  }

  mt_pthread_mutex_unlock(&mcast->group_mutex);

  /* report to switch to join group */
  if (!mt_drv_use_kernel_ctl(impl, port)) {
    mcast_membership_report(impl, MCAST_MODE_IS_EXCLUDE, port);
  }

  info("%s(%d), new group %d.%d.%d.%d\n", __func__, port, ip[0], ip[1], ip[2], ip[3]);
  return 0;
}

/* not implement fast leave report for IGMPv3, just stop sending join report, */
/* after a while the switch will delete the port in the multicast group */
int mt_mcast_leave(struct mtl_main_impl* impl, uint32_t group_addr, enum mtl_port port) {
  struct mt_mcast_impl* mcast = get_mcast(impl, port);
  int group_num = mcast->group_num;
  struct mt_interface* inf = mt_if(impl, port);
  uint8_t* ip = (uint8_t*)&group_addr;
  struct rte_ether_addr mcast_mac;

  mt_pthread_mutex_lock(&mcast->group_mutex);
  /* search the group ip list and delete the addr */
  for (int i = 0; i < group_num; ++i) {
    if (mcast->group_ip[i] == group_addr) {
      dbg("%s, found group ip in the group list, delete it\n", __func__);
      mcast->group_ref_cnt[i]--;
      info("%s(%d), group %d.%d.%d.%d ref cnt %u\n", __func__, port, ip[0], ip[1], ip[2],
           ip[3], mcast->group_ref_cnt[i]);
      if (mcast->group_ref_cnt[i]) {
        mt_pthread_mutex_unlock(&mcast->group_mutex);
        return 0;
      }
      mcast->group_ip[i] = mcast->group_ip[group_num - 1];
      mcast->group_num--;
      if (mt_drv_use_kernel_ctl(impl, port)) {
        mt_socket_drop_mcast(impl, port, group_addr);
      } else {
        /* remove mcast mac from interface */
        mt_mcast_ip_to_mac(ip, &mcast_mac);
        mcast_inf_remove_mac(inf, &mcast_mac);
      }
      mt_pthread_mutex_unlock(&mcast->group_mutex);

      return 0;
    }
  }
  mt_pthread_mutex_unlock(&mcast->group_mutex);
  warn("%s, group ip not found, nothing to delete\n", __func__);
  return 0;
}

int mt_mcast_restore(struct mtl_main_impl* impl, enum mtl_port port) {
  struct mt_interface* inf = mt_if(impl, port);
  uint16_t port_id = inf->port_id;

  if (inf->drv_info.flags & MT_DRV_F_USE_MC_ADDR_LIST) {
    rte_eth_dev_set_mc_addr_list(port_id, inf->mcast_mac_lists, inf->mcast_nb);
  } else {
    for (uint32_t i = 0; i < inf->mcast_nb; i++)
      rte_eth_dev_mac_addr_add(port_id, &inf->mcast_mac_lists[i], 0);
  }
  mcast_membership_report(impl, MCAST_MODE_IS_EXCLUDE, port);
  return 0;
}

int mt_mcast_l2_join(struct mtl_main_impl* impl, struct rte_ether_addr* addr,
                     enum mtl_port port) {
  struct mt_interface* inf = mt_if(impl, port);
  return mcast_inf_add_mac(inf, addr);
}

int mt_mcast_l2_leave(struct mtl_main_impl* impl, struct rte_ether_addr* addr,
                      enum mtl_port port) {
  struct mt_interface* inf = mt_if(impl, port);
  return mcast_inf_remove_mac(inf, addr);
}
