/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>

#include "../../../lib/src/st2110/st_rx_video_session.c"

/* Inputs are HOST byte order — the bridge htons() them so callers
 * don't have to model the rtp wire layout. */
uint32_t test_seq_id(uint16_t base_seq, uint16_t ext_seq) {
  struct st20_rfc4175_rtp_hdr rtp;
  memset(&rtp, 0, sizeof(rtp));
  rtp.base.seq_number = htons(base_seq);
  rtp.seq_number_ext = htons(ext_seq);
  return rfc4175_rtp_seq_id(&rtp);
}
