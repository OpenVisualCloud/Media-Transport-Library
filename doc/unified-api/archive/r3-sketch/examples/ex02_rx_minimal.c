/* 10 §4 — minimal RX (P1): library pool, missing data reads as zero. */
#include "ex_common.h"

void show(const void* addr, uint32_t stride); /* the app's display */

int main(void) {
  mtl_instance_h mt;
  int ret = mtl_instance_open_simple("0000:af:01.1=192.168.1.11", &mt);
  if (ret < 0) return ex_fail("open", ret);

  struct mtl_session_config sc;
  mtl_session_config_init(&sc);
  sc.direction = MTL_DIR_RX;
  struct mtl_video_config vc;
  mtl_video_config_init(&vc);
  ret = mtl_flow_parse("239.168.85.20:20000,pt=112", &sc.flows[0]);
  if (ret < 0) return ex_fail("flow", ret);
  vc.width = 1920;
  vc.height = 1080;
  vc.fps = mtl_fps_rational(MTL_FPS_59_94);
  vc.transport_format = MTL_VIDEO_YUV422_10BIT;

  mtl_session_h s;
  ret = mtl_video_session_create(mt, &sc, &vc, &s);
  if (ret < 0) {
    ex_fail("create", ret);
    goto out_instance;
  }
  ret = mtl_session_start(s, NULL); /* IGMP join + flow rule at the first start */
  if (ret < 0) {
    ex_fail("start", ret);
    goto out_session;
  }

  uint64_t units = 0, incomplete = 0;
  while (g_running) {
    mtl_lease_h lease;
    struct mtl_buffer_view v;
    struct mtl_rx_unit u;
    mtl_buffer_view_init(&v);
    ret = mtl_rx_dequeue(s, &lease, &v, &u, sizeof(u), MTL_MS(200));
    /* no signal (also before start: never ESHUTDOWN) */
    if (ret == -MTL_ETIMEDOUT) continue;
    if (ret < 0) {
      ex_fail("dequeue", ret);
      break;
    }
    units++;
    if (u.hdr.status == MTL_RX_INCOMPLETE) incomplete++; /* missing ranges read as zero */
    show(v.plane[0].addr, v.plane[0].stride);
    ret = mtl_rx_release(s, lease);
    if (ret < 0) {
      ex_fail("release", ret);
      break;
    }
  }
  printf("units %llu, incomplete %llu\n", (unsigned long long)units,
         (unsigned long long)incomplete);

  ret = mtl_session_stop(s, MTL_STOP_FLUSH, 0);
  if (ret < 0) ex_fail("stop", ret);
out_session:
  mtl_session_destroy(s, 0);
out_instance:
  mtl_instance_release(mt);
  return ret < 0;
}
