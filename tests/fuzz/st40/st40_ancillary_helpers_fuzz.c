/* SPDX-License-Identifier: BSD-3-Clause */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "st40_api.h"

#define ST40_HELPER_BUF_SIZE 512

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (!data || !size) return 0;

  uint8_t scratch[ST40_HELPER_BUF_SIZE] = {0};
  size_t copy = size < ST40_HELPER_BUF_SIZE ? size : ST40_HELPER_BUF_SIZE;
  memcpy(scratch, data, copy);

  uint16_t seed = (copy >= 2) ? ((uint16_t)scratch[0] << 8 | scratch[1]) : scratch[0];
  uint16_t udw_val = seed & 0x3FF; /* 10-bit */
  uint32_t max_fields = (uint32_t)((copy * 8) / 10);
  if (!max_fields) max_fields = 1;
  uint32_t idx = scratch[copy - 1] % max_fields;

  st40_set_udw(idx, udw_val, scratch);
  (void)st40_get_udw(idx, scratch);

  uint32_t field_count = scratch[0] % (max_fields + 1);
  (void)st40_calc_checksum(field_count, scratch);

  uint16_t parity = st40_add_parity_bits(seed);
  (void)st40_check_parity_bits(parity);
  (void)st40_check_parity_bits(seed);

  return 0;
}
