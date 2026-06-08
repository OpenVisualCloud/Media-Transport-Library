/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

/**
 * @file mt_session_event.c
 *
 * Event queue implementation for the unified session API.
 * Uses rte_ring for lock-free event queuing from callbacks to poll().
 */

#include "mt_session.h"

#include <errno.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "../mt_log.h"
#include "../mt_mem.h"

#define MTL_EVENT_RING_SIZE 64 /* Must be power of 2 */

/*************************************************************************
 * Event Queue Lifecycle
 *************************************************************************/

int mtl_session_events_init(struct mtl_session_impl* s) {
  char ring_name[RTE_RING_NAMESIZE];

  snprintf(ring_name, sizeof(ring_name), "mtl_ev_%p", s);

  s->event_ring =
      rte_ring_create(ring_name, MTL_EVENT_RING_SIZE, s->socket_id, 0);
  if (!s->event_ring) {
    err("%s(%s), failed to create event ring\n", __func__, s->name);
    return -ENOMEM;
  }

  /* Create eventfd for epoll/select integration */
  s->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (s->event_fd < 0) {
    /* Not fatal - event_fd is optional */
    dbg("%s(%s), eventfd creation failed (optional)\n", __func__, s->name);
    s->event_fd = -1;
  }

  dbg("%s(%s), event queue initialized\n", __func__, s->name);
  return 0;
}

void mtl_session_events_uinit(struct mtl_session_impl* s) {
  /* Drain and free any remaining events */
  if (s->event_ring) {
    void* obj = NULL;
    while (rte_ring_dequeue(s->event_ring, &obj) == 0 && obj) {
      mt_rte_free(obj);
      obj = NULL;
    }
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

int mtl_session_event_post(struct mtl_session_impl* s, const mtl_event_t* event) {
  mtl_event_t* ev_copy;

  if (!s->event_ring) {
    return -EINVAL;
  }

  /* Allocate a copy of the event for the ring */
  ev_copy = mt_rte_zmalloc_socket(sizeof(*ev_copy), s->socket_id);
  if (!ev_copy) {
    dbg("%s(%s), failed to alloc event copy\n", __func__, s->name);
    return -ENOMEM;
  }

  *ev_copy = *event;

  if (rte_ring_enqueue(s->event_ring, ev_copy) != 0) {
    /* Ring full - drop event */
    mt_rte_free(ev_copy);
    dbg("%s(%s), event ring full, dropping event type %d\n", __func__, s->name,
        event->type);
    return -ENOSPC;
  }

  /* Signal eventfd if available */
  if (s->event_fd >= 0) {
    uint64_t val = 1;
    ssize_t n = write(s->event_fd, &val, sizeof(val));
    (void)n; /* Ignore write failures on non-blocking fd */
  }

  /* Call optional callback */
  if (event->type == MTL_EVENT_BUFFER_READY && s->notify_buffer_ready) {
    s->notify_buffer_ready(s->notify_priv);
  }

  return 0;
}
