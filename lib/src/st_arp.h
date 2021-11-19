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

#ifndef _ST_LIB_ARP_HEAD_H_
#define _ST_LIB_ARP_HEAD_H_

#include "st_main.h"

int st_arp_parse(struct st_main_impl* impl, struct rte_arp_hdr* hdr, enum st_port port);

#ifdef ST_HAS_KNI
int st_arp_socket_get_mac(struct st_main_impl* impl, struct rte_ether_addr* ea,
                          uint8_t dip[ST_IP_ADDR_LEN], const char* ifname);
#else
static inline int st_arp_socket_get_mac(struct st_main_impl* impl,
                                        struct rte_ether_addr* ea,
                                        uint8_t dip[ST_IP_ADDR_LEN], const char* ifname) {
  return -EIO;
}
#endif
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
