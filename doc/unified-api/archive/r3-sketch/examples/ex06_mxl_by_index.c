/* 10 §8 — MXL grains: RX unit k lands in buffer k mod N (MTL_RX_SLOT_BY_INDEX). */
#include "ex_common.h"

#define GRAINS 8

/* The MXL flow: GRAINS grains in one shared-memory ring, already imported as `ring`
   (access 0 or MTL_MEM_WRITE: RX writes), one attached buffer per grain in grain order.
 */
void mxl_commit_grain(uint32_t grain, int64_t media_index, uint32_t valid);

int mxl_bridge(mtl_instance_h mt, const struct mtl_video_config* vc,
               const mtl_buffer_h* grain_buf) {
  struct mtl_session_config sc;
  mtl_session_config_init(&sc);
  sc.direction = MTL_DIR_RX;
  int ret = mtl_flow_parse("239.168.85.20:20000", &sc.flows[0]);
  if (ret < 0) return ex_fail("flow", ret);
  sc.pool.source = MTL_POOL_ATTACHED;
  /* sizing: > downstream hold + 1 RECEIVING + ceil(skew / period) */
  sc.pool.count = GRAINS;
  sc.pool.rx_slot_select = MTL_RX_SLOT_BY_INDEX;
  sc.pool.rx_overflow = MTL_RX_RECLAIM_OLDEST_READY; /* reclaims only the target slot */
  sc.timing.timeline = mtl_timeline_epoch(mt); /* index = frames since the SMPTE epoch */

  mtl_session_h s;
  ret = mtl_video_session_create(mt, &sc, vc, &s);
  if (ret < 0) return ex_fail("create", ret);
  ret = mtl_session_attach_buffers(s, grain_buf, GRAINS);
  if (ret >= 0) ret = mtl_session_start(s, NULL);
  if (ret < 0) {
    ex_fail("setup", ret);
    mtl_session_destroy(s, 0);
    return ret;
  }

  while (g_running) {
    mtl_lease_h lease;
    struct mtl_rx_unit u;
    /* no view needed */
    ret = mtl_rx_dequeue(s, &lease, NULL, &u, sizeof(u), MTL_MS(100));
    if (ret == -MTL_ETIMEDOUT) continue;
    if (ret < 0) {
      ex_fail("dequeue", ret);
      break;
    }
    /* == media_index mod GRAINS */
    uint32_t grain = mtl_buffer_index(mtl_lease_buffer(lease));
    if (u.hdr.time_valid & MTL_RT_MEDIA)
      mxl_commit_grain(grain, u.timing.media_index, u.hdr.status == MTL_RX_COMPLETE);
    ret = mtl_rx_release(s, lease);
    if (ret < 0) {
      ex_fail("release", ret);
      break;
    }
  }
  mtl_session_stop(s, MTL_STOP_FLUSH, 0);
  return mtl_session_destroy(s, 0);
}
