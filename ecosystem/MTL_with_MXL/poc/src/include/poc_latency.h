/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — lightweight latency accumulator
 *
 * Designed for per-window (2 s) min/max/avg + percentile reporting.
 * All values in microseconds (µs).
 *
 * Thread-safety: single-writer (bridge thread OR consumer thread).
 * The stats reporter reads a snapshot — occasional tearing is acceptable
 * because we reset the window atomically from the writer thread.
 */

#ifndef POC_LATENCY_H
#define POC_LATENCY_H

#include <stdint.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <stdlib.h>

/* Max raw samples kept per window.  At 30 fps × 2 s = 60 frames;
 * 128 gives headroom for bursts / higher frame rates.  If more
 * samples arrive, old ones are overwritten (ring buffer). */
#define POC_LATENCY_SAMPLES_MAX 128

typedef struct {
    uint64_t min_us;    /* minimum latency in current window */
    uint64_t max_us;    /* maximum latency in current window */
    uint64_t sum_us;    /* sum for average computation */
    uint64_t count;     /* total samples recorded (may exceed ring size) */

    /* Ring buffer of raw samples for percentile computation */
    uint64_t samples[POC_LATENCY_SAMPLES_MAX];
    uint32_t ring_wr;   /* next write index (wraps at SAMPLES_MAX) */
    uint32_t ring_n;    /* valid entries in ring (≤ SAMPLES_MAX) */
} poc_latency_t;

/* Initialise / reset a latency accumulator window */
static inline void poc_latency_reset(poc_latency_t *lat)
{
    lat->min_us = UINT64_MAX;
    lat->max_us = 0;
    lat->sum_us = 0;
    lat->count  = 0;
    lat->ring_wr = 0;
    lat->ring_n  = 0;
}

/* Record a single sample (in µs) */
static inline void poc_latency_record(poc_latency_t *lat, uint64_t us)
{
    if (us < lat->min_us) lat->min_us = us;
    if (us > lat->max_us) lat->max_us = us;
    lat->sum_us += us;
    lat->count++;

    /* Store raw sample in ring buffer */
    lat->samples[lat->ring_wr] = us;
    lat->ring_wr = (lat->ring_wr + 1) % POC_LATENCY_SAMPLES_MAX;
    if (lat->ring_n < POC_LATENCY_SAMPLES_MAX)
        lat->ring_n++;
}

/* Compute average; returns 0 if no samples */
static inline uint64_t poc_latency_avg(const poc_latency_t *lat)
{
    return lat->count ? lat->sum_us / lat->count : 0;
}

/* ── Percentile helpers ── */

/* qsort comparator for uint64_t */
static inline int _poc_lat_cmp_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

/* Compute a percentile (0–100) from the ring buffer.
 * Returns 0 if no samples.  Caller provides a scratch buffer of at
 * least POC_LATENCY_SAMPLES_MAX elements to avoid alloc. */
static inline uint64_t poc_latency_percentile(const poc_latency_t *lat,
                                               uint64_t *scratch,
                                               unsigned pct)
{
    uint32_t n = lat->ring_n;
    if (n == 0) return 0;
    memcpy(scratch, lat->samples, n * sizeof(uint64_t));
    qsort(scratch, n, sizeof(uint64_t), _poc_lat_cmp_u64);
    uint32_t idx = (uint32_t)((uint64_t)pct * (n - 1) / 100);
    return scratch[idx];
}

/* ── Timestamping helpers ── */

/* Get current time as nanoseconds (CLOCK_REALTIME for cross-server PTP sync) */
static inline uint64_t poc_now_realtime_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Get current time as nanoseconds (CLOCK_MONOTONIC for same-process deltas) */
static inline uint64_t poc_now_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Convert nanosecond delta to microseconds */
static inline uint64_t poc_ns_to_us(uint64_t ns)
{
    return ns / 1000;
}

/* ── Cross-machine timestamp embedding ──
 * Written by sender into first 16 bytes of grain payload.
 * Read by receiver to compute true PTP-synced end-to-end latency.
 * Requires PTP synchronization (phc2sys) between machines. */
#define POC_TS_MAGIC 0x504F435F54494D45ULL  /* "POC_TIME" */

typedef struct __attribute__((packed)) {
    uint64_t magic;        /* POC_TS_MAGIC — validates timestamp presence */
    uint64_t enqueue_ns;   /* sender's CLOCK_REALTIME at frame enqueue (T1) */
} poc_frame_ts_t;

static inline void poc_frame_ts_write(uint8_t *payload, uint64_t enqueue_ns)
{
    poc_frame_ts_t *ts = (poc_frame_ts_t *)payload;
    ts->magic      = POC_TS_MAGIC;
    ts->enqueue_ns = enqueue_ns;
}

/* Returns enqueue_ns or 0 if magic doesn't match */
static inline uint64_t poc_frame_ts_read(const uint8_t *payload)
{
    const poc_frame_ts_t *ts = (const poc_frame_ts_t *)payload;
    if (ts->magic != POC_TS_MAGIC)
        return 0;
    return ts->enqueue_ns;
}

#endif /* POC_LATENCY_H */
