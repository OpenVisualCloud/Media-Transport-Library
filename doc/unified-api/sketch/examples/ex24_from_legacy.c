/* ex24 — from the legacy API: a legacy program's st20p_tx_ops become a unified config,
   and its instance from mtl_init() is wrapped, so ex01's loop runs on it (MS1's way onto
   a VF). The wrapper runs on the legacy clock, here CLOCK_TAI. Run: ex24 0000:af:01.0
   192.168.1.10. Needs: MS1 and the legacy headers (mtl_api.h, st_pipeline_api.h). */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <mtl/experimental/mtl_legacy.h>
#include <mtl_api.h>
#include <st_pipeline_api.h>
#include <time.h>

#include "ex_common.h"

void render(void* addr, uint32_t stride, int64_t frame);
void legacy_ops(struct st20p_tx_ops* ops); /* the ops it passes to st20p_tx_create */

/* Called on MTL's tasklets (a vDSO read, no lock); without it the legacy default is UTC
   read as TAI, 37 s off PTP time, and mtl_time_now() is -MTL_ENOTSUP until MS2a. */
static uint64_t tai_now(void* priv) {
  struct timespec ts;
  (void)priv;
  if (clock_gettime(CLOCK_TAI, &ts) < 0) return 0;
  return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* The ops field by field, every enum through its converter (copied by the wrong rule, a
   value is still valid and changes the wire); rtp_timestamp_delta_us: tx.rtp_trim_ns. */
static int config_from_legacy(const struct st20p_tx_ops* ops,
                              struct mtl_session_config* sc) {
  MTL_INIT(sc);
  sc->direction = MTL_TX;
  sc->essence = MTL_VIDEO;
  for (uint32_t i = 0; i < ops->port.num_port && i < MTL_MAX_LEGS; i++) {
    const uint8_t* ip = ops->port.dip_addr[i]; /* leg i on instance port i, P first */
    mtl_flow_ipv4(&sc->flows[i], ip[0], ip[1], ip[2], ip[3], ops->port.udp_port[i]);
    sc->flows[i].udp_src_port = ops->port.udp_src_port[i];
  }
  sc->payload_type = ops->port.payload_type;
  sc->ssrc = ops->port.ssrc;
  sc->pool_count = ops->framebuff_cnt;
  sc->video.raster.width = ops->width;
  sc->video.raster.height = ops->height;
  int ret = mtl_raster_from_legacy(ops->fps, ops->interlaced, &sc->video.raster);
  if (ret >= 0) ret = mtl_video_format_from_legacy(ops->transport_fmt, &sc->video.format);
  if (ret >= 0) ret = mtl_app_format_from_legacy(ops->input_fmt, &sc->video.app_format);
  if (ret >= 0) ret = mtl_packing_from_legacy(ops->transport_packing, &sc->video.packing);
  if (ret >= 0)
    ret = mtl_sender_type_from_legacy(ops->transport_pacing, &sc->video.sender_type);
  return ret;
}

int main(int argc, char** argv) {
  struct mtl_init_params p;
  struct st20p_tx_ops ops;
  mtl_handle legacy = NULL;
  mtl_instance_h mt = MTL_NULL(mtl_instance_h);
  mtl_session_h s = MTL_NULL(mtl_session_h);
  struct mtl_session_config sc;
  struct mtl_unit u;
  MTL_INIT(&u);
  memset(&p, 0, sizeof(p));
  memset(&ops, 0, sizeof(ops));
  if (argc < 3) return 2;

  snprintf(p.port[MTL_PORT_P], sizeof(p.port[MTL_PORT_P]), "%s", argv[1]);
  p.pmd[MTL_PORT_P] = mtl_pmd_by_port_name(argv[1]);
  if (inet_pton(AF_INET, argv[2], p.sip_addr[MTL_PORT_P]) != 1) return 2;
  p.num_ports = 1;
  p.tx_queues_cnt[MTL_PORT_P] = 1;
  p.log_level = MTL_LOG_LEVEL_INFO;
  p.ptp_get_time_fn = tai_now; /* without it: UTC read as TAI (MTL_TIMEF_UTC) */
  legacy_ops(&ops);

  /* mtl_start() before the first unified start; the wrap returns after TSC calibration */
  int ret = config_from_legacy(&ops, &sc);
  if (ret >= 0) {
    ret = -MTL_EIO;
    legacy = mtl_init(&p);
    if (legacy && mtl_start(legacy) == 0) ret = mtl_instance_from_legacy(legacy, &mt);
  }
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
  /* the wrapper first (it never stops the legacy devices): mtl_uninit() before, -EBUSY */
  mtl_session_close(s, MTL_SEC(1));
  mtl_instance_close(mt, MTL_SEC(1));
  if (legacy) mtl_uninit(legacy);
  return ret < 0;
}
