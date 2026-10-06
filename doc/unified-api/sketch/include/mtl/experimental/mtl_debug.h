/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* Hand-formatted: clang-format stays off so the milestone tags keep their place
   (check.sh lint 5). */
/* clang-format off */
/*
 * mtl_debug.h - test and debug API of the unified MTL API, revision 0.2.
 *
 * Built only with -Denable_debug_api=true; a release library returns -MTL_ENOTSUP from
 * every function here. Not part of any ABI promise. With the null backend (port spec
 * "null:<n>": no NIC, no root, no hugepages; mtl.h) and the test clock (the faults
 * TEST_CLOCK and CLOCK_ADVANCE), every horizon, late-policy and state-machine rule is
 * deterministic in unit tests and bindings: under the test clock the completions that
 * fall due run synchronously inside CLOCK_ADVANCE, on the caller's thread, in media-time
 * order, so the results, events and RX units they produce are ready when it returns.
 */

#ifndef MTL_EXPERIMENTAL_MTL_DEBUG_H
#define MTL_EXPERIMENTAL_MTL_DEBUG_H

#include "mtl.h"

#if defined(__cplusplus)
extern "C" {
#endif

enum mtl_fault {
  MTL_FAULT_LEG_DOWN = 1, /* session: leg goes oper down */
  MTL_FAULT_LEG_UP = 2,
  MTL_FAULT_TX_QUEUE_HANG = 3, /* session: its TX queue stops completing */
  MTL_FAULT_PORT_RESET = 4,    /* port: the VF reset path */
  MTL_FAULT_TIME_STEP = 5,     /* instance: step the time base by step_ns */
  MTL_FAULT_TIME_LOST = 6,     /* instance: time source lost (holdover) */
  MTL_FAULT_MANAGER_LOST = 7,  /* instance: drop the MtlManager connection */
  MTL_FAULT_FORCE_ERROR = 8,   /* session: enter ERROR with `reason` */
  MTL_FAULT_DROP_PKTS = 9,     /* session: drop packets by pattern, per leg */
  MTL_FAULT_TX_MUTATE = 10,    /* session TX: mutate library-built packets */
  MTL_FAULT_DROP_RANDOM = 11,  /* session: drop packets at random, per leg (legacy
                                  rtcp.sim_loss_rate, burst_loss_max) */
  /* instance: the time base becomes a test clock that runs at `rate` from base_tai_ns, or
     only advances by CLOCK_ADVANCE when rate is {0, 0} */
  MTL_FAULT_TEST_CLOCK = 12,
  MTL_FAULT_CLOCK_ADVANCE = 13, /* instance: advance the test clock by step_ns */
  MTL_FAULT_DUMP_STATE = 14,    /* any object: log every field of its state register
                                   at err level; changes nothing (engine.md §1.8) */
};
enum mtl_tx_mutation {
  MTL_TX_MUTATE_NO_MARKER = 1,
  MTL_TX_MUTATE_SEQ_GAP = 2,
  MTL_TX_MUTATE_BAD_PARITY = 3,
  MTL_TX_MUTATE_PACED = 4,
};
struct mtl_fault_params {
  uint32_t struct_size;
  uint32_t leg;
  uint32_t reason;     /* FORCE_ERROR (mtl_reasons.h) */
  uint32_t drop_every; /* DROP_PKTS: drop drop_count packets every drop_every */
  uint32_t drop_count;
  uint32_t drop_offset;
  uint32_t mutation;    /* TX_MUTATE: enum mtl_tx_mutation */
  uint32_t unit_count;  /* TX_MUTATE: units to mutate; 0 = once */
  uint32_t paced_pkts;  /* TX_MUTATE PACED */
  uint32_t paced_gap_ns;
  int64_t step_ns;       /* TIME_STEP, CLOCK_ADVANCE */
  uint32_t unrecoverable; /* PORT_RESET: the device does not come back */
  uint32_t drop_ppm;      /* DROP_RANDOM: drops per million packets; bursts up to drop_count */
  int64_t base_tai_ns;      /* TEST_CLOCK */
  struct mtl_rational rate; /* TEST_CLOCK */
  uint64_t reserved[3];
};
/* Applies the fault once, or until its inverse (LEG_UP). p may be NULL. CP. (MS1) */
MTL_API_CP(1) int mtl_debug_inject(struct mtl_object obj, uint32_t fault,
                                   const struct mtl_fault_params* MTL_NULLABLE p);

MTL_SIZE_CHECK(mtl_fault_params, 104);

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_DEBUG_H */
