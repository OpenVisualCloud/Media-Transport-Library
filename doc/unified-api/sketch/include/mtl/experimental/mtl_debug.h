/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* A hand-formatted design sketch: formatted when it moves to include/ (milestone M0). */
/* clang-format off */
/*
 * mtl_debug.h - test and debug API of the unified MTL API, revision 0.2.
 *
 * Built only with -Denable_debug_api=true; a release library returns -MTL_ENOTSUP from
 * every function here. Not part of any ABI promise. With the null backend (port spec
 * "null:<n>": no NIC, no root, no hugepages) and the test clock below, every horizon,
 * late-policy and state-machine rule is deterministic in unit tests and bindings.
 */

#ifndef MTL_EXPERIMENTAL_MTL_DEBUG_H
#define MTL_EXPERIMENTAL_MTL_DEBUG_H

#include "mtl.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* The time base runs at `rate` from base_tai_ns, or only advances by
   mtl_test_clock_advance() when rate is {0, 0}. CP. */
MTL_API_CP int mtl_test_clock(mtl_instance_h mt, int64_t base_tai_ns,
                              struct mtl_rational rate);
MTL_API_CP int mtl_test_clock_advance(mtl_instance_h mt, int64_t ns);

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
  int64_t step_ns;       /* TIME_STEP */
  uint32_t unrecoverable; /* PORT_RESET: the device does not come back */
  uint32_t drop_ppm;      /* DROP_RANDOM: drops per million packets; bursts up to drop_count */
  uint64_t reserved[3];
};
/* Applies the fault once, or until its inverse (LEG_UP). p may be NULL. CP. */
MTL_API_CP int mtl_debug_inject(struct mtl_object obj, uint32_t fault,
                                const struct mtl_fault_params* MTL_NULLABLE p);

MTL_SIZE_CHECK(mtl_fault_params, 80);

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_DEBUG_H */
