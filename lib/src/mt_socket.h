/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_SOCKET_HEAD_H_
#define _MT_LIB_SOCKET_HEAD_H_

#include "mt_main.h"

int mt_socket_get_if_ip(const char* if_name, uint8_t ip[MTL_IP_ADDR_LEN],
                        uint8_t netmask[MTL_IP_ADDR_LEN]);

int mt_socket_set_if_ip(const char* if_name, uint8_t ip[MTL_IP_ADDR_LEN],
                        uint8_t netmask[MTL_IP_ADDR_LEN]);

int mt_socket_get_if_gateway(const char* if_name, uint8_t gateway[MTL_IP_ADDR_LEN]);

int mt_socket_get_if_mac(const char* if_name, struct rte_ether_addr* ea);

int mt_socket_set_if_up(const char* if_name);

int mt_socket_get_numa(const char* if_name);

int mt_socket_join_mcast(struct mtl_main_impl* impl, enum mtl_port port, uint32_t group);

int mt_socket_drop_mcast(struct mtl_main_impl* impl, enum mtl_port port, uint32_t group);

int mt_socket_get_mac(struct mtl_main_impl* impl, const char* if_name,
                      uint8_t dip[MTL_IP_ADDR_LEN], struct rte_ether_addr* ea,
                      int timeout_ms);

int mt_socket_add_flow(struct mtl_main_impl* impl, enum mtl_port port, uint16_t queue_id,
                       struct mt_rxq_flow* flow);

int mt_socket_remove_flow(struct mtl_main_impl* impl, enum mtl_port port, int flow_id);

#endif
