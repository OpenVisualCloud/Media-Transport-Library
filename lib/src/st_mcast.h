/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_MCAST_HEAD_H_
#define _ST_LIB_MCAST_HEAD_H_

#include "st_main.h"

#define IGMP_PROTOCOL 0x02

#define IP_IGMP_DSCP_VALUE 0xc0

#define IGMP_REPORT_IP "224.0.0.22"
#define IGMP_QUERY_IP "224.0.0.1"
#define IGMP_JOIN_GROUP_PERIOD_S (10)
#define IGMP_JOIN_GROUP_PERIOD_US (IGMP_JOIN_GROUP_PERIOD_S * US_PER_S)

enum mcast_msg_type {
  MEMBERSHIP_QUERY = 0x11,
  MEMBERSHIP_REPORT_V3 = 0x22,
};

enum mcast_group_record_type {
  MCAST_MODE_IS_INCLUDE = 0x01,
  MCAST_MODE_IS_EXCLUDE = 0x02,
  MCAST_CHANGE_TO_INCLUDE_MODE = 0x03,
  MCAST_CHANGE_TO_EXCLUDE_MODE = 0x04,
  MCAST_ALLOW_NEW_SOURCES = 0x05,
  MCAST_BLOCK_OLD_SOURCES = 0x06
};

struct mcast_group_record {
  uint8_t record_type;
  uint8_t aux_data_len;
  uint16_t num_sources;
  uint32_t multicast_addr;
} __attribute__((__packed__));

/* membership report */
struct mcast_mb_report_v3 {
  uint8_t type;
  uint8_t reserved_1;
  uint16_t checksum;
  uint16_t reserved_2;
  uint16_t num_group_records;
  struct mcast_group_record group_record;
} __attribute__((__packed__)) __rte_aligned(2);

/* membership report without group records */
struct mcast_mb_report_v3_wo_gr {
  uint8_t type;
  uint8_t reserved_1;
  uint16_t checksum;
  uint16_t reserved_2;
  uint16_t num_group_records;
} __attribute__((__packed__)) __rte_aligned(2);

/* membership query */
struct mcast_mb_query_v3 {
  uint8_t type;
  uint8_t max_resp_code;
  uint16_t checksum;
  uint32_t group_addr;
  struct {
    uint8_t qrv : 3;
    uint8_t s : 1;
    uint8_t resv : 4;
  };
  uint8_t qqic;
  uint16_t num_sources;
  uint32_t source_addr;
} __attribute__((__packed__)) __rte_aligned(2);

int st_mcamtl_init(struct mtl_main_impl* impl);
int st_mcast_uinit(struct mtl_main_impl* impl);
int st_mcast_join(struct mtl_main_impl* impl, uint32_t group_addr, enum mtl_port port);
int st_mcast_leave(struct mtl_main_impl* impl, uint32_t group_addr, enum mtl_port port);
int st_mcast_restore(struct mtl_main_impl* impl, enum mtl_port port);
int st_mcast_l2_join(struct mtl_main_impl* impl, struct rte_ether_addr* addr,
                     enum mtl_port port);
int st_mcast_l2_leave(struct mtl_main_impl* impl, struct rte_ether_addr* addr,
                      enum mtl_port port);

static inline void st_mcast_ip_to_mac(uint8_t* mcast_ip4_addr,
                                      struct rte_ether_addr* mcast_mac) {
  /* Fixed multicast area */
  mcast_mac->addr_bytes[0] = 0x01;
  mcast_mac->addr_bytes[1] = 0x00;
  mcast_mac->addr_bytes[2] = 0x5e;
  /* Coming from multicast ip */
  mcast_mac->addr_bytes[3] = mcast_ip4_addr[1] & 0x7f;
  mcast_mac->addr_bytes[4] = mcast_ip4_addr[2];
  mcast_mac->addr_bytes[5] = mcast_ip4_addr[3];
}

#endif
