/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#ifndef _MT_LIB_DHCP_HEAD_H_
#define _MT_LIB_DHCP_HEAD_H_

#include "mt_main.h"

enum mt_dhcp_udp_ports {
  MT_DHCP_UDP_SERVER_PORT = 67,
  MT_DHCP_UDP_CLIENT_PORT,
};

/* DHCP header defined in RFC2131 */
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

static inline struct mt_dhcp_impl* mt_get_dhcp(struct mtl_main_impl* impl,
                                               enum mtl_port port) {
  return impl->dhcp[port];
}

int mt_dhcp_init(struct mtl_main_impl* impl);

int mt_dhcp_uinit(struct mtl_main_impl* impl);

int mt_dhcp_parse(struct mtl_main_impl* impl, struct mt_dhcp_hdr* hdr,
                  enum mtl_port port);

uint8_t* mt_dhcp_get_ip(struct mtl_main_impl* impl, enum mtl_port port);

uint8_t* mt_dhcp_get_netmask(struct mtl_main_impl* impl, enum mtl_port port);

uint8_t* mt_dhcp_get_gateway(struct mtl_main_impl* impl, enum mtl_port port);

#endif