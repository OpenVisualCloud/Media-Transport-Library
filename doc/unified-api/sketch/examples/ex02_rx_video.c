/* ex02 — a video receiver on two ST 2022-7 legs: dequeue, read, release. Needs: MS1. */
#include "ex_common.h"

void show(const void* addr, uint32_t stride, int complete, int64_t media_index);

int rx_video(mtl_instance_h mt) {
  mtl_session_h s = MTL_NULL(mtl_session_h);
  struct mtl_session_config sc;
  struct mtl_unit u;
  MTL_INIT(&sc);
  MTL_INIT(&u);

  sc.direction = MTL_RX;
  sc.essence = MTL_VIDEO;
  /* two flows = two legs on instance ports 0 and 1 (two ports, e.g.
     MTL_PORTS=null:1,null:2); packets merge by sequence */
  mtl_flow_ipv4(&sc.flows[0], 239, 168, 85, 20, 20000);
  mtl_flow_ipv4(&sc.flows[1], 239, 168, 86, 20, 20000);
  sc.video.raster.width = 1920;
  sc.video.raster.height = 1080;
  sc.video.raster.fps = mtl_fps_rational(MTL_FPS_59_94);
  sc.video.format = MTL_YUV422_10;
  int ret = mtl_session_open(mt, &sc, &s);

  while (ret >= 0) {
    ret = mtl_rx_dequeue(s, &u, MTL_MS(100));
    if (ret == -MTL_EAGAIN) { /* no frame for 100 ms: no signal (status.flags) */
      ret = 0;
      continue;
    }
    if (ret < 0) break;                         /* -MTL_ECANCELED: the interrupt */
    int complete = u.status == MTL_RX_COMPLETE; /* else lost packets read as zero */
    int64_t index = (u.flags & MTL_UNITF_INDEX_VALID) ? u.media_index : -1;
    show(u.plane[0].addr, u.plane[0].stride, complete, index);
    ret = mtl_rx_release(s, u.lease);
  }

  if (ret != -MTL_ECANCELED) ex_fail("rx", ret);
  mtl_session_close(s, 0); /* 1 while a frame is still out: it finishes by itself */
  return ret == -MTL_ECANCELED ? 0 : ret;
}
