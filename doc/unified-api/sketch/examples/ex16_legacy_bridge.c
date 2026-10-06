/* ex16 — MS1 on a NIC: ex01's sender on a VF that the legacy mtl_init() opened.
   Until MS2a mtl_instance_open() opens null: and kernel: ports only, so a VF is opened
   with mtl_init() and wrapped (mtl_legacy.h). The wrapper runs on the legacy clock: the
   legacy default is UTC read as TAI, 37 s off a PTP receiver, so this program installs
   ptp_get_time_fn on CLOCK_TAI, as the FFmpeg plugin does (the host needs ptp4l and
   phc2sys). That function runs on MTL's tasklets. With it mtl_time_now() is -MTL_ENOTSUP
   until MS2a; read CLOCK_TAI, the same clock. Run: ex16 0000:af:01.0 192.168.1.10.
   Needs: MS1 and the legacy headers (mtl_api.h).
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <mtl/experimental/mtl_legacy.h>
#include <mtl_api.h>
#include <time.h>

#include "ex_common.h"

void render(void* addr, uint32_t stride, int64_t frame);

/* Called on MTL's tasklets: a vDSO read, no lock, no allocation. */
static uint64_t tai_now(void* priv) {
  struct timespec ts;
  (void)priv;
  if (clock_gettime(CLOCK_TAI, &ts) < 0) return 0;
  return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

int main(int argc, char** argv) {
  struct mtl_init_params p;
  mtl_handle legacy = NULL;
  mtl_instance_h mt = MTL_NULL(mtl_instance_h);
  mtl_session_h s = MTL_NULL(mtl_session_h);
  struct mtl_session_config sc;
  struct mtl_unit u;
  MTL_INIT(&sc);
  MTL_INIT(&u);
  memset(&p, 0, sizeof(p));
  if (argc < 3) return 2;

  snprintf(p.port[MTL_PORT_P], sizeof(p.port[MTL_PORT_P]), "%s", argv[1]);
  p.pmd[MTL_PORT_P] = mtl_pmd_by_port_name(argv[1]);
  if (inet_pton(AF_INET, argv[2], p.sip_addr[MTL_PORT_P]) != 1) return 2;
  p.num_ports = 1;
  p.tx_queues_cnt[MTL_PORT_P] = 1;
  p.log_level = MTL_LOG_LEVEL_INFO;
  p.ptp_get_time_fn = tai_now; /* without it: UTC read as TAI (MTL_TIMEF_UTC) */

  sc.direction = MTL_TX;
  sc.essence = MTL_VIDEO;
  /* flows[0] is on instance port 0, which is the legacy MTL_PORT_P */
  mtl_flow_ipv4(&sc.flows[0], 239, 168, 85, 20, 20000);
  sc.video.raster.width = 1920;
  sc.video.raster.height = 1080;
  sc.video.raster.fps = mtl_fps_rational(MTL_FPS_59_94);
  sc.video.format = MTL_YUV422_10;

  /* mtl_init() returns NULL on failure (its log says why); mtl_start() comes before the
     first unified start; the wrap returns after the TSC calibration. */
  int ret = -MTL_EIO;
  legacy = mtl_init(&p);
  if (legacy && mtl_start(legacy) == 0) ret = mtl_instance_from_legacy(legacy, &mt);
  if (ret >= 0) ret = mtl_session_open(mt, &sc, &s);

  for (int64_t k = 0; ret >= 0 && g_running;) { /* ex01's loop */
    ret = mtl_tx_acquire(s, &u, MTL_MS(100));
    if (ret == -MTL_EAGAIN) {
      ret = 0;
      continue;
    }
    if (ret == 0) {
      render(u.plane[0].addr, u.plane[0].stride, k++);
      ret = mtl_tx_submit(s, &u);
    }
  }

  if (ret < 0) ex_fail("mtl", ret);
  /* The wrapper first (it never stops the legacy devices), then the legacy instance:
     mtl_uninit() is -EBUSY while a wrapper lives. */
  mtl_session_close(s, MTL_SEC(1));
  mtl_instance_close(mt, MTL_SEC(1));
  if (legacy) mtl_uninit(legacy);
  return ret < 0;
}
