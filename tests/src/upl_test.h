/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#pragma once

#include <gtest/gtest.h>

#include "test_util.h"

#define UPLT_IP_ADDR_LEN (4)

#define UPLT_PORT_P (0)
#define UPLT_PORT_R (1)

struct uplt_ctx {
  uint8_t sip_addr[2][UPLT_IP_ADDR_LEN];
  uint8_t mcast_ip_addr[UPLT_IP_ADDR_LEN];
};

struct uplt_ctx* uplt_get_ctx(void);

int uplt_socket_port(int domain, int type, int protocol, int port);

static void inline uplt_init_sockaddr(struct sockaddr_in* saddr,
                                      uint8_t ip[UPLT_IP_ADDR_LEN], uint16_t port) {
  memset(saddr, 0, sizeof(*saddr));
  saddr->sin_family = AF_INET;
  memcpy(&saddr->sin_addr.s_addr, ip, UPLT_IP_ADDR_LEN);
  saddr->sin_port = htons(port);
}

static void inline uplt_init_sockaddr_any(struct sockaddr_in* saddr, uint16_t port) {
  memset(saddr, 0, sizeof(*saddr));
  saddr->sin_family = AF_INET;
  saddr->sin_addr.s_addr = INADDR_ANY;
  saddr->sin_port = htons(port);
}
