/* 10 §3 — minimal TX (P1): library pool, completion NONE, media mode AUTO. */
#include "ex_common.h"

void render_yuv422_10(void* addr, uint32_t stride); /* the app's renderer */

int main(void) {
  mtl_instance_h mt;
  int ret = mtl_instance_open_simple("0000:af:01.0=192.168.1.10", &mt);
  if (ret < 0) return ex_fail("open", ret);

  struct mtl_session_config sc;
  mtl_session_config_init(&sc);
  sc.direction = MTL_DIR_TX;
  struct mtl_video_config vc;
  mtl_video_config_init(&vc);
  ret = mtl_flow_parse("239.168.85.20:20000,pt=112", &sc.flows[0]);
  if (ret < 0) return ex_fail("flow", ret);
  vc.width = 1920;
  vc.height = 1080;
  vc.fps = mtl_fps_rational(MTL_FPS_59_94);
  vc.transport_format = MTL_VIDEO_YUV422_10BIT;
  /* pool.count 0 = default; library pool; completion NONE: nothing has to be read */

  mtl_session_h s;
  ret = mtl_video_session_create(mt, &sc, &vc, &s);
  if (ret < 0) {
    ex_fail("create", ret);
    goto out_instance;
  }
  ret = mtl_session_start(s, NULL);
  if (ret < 0) {
    ex_fail("start", ret);
    goto out_session;
  }

  while (g_running) {
    mtl_lease_h lease;
    struct mtl_buffer_view v;
    mtl_buffer_view_init(&v);
    ret = mtl_tx_acquire(s, &lease, &v, NULL, MTL_MS(100));
    if (ret == -MTL_ETIMEDOUT) continue; /* back-pressure; blocked_on says why */
    if (ret < 0) {                       /* -MTL_ECANCELED, -MTL_EIO (ERROR), ... */
      ex_fail("acquire", ret);
      break;
    }
    /* one plane: transport format */
    render_yuv422_10(v.plane[0].addr, v.plane[0].stride);
    ret = mtl_tx_submit(s, lease, NULL);
    if (ret < 0) { /* the lease is still ours: give it back, never leak it */
      ex_fail("submit", ret);
      mtl_tx_release(s, lease);
      break;
    }
  }

  ret = mtl_session_stop(s, MTL_STOP_DRAIN, MTL_SEC(1));
  if (ret < 0 && ret != -MTL_ETIMEDOUT) ex_fail("stop", ret);
out_session:
  mtl_session_destroy(s, 0);
out_instance:
  mtl_instance_release(mt);
  return ret < 0;
}
