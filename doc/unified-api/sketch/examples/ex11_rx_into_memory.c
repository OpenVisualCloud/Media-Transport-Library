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

  while (ret >= 0 && (ret = mtl_rx_dequeue(s, &u, MTL_FOREVER)) == 0) {
    if (u.flags & MTL_UNITF_INDEX_VALID) /* u.slot == media_index mod GRAINS */
      mxl_commit_grain(u.slot, u.media_index, u.status == MTL_RX_COMPLETE);
    ret = mtl_rx_release(s, u.lease);
  }

  if (ret != -MTL_ECANCELED) ex_fail("mxl", ret);
  mtl_session_close(s, MTL_FOREVER); /* once it returns, MXL may reuse the ring */
  return ret == -MTL_ECANCELED ? 0 : ret;
}

#define FRAMES 64 /* the recorder's arena, about 1 s at 59.94 */
#define AHEAD 4   /* destinations MTL holds: the session's pool_count */

/* A recorder: s is created with MTL_SESSION_POOL_ATTACHED, pool_count AHEAD and no slot.
   Each unit lands in a destination the recorder handed over, and its address says which.
   Lost packets are never zero-filled here: the unit is MTL_RX_INCOMPLETE. */
int record(mtl_instance_h mt, mtl_session_h s, uint64_t frame_bytes) {
  struct mtl_mem_desc d;
  struct mtl_attach dest;
  struct mtl_unit u;
  void* va = NULL;
  uint64_t next = 0; /* the offset of the next destination */
  MTL_INIT(&d);
  MTL_INIT(&dest);
  MTL_INIT(&u);
  d.length = FRAMES * frame_bytes;
  d.flags = MTL_MEM_WRITE | MTL_MEM_MAP_ALL; /* a destination must be mapped already */
  int ret = mtl_mem_alloc(mt, &d, &dest.region, &va);
  dest.count = 1;
  for (int i = 0; ret >= 0 && i < AHEAD; i++) {
    dest.offset = next;
    ret = mtl_rx_provide(s, &dest);
    next = (next + frame_bytes) % d.length;
  }
  if (ret >= 0) ret = mtl_session_start(&s, 1, NULL, NULL);

  while (ret >= 0 && (ret = mtl_rx_dequeue(s, &u, MTL_FOREVER)) == 0) {
    uint64_t off = (uint64_t)((uint8_t*)u.plane[0].addr - (uint8_t*)va);
    ret = disk_write(off, u.media_index, u.status == MTL_RX_COMPLETE);
    int r = mtl_rx_release(s, u.lease);
    if (ret >= 0) ret = r;
    dest.offset = next; /* one came back: hand over the next */
    if (ret >= 0) ret = mtl_rx_provide(s, &dest);
    next = (next + frame_bytes) % d.length;
  }

  if (ret != -MTL_ECANCELED) ex_fail("record", ret);
  mtl_session_close(s, MTL_FOREVER); /* hands every destination back */
  if (mtl_mem_close(dest.region) == MTL_RETIRING) ex_fail("arena", -MTL_EBUSY);
  return ret == -MTL_ECANCELED ? 0 : ret;
}
