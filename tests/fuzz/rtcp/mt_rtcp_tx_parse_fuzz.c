/* SPDX-License-Identifier: BSD-3-Clause */
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Include the production .c directly so the parser (and its static retransmit
 * helper) are compiled into this translation unit under the fuzz target's
 * sanitizer flags. Disable USDT to avoid linker references to probe semaphores.
 */
#undef MTL_HAS_USDT
#include "mt_rtcp.c"

/*
 * A real UDP datagram is delivered inside an mbuf whose backing store is always
 * at least MTU-sized, so mt_rtcp_tx_parse_rtcp_packet() may read the fixed
 * header fields even when the logical length is short. Model that: keep the
 * physical buffer fixed while letting the fuzzer drive only the reported length
 * and the header/fci bytes. A crafted len that makes the fci walk run past the
 * received data therefore overflows this bounded buffer and trips ASan.
 */
#define RTCP_FUZZ_BUF_SIZE 2048
/* Nonzero ring capacity so the retransmit VLA clamp is exercised; empty (used
 * == 0) so mt_u64_fifo_read_front() bails before any mempool/queue is touched. */
#define RTCP_FUZZ_RING_SIZE 512

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (!data || size == 0) return 0;

  static bool logging_quiet;
  if (!logging_quiet) {
    mt_set_log_global_level(MTL_LOG_LEVEL_CRIT);
    logging_quiet = true;
  }

  size_t copy = size < RTCP_FUZZ_BUF_SIZE ? size : RTCP_FUZZ_BUF_SIZE;
  uint8_t* buf = calloc(1, RTCP_FUZZ_BUF_SIZE);
  if (!buf) return 0;
  memcpy(buf, data, copy);

  struct mt_u64_fifo ring;
  memset(&ring, 0, sizeof(ring));
  ring.size = RTCP_FUZZ_RING_SIZE;

  struct mt_rtcp_tx tx;
  memset(&tx, 0, sizeof(tx));
  tx.active = true;
  tx.mbuf_ring = &ring;
  snprintf(tx.name, sizeof(tx.name), "rtcp_fuzz");

  /* Report only the received bytes as the length, mirroring the caller. */
  mt_rtcp_tx_parse_rtcp_packet(&tx, (struct mt_rtcp_hdr*)buf, copy);

  free(buf);
  return 0;
}
