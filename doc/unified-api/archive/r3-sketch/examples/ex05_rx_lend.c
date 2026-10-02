/* 10 §7 — RX lent to a framework (P2, P5): dequeue, wrap, release from any thread; the
   GStreamer unlock()/unlock_stop() pair maps to interrupt/uninterrupt. */
#include "ex_common.h"

/* framework glue (GstBuffer / AVBufferRef): calls release_cb(user, lease) when downstream
   drops the buffer, on whatever thread that happens */
typedef void (*release_fn)(void* user, mtl_lease_h lease);
int wrap_as_framework_buffer(mtl_lease_h lease, const struct mtl_buffer_view* v,
                             const struct mtl_rx_unit* u, release_fn cb, void* user);
void wait_for_unlock_stop(void);
extern uint64_t g_missed;

static void release_cb(void* user, mtl_lease_h lease) {
  mtl_session_h* s = (mtl_session_h*)user;
  int ret = mtl_rx_release(*s, lease); /* any thread, any order; legal while DESTROYING */
  if (ret < 0) ex_fail("release", ret);
}

/* Latest-only receivers (a monitor that must show the freshest frame): pool.count = 2 and
   pool.rx_overflow = MTL_RX_RECLAIM_OLDEST_READY before create. */
int rx_streaming_thread(mtl_session_h* s) {
  for (;;) {
    mtl_lease_h lease;
    struct mtl_buffer_view v;
    struct mtl_rx_unit u;
    mtl_buffer_view_init(&v);
    int ret = mtl_rx_dequeue(*s, &lease, &v, &u, sizeof(u), MTL_MS(200));
    if (ret == -MTL_ETIMEDOUT || ret == -MTL_EAGAIN) continue;
    if (ret == -MTL_ECANCELED) { /* unlock(): someone called mtl_session_interrupt */
      wait_for_unlock_stop();
      ret = mtl_session_uninterrupt(*s); /* unlock_stop() */
      if (ret < 0) return ex_fail("uninterrupt", ret);
      continue;
    }
    if (ret == -MTL_ESHUTDOWN) return 0; /* we asked for stop/destroy */
    /* -MTL_EIO: ERROR, see get_status().reason */
    if (ret < 0) return ex_fail("dequeue", ret);
    g_missed += u.units_missed_before;
    ret = wrap_as_framework_buffer(lease, &v, &u, release_cb, s);
    if (ret < 0) mtl_rx_release(*s, lease);
  }
}

/* GstBaseSrc::stop / FFmpeg read_close: destroy is deferred while downstream still holds
   buffers; SESSION_RETIRED (on any EQ subscribed with MTL_EQ_SUB_SESSION) says when the
   pool is gone. */
int rx_stop(mtl_session_h s) {
  int ret = mtl_session_stop(s, MTL_STOP_FLUSH, 0);
  if (ret < 0) ex_fail("stop", ret);
  return mtl_session_destroy(s, 0);
}
