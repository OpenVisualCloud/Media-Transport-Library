/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — lock-free SPSC frame queue implementation
 */

#include "poc_frame_queue.h"
#include <string.h>

int poc_queue_init(poc_frame_queue_t *q, uint32_t depth)
{
    if (!q || depth == 0 || depth > POC_QUEUE_MAX_DEPTH)
        return -1;
    /* depth must be power of 2 */
    if ((depth & (depth - 1)) != 0)
        return -1;

    memset(q->entries, 0, sizeof(q->entries));
    atomic_store(&q->head, 0);
    atomic_store(&q->tail, 0);
    q->depth = depth;
    q->mask  = depth - 1;
    return 0;
}

int poc_queue_push(poc_frame_queue_t *q, const poc_frame_entry_t *entry)
{
    uint32_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);

    /* full? */
    if (((head + 1) & q->mask) == (tail & q->mask))
        return -1;

    q->entries[head & q->mask] = *entry;

    atomic_store_explicit(&q->head, (head + 1) & (q->depth * 2 - 1),
                          memory_order_release);
    return 0;
}

int poc_queue_pop(poc_frame_queue_t *q, poc_frame_entry_t *out)
{
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&q->head, memory_order_acquire);

    /* empty? */
    if (tail == head)
        return -1;

    *out = q->entries[tail & q->mask];

    atomic_store_explicit(&q->tail, (tail + 1) & (q->depth * 2 - 1),
                          memory_order_release);
    return 0;
}

uint32_t poc_queue_count(const poc_frame_queue_t *q)
{
    uint32_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    return (head - tail) & q->mask;
}
