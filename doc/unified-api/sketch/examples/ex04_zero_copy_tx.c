/* ex04 — zero-copy TX from a framework's pool: N surfaces in one page-aligned arena are
   the session's slots. App memory always produces results, so a surface goes back to the
   framework only when its result says the NIC is done with it. */
#include <mtl/experimental/mtl_mem.h>

#include "ex_common.h"

#define N 4
extern void* arena; /* the framework's pool: N surfaces, page aligned */
extern uint64_t arena_len;
int next_framework_frame(uint32_t* surface, uint64_t* id); /* 0 = one ready */
void framework_frame_done(uint64_t id);

/* Gives the surfaces whose frames have left back to the framework; 0 or an error. */
static int reap(mtl_session_h s) {
  struct mtl_tx_result r[8];
  int n;
  while ((n = mtl_tx_reap(s, r, 8, 0)) > 0)
    for (int i = 0; i < n; i++) framework_frame_done(r[i].cookie);
  return n == -MTL_EAGAIN ? 0 : n;
}

int zero_copy_tx(mtl_instance_h mt) {
  mtl_session_h s = MTL_NULL(mtl_session_h);
  struct mtl_session_config sc;
  struct mtl_attach a;
  struct mtl_unit u;
  MTL_INIT(&sc);
  MTL_INIT(&a);
  MTL_INIT(&u);

  int ret = 0;
  sc.direction = MTL_TX;
  sc.essence = MTL_VIDEO;
  mtl_flow_ipv4(&sc.flows[0], 239, 168, 85, 21, 20000);
  sc.video.raster.width = 1920;
  sc.video.raster.height = 1080;
  sc.video.raster.fps = mtl_fps_rational(MTL_FPS_59_94);
  sc.video.format = MTL_YUV422_10;
  sc.flags = MTL_SESSION_POOL_ATTACHED | MTL_SESSION_REQUIRE_DIRECT; /* never a copy */
  sc.pool_count = N;
  a.va = arena; /* imported for this session; surface i = slot i, natural layout */
  a.length = arena_len;
  a.count = N;
  if (ret >= 0) ret = mtl_session_create(mt, &sc, &s);
  if (ret >= 0)
    ret = mtl_session_attach(s, &a); /* fails here, with a reason, if too small */
  if (ret >= 0) ret = mtl_session_start(&s, 1, NULL, NULL);

  while (ret >= 0 && g_running) {
    uint32_t i;
    uint64_t id;
    ret = reap(s);
    if (ret < 0 || next_framework_frame(&i, &id) != 0) continue;
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
  mtl_session_stop(&s, 1, MTL_STOP_DRAIN,
                   MTL_SEC(1)); /* every accepted unit gets a result */
  reap(s);
  return mtl_session_close(s, MTL_SEC(1)); /* 0: retired, the arena may be freed */
}
