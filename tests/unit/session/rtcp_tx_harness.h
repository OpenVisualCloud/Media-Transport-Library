/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for mt_rtcp_tx_parse_rtcp_packet() bounds unit tests.
 * Wraps the internal RTCP TX parser so C++ tests can feed crafted NACK
 * packets without pulling in internal MTL headers.
 */

#ifndef _UT_RTCP_TX_HARNESS_H_
#define _UT_RTCP_TX_HARNESS_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Observations after feeding one crafted RTCP packet to the TX parser. */
struct ut_rtcp_parse_result {
  int ret;                  /* parser return value (0 accept, <0 reject) */
  uint32_t retransmit_fail; /* stat_rtp_retransmit_fail: proxy for fci-walk count */
  uint32_t nack_received;   /* stat_nack_received */
  uint32_t drop_invalid;    /* stat_nack_drop_invalid */
};

/*
 * Build a NACK packet and feed it to mt_rtcp_tx_parse_rtcp_packet() with an
 * empty ring of capacity ring_size. With an empty ring each retransmit attempt
 * fails fast and adds its (clamped) bulk to stat_rtp_retransmit_fail, so that
 * counter is a deterministic proxy for how many FCIs the parse loop walked.
 *
 *   len_field -> value written to rtcp->len (host order; harness applies htons)
 *   fci_count -> number of mt_rtcp_fci entries actually written (start=0)
 *   follow    -> follow value written into every laid-out fci
 *   recv_len  -> byte length reported to the parser (bytes after the udp header)
 *
 * The packet lives in a large zero-filled buffer, so an unpatched over-read
 * (num_fcis underflow) stays in-bounds and reads zeros — deterministic, no
 * crash, regardless of ASan.
 */
struct ut_rtcp_parse_result ut_rtcp_tx_feed_nack(int ring_size, uint16_t len_field,
                                                 uint32_t fci_count, uint16_t follow,
                                                 size_t recv_len);

#ifdef __cplusplus
}
#endif

#endif /* _UT_RTCP_TX_HARNESS_H_ */
