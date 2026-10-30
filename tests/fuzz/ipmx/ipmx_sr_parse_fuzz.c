/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/**
 * @file ipmx_sr_parse_fuzz.c
 *
 * Sends each input as one RTCP compound packet to ipmx_sr_rx_parse(), the IPMX Sender
 * Report parser of the st20 RX session.
 *
 * The harness copies the input to a heap buffer of exactly its size, so a read past the
 * packet trips ASan. The harness starts no EAL, and sets the MTL log level to CRIT.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#undef MTL_HAS_USDT
#include "ipmx/ipmx_sr_rx.c"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  static bool logging_quiet;
  struct ipmx_rx_sr_info sr;
  uint8_t* buf;

  if (!logging_quiet) {
    mt_set_log_global_level(MTL_LOG_LEVEL_CRIT);
    logging_quiet = true;
  }

  buf = malloc(size ? size : 1);
  if (!buf) return 0;
  if (size) memcpy(buf, data, size);
  ipmx_sr_rx_parse(buf, size, &sr);
  free(buf);
  return 0;
}
