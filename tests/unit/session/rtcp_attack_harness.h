/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Attack harness for the RTCP TX NACK path. Unlike rtcp_tx_harness.c it keeps
 * a filled retransmit ring of real mbufs, puts the packet in front of a
 * PROT_NONE page, and measures the stack depth of the retransmit call.
 */

#ifndef _UT_RTCP_ATTACK_HARNESS_H_
#define _UT_RTCP_ATTACK_HARNESS_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_rtk_ctx ut_rtk_ctx;

struct ut_rtk_fci {
  uint16_t start;
  uint16_t follow;
};

struct ut_rtk_stats {
  int ret;
  uint32_t nack_received;
  uint32_t succ;
  uint32_t fail;
  uint32_t fail_obsolete;
  uint32_t fail_read;
  uint32_t fail_nobuf;
  uint32_t fail_burst;
  uint32_t sent;          /* mbufs given to the mock burst */
  size_t max_stack_depth; /* bytes between the parse caller and read_front */
};

int ut_rtk_init(void);

/* ring_size: fifo capacity. rfc4175: set the payload format so the retransmit
 * bit is written. pool_n: mbufs of the copy pool (0 = shared big pool). */
ut_rtk_ctx* ut_rtk_create(int ring_size, int rfc4175, unsigned int pool_n);
void ut_rtk_destroy(ut_rtk_ctx* ctx);

/* Buffer n RTP packets with seq first, first+1, ... in the ring. */
int ut_rtk_buffer(ut_rtk_ctx* ctx, uint16_t first, unsigned int n);

/* The burst mock accepts at most this many mbufs per call (-1 = all). */
void ut_rtk_set_burst_limit(ut_rtk_ctx* ctx, int limit);

/* Turn on the RFC4585 nack ssrc check and set the session ssrc it compares to. */
void ut_rtk_enable_ssrc_check(ut_rtk_ctx* ctx, uint32_t session_ssrc);

/* Set the ssrc that ut_rtk_nack writes into the nack. Default is 0. */
void ut_rtk_set_nack_ssrc(ut_rtk_ctx* ctx, uint32_t ssrc);

/* Count of nacks dropped because the ssrc did not match, since create. */
uint32_t ut_rtk_drop_ssrc(ut_rtk_ctx* ctx);

/* Build a NACK with len_field and the FCIs, put its last received byte on the
 * last byte before a PROT_NONE page, and parse it with recv_len. */
struct ut_rtk_stats ut_rtk_nack(ut_rtk_ctx* ctx, uint16_t len_field,
                                const struct ut_rtk_fci* fcis, uint32_t nb_fci,
                                size_t recv_len);

/* Parse recv_len raw bytes placed in front of a PROT_NONE page. */
struct ut_rtk_stats ut_rtk_raw(ut_rtk_ctx* ctx, const uint8_t* bytes, size_t recv_len);

/* seq of the idx-th mbuf given to the burst mock, and its RFC4175 row_length. */
uint16_t ut_rtk_sent_seq(ut_rtk_ctx* ctx, unsigned int idx);
uint16_t ut_rtk_sent_row_length(ut_rtk_ctx* ctx, unsigned int idx);

/* Guard slots after the fifo data are untouched. */
int ut_rtk_fifo_guard_intact(ut_rtk_ctx* ctx);

/* Free mbufs of the copy pool now, and at create time. */
unsigned int ut_rtk_pool_avail(ut_rtk_ctx* ctx);
unsigned int ut_rtk_pool_avail_at_create(ut_rtk_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* _UT_RTCP_ATTACK_HARNESS_H_ */
