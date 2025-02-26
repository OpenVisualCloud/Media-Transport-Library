/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#pragma once

#include <gtest/gtest.h>

#include "test_util.h"

/* include "struct sockaddr_in" define before include mudp_sockfd_api */
// clang-format off
#ifdef WINDOWSENV
#include <mtl/mudp_win.h>
#endif
#include <mtl/mudp_sockfd_api.h>
#include <mtl/mudp_sockfd_internal.h>
// clang-format on

struct utest_ctx {
  struct mufd_init_params init_params;
  uint8_t mcast_ip_addr[MTL_IP_ADDR_LEN];
  bool dhcp;
};

struct utest_ctx* utest_get_ctx(void);
