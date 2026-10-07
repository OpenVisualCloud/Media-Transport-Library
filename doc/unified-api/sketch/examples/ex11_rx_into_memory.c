/* ex11 — receive into your memory: an MXL ring where frame k lands in grain k mod GRAINS,
   or a recorder that names each frame's place as it goes. Needs: MS2b. */
#include <mtl/experimental/mtl_mem.h>

#include "ex_common.h"

#define GRAINS 8
extern void* ring;          /* GRAINS grains in shared memory, page aligned */
extern uint64_t grain_size; /* bytes between grains */
void mxl_commit_grain(uint32_t grain, int64_t media_index, int complete);
/* the disk writer: takes the frame at offset; 0 once the bytes are its own */
int disk_write(uint64_t offset, int64_t media_index, int complete);

int mxl_bridge(mtl_instance_h mt) {
  mtl_session_h s = MTL_NULL(mtl_session_h);
  struct mtl_session_config sc;
  struct mtl_attach a;
  struct mtl_unit u;
  MTL_INIT(&sc);
  MTL_INIT(&a);
  MTL_INIT(&u);

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
  int ret = mtl_session_create(mt, &sc, &s);
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
  /* MTL_RETIRING: poll until 0, then MXL may reuse the ring */
  while (mtl_session_close(s, MTL_SEC(1)) == MTL_RETIRING) {
  }
  return ret;
}

#define FRAMES 64 /* the recorder's arena, about 1 s at 59.94 */

/* A recorder: s is created with MTL_SESSION_POOL_ATTACHED, pool_count 4 and no slot; each
   unit lands at the cursor the recorder handed over, named by its cookie (never 0). */
int record(mtl_instance_h mt, mtl_session_h s, uint64_t frame_bytes) {
  struct mtl_mem_desc d;
  struct mtl_attach one;
  struct mtl_unit u;
  MTL_ADDR(void) va;
  uint64_t cursor = 0;
  MTL_INIT(&d);
  MTL_INIT(&one);
  MTL_INIT(&u);
  d.length = FRAMES * frame_bytes;
  d.flags = MTL_MEM_WRITE | MTL_MEM_MAP_ALL; /* a destination must be mapped already */
  int ret = mtl_mem_alloc(mt, &d, &one.region, &va);
  one.count = 1;
  for (int ready = 0; ret >= 0 && g_running;) {
    for (; ret >= 0 && ready < 4; ready++, cursor = (cursor + frame_bytes) % d.length) {
      one.offset = cursor;
      one.cookie = cursor + 1;
      ret = mtl_rx_provide(s, &one); /* -MTL_ENOSPC: pool_count held */
    }
    if (ret >= 0 && mtl_session_get_state(s) == MTL_STATE_CREATED)
      ret = mtl_session_start(&s, 1, NULL, NULL);
    if (ret >= 0) ret = mtl_rx_dequeue(s, &u, MTL_MS(100));
    if (ret == 0) { /* never zero-filled: lost packets show as MTL_RX_INCOMPLETE */
      ready--;
      ret = disk_write(u.cookie - 1, u.media_index, u.status == MTL_RX_COMPLETE);
      int r = mtl_rx_release(s, u.lease);
      if (ret >= 0) ret = r;
    } else if (ret == -MTL_EAGAIN) {
      ret = 0;
    }
  }
  while (mtl_session_close(s, MTL_SEC(1)) == MTL_RETIRING) { /* hands every one back */
  }
  if (mtl_mem_close(one.region) == MTL_RETIRING) ex_fail("arena", -MTL_EBUSY);
  return ret;
}
