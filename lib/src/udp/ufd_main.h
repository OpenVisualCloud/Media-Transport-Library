/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_UFD_MAIN_H_
#define _MT_LIB_UFD_MAIN_H_

#include "../mt_main.h"

/* include "struct sockaddr_in" define before include mudp_sockfd_api */
// clang-format off
#include <mtl/mudp_sockfd_api.h>
// clang-format on

#define UFD_FD_BASE_DEFAULT (10000)

struct ufd_slot {
  mudp_handle handle;
  int idx;
};

struct ufd_mt_ctx {
  struct mtl_init_params mt_params;
  struct mtl_main_impl* mt;
  int socket;
  int fd_base;

  int slots_nb_max;
  int slot_last_idx;
  struct ufd_slot** slots;
  pthread_mutex_t slots_lock; /* lock slots */
};

#endif
