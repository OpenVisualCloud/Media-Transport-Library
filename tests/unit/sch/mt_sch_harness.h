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
#include <stdbool.h>

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

/* Keys the lcore shm table on a private file instead of the host's /dev/null, so a test
 * neither sees nor disturbs the table of running MTL processes. Returns 0 on success. */
int ut_sch_shm_private_begin(void);
/* Detaches whatever the helpers below attached, removes the private table and goes back
 * to the host's key. */
void ut_sch_shm_private_end(void);
/* The byte size of a table with `entries` lcore entries. */
long ut_sch_shm_table_size(unsigned int entries);
/* The byte size of the segment on the private key, or a negative errno. */
long ut_sch_shm_size(void);
/* Creates a segment of `bytes` on the private key, as an older MTL or another program
 * would; with `attached` its creator stays attached, like a running process. Returns 0
 * on success. */
int ut_sch_shm_create(long bytes, bool attached);
/* Marks the entry of `lcore` active for `pid`, as another process's claim. Returns 0, or
 * -ERANGE if the table has no entry for `lcore`. */
int ut_sch_shm_mark_active(unsigned int lcore, int pid);
/* Attaches the way mtl_init() does, as the first user when `first_user`, and stays
 * attached; returns the size of the table it attached, or a negative errno. */
long ut_sch_shm_join(bool first_user);

#ifdef __cplusplus
}
#endif

#endif /* TESTS_UNIT_SCH_MT_SCH_HARNESS_H */
