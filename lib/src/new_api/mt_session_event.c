/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

/**
 * @file mt_session_event.c
 *
 * Event queue implementation for the unified session API.
 * Uses rte_ring for lock-free event queuing from callbacks to poll().
 */

#include <errno.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "../mt_log.h"
#include "mt_session.h"

#define MTL_EVENT_RING_SIZE 64 /* Must be power of 2 */

/*************************************************************************
 * Event Queue Lifecycle
 *************************************************************************/

int mtl_session_events_init(struct mtl_session_impl* s) {
  char ring_name[RTE_RING_NAMESIZE];

  snprintf(ring_name, sizeof(ring_name), "mtl_ev_%p", s);

  /* Value-backed ring: events are copied in, so the producer (tasklet) never
   * allocates. Single consumer (the app thread) -> RING_F_SC_DEQ; the producer
   * stays MP-safe (notify_frame_* and the vsync callback are distinct
   * contexts), so no RING_F_SP_ENQ. */
  s->event_ring = rte_ring_create_elem(ring_name, sizeof(mtl_event_t),
                                       MTL_EVENT_RING_SIZE, s->socket_id, RING_F_SC_DEQ);
  if (!s->event_ring) {
    err("%s(%s), failed to create event ring\n", __func__, s->name);
    return -ENOMEM;
  }

  /* Wakeup fd so the consumer can block instead of spin. */
  s->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (s->event_fd < 0) {
    /* Not fatal - event_fd is optional */
    dbg("%s(%s), eventfd creation failed (optional)\n", __func__, s->name);
    s->event_fd = -1;
  }

  s->events_dropped = 0;

  dbg("%s(%s), event queue initialized\n", __func__, s->name);
  return 0;
}

void mtl_session_events_uinit(struct mtl_session_impl* s) {
  if (s->event_ring) {
    rte_ring_free(s->event_ring);
    s->event_ring = NULL;
  }

  if (s->event_fd >= 0) {
    close(s->event_fd);
    s->event_fd = -1;
  }
}

/*************************************************************************
 * Event Posting (called from callbacks / library threads)
 *************************************************************************/

void mtl_session_events_signal(struct mtl_session_impl* s) {
  /* Non-blocking 8-byte counter bump to wake a blocked consumer. The fd is
   * O_NONBLOCK so this never blocks (ignore EAGAIN); write() is async-signal-
   * safe, so the stop path may call this from a signal handler. */
  if (s->event_fd >= 0) {
    uint64_t val = 1;
    ssize_t n = write(s->event_fd, &val, sizeof(val));
    (void)n;
  }
}

int mtl_session_event_post(struct mtl_session_impl* s, const mtl_event_t* event) {
  if (!s->event_ring) {
    return -EINVAL;
  }

  /* Producer (tasklet) path: copy the event in by value - no alloc, no block.
   * On a full ring drop and count rather than wait. */
  if (rte_ring_enqueue_elem(s->event_ring, (void*)event, sizeof(*event)) != 0) {
    __atomic_fetch_add(&s->events_dropped, 1, __ATOMIC_RELAXED);
    dbg("%s(%s), event ring full, dropping event type %d\n", __func__, s->name,
        event->type);
    return -ENOSPC;
  }

  /* Non-blocking 8-byte counter bump to wake a blocked consumer; the fd is
   * O_NONBLOCK so this cannot block the tasklet (ignore EAGAIN). */
  mtl_session_events_signal(s);

  /* Call optional callback */
  if (event->type == MTL_EVENT_BUFFER_READY && s->notify_buffer_ready) {
    s->notify_buffer_ready(s->notify_priv);
  }

  return 0;
}
