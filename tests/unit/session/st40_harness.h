/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * C header for ST40 session-layer unit tests.
 * The test context is opaque — all access goes through accessors.
 */

#ifndef _ST40_SESSION_HARNESS_H_
#define _ST40_SESSION_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#include "mtl_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_test_ctx ut_test_ctx;

int ut40_init(void);
ut_test_ctx* ut40_ctx_create(int num_port);
void ut40_ctx_destroy(ut_test_ctx* ctx);
void ut40_drain_ring(void);

int ut40_feed_pkt(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                  enum mtl_session_port port);
void ut40_feed_burst(ut_test_ctx* ctx, uint16_t seq_start, int count, uint32_t ts,
                     int last_marker, enum mtl_session_port port);
int ut40_feed_pkt_pt(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                     enum mtl_session_port port, uint8_t payload_type);
int ut40_feed_pkt_ssrc(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                       enum mtl_session_port port, uint32_t ssrc);
int ut40_feed_pkt_fbits(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                        enum mtl_session_port port, uint8_t f_bits);

/* Designated-initializer-friendly packet description for ut40_feed_spec().
 * Fields are in the order tests overwhelmingly use; defaults (0) match the
 * thin wrappers above so callers only set what they want to override. */
struct ut40_pkt_spec {
  uint16_t seq;
  uint32_t ts;
  int marker;
  enum mtl_session_port port;
  uint8_t payload_type; /* 0 = use session default */
  uint32_t ssrc;        /* 0 = use session default */
  uint8_t f_bits;       /* 0 = progressive */
};

int ut40_feed_spec(ut_test_ctx* ctx, struct ut40_pkt_spec spec);

void ut40_ctx_set_pt(ut_test_ctx* ctx, uint8_t pt);
void ut40_ctx_set_ssrc(ut_test_ctx* ctx, uint32_t ssrc);
void ut40_ctx_set_interlace_auto(ut_test_ctx* ctx, bool enable);

uint64_t ut40_stat_unrecovered(const ut_test_ctx* ctx);
uint64_t ut40_stat_redundant(const ut_test_ctx* ctx);
uint64_t ut40_stat_received(const ut_test_ctx* ctx);
uint64_t ut40_stat_lost_pkts(const ut_test_ctx* ctx);
int ut40_session_seq_id(const ut_test_ctx* ctx);

uint64_t ut40_stat_port_pkts(const ut_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut40_stat_port_bytes(const ut_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut40_stat_port_lost(const ut_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut40_stat_port_frames(const ut_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut40_stat_port_reordered(const ut_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut40_stat_port_duplicates(const ut_test_ctx* ctx, enum mtl_session_port port);
uint64_t ut40_stat_field_bit_mismatch(const ut_test_ctx* ctx);

uint64_t ut40_stat_wrong_pt(const ut_test_ctx* ctx);
uint64_t ut40_stat_wrong_ssrc(const ut_test_ctx* ctx);
uint64_t ut40_stat_wrong_interlace(const ut_test_ctx* ctx);
uint64_t ut40_stat_interlace_first(const ut_test_ctx* ctx);
uint64_t ut40_stat_interlace_second(const ut_test_ctx* ctx);
uint64_t ut40_stat_enqueue_fail(const ut_test_ctx* ctx);
int ut40_frames_received(const ut_test_ctx* ctx);

void ut40_set_skip_drain(bool skip);
void ut40_session_reset(ut_test_ctx* ctx);
int ut40_ring_dequeue_markers(int* out_count, bool* out_has_marker);

#ifdef __cplusplus
}

/* RAII guard: pause ring drain for the lifetime of the guard. Use in tests
 * that need to inspect queued packets directly. Restores the previous state
 * on scope exit (including via gtest assertion failures). */
class ut40_drain_paused {
 public:
  ut40_drain_paused() {
    ut40_set_skip_drain(true);
  }
  ~ut40_drain_paused() {
    ut40_set_skip_drain(false);
  }
  ut40_drain_paused(const ut40_drain_paused&) = delete;
  ut40_drain_paused& operator=(const ut40_drain_paused&) = delete;
};

extern "C" {
#endif

#ifdef __cplusplus
}
#endif

#endif /* _ST40_SESSION_HARNESS_H_ */
