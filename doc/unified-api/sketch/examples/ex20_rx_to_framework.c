/* ex20 — received frames lent to a framework (GstBuffer, AVBufferRef) without a copy, and
   released on any thread. Needs: MS1. */
#include <mtl/experimental/mtl_util.h>
#include <stdlib.h>

#include "ex_common.h"

#define EX_FLUSHING 1 /* the framework's "flushing" return (GST_FLOW_FLUSHING) */

/* What one framework buffer keeps until its free callback runs: the handles by value,
   never a pointer into the element, since a sink, a queue or an appsink application may
   hold the buffer after the element is gone. */
struct rx_ref {
  mtl_session_h s;
  mtl_lease_h lease;
};
/* The framework's wrap: gst_buffer_new_wrapped_full(0, data, size + MTL_RX_TAIL_BYTES, 0,
   size, ref, cb) or av_buffer_create(data, size, cb, ref, 0) (cb takes a second argument
   there). Library slots are packed (FFmpeg av_image_fill_arrays with align 1) and end in
   MTL_RX_TAIL_BYTES zero bytes (AV_INPUT_BUFFER_PADDING_SIZE). Then its copy into a
   buffer of its own, and its count of wrapped buffers still out (an atomic the callback
   decrements). */
typedef void (*release_fn)(void* ref);
int wrap_as_framework_buffer(void* data, uint64_t size, release_fn cb, void* ref);
int copy_to_framework_buffer(const void* data, uint64_t size);
uint32_t framework_wrapped_out(void);

static void release_cb(void* p) { /* any thread, any order, also after close */
  struct rx_ref* ref = (struct rx_ref*)p;
  mtl_rx_release(ref->s, ref->lease); /* 0 also once the session or instance closed */
  free(ref);
}

/* At start and at each GST_EVENT_LATENCY: the slots to keep for MTL, the units not yet
   handed on. Once downstream holds the rest, the next unit is copied, so a slow consumer
   costs a copy, never a lost unit. latency_ns: the pipeline's configured latency;
   copy_ns: the measured copy of one unit (about 1 ms at 1080p). A pool_count below this +
   the units held downstream (a sink's last sample, an aggregator pad) makes the copy path
   run: log the pool_count it needs once. */
int rx_reserve(mtl_session_h s, int64_t latency_ns, int64_t copy_ns) {
  struct mtl_session_info info;
  int ret = mtl_session_get_info(s, &info, sizeof(info));
  return ret < 0 ? ret : mtl_rx_reserve(&info, NULL, latency_ns, copy_ns);
}

/* GstBaseSrc::create on the streaming thread: 0 = one buffer handed on; EX_FLUSHING after
   unlock() or a stop, without waiting for unlock_stop(): the framework calls create again
   once it has run; a negative code on an error. unit_bytes: mtl_session_info.unit_bytes
   (with detection, the sum of stride x rows over the unit's planes); reserve: the last
   rx_reserve(). */
int rx_create(mtl_session_h s, uint64_t unit_bytes, uint32_t pool_count,
              uint32_t reserve) {
  struct mtl_unit u;
  MTL_INIT(&u);
  int ret = mtl_rx_dequeue(s, &u, MTL_FOREVER);
  if (ret == -MTL_ECANCELED || ret == -MTL_ESHUTDOWN) return EX_FLUSHING;
  if (ret < 0) return ex_fail("dequeue", ret); /* -MTL_EIO: status.error_reason */
  if (framework_wrapped_out() + reserve >= pool_count) { /* copy into its own buffer */
    ret = copy_to_framework_buffer(u.plane[0].addr, unit_bytes);
    mtl_rx_release(s, u.lease);
    return ret < 0 ? -MTL_ENOMEM : 0;
  }
  struct rx_ref* ref = (struct rx_ref*)malloc(sizeof(*ref));
  if (ref) {
    ref->s = s; /* by value */
    ref->lease = u.lease;
    if (wrap_as_framework_buffer(u.plane[0].addr, unit_bytes, release_cb, ref) == 0)
      return 0;
    free(ref);
  }
  mtl_rx_release(s, u.lease);
  return -MTL_ENOMEM;
}

/* GstBaseSrc::unlock and ::unlock_stop, on the application thread that flushes:
   mtl_session_interrupt(s, 1) and (s, 0). */
int rx_unlock(mtl_session_h s) {
  return mtl_session_interrupt(s, 1); /* the dequeue in rx_create returns at once */
}
int rx_unlock_stop(mtl_session_h s) {
  return mtl_session_interrupt(s, 0);
}

/* GstBaseSrc::stop: never waits for downstream. Close returns MTL_RETIRING while buffers
   are still out, which is not a failure: the last release retires the session, with no
   further call. */
int rx_stop(mtl_session_h s) {
  int ret = mtl_session_close(s, 0);
  return ret < 0 ? ret : 0;
}
