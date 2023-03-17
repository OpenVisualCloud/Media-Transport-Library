/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _MT_LIB_DHCP_HEAD_H_
#define _MT_LIB_DHCP_HEAD_H_

#include "mt_main.h"

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

#define DHCP_OP_BOOTREQUEST 1
#define DHCP_OP_BOOTREPLY 2
#define DHCP_HTYPE_ETHERNET 1
#define DHCP_HLEN_ETHERNET 6
#define DHCP_MAGIC_COOKIE 0x63825363

#define DHCP_OPTION_PAD 0
#define DHCP_OPTION_END 255
#define DHCP_OPTION_SUBNET_MASK 1
#define DHCP_OPTION_ROUTER 3
#define DHCP_OPTION_DNS_SERVER 6
#define DHCP_OPTION_REQUESTED_IP_ADDRESS 50
#define DHCP_OPTION_LEASE_TIME 51
#define DHCP_OPTION_MESSAGE_TYPE 53
#define DHCP_OPTION_SERVER_IDENTIFIER 54
#define DHCP_OPTION_PARAMETER_REQUEST_LIST 55

#define DHCP_MESSAGE_TYPE_DISCOVER 1
#define DHCP_MESSAGE_TYPE_OFFER 2
#define DHCP_MESSAGE_TYPE_REQUEST 3
#define DHCP_MESSAGE_TYPE_ACK 5
#define DHCP_MESSAGE_TYPE_NAK 6
#define DHCP_MESSAGE_TYPE_RELEASE 7

struct mt_dhcp_hdr {
  uint8_t op;
  uint8_t htype;
  uint8_t hlen;
  uint8_t hops;
  uint32_t xid;
  uint16_t secs;
  uint16_t flags;
  uint32_t ciaddr;
  uint32_t yiaddr;
  uint32_t siaddr;
  uint32_t giaddr;
  uint8_t chaddr[16];
  uint8_t sname[64];
  uint8_t file[128];
  uint32_t magic_cookie;
  uint8_t options[0];
} __attribute__((packed));

int mt_dhcp_init(struct mtl_main_impl* impl);

int mt_dhcp_uinit(struct mtl_main_impl* impl);

int mt_dhcp_parse(struct mtl_main_impl* impl, struct mt_dhcp_hdr* hdr,
                  enum mtl_port port);

uint8_t* mt_dhcp_get_ip(struct mtl_main_impl* impl, enum mtl_port port);

uint8_t* mt_dhcp_get_netmask(struct mtl_main_impl* impl, enum mtl_port port);

uint8_t* mt_dhcp_get_gateway(struct mtl_main_impl* impl, enum mtl_port port);

#endif