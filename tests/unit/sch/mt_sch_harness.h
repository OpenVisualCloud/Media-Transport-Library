/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness API for the scheduler unit tests. Plain C declarations only, so
 * the header is safe to include from C++; mt_main.h cannot be compiled by g++ because
 * st_header.h uses _Atomic.
 */

#ifndef TESTS_UNIT_SCH_MT_SCH_HARNESS_H
#define TESTS_UNIT_SCH_MT_SCH_HARNESS_H

#include <mtl_api.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the shared DPDK EAL (idempotent). Returns 0 on success. */
int ut_sch_init(void);

/* A zeroed main handle: no manager connection and no lcore shm. */
mtl_handle ut_sch_create_main(void);
void ut_sch_destroy_main(mtl_handle mt);

/* RTE_MAX_LCORE, the first lcore id mt_sch_lcore_valid() must reject. */
unsigned int ut_sch_max_lcore(void);

/* MT_MAX_SCH_NUM, the sch[] array size mtl_sch_enable_sleep() must bound its index to. */
int ut_sch_max_sch_num(void);

/* A main handle whose sch[] neighbourhood is poisoned non-zero, so an out-of-range index
 * aliases memory that looks like an active scheduler; sch[MT_MAX_SCH_NUM - 1] is a real
 * zeroed active one. Caller frees with ut_sch_destroy_probe_main. */
mtl_handle ut_sch_create_probe_main(void);
void ut_sch_destroy_probe_main(mtl_handle mt);

/* Starts and stops a tasklet-thread scheduler through sch_start()/sch_stop(); *mode is
 * the get_mempolicy() mode its tasklet start callback saw, or -errno. Returns 0 on
 * success. */
int ut_sch_thread_mode_mempolicy(int* mode);

/* Runs the lcore-mode scheduler entry on a plain pthread with stop already requested;
 * *mode and the return value as ut_sch_thread_mode_mempolicy(), *exit_mode is the
 * thread's mode once the entry returns. */
int ut_sch_lcore_entry_mempolicy(int* mode, int* exit_mode);

/* Creates the lcore lock file through sch_filelock_lock() under umask 0, in a fresh
 * temporary directory; *mode is its permission bits. Returns 0 on success. */
int ut_sch_filelock_create_mode(unsigned int* mode);

#define UT_SCH_SPIN_TASKLET_NAME "ut_spin"

/* A main handle built with mtl_init_params flags whose sch 0 is active in thread mode
 * and holds one tasklet, UT_SCH_SPIN_TASKLET_NAME in slot 1. With work_every 0 the
 * tasklet spins spin_us on each call and returns MTL_TASKLET_ALL_DONE; with N it spins
 * and returns MTL_TASKLET_HAS_PENDING on each Nth call only, and returns
 * MTL_TASKLET_ALL_DONE at once on the others. Slot 0 stays empty. Caller frees with
 * ut_sch_stats_main_destroy. */
mtl_handle ut_sch_stats_main_create(uint64_t flags, unsigned int spin_us,
                                    unsigned int work_every);
void ut_sch_stats_main_destroy(mtl_handle mt);

/* Fills the next count free slots of sch 0 with idle tasklets named ut_idle_<slot>.
 * Returns 0 on success, -ENOSPC past the slot array. */
int ut_sch_stats_add_idle_tasklets(mtl_handle mt, int count);

/* Leaves sch 0 active with its tasklet array gone, the state sch_free() passes through.
 */
void ut_sch_stats_drop_tasklets(mtl_handle mt);

/* sch_start()/sch_stop() of sch 0. Return 0 on success. */
int ut_sch_stats_start(mtl_handle mt);
int ut_sch_stats_stop(mtl_handle mt);

/* Returns 0 once the spin tasklet ran at least calls times, -ETIMEDOUT after 2 s. */
int ut_sch_stats_wait_calls(mtl_handle mt, uint64_t calls);

/* Register a tasklet that always has work through mtl_sch_register_tasklet(), into the
 * first free slot; a second call unregisters the first, so it lands in the same slot. */
int ut_sch_stats_register_busy(mtl_handle mt);

/* The calls of the spin tasklet so far. */
uint64_t ut_sch_stats_calls(mtl_handle mt);

/* The periodic stats dump callback of sch 0, sch_stat(). */
int ut_sch_stats_dump(mtl_handle mt);

/* Sample count of the per-dump loop time of sch 0, the one sch_stat() resets. */
uint64_t ut_sch_stats_dump_loop_cnt(mtl_handle mt);

/* Sample count of the per-dump time of the spin tasklet, the one sch_stat() resets. */
uint64_t ut_sch_stats_dump_tasklet_cnt(mtl_handle mt);

#ifdef __cplusplus
}
#endif

#endif /* TESTS_UNIT_SCH_MT_SCH_HARNESS_H */
