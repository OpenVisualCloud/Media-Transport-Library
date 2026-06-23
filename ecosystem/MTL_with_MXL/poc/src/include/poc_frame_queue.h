/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — lock-free SPSC frame queue (single-producer / single-consumer)
 */

#ifndef POC_FRAME_QUEUE_H
#define POC_FRAME_QUEUE_H

#include <stdatomic.h>

#include "poc_types.h"

/*
 * Power-of-2 bounded SPSC ring. The MTL RX callback is the only producer;
 * the bridge worker thread is the only consumer.  No locks needed.
 */

#define POC_QUEUE_MAX_DEPTH 64 /* must be power of 2 */

typedef struct {
  poc_frame_entry_t entries[POC_QUEUE_MAX_DEPTH];
  atomic_uint_fast32_t head; /* next write position (producer) */
  atomic_uint_fast32_t tail; /* next read  position (consumer) */
  uint32_t mask;             /* depth - 1 */
  uint32_t depth;
} poc_frame_queue_t;

/* Initialise a queue.  depth must be power of 2 and <= POC_QUEUE_MAX_DEPTH */
int poc_queue_init(poc_frame_queue_t* q, uint32_t depth);

/* Enqueue (producer).  Returns 0 on success, -1 if full. */
int poc_queue_push(poc_frame_queue_t* q, const poc_frame_entry_t* entry);

/* Dequeue (consumer).  Returns 0 on success, -1 if empty. */
int poc_queue_pop(poc_frame_queue_t* q, poc_frame_entry_t* out);

/* Current occupancy. */
uint32_t poc_queue_count(const poc_frame_queue_t* q);

#endif /* POC_FRAME_QUEUE_H */
