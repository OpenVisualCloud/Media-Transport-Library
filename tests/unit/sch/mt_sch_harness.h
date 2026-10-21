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

/* A main handle without MtlManager whose lcore get/put walk lcores 1..nb_lcores over a
 * private shm table and lock file, and log errors only. One at a time; caller frees with
 * ut_sch_destroy_lcore_main. */
mtl_handle ut_sch_create_lcore_main(unsigned int nb_lcores);
void ut_sch_destroy_lcore_main(mtl_handle mt);
/* The lcores its shm table counts as used, and its own lcore count. */
int ut_sch_lcore_shm_used(mtl_handle mt);
int ut_sch_lcore_cnt(mtl_handle mt);

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

/* mt_sch_quiescent_wait() once a tasklet-thread scheduler ran and stopped; *polls is how
 * often the wait polled. Returns 0 on success. */
int ut_sch_qs_wait_after_stop(int* polls);
/* The same, called by a tasklet of the running scheduler it waits on. */
int ut_sch_qs_wait_on_sch_thread(int* polls);

enum ut_sch_qs_pass_end {
  UT_SCH_QS_END_OF_PASS, /* the pass in flight ends */
  UT_SCH_QS_GO_OFFLINE,  /* the scheduler goes to sleep */
};
/* The caller acts as a scheduler in a pass while another thread waits on it, then ends
 * the pass by end. *returned_in_pass is whether the wait returned before that,
 * *returned_after_end whether it did promptly after. Returns 0 on success. */
int ut_sch_qs_wait_across_pass(enum ut_sch_qs_pass_end end, bool* returned_in_pass,
                               bool* returned_after_end);

/* Runs an idle tasklet-thread scheduler that may sleep until it slept a few times;
 * *online_sleeps is how many of its *sleeps began while it still read as in a pass. */
int ut_sch_qs_sleeps_offline(int* sleeps, int* online_sleeps);

/* Holds a pass of a running tasklet-thread scheduler until a wait on it began; *published
 * is whether the end of that pass satisfied the wait, checked in the next pass. */
int ut_sch_qs_pass_end_publishes(bool* published);

#ifdef __cplusplus
}
#endif

#endif /* TESTS_UNIT_SCH_MT_SCH_HARNESS_H */
