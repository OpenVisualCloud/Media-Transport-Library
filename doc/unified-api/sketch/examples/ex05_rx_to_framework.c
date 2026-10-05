/* ex05 — RX frames lent to a framework (GstBuffer, AVBufferRef): dequeue on the streaming
   thread, release on whatever thread drops the buffer. GStreamer unlock()/unlock_stop()
   run on another thread and map to mtl_session_interrupt(s, 1) and (s, 0). Needs: MS1. */
#include "ex_common.h"

#define EX_FLUSHING 1 /* the framework's "flushing" return (GST_FLOW_FLUSHING) */

typedef void (*release_fn)(void* user, mtl_lease_h lease);
int wrap_as_framework_buffer(const struct mtl_unit* u, release_fn cb, void* user);

static void release_cb(void* user, mtl_lease_h lease) {
  mtl_rx_release(*(mtl_session_h*)user, lease); /* any thread, any order */
}

/* GstBaseSrc::create on the streaming thread: 0 = one buffer handed on; EX_FLUSHING after
   unlock() or a stop, without waiting for unlock_stop(): the framework calls create again
   once it has run; a negative code on an error. */
int rx_create(mtl_session_h* s) {
  struct mtl_unit u;
  MTL_INIT(&u);
  for (;;) {
    int ret = mtl_rx_dequeue(*s, &u, MTL_MS(200));
    if (ret == -MTL_EAGAIN) continue;
    if (ret == -MTL_ECANCELED || ret == -MTL_ESHUTDOWN) return EX_FLUSHING;
    if (ret < 0) return ex_fail("dequeue", ret); /* -MTL_EIO: status.error_reason */
    if (wrap_as_framework_buffer(&u, release_cb, s) < 0) {
      mtl_rx_release(*s, u.lease);
      return -MTL_ENOMEM;
    }
    return 0;
  }
}

/* GstBaseSrc::unlock and ::unlock_stop, on the application thread that flushes. */
int rx_unlock(mtl_session_h s) {
  return mtl_session_interrupt(s, 1); /* the dequeue in rx_create returns at once */
}
int rx_unlock_stop(mtl_session_h s) {
  return mtl_session_interrupt(s, 0);
}

/* GstBaseSrc::stop: close returns 1 while downstream still holds buffers, which is not a
   failure; the session retires on the last release. */
int rx_stop(mtl_session_h s) {
  mtl_session_close(s, 0);
  return 0;
}
