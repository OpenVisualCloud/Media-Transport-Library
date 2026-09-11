/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "core/mt_numa_harness.h"

#include "mt_main.h"

int ut_bind_process_numa(int socket_id) {
  return mt_bind_process_numa(socket_id);
}
