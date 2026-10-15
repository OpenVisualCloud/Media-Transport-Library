/* ex20 — received frames lent to a framework (GstBuffer, AVBufferRef) without a copy, and
   released on any thread; MTL says when to copy instead. It lends with a pool_count of
   R_copy + H (R_copy the slots MTL keeps, MTL_UNITF_COPY_ADVISED; H the units held
   downstream at once); at the default pool of 3 every unit is copied. Needs: MS1. */
#include "ex_common.h"

#define EX_FLUSHING 1 /* the framework's "flushing" return (GST_FLOW_FLUSHING) */
#define EX_DROPPED 2  /* the framework's "no buffer this time, call again" return */

/* The framework's wrap: gst_buffer_new_wrapped_full(0, data, size + MTL_RX_TAIL_BYTES, 0,
   size, ref, cb) or av_buffer_create(data, size, cb, ref, 0) (cb takes a second argument
   there). Library slots are packed (FFmpeg av_image_fill_arrays with align 1) and end in
   MTL_RX_TAIL_BYTES zero bytes (AV_INPUT_BUFFER_PADDING_SIZE). Then its copy into a
   buffer of its own: the downstream pool decide_allocation chose (GL, DMA-BUF or system
   memory, so the copy is the upload), or an av_buffer_pool. The copy takes its buffer
   without waiting (GST_BUFFER_POOL_ACQUIRE_FLAG_DONTWAIT) and returns 1 when none is
   free: create never blocks on downstream. */
typedef void (*release_fn)(void* ref);
int wrap_as_framework_buffer(void* data, uint64_t size, release_fn cb, void* ref);
int copy_to_framework_buffer(const void* data, uint64_t size);

/* What a framework buffer keeps until its free callback runs: the lease's id as the
   callback's pointer, nothing of the element, since a sink, a queue or an appsink
   application may hold the buffer after the element is gone. */
static void release_cb(void* p) { /* any thread, any order, also after close */
  mtl_lease_h lease = {(uint64_t)(uintptr_t)p};   /* names its session */
  mtl_rx_release(MTL_NULL(mtl_session_h), lease); /* 0 also once either closed */
}

/* GST_QUERY_LATENCY: live, min = latency_min_ns (the caller's work included), max =
   latency_max_ns (GST_CLOCK_TIME_NONE for INT64_MAX, RX_LATEST). The units downstream
   holds change neither: past MTL's budget the source copies, so no unit is lost to a
   holder. Post a LATENCY message when either changes. */
int rx_latency_query(mtl_session_h s, int64_t* min_ns, int64_t* max_ns) {
  struct mtl_session_info info;
  int ret = mtl_session_get_info(s, &info, sizeof(info));
  if (ret < 0) return ret;
  *min_ns = info.latency_min_ns;
  *max_ns = info.latency_max_ns;
  return 0;
}

/* GstBaseSrc::create on the streaming thread: 0 = one buffer handed on; EX_DROPPED = a
   flagged unit found no free buffer and was dropped; EX_FLUSHING after unlock() or a
   stop, without waiting for unlock_stop(): the framework calls create again once it has
   run; a negative code on an error. unit_bytes: mtl_session_info.unit_bytes
   (with detection, the sum of stride x rows over the unit's planes). */
int rx_create(mtl_session_h s, uint64_t unit_bytes) {
  struct mtl_unit u;
  MTL_INIT(&u);
  int ret = mtl_rx_dequeue(s, &u, MTL_FOREVER);
  if (ret == -MTL_ECANCELED || ret == -MTL_ESHUTDOWN) return EX_FLUSHING;
  if (ret < 0) return ex_fail("dequeue", ret); /* -MTL_EIO: status.error_reason */
  if (u.flags & MTL_UNITF_COPY_ADVISED) { /* lending it would leave MTL too few slots */
    ret = copy_to_framework_buffer(u.plane[0].addr, unit_bytes);
    mtl_rx_release(s, u.lease);
    if (ret == 1) return EX_DROPPED; /* no free buffer: the unit is dropped (QoS) */
    return ret < 0 ? -MTL_ENOMEM : 0;
  }
  if (wrap_as_framework_buffer(u.plane[0].addr, unit_bytes, release_cb,
                               (void*)(uintptr_t)u.lease.id) == 0)
    return 0;
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
