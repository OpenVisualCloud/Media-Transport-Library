/* ex05 — RX frames lent to a framework (GstBuffer, AVBufferRef): dequeue on the streaming
   thread, release on whatever thread drops the buffer. GStreamer unlock()/unlock_stop()
   map to mtl_session_interrupt(s, 1) and (s, 0). */
#include "ex_common.h"

typedef void (*release_fn)(void* user, mtl_lease_h lease);
int wrap_as_framework_buffer(const struct mtl_unit* u, release_fn cb, void* user);
void wait_for_unlock_stop(void);

static void release_cb(void* user, mtl_lease_h lease) {
  mtl_rx_release(*(mtl_session_h*)user, lease); /* any thread, any order */
}

int rx_streaming_thread(mtl_session_h* s) {
  struct mtl_unit u;
  MTL_INIT(&u);
  for (;;) {
    int ret = mtl_rx_dequeue(*s, &u, MTL_MS(200));
    if (ret == -MTL_EAGAIN) continue;
    if (ret == -MTL_ECANCELED) { /* unlock() */
      wait_for_unlock_stop();
      mtl_session_interrupt(*s, 0); /* unlock_stop() */
      continue;
    }
    if (ret == -MTL_ESHUTDOWN) return 0;         /* we stopped or closed it */
    if (ret < 0) return ex_fail("dequeue", ret); /* -MTL_EIO: status.error_reason */
    if (wrap_as_framework_buffer(&u, release_cb, s) < 0) mtl_rx_release(*s, u.lease);
  }
}

/* GstBaseSrc::stop: close returns 1 while downstream still holds buffers; the session
   retires on the last release. */
int rx_stop(mtl_session_h s) {
  return mtl_session_close(s, 0);
}
