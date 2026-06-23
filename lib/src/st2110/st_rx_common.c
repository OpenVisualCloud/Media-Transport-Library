/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "st_rx_common.h"

#include <stdio.h>

void st_rx_backpressure_arm(uint32_t* consecutive, char* suffix_out, size_t cap) {
  (*consecutive)++;
  if (suffix_out && cap) {
    if (*consecutive >= 3) {
      snprintf(suffix_out, cap, " (sustained %ux)", *consecutive);
    } else {
      suffix_out[0] = '\0';
    }
  }
}

void st_rx_backpressure_reset(uint32_t* consecutive) {
  *consecutive = 0;
}
