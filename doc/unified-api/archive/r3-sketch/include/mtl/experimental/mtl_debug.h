/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * mtl_debug.h - test and debug API of the unified MTL API, experimental revision 0.1
 * (design sketch; C5 §3.3, §8.10, response A12). Since 0.1 throughout.
 *
 * Built only with -Denable_debug_api=true; a release library returns -MTL_ENOTSUP from
 * every function here. Not part of any ABI promise.
 *
 * The third piece of the test substrate, the null backend, needs no API of its own: open
 * an instance with port spec "null:<n>" (mtl_port_spec.name, mtl_instance_open_simple).
 * It completes units at their scheduled time from the instance clock, needs no NIC, root
 * or hugepages, ships in the library as experimental (Phase 1), and together with
 * mtl_time_test_source() makes every horizon, late-policy and state-machine rule
 * deterministic in the U tier and in bindings.
 */

#ifndef MTL_EXPERIMENTAL_MTL_DEBUG_H
#define MTL_EXPERIMENTAL_MTL_DEBUG_H

#include "mtl_unified.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* Test time source: the published time base runs at `rate` from base_tai_ns, and only
   advances through mtl_time_test_advance() when rate is {0, 0}. Every rule relative to
   "now" (horizon, deadlines, late policy, EPOCH_TICK) honours it. */
MTL_API_CP int mtl_time_test_source(mtl_instance_h mt, int64_t base_tai_ns,
                                    struct mtl_rational rate);
MTL_API_CP int mtl_time_test_advance(mtl_instance_h mt, int64_t ns);

/* Faults, injected at an object: mtl_object_instance(), mtl_object_port() or
   mtl_object_session() (mtl_unified.h). The 13 §6 fault matrix rows map to these. */
enum mtl_fault {
  MTL_FAULT_LEG_DOWN = 1, /* session: leg `leg` goes oper DOWN */
  MTL_FAULT_LEG_UP = 2,
  MTL_FAULT_TX_QUEUE_HANG = 3, /* session: its TX queue stops completing */
  MTL_FAULT_PORT_RESET = 4,    /* port: the VF reset path */
  MTL_FAULT_PTP_STEP = 5,      /* instance: step the time base by step_ns */
  MTL_FAULT_PTP_LOST = 6,      /* instance: time source LOST (holdover) */
  MTL_FAULT_MANAGER_LOST = 7,  /* instance: drop the MtlManager connection */
  MTL_FAULT_FORCE_ERROR = 8,   /* session: enter ERROR with `reason` */
  MTL_FAULT_DROP_PKTS = 9,     /* session: drop packets by pattern, per leg */
  MTL_FAULT_TX_MUTATE = 10,    /* session TX: mutate packets (today's ST40 test knobs) */
};
/* MTL_FAULT_TX_MUTATE patterns; today's enum st40_tx_test_pattern,
   include/st40_api.h:90-96. */
enum mtl_tx_mutation {
  MTL_TX_MUTATE_NONE = 0,
  MTL_TX_MUTATE_NO_MARKER = 1,
  MTL_TX_MUTATE_SEQ_GAP = 2,
  MTL_TX_MUTATE_BAD_PARITY = 3,
  MTL_TX_MUTATE_PACED = 4,
};
struct mtl_fault_params {
  uint32_t struct_size;
  uint32_t leg;        /* LEG_*, DROP_PKTS */
  uint32_t reason;     /* FORCE_ERROR: enum mtl_state_reason */
  uint32_t drop_every; /* DROP_PKTS: drop drop_count packets every drop_every */
  uint32_t drop_count;
  uint32_t drop_offset;
  uint32_t mutation;        /* TX_MUTATE: enum mtl_tx_mutation */
  uint32_t frame_count;     /* TX_MUTATE: units to mutate; 0 = once */
  uint32_t paced_pkt_count; /* TX_MUTATE PACED */
  uint32_t paced_gap_ns;
  int64_t step_ns;        /* PTP_STEP */
  uint32_t unrecoverable; /* PORT_RESET: 1 = resources cannot be restored (device gone) */
  uint32_t reserved0;
  uint64_t reserved[3];
};
MTL_API_DP void mtl_fault_params_init(struct mtl_fault_params* p);
/* CP. Applies the fault once, or until its inverse (LEG_UP). p may be NULL. */
MTL_API_CP int mtl_debug_inject(struct mtl_object obj, uint32_t fault,
                                const struct mtl_fault_params* MTL_NULLABLE p);

#if !defined(__cplusplus)
#define MTL_FAULT_PARAMS_INIT(...) MTL_STRUCT_INIT_(mtl_fault_params, __VA_ARGS__)
#endif

MTL_SIZE_CHECK(mtl_fault_params, 80);

#if defined(__cplusplus)
}
#endif

#endif /* MTL_EXPERIMENTAL_MTL_DEBUG_H */
