/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _MT_LIB_UFD_MAIN_H_
#define _MT_LIB_UFD_MAIN_H_

#include "../../mt_main.h"

/* include "struct sockaddr_in" define before include mudp_sockfd_api */
// clang-format off
#ifdef WINDOWSENV
#include "deprecated/mudp_win.h"
#endif
#include "deprecated/mudp_sockfd_api.h"
#include "deprecated/mudp_sockfd_internal.h"
// clang-format on

struct ufd_slot {
  mudp_handle handle;
  int idx;
  void* opaque;
};

struct ufd_mt_ctx {
  struct mufd_init_params init_params;
  struct mtl_main_impl* mt;
  bool alloc_with_rte;
  pid_t parent_pid;

  int slot_last_idx;
  struct ufd_slot** slots;    /* slots with init_params.slots_nb_max */
  pthread_mutex_t slots_lock; /* lock slots */
};

#endif
