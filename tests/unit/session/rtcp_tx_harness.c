/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Harness for the internal RTCP TX NACK parser.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Include the production .c directly so the parser and its static helper
 * (rtcp_tx_retransmit_rtp_packets) become visible in this translation unit.
 * Non-static symbols duplicate those in libmtl; this object's definition
 * preempts the shared library's. Disable USDT to avoid linker references to
 * probe semaphores.
 */
#undef MTL_HAS_USDT
#include "mt_rtcp.c"
#include "session/rtcp_tx_harness.h"

struct ut_rtcp_parse_result ut_rtcp_tx_feed_nack(int ring_size, uint16_t len_field,
                                                 uint32_t fci_count, uint16_t follow,
                                                 size_t recv_len) {
  struct ut_rtcp_parse_result r = {0, 0, 0, 0};

  /* Large zero-filled buffer: an unpatched num_fcis underflow reads up to
   * 65535 * sizeof(fci) ~= 256KB past the header; keep that in-bounds and
   * zero so the (buggy) walk is deterministic and never crashes. */
  const size_t buf_size = 512 * 1024;
  uint8_t* buf = calloc(1, buf_size);
  if (!buf) {
    r.ret = -ENOMEM;
    return r;
  }

  struct mt_rtcp_hdr* rtcp = (struct mt_rtcp_hdr*)buf;
  rtcp->flags = 0x80;
  rtcp->ptype = MT_RTCP_PTYPE_NACK;
  rtcp->len = htons(len_field);
  rtcp->ssrc = 0;
  memcpy(rtcp->name, "IMTL", 4);
  for (uint32_t i = 0; i < fci_count; i++) {
    rtcp->fci[i].start = htons(0);
    rtcp->fci[i].follow = htons(follow);
  }

  /* Hand-built empty ring: read_front() returns -EIO before touching data, so
   * no mempool/EAL is needed and each retransmit attempt fails fast. */
  struct mt_u64_fifo ring;
  memset(&ring, 0, sizeof(ring));
  ring.size = ring_size;

  struct mt_rtcp_tx tx;
  memset(&tx, 0, sizeof(tx));
  tx.active = true;
  tx.mbuf_ring = &ring;
  snprintf(tx.name, sizeof(tx.name), "ut_rtcp");

  /* Suppress the per-attempt "empty ring" err() spam without affecting other
   * suites: raise the global level around the call and restore it after. */
  enum mtl_log_level old_level = mt_get_log_global_level();
  mt_set_log_global_level(MTL_LOG_LEVEL_CRIT);
  r.ret = mt_rtcp_tx_parse_rtcp_packet(&tx, rtcp, recv_len);
  mt_set_log_global_level(old_level);

  r.retransmit_fail = tx.stat_rtp_retransmit_fail;
  r.nack_received = tx.stat_nack_received;
  r.drop_invalid = tx.stat_nack_drop_invalid;

  free(buf);
  return r;
}
