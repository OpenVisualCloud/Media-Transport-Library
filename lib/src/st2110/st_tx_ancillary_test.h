/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 *
 * ST40 TX test-mutation hooks.
 *
 * In debug builds (MTL_SIMULATE_PACKET_DROPS defined) the macros below expand to real
 * fault-injection logic (seq-gap, bad-parity, no-marker, paced bursts).
 * In release builds they collapse to no-ops / constant defaults so the hot
 * path carries zero test overhead.
 *
 * The hot-path source (st_tx_ancillary_session.c) includes this file and
 * uses *only* the macro names — no #ifdef/#endif blocks leak into the
 * datapath code.
 */

#ifndef _ST_TX_ANCILLARY_TEST_H
#define _ST_TX_ANCILLARY_TEST_H

#include "st_header.h"

/* ------------------------------------------------------------------ */
#ifdef MTL_SIMULATE_PACKET_DROPS
/* ========================  DEBUG BUILD  =========================== */

/* -- helpers (static inline, visible only inside the TU) ----------- */

static inline bool tx_ancillary_test_frame_active(
    struct st_tx_ancillary_session_impl* s) {
  return s->test.pattern != ST40_TX_TEST_NONE && s->test_frame_active;
}

/* -- hot-path macros ---------------------------------------------- */

/**
 * Override the RTP sequence step for single-port seq-gap injection.
 * @p _step is an lvalue (uint16_t) mutated in place.
 */
#define TX_ANC_TEST_SEQ_STEP(_s, _step)                                          \
  do {                                                                           \
    if (tx_ancillary_test_frame_active(_s) &&                                    \
        (_s)->test.pattern == ST40_TX_TEST_SEQ_GAP && (_s)->ops.num_port <= 1 && \
        !(_s)->test_seq_gap_fired) {                                             \
      (_step) = 2;                                                               \
      (_s)->test_seq_gap_fired = true;                                           \
    }                                                                            \
  } while (0)

/**
 * Return parity-corrupted value when the BAD_PARITY pattern is active,
 * otherwise fall through to st40_add_parity_bits().
 */
#define TX_ANC_TEST_APPLY_PARITY(_s, _val)                         \
  do {                                                             \
    if (tx_ancillary_test_frame_active(_s) &&                      \
        (_s)->test.pattern == ST40_TX_TEST_BAD_PARITY) {           \
      uint16_t _stripped = (_val);                                 \
      return _stripped & 0x3FF; /* strip parity bits to corrupt */ \
    }                                                              \
  } while (0)

/**
 * Plan the next redundant-path seq-gap (port + size).
 * Full implementation lives here to keep the hot-path source clean.
 */
#define TX_ANC_TEST_SEQ_GAP_PLAN(_s)                                        \
  do {                                                                      \
    static const struct {                                                   \
      enum mtl_session_port port;                                           \
      uint16_t size;                                                        \
    } _sched[] = {{MTL_SESSION_PORT_P, 5}, {MTL_SESSION_PORT_R, 4},         \
                  {MTL_SESSION_PORT_P, 4}, {MTL_SESSION_PORT_R, 5},         \
                  {MTL_SESSION_PORT_P, 4}, {MTL_SESSION_PORT_R, 4},         \
                  {MTL_SESSION_PORT_P, 5}, {MTL_SESSION_PORT_R, 5}};        \
    const uint16_t _slen = RTE_DIM(_sched);                                 \
    if (!(tx_ancillary_test_frame_active(_s) &&                             \
          (_s)->test.pattern == ST40_TX_TEST_SEQ_GAP)) {                    \
      (_s)->test_seq_gap_remaining = 0;                                     \
      (_s)->test_seq_gap_size = 0;                                          \
      break;                                                                \
    }                                                                       \
    if ((_s)->test_seq_gap_remaining) break;                                \
    if ((_s)->ops.num_port <= 1) {                                          \
      uint16_t _tp = (_s)->st40_total_pkts > 0 ? (_s)->st40_total_pkts : 1; \
      uint16_t _g = (_tp >= 5) ? 5 : ((_tp >= 4) ? 4 : 2);                  \
      (_s)->test_seq_gap_target_port = MTL_SESSION_PORT_P;                  \
      (_s)->test_seq_gap_size = _g;                                         \
      (_s)->test_seq_gap_remaining = _g;                                    \
      break;                                                                \
    }                                                                       \
    uint16_t _pi = (_s)->test_seq_gap_plan_idx % _slen;                     \
    (_s)->test_seq_gap_plan_idx = (_pi + 1) % _slen;                        \
    (_s)->test_seq_gap_target_port = _sched[_pi].port;                      \
    (_s)->test_seq_gap_size = _sched[_pi].size;                             \
    (_s)->test_seq_gap_remaining = (_s)->test_seq_gap_size;                 \
  } while (0)

/**
 * Reset test state when a frame build is aborted.
 */
#define TX_ANC_TEST_RESET_STATE(_s)   \
  do {                                \
    (_s)->test_frame_active = false;  \
    (_s)->test_seq_gap_fired = false; \
    (_s)->test_seq_gap_remaining = 0; \
    (_s)->test_seq_gap_size = 0;      \
    (_s)->test_seq_gap_plan_idx = 0;  \
  } while (0)

/**
 * Evaluate to true when the NO_MARKER test pattern is active.
 */
#define TX_ANC_TEST_NO_MARKER(_s) \
  (tx_ancillary_test_frame_active(_s) && (_s)->test.pattern == ST40_TX_TEST_NO_MARKER)

/**
 * Clamp @p _idx so it doesn't exceed @p _cnt when test mode forces extra packets.
 */
#define TX_ANC_TEST_CLAMP_ANC_IDX(_s, _idx, _cnt)                                  \
  do {                                                                             \
    if (tx_ancillary_test_frame_active(_s) && (_s)->split_payload && (_cnt) > 0 && \
        (_idx) >= (_cnt))                                                          \
      (_idx) = (_cnt)-1;                                                           \
  } while (0)

/**
 * At the start of a new frame decide whether test mutation is active.
 * Also overrides st40_total_pkts for the PACED pattern.
 */
#define TX_ANC_TEST_ACTIVATE_FRAME(_s)                                       \
  do {                                                                       \
    if ((_s)->test.pattern != ST40_TX_TEST_NONE && (_s)->test_frames_left) { \
      (_s)->test_frame_active = true;                                        \
      (_s)->test_frames_left--;                                              \
      (_s)->test_seq_gap_fired = false;                                      \
    } else {                                                                 \
      (_s)->test_frame_active = false;                                       \
    }                                                                        \
    if ((_s)->test_frame_active && (_s)->test.paced_pkt_count)               \
      (_s)->st40_total_pkts = RTE_MAX(1, (int)(_s)->test.paced_pkt_count);   \
  } while (0)

/**
 * Try to drop @p _pkt for redundant-path seq-gap injection.
 * Evaluates to true (and frees the mbuf) if the packet was dropped.
 */
#define TX_ANC_TEST_DROP_PKT(_s, _s_port, _pkt)                                        \
  (tx_ancillary_test_frame_active(_s) && (_s)->test.pattern == ST40_TX_TEST_SEQ_GAP && \
   (_s)->ops.num_port > 1 && (_s_port) == (_s)->test_seq_gap_target_port &&            \
   (_s)->test_seq_gap_remaining &&                                                     \
   (dbg("%s(%d), drop pkt %u on %s gap=%u/%u frame=%u\n", __func__, (_s)->idx,         \
        st_tx_mbuf_get_idx(_pkt), (_s_port) == MTL_SESSION_PORT_P ? "P" : "R",         \
        (_s)->test_seq_gap_size, (_s)->test_seq_gap_remaining, (_s)->st40_frame_idx),  \
    (_s)->test_seq_gap_remaining--, rte_pktmbuf_free(_pkt), true))

/**
 * Override pacing interval for the PACED test pattern.
 * @p _pkt_time is an lvalue (double) mutated in place.
 */
#define TX_ANC_TEST_PACING_OVERRIDE(_s, _pkt_time)                           \
  do {                                                                       \
    if (tx_ancillary_test_frame_active(_s) &&                                \
        (_s)->test.pattern == ST40_TX_TEST_PACED && (_s)->test.paced_gap_ns) \
      (_pkt_time) = (_s)->test.paced_gap_ns;                                 \
  } while (0)

/**
 * Clear per-frame test flags at the end of a frame.
 */
#define TX_ANC_TEST_FRAME_DONE(_s)    \
  do {                                \
    (_s)->test_frame_active = false;  \
    (_s)->test_seq_gap_fired = false; \
  } while (0)

/**
 * Initialise test fields from ops at session create time.
 */
#define TX_ANC_TEST_INIT(_s, _ops)                                            \
  do {                                                                        \
    (_s)->test = (_ops)->test;                                                \
    if ((_s)->test.pattern != ST40_TX_TEST_NONE && !(_s)->test.frame_count)   \
      (_s)->test.frame_count = 1;                                             \
    if ((_s)->test.pattern == ST40_TX_TEST_SEQ_GAP && (_ops)->num_port > 1 && \
        (_s)->test.frame_count < ST40_SEQ_GAP_SCHEDULE_LEN)                   \
      (_s)->test.frame_count = ST40_SEQ_GAP_SCHEDULE_LEN;                     \
    (_s)->test_frames_left = (_s)->test.frame_count;                          \
    (_s)->test_frame_active = false;                                          \
    (_s)->test_seq_gap_fired = false;                                         \
    (_s)->test_seq_gap_target_port = MTL_SESSION_PORT_P;                      \
    (_s)->test_seq_gap_remaining = 0;                                         \
    (_s)->test_seq_gap_size = 0;                                              \
    (_s)->test_seq_gap_plan_idx = 0;                                          \
    if ((_s)->test.pattern != ST40_TX_TEST_NONE) (_s)->split_payload = true;  \
  } while (0)

/* ------------------------------------------------------------------ */
#else /* !MTL_SIMULATE_PACKET_DROPS */
/* ======================== RELEASE BUILD =========================== */

static inline bool tx_ancillary_test_frame_active(
    struct st_tx_ancillary_session_impl* s __rte_unused) {
  return false;
}

#define TX_ANC_TEST_SEQ_STEP(_s, _step) \
  do {                                  \
  } while (0)
#define TX_ANC_TEST_APPLY_PARITY(_s, _val) \
  do {                                     \
  } while (0)
#define TX_ANC_TEST_SEQ_GAP_PLAN(_s) \
  do {                               \
  } while (0)
#define TX_ANC_TEST_RESET_STATE(_s) \
  do {                              \
  } while (0)
#define TX_ANC_TEST_NO_MARKER(_s) false
#define TX_ANC_TEST_CLAMP_ANC_IDX(_s, _idx, _cnt) \
  do {                                            \
  } while (0)
#define TX_ANC_TEST_ACTIVATE_FRAME(_s) \
  do {                                 \
  } while (0)
#define TX_ANC_TEST_DROP_PKT(_s, _s_port, _pkt) false
#define TX_ANC_TEST_PACING_OVERRIDE(_s, _pkt_time) \
  do {                                             \
  } while (0)
#define TX_ANC_TEST_FRAME_DONE(_s) \
  do {                             \
  } while (0)
#define TX_ANC_TEST_INIT(_s, _ops) \
  do {                             \
  } while (0)

#endif /* MTL_SIMULATE_PACKET_DROPS */
/* ------------------------------------------------------------------ */

#endif /* _ST_TX_ANCILLARY_TEST_H */
