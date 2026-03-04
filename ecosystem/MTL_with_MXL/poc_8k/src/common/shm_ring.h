/* SPDX-License-Identifier: BSD-3-Clause
 * shm_ring.h — Lock-free SPSC ring buffer in POSIX shared memory
 *
 * Used for inter-process communication between:
 *   - Compositor (producer): writes encoded codestreams
 *   - Compositor TX (consumer): reads codestreams → ST22 TX
 *
 * Design:
 *   - Single Producer, Single Consumer (SPSC) — no locks needed
 *   - Fixed number of slots, each holding up to MAX_CODESTREAM_SIZE bytes
 *   - Producer and consumer use atomic head/tail indices
 *   - Stored in /dev/shm/ via shm_open + mmap
 *
 * Memory layout:
 *   [shm_ring_header_t] [slot_header_0] [payload_0] [slot_header_1] [payload_1] ...
 */

#ifndef POC_8K_SHM_RING_H
#define POC_8K_SHM_RING_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/* ── Configuration ── */
#define SHM_RING_NUM_SLOTS      4
#define SHM_RING_MAX_PAYLOAD    (20 * 1024 * 1024)  /* 20 MB per slot */
#define SHM_RING_MAGIC          0x534852494E473031ULL  /* "SHRING01" */

/* ── Per-slot metadata ── */
typedef struct {
    uint32_t payload_size;       /* actual bytes in this slot */
    uint64_t encode_start_ns;    /* CLOCK_MONOTONIC timestamp */
    uint32_t frame_idx;          /* monotonic frame counter */
    uint32_t encode_duration_us; /* encode time in microseconds */
    uint64_t shm_commit_ns;      /* compositor: after payload memcpy + commit */
    uint64_t comptx_pop_ns;      /* compositor_tx: when slot is consumed */
    uint64_t comptx_tx_ready_ns; /* compositor_tx: framebuffer published to ST22 */
} shm_slot_header_t;

/* Total size of one slot (header + payload area) */
#define SHM_SLOT_SIZE  (sizeof(shm_slot_header_t) + SHM_RING_MAX_PAYLOAD)

/* ── Ring buffer header (at start of shared memory region) ── */
typedef struct {
    uint64_t          magic;         /* SHM_RING_MAGIC */
    uint32_t          num_slots;     /* SHM_RING_NUM_SLOTS */
    uint32_t          max_payload;   /* SHM_RING_MAX_PAYLOAD */

    /* SPSC indices — producer increments head, consumer increments tail.
     * Both are monotonic counters; actual slot = index % num_slots.
     * Full:  head - tail == num_slots
     * Empty: head == tail */
    _Alignas(64) atomic_uint_fast64_t head;  /* next slot to write (producer) */
    _Alignas(64) atomic_uint_fast64_t tail;  /* next slot to read  (consumer) */

    /* Producer sets this to signal clean shutdown */
    atomic_bool       producer_done;
} shm_ring_header_t;

/* Total shared memory size */
#define SHM_RING_TOTAL_SIZE  (sizeof(shm_ring_header_t) + SHM_RING_NUM_SLOTS * SHM_SLOT_SIZE)

/* ── Helper: pointer to slot N ── */
static inline void *shm_ring_slot_ptr(void *base, uint32_t slot_idx)
{
    return (uint8_t *)base + sizeof(shm_ring_header_t) + (size_t)slot_idx * SHM_SLOT_SIZE;
}

static inline shm_slot_header_t *shm_ring_slot_hdr(void *base, uint32_t slot_idx)
{
    return (shm_slot_header_t *)shm_ring_slot_ptr(base, slot_idx);
}

static inline uint8_t *shm_ring_slot_payload(void *base, uint32_t slot_idx)
{
    return (uint8_t *)shm_ring_slot_ptr(base, slot_idx) + sizeof(shm_slot_header_t);
}

/* ═══════════════════════════════════════════════
 * Producer API (compositor side)
 * ═══════════════════════════════════════════════ */

typedef struct {
    void             *base;   /* mmap'd region */
    shm_ring_header_t *hdr;
    int               fd;
    char              name[128];  /* shm name e.g. "/poc_8k_ring" */
} shm_ring_producer_t;

/* Create and initialize the shared memory ring.
 * Returns 0 on success, -1 on failure. */
static inline int shm_ring_producer_create(shm_ring_producer_t *p, const char *shm_name)
{
    memset(p, 0, sizeof(*p));
    strncpy(p->name, shm_name, sizeof(p->name) - 1);

    /* Remove stale shm if it exists */
    shm_unlink(shm_name);

    p->fd = shm_open(shm_name, O_CREAT | O_RDWR | O_EXCL, 0666);
    if (p->fd < 0) {
        fprintf(stderr, "[SHM-RING] shm_open(%s) failed: %s\n", shm_name, strerror(errno));
        return -1;
    }

    if (ftruncate(p->fd, (off_t)SHM_RING_TOTAL_SIZE) < 0) {
        fprintf(stderr, "[SHM-RING] ftruncate(%zu) failed: %s\n",
                (size_t)SHM_RING_TOTAL_SIZE, strerror(errno));
        close(p->fd);
        shm_unlink(shm_name);
        return -1;
    }

    p->base = mmap(NULL, SHM_RING_TOTAL_SIZE, PROT_READ | PROT_WRITE,
                   MAP_SHARED, p->fd, 0);
    if (p->base == MAP_FAILED) {
        fprintf(stderr, "[SHM-RING] mmap failed: %s\n", strerror(errno));
        close(p->fd);
        shm_unlink(shm_name);
        return -1;
    }

    /* Initialize header */
    p->hdr = (shm_ring_header_t *)p->base;
    memset(p->base, 0, SHM_RING_TOTAL_SIZE);
    p->hdr->magic = SHM_RING_MAGIC;
    p->hdr->num_slots = SHM_RING_NUM_SLOTS;
    p->hdr->max_payload = SHM_RING_MAX_PAYLOAD;
    atomic_store(&p->hdr->head, 0);
    atomic_store(&p->hdr->tail, 0);
    atomic_store(&p->hdr->producer_done, false);

    printf("[SHM-RING] Producer created: %s (%zu bytes, %u slots × %u MB)\n",
           shm_name, (size_t)SHM_RING_TOTAL_SIZE,
           SHM_RING_NUM_SLOTS, SHM_RING_MAX_PAYLOAD / (1024 * 1024));
    return 0;
}

/* Try to acquire a slot for writing.
 * Returns slot index (0..N-1) or -1 if ring is full. */
static inline int shm_ring_producer_try_acquire(shm_ring_producer_t *p)
{
    uint64_t head = atomic_load_explicit(&p->hdr->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&p->hdr->tail, memory_order_acquire);
    if (head - tail >= p->hdr->num_slots)
        return -1;  /* full */
    return (int)(head % p->hdr->num_slots);
}

/* Commit a written slot (advance head).
 * payload_size, encode_start_ns, etc. must be set in the slot header
 * BEFORE calling this. */
static inline void shm_ring_producer_commit(shm_ring_producer_t *p)
{
    atomic_fetch_add_explicit(&p->hdr->head, 1, memory_order_release);
}

/* Signal that producer is done (no more frames) */
static inline void shm_ring_producer_finish(shm_ring_producer_t *p)
{
    atomic_store(&p->hdr->producer_done, true);
}

/* Destroy producer side */
static inline void shm_ring_producer_destroy(shm_ring_producer_t *p)
{
    if (p->base && p->base != MAP_FAILED) {
        shm_ring_producer_finish(p);
        munmap(p->base, SHM_RING_TOTAL_SIZE);
    }
    if (p->fd >= 0)
        close(p->fd);
    shm_unlink(p->name);
    memset(p, 0, sizeof(*p));
    p->fd = -1;
}

/* ═══════════════════════════════════════════════
 * Consumer API (compositor_tx side)
 * ═══════════════════════════════════════════════ */

typedef struct {
    void             *base;
    shm_ring_header_t *hdr;
    int               fd;
    char              name[128];
} shm_ring_consumer_t;

/* Open an existing shared memory ring for reading.
 * Returns 0 on success, -1 on failure. */
static inline int shm_ring_consumer_open(shm_ring_consumer_t *c, const char *shm_name)
{
    memset(c, 0, sizeof(*c));
    strncpy(c->name, shm_name, sizeof(c->name) - 1);

    c->fd = shm_open(shm_name, O_RDWR, 0);
    if (c->fd < 0) {
        fprintf(stderr, "[SHM-RING] Consumer: shm_open(%s) failed: %s\n",
                shm_name, strerror(errno));
        return -1;
    }

    c->base = mmap(NULL, SHM_RING_TOTAL_SIZE, PROT_READ | PROT_WRITE,
                   MAP_SHARED, c->fd, 0);
    if (c->base == MAP_FAILED) {
        fprintf(stderr, "[SHM-RING] Consumer: mmap failed: %s\n", strerror(errno));
        close(c->fd);
        return -1;
    }

    c->hdr = (shm_ring_header_t *)c->base;

    /* Validate magic */
    if (c->hdr->magic != SHM_RING_MAGIC) {
        fprintf(stderr, "[SHM-RING] Consumer: bad magic 0x%lx (expected 0x%lx)\n",
                (unsigned long)c->hdr->magic, (unsigned long)SHM_RING_MAGIC);
        munmap(c->base, SHM_RING_TOTAL_SIZE);
        close(c->fd);
        return -1;
    }

    printf("[SHM-RING] Consumer opened: %s (%u slots × %u MB)\n",
           shm_name, c->hdr->num_slots, c->hdr->max_payload / (1024 * 1024));
    return 0;
}

/* Try to get the next slot for reading.
 * Returns slot index (0..N-1) or -1 if ring is empty. */
static inline int shm_ring_consumer_try_peek(shm_ring_consumer_t *c)
{
    uint64_t tail = atomic_load_explicit(&c->hdr->tail, memory_order_relaxed);
    uint64_t head = atomic_load_explicit(&c->hdr->head, memory_order_acquire);
    if (tail >= head)
        return -1;  /* empty */
    return (int)(tail % c->hdr->num_slots);
}

/* Release a consumed slot (advance tail). */
static inline void shm_ring_consumer_release(shm_ring_consumer_t *c)
{
    atomic_fetch_add_explicit(&c->hdr->tail, 1, memory_order_release);
}

/* Check if producer has signaled completion AND ring is empty */
static inline bool shm_ring_consumer_done(shm_ring_consumer_t *c)
{
    if (!atomic_load(&c->hdr->producer_done))
        return false;
    uint64_t tail = atomic_load_explicit(&c->hdr->tail, memory_order_acquire);
    uint64_t head = atomic_load_explicit(&c->hdr->head, memory_order_acquire);
    return tail >= head;
}

/* Destroy consumer side */
static inline void shm_ring_consumer_destroy(shm_ring_consumer_t *c)
{
    if (c->base && c->base != MAP_FAILED)
        munmap(c->base, SHM_RING_TOTAL_SIZE);
    if (c->fd >= 0)
        close(c->fd);
    /* Consumer does NOT shm_unlink — that's producer's job */
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

#endif /* POC_8K_SHM_RING_H */
