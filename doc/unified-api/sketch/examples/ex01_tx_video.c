/* ex01 — the smallest video sender: describe the stream, open it, then acquire, draw and
   submit one frame at a time. Needs: MS1. */
#include "ex_common.h"

void render(void* addr, uint32_t stride, int64_t frame);

int main(void) {
  mtl_instance_h mt = MTL_NULL(mtl_instance_h);
  mtl_session_h s = MTL_NULL(mtl_session_h);
  struct mtl_session_config sc;
  struct mtl_unit u;
  MTL_INIT(&sc);
  MTL_INIT(&u);

  sc.direction = MTL_TX;
  sc.essence = MTL_VIDEO;
  mtl_flow_ipv4(&sc.flows[0], 239, 168, 85, 20, 20000);
  sc.video.raster.width = 1920;
  sc.video.raster.height = 1080;
  sc.video.raster.fps = mtl_fps_rational(MTL_FPS_59_94);
  sc.video.format = MTL_YUV422_10;

  int ret = mtl_instance_open(NULL, &mt); /* ports from MTL_PORTS, e.g. "null:1" */
  if (ret < 0) {
    ex_fail("instance", ret);
    return 1;
  }
  ex_install_interrupt(mt);
  ret = mtl_session_open(mt, &sc, &s); /* create and start */

  for (int64_t k = 0; ret >= 0; k++) {
    ret = mtl_tx_acquire(s, &u, MTL_FOREVER); /* waits while every frame is queued */
    if (ret == 0) {
      render(u.plane[0].addr, u.plane[0].stride, k);
      ret = mtl_tx_submit(s, &u); /* sent at the next frame time */
    }
  }

  if (ret != -MTL_ECANCELED) ex_fail("mtl", ret); /* -MTL_ECANCELED: the interrupt */
  mtl_session_close(s, MTL_SEC(1));               /* sends what is queued, then retires */
  mtl_instance_close(mt, MTL_SEC(1));             /* leaves groups, stops the devices */
  return ret != -MTL_ECANCELED;
}
