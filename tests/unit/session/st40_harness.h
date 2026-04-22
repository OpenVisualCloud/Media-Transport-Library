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

/* T3: switch session to FRAME_LEVEL transport dispatch. After this returns,
 * subsequent ut40_feed_* calls bypass the rtp ring and go to the assembler
 * stub. Stats (received, port frames, seq tracking) still update. */
void ut40_set_frame_level(ut_test_ctx* ctx);
uint64_t ut40_stat_assemble_dispatched(const ut_test_ctx* ctx);
int ut40_notify_rtp_calls(void);
void ut40_notify_rtp_calls_reset(void);

/* T4: full FRAME_LEVEL setup with allocated slot pool + capturing notify
 * callback. After this, ut40_feed_* drives full assembly; delivered frames
 * are captured and inspectable via ut40_captured_*. Tests must call
 * ut40_release_frame() (or ut40_teardown_frame_pool) to recycle slots. */
void ut40_setup_frame_pool(ut_test_ctx* ctx, uint16_t framebuff_cnt,
                           uint32_t framebuff_size);
void ut40_teardown_frame_pool(ut_test_ctx* ctx);

int ut40_captured_count(void);
void ut40_captured_reset(void);
void* ut40_captured_addr(int i);
uint16_t ut40_captured_meta_num(int i);
uint32_t ut40_captured_udw_fill(int i);
uint32_t ut40_captured_rtp_ts(int i);
bool ut40_captured_marker(int i);
bool ut40_captured_interlaced(int i);
int ut40_captured_meta_did(int frame_i, int meta_i);
int ut40_captured_meta_sdid(int frame_i, int meta_i);
int ut40_captured_meta_udw_size(int frame_i, int meta_i);
uint32_t ut40_captured_meta_udw_offset(int frame_i, int meta_i);
uint8_t ut40_captured_udw_byte(int frame_i, uint32_t off);

/* T5 seq-stat accessors */
uint32_t ut40_captured_seq_lost(int i);
bool ut40_captured_seq_discont(int i);
uint32_t ut40_captured_port_seq_lost(int i, enum mtl_session_port p);
bool ut40_captured_port_seq_discont(int i, enum mtl_session_port p);
uint32_t ut40_captured_port_pkts_recv(int i, enum mtl_session_port p);
uint32_t ut40_captured_pkts_total(int i);
int ut40_captured_status(int i);

void ut40_set_notify_frame_fail_after(int n);
void ut40_release_frame(ut_test_ctx* ctx, void* addr);

uint64_t ut40_stat_anc_frames_ready(const ut_test_ctx* ctx);
uint64_t ut40_stat_anc_frames_dropped(const ut_test_ctx* ctx);
uint64_t ut40_stat_anc_pkt_parse_err(const ut_test_ctx* ctx);

/* Build + feed an mbuf carrying one real ANC packet (parity + checksum
 * applied by harness). Use corrupt_parity_word>=0 to flip parity bits on
 * that UDW word; corrupt_checksum=true to bit-flip the checksum word. */
int ut40_feed_anc_pkt(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                      enum mtl_session_port port, uint8_t did, uint8_t sdid,
                      const uint8_t* udw_bytes, uint16_t udw_size,
                      int corrupt_parity_word, bool corrupt_checksum);

/* Framing-only feed: well-formed empty ANC packet (udw_size=0). */
int ut40_feed_pkt_anc0(ut_test_ctx* ctx, uint16_t seq, uint32_t ts, int marker,
                       enum mtl_session_port port);

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
