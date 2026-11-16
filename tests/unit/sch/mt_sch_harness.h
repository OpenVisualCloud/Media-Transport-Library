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

#define UT_SCH_IDLE_NS (100)
#define UT_SCH_WORK_NS (50000)
/* not 0, so a counter lookup that ignores the sch index fails */
#define UT_SCH_STATS_IDX (1)
/* past glibc's tcache sizes, so the free of the counters checks the chunk after them */
#define UT_SCH_REQUEST_TASKLETS (64)

/* The arguments of sys:tasklet_stat, or the counters of a tasklet slot. */
struct ut_sch_tasklet_stat {
  int sch_idx;
  const char* name;
  uint64_t cnt;
  uint64_t sum_ns;
  uint64_t work_cnt;
  uint64_t work_sum_ns;
};

/* Sch UT_SCH_STATS_IDX, stopped, with mtl_init_params flags and ut_tasklet in slot 0: on
 * the fake clock its every work_every-th call (none for 0) takes UT_SCH_WORK_NS and
 * returns MTL_TASKLET_HAS_PENDING, the others take UT_SCH_IDLE_NS and are idle. */
struct ut_sch_stats;
struct ut_sch_stats* ut_sch_stats_create(uint64_t flags, unsigned int work_every);
/* The same, through sch_request() on a zeroed main: UT_SCH_REQUEST_TASKLETS slots, all
 * registered, ut_idle in slot 1. Its destroy unregisters them and returns sch_free(). */
struct ut_sch_stats* ut_sch_stats_request(uint64_t flags, unsigned int work_every);
int ut_sch_stats_destroy(struct ut_sch_stats* s);
/* Registers the idle ut_idle into slot 1, unregistering the one registered before. */
int ut_sch_stats_register_idle(struct ut_sch_stats* s);
void ut_sch_stats_set_work_every(struct ut_sch_stats* s, unsigned int work_every);
/* Runs the loop until ut_tasklet was called calls times more. */
int ut_sch_stats_run(struct ut_sch_stats* s, uint64_t calls);
/* Attaches or detaches sys:tasklet_stat, through its semaphore as bpftrace does. */
void ut_sch_stats_attach(bool on);
struct ut_sch_tasklet_stat ut_sch_stats_counters(int slot);
/* Runs sch_stat(): the arguments of the last sys:tasklet_stat it fired. */
struct ut_sch_tasklet_stat ut_sch_stats_dump(struct ut_sch_stats* s);
/* The calls of ut_tasklet in the per-dump time that sch_stat() prints and resets. */
uint64_t ut_sch_stats_measured(struct ut_sch_stats* s);
/* The calls in the time histogram of a slot, and in the stats thread's copy of it. */
uint64_t ut_sch_stats_hist_calls(struct ut_sch_stats* s, int slot);
uint64_t ut_sch_stats_hist_prev_calls(struct ut_sch_stats* s, int slot);
/* MT_SCH_TIME_SAMPLE_PERIOD, MT_SCH_TIME_SAMPLE_SEED and sch_time_sample_gap(). */
uint32_t ut_sch_time_sample_period(void);
uint32_t ut_sch_time_sample_seed(void);
uint32_t ut_sch_time_sample_gap(uint32_t* state);

/* The time histogram: its bucket count, the bucket of ns, the lower bound of bucket b,
 * and the q-th percentile it gives of n call times. */
unsigned int ut_sch_hist_nb(void);
unsigned int ut_sch_hist_bucket(uint64_t ns);
uint64_t ut_sch_hist_bucket_lo(unsigned int b);
double ut_sch_hist_quantile(const uint64_t* ns, size_t n, unsigned int q);

#ifdef __cplusplus
}
#endif

#endif /* TESTS_UNIT_SCH_MT_SCH_HARNESS_H */
