/* ex10 — send from your memory without a copy: a framework's surfaces attached as the
   session's slots, or a buffer per frame bound to a slot at acquire. A buffer goes back
   to the framework only when its result says the NIC is done with it. Needs: MS2b. */
#include <mtl/experimental/mtl_mem.h>

#include "ex_common.h"

#define N 4
extern void* arena; /* the framework's pool: N surfaces, page aligned */
extern uint64_t arena_len;
/* 0 = a frame is ready within timeout_ns; the thread sleeps meanwhile */
int next_framework_frame(uint32_t* surface, uint64_t* id, int64_t timeout_ns);
void framework_frame_done(uint64_t id);

/* Gives the surfaces whose frames have left back to the framework; 0 or an error. */
static int reap(mtl_session_h s) {
  struct mtl_tx_result r[8];
  int n;
  while ((n = mtl_tx_reap(s, r, 8, 0)) > 0)
    for (int i = 0; i < n; i++) framework_frame_done(r[i].cookie);
  return n == -MTL_EAGAIN ? 0 : n;
}

/* base: a video TX config. Surface i is slot i, in its natural layout. */
int zero_copy_tx(mtl_instance_h mt, const struct mtl_session_config* base) {
  mtl_session_h s = MTL_NULL(mtl_session_h);
  struct mtl_session_config sc = *base;
  struct mtl_attach a;
  struct mtl_unit u;
  MTL_INIT(&a);
  MTL_INIT(&u);
  sc.flags = MTL_SESSION_POOL_ATTACHED | MTL_SESSION_REQUIRE_DIRECT; /* never a copy */
  sc.pool_count = N;
  a.va = arena; /* imported for this session */
  a.length = arena_len;
  a.count = N;
  int ret = mtl_session_create(mt, &sc, &s);
  if (ret >= 0) ret = mtl_session_attach(s, &a); /* fails here, with a reason */
  if (ret >= 0) ret = mtl_session_start(&s, 1, NULL, NULL);

  while (ret >= 0 && g_running) {
    uint32_t i;
    uint64_t id;
    ret = reap(s);
    if (ret < 0 || next_framework_frame(&i, &id, MTL_MS(20)) != 0) continue;
    ret = mtl_tx_acquire_slot(s, i, &u, MTL_MS(20)); /* exactly surface i */
    if (ret == 0) {
      u.cookie = id;
      ret = mtl_tx_submit(s, &u);
    } else if (ret == -MTL_EAGAIN) { /* surface i still in flight: skip this frame */
      framework_frame_done(id);
      ret = 0;
    }
  }

  if (ret < 0) ex_fail("zero copy", ret);
  mtl_session_stop(&s, 1, MTL_STOP_DRAIN, MTL_SEC(1)); /* a result for every unit */
  reap(s);
  /* MTL_RETIRING: the NIC may still read a surface; 0: the arena may be freed */
  while (mtl_session_close(s, MTL_SEC(1)) == MTL_RETIRING) {
  }
  return ret;
}

/* A buffer per frame (a GStreamer upstream pool, an FFmpeg frame): the arena imported
   once, mapped into every port now (copy-only memory: mi.direct 0). The session:
   MTL_SESSION_POOL_ATTACHED, pool_count = the buffers in flight, no slot attached. */
int import_arena(mtl_instance_h mt, void* va, uint64_t len, mtl_region_h* r) {
  struct mtl_mem_desc d;
  struct mtl_mem_info mi;
  MTL_INIT(&d);
  d.va = va;
  d.length = len;
  d.flags = MTL_MEM_READ | MTL_MEM_MAP_ALL;
  int ret = mtl_mem_import(mt, &d, r);
  if (ret >= 0) ret = mtl_mem_get_info(*r, &mi, sizeof(mi));
  if (ret >= 0 && !mi.direct) ret = -MTL_ENOTSUP;
  return ret;
}

/* The buffer at offset, sent as it is; its result (reap) names it by id. */
int send_buffer(mtl_session_h s, mtl_region_h r, uint64_t offset, uint64_t id) {
  struct mtl_attach one;
  struct mtl_unit u;
  MTL_INIT(&one);
  MTL_INIT(&u);
  one.count = 1;
  one.region = r;
  one.offset = offset;
  one.cookie = id; /* u.cookie starts as id */
  int ret = mtl_tx_acquire_layout(s, &one, &u, MTL_MS(20));
  return ret < 0 ? ret : mtl_tx_submit(s, &u);
}
