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

#define UT_SCH_IDLE_NS (100)
#define UT_SCH_WORK_NS (50000)
#define UT_SCH_LONG_NS (200000)
/* not 0, so a tasklet timed from the loop start, not from its own call, is off by it */
#define UT_SCH_FILLER_NS (7)
/* not 0, so a stats lookup that ignores the scheduler index fails */
#define UT_SCH_STATS_SCH (1)

/* A main handle built with mtl_init_params flags whose sch UT_SCH_STATS_SCH is active
 * with three tasklet slots, the tasklet ut_filler set straight into slot 0, so it runs
 * however mtl_sch_register_tasklet() behaves: on the fake clock each of its calls takes
 * UT_SCH_FILLER_NS and returns MTL_TASKLET_ALL_DONE. Caller frees with
 * ut_sch_stats_main_destroy. */
mtl_handle ut_sch_stats_main_create(uint64_t flags);
void ut_sch_stats_main_destroy(mtl_handle mt);

/* Registers the tasklet ut_tasklet through mtl_sch_register_tasklet(), into slot 1, the
 * first free one, unregistering the one before. On the fake clock its every work_every-th
 * call (none with 0) takes UT_SCH_WORK_NS and its every long_every-th (none with 0)
 * UT_SCH_LONG_NS, both returning MTL_TASKLET_HAS_PENDING; the others take UT_SCH_IDLE_NS
 * and return MTL_TASKLET_ALL_DONE. Returns 0 on success. */
int ut_sch_stats_register(mtl_handle mt, unsigned int work_every,
                          unsigned int long_every);

/* Registers the tasklet ut_idle, which returns MTL_TASKLET_ALL_DONE at once, through
 * mtl_sch_register_tasklet() into the first free slot. Returns 0 on success. */
int ut_sch_stats_register_idle(mtl_handle mt);

/* Runs the loop of sch UT_SCH_STATS_SCH, once per handle, on a thread, on the fake clock,
 * until ut_tasklet has been called calls times. Returns 0 on success, -EIO if ut_tasklet
 * fell short: then ut_filler stops the loop after 4 * calls loops. A loop that calls no
 * tasklet at all never ends. */
int ut_sch_stats_run(mtl_handle mt, uint64_t calls);

/* sch_time_sample_gap(): the loops from one sampled loop to the next, from *state. */
uint32_t ut_sch_time_sample_gap(uint32_t* state);

/* The arguments of sys:tasklet_stat. */
struct ut_sch_tasklet_stat {
  int sch_idx;
  const char* name;
  uint64_t cnt;
  uint64_t sum;
  uint64_t work_cnt;
  uint64_t work_sum;
};

/* The counters of a tasklet slot of sch UT_SCH_STATS_SCH, without sch_idx and name. */
struct ut_sch_tasklet_stat ut_sch_stats_counter(int slot);

/* The calls in the per-dump time histogram of a tasklet slot of sch UT_SCH_STATS_SCH. */
uint64_t ut_sch_stats_hist_cnt(int slot);

/* The per-dump time histogram: its bucket count, the bucket of a call of ns, the lower
 * bound in ns of bucket b, and the q-th percentile in ns of hist[ut_sch_hist_nb()] for
 * calls of min to max ns. */
unsigned int ut_sch_hist_nb(void);
unsigned int ut_sch_hist_bucket(uint64_t ns);
uint64_t ut_sch_hist_bucket_lo(unsigned int b);
double ut_sch_hist_quantile(const uint64_t* hist, uint64_t min, uint64_t max,
                            unsigned int q);

/* The arguments of the last sys:tasklet_stat that the stats dump fired. */
struct ut_sch_tasklet_stat ut_sch_stats_probe(void);

/* Attach (on) or detach sys:tasklet_time_sample, as bpftrace does: through the semaphore
 * of the probe. */
void ut_sch_stats_sample(bool on);

/* The periodic stats dump callback of sch UT_SCH_STATS_SCH, sch_stat(). */
int ut_sch_stats_dump(mtl_handle mt);

/* Sample count of the per-dump loop time of sch UT_SCH_STATS_SCH, the one sch_stat()
 * resets. */
uint64_t ut_sch_stats_dump_loop_cnt(mtl_handle mt);

/* Sample count of the per-dump time of ut_tasklet, the one sch_stat() resets. */
uint64_t ut_sch_stats_dump_tasklet_cnt(mtl_handle mt);

#ifdef __cplusplus
}
#endif

#endif /* TESTS_UNIT_SCH_MT_SCH_HARNESS_H */
