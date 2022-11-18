/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_SOCKET_HEAD_H_
#define _MT_LIB_SOCKET_HEAD_H_

#include "mt_main.h"

int st_socket_get_if_ip(char* if_name, uint8_t ip[MTL_IP_ADDR_LEN]);

int st_socket_get_if_mac(char* if_name, struct rte_ether_addr* ea);

int st_socket_join_mcast(struct mtl_main_impl* impl, enum mtl_port port, uint32_t group);

int st_socket_drop_mcast(struct mtl_main_impl* impl, enum mtl_port port, uint32_t group);

int st_socket_get_mac(struct mtl_main_impl* impl, char* if_name,
                      uint8_t dip[MTL_IP_ADDR_LEN], struct rte_ether_addr* ea);

int st_socket_add_flow(struct mtl_main_impl* impl, enum mtl_port port, uint16_t queue_id,
                       struct mt_rx_flow* flow);

int st_socket_remove_flow(struct mtl_main_impl* impl, enum mtl_port port,
                          uint16_t queue_id, struct mt_rx_flow* flow);

#endif
