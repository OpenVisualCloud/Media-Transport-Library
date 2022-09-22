/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_ARP_HEAD_H_
#define _ST_LIB_ARP_HEAD_H_

#include "st_main.h"

int st_arp_parse(struct st_main_impl* impl, struct rte_arp_hdr* hdr, enum st_port port);

int st_arp_cni_get_mac(struct st_main_impl* impl, struct rte_ether_addr* ea,
                       enum st_port port, uint32_t ip);

int st_arp_init(struct st_main_impl* impl);
int st_arp_uinit(struct st_main_impl* impl);

static inline struct st_arp_impl* st_get_arp(struct st_main_impl* impl) {
  return &impl->arp;
}

static inline void st_reset_arp(struct st_main_impl* impl, enum st_port port) {
  rte_atomic32_set(&impl->arp.mac_ready[port], 0);
  impl->arp.ip[port] = 0;
  memset(impl->arp.ea[port].addr_bytes, 0, sizeof(uint8_t) * RTE_ETHER_ADDR_LEN);
}

#endif
