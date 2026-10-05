/* ex06 — receive into an MXL ring: unit k lands in grain k mod GRAINS, so the reader
   finds frame k at a known place. The ring is the session's pool (app memory). Every
   session runs on the SMPTE epoch: k counts frames since 1970 TAI, the same in every
   process. Needs: MS2b. */
#include <mtl/experimental/mtl_mem.h>

#include "ex_common.h"

#define GRAINS 8
extern void* ring;          /* GRAINS grains in shared memory, page aligned */
extern uint64_t grain_size; /* bytes between grains */
void mxl_commit_grain(uint32_t grain, int64_t media_index, int complete);

int mxl_bridge(mtl_instance_h mt) {
  mtl_session_h s = MTL_NULL(mtl_session_h);
  struct mtl_session_config sc;
  struct mtl_attach a;
  struct mtl_unit u;
  MTL_INIT(&sc);
  MTL_INIT(&a);
  MTL_INIT(&u);

  int ret = 0;
  sc.direction = MTL_RX;
  sc.essence = MTL_VIDEO;
  mtl_flow_ipv4(&sc.flows[0], 239, 168, 85, 20, 20000);
  sc.video.raster.width = 1920;
  sc.video.raster.height = 1080;
  sc.video.raster.fps = mtl_fps_rational(MTL_FPS_59_94);
  sc.video.format = MTL_YUV422_10;
  sc.flags = MTL_SESSION_POOL_ATTACHED | MTL_SESSION_RX_BY_INDEX | MTL_SESSION_RX_LATEST;
  sc.pool_count = GRAINS;
  a.va = ring;
  a.length = (uint64_t)GRAINS * grain_size;
  a.count = GRAINS;
  a.pitch = grain_size;
  if (ret >= 0) ret = mtl_session_create(mt, &sc, &s);
  if (ret >= 0) ret = mtl_session_attach(s, &a);
  if (ret >= 0) ret = mtl_session_start(&s, 1, NULL, NULL);

  while (ret >= 0 && g_running) {
    ret = mtl_rx_dequeue(s, &u, MTL_MS(100));
    if (ret == 0) {
      if (u.flags & MTL_UNITF_INDEX_VALID) /* u.slot == media_index mod GRAINS */
        mxl_commit_grain(u.slot, u.media_index, u.status == MTL_RX_COMPLETE);
      ret = mtl_rx_release(s, u.lease);
    } else if (ret == -MTL_EAGAIN) {
      ret = 0;
    }
  }

  if (ret < 0) ex_fail("mxl", ret);
  /* 1: still retiring; poll until 0, then MXL may reuse the ring */
  while (mtl_session_close(s, MTL_SEC(1)) == 1) {
  }
  return ret;
}
