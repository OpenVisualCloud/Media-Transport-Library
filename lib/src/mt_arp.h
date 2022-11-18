/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_ARP_HEAD_H_
#define _MT_LIB_ARP_HEAD_H_

#include "mt_main.h"

int mt_arp_parse(struct mtl_main_impl* impl, struct rte_arp_hdr* hdr, enum mtl_port port);

int mt_arp_cni_get_mac(struct mtl_main_impl* impl, struct rte_ether_addr* ea,
                       enum mtl_port port, uint32_t ip);

int mt_arp_init(struct mtl_main_impl* impl);
int mt_arp_uinit(struct mtl_main_impl* impl);

static inline struct mt_arp_impl* mt_get_arp(struct mtl_main_impl* impl) {
  return &impl->arp;
}

static inline void mt_reset_arp(struct mtl_main_impl* impl, enum mtl_port port) {
  rte_atomic32_set(&impl->arp.mac_ready[port], 0);
  impl->arp.ip[port] = 0;
  memset(impl->arp.ea[port].addr_bytes, 0, sizeof(uint8_t) * RTE_ETHER_ADDR_LEN);
}

#endif
