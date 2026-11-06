/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

/**
 * @file ipmx_sr_parse_fuzz.c
 *
 * Sends each input as one RTCP compound packet to ipmx_sr_rx_parse(), the IPMX Sender
 * Report parser of the st20 and st22 RX sessions. The low bit of the first input byte
 * picks the session's video Media Info Block, 0 the st20 0x0001, 1 the st22 0x0003 (with
 * the JPEG XS 0x0008); the rest is the packet.
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
  uint16_t video_mib = IPMX_MEDIA_INFO_VIDEO;
  uint8_t* buf;

  if (!logging_quiet) {
    mt_set_log_global_level(MTL_LOG_LEVEL_CRIT);
    logging_quiet = true;
  }

  if (size) {
    if (data[0] & 1) video_mib = IPMX_MEDIA_INFO_CBR_VIDEO;
    data++;
    size--;
  }
  buf = malloc(size ? size : 1);
  if (!buf) return 0;
  if (size) memcpy(buf, data, size);
  ipmx_sr_rx_parse(buf, size, video_mib, &sr);
  free(buf);
  return 0;
}
