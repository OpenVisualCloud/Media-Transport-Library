/* ex09 — zero-copy 1 -> 4 forwarder: each 2160p frame received feeds four 1080p senders,
   one per quadrant. Every TX pool lies over the RX pool (any stride >= row_bytes is
   direct), and each TX unit holds the RX unit it reads until its own result. */
#include <mtl/experimental/mtl_util.h>

#include "ex_common.h"

#define QUADS 4

/* rx: a created 2160p RX session (library pool). tx[q]: created 1080p TX sessions with
   MTL_SESSION_POOL_ATTACHED, media_mode TAI and min_tx_delay_ns = one frame period plus
   the pick-up lead (a frame exists only once it is received). */
static int attach_quadrants(mtl_session_h rx, const mtl_session_h* tx) {
  struct mtl_session_info ri;
  struct mtl_unit slot0;
  mtl_region_h pool;
  MTL_INIT(&slot0);
  int ret = mtl_session_get_info(rx, &ri, sizeof(ri));
  if (ret >= 0) ret = mtl_session_get_slot(rx, 0, &slot0);
  if (ret >= 0) ret = mtl_session_get_pool_region(rx, &pool);
  const uint32_t stride = slot0.plane[0].stride, half_row = slot0.plane[0].row_bytes / 2;
  for (uint32_t q = 0; ret >= 0 && q < QUADS; q++) {
    struct mtl_attach a;
    MTL_INIT(&a);
    a.region = pool; /* library memory: holds, not results, keep it safe */
    a.count = ri.pool_count;
    a.pitch = ri.pool_slot_pitch; /* TX slot j lies over RX slot j */
    a.offset =
        (uint64_t)(q / 2) * (slot0.plane[0].rows / 2) * stride + (q % 2) * half_row;
    a.stride[0] = stride;
    ret = mtl_session_attach(tx[q], &a);
  }
  return ret;
}

int split_forward(mtl_session_h rx, mtl_session_h* tx) {
  struct mtl_unit in, how;
  MTL_INIT(&in);
  MTL_INIT(&how);
  int ret = attach_quadrants(rx, tx);
  for (uint32_t q = 0; ret >= 0 && q < QUADS; q++)
    ret = mtl_session_start(&tx[q], 1, NULL, NULL); /* one each: start arrays are MS6 */
  if (ret >= 0) ret = mtl_session_start(&rx, 1, NULL, NULL);

  while (ret >= 0 && g_running) {
    ret = mtl_rx_dequeue(rx, &in, MTL_MS(50));
    if (ret == -MTL_EAGAIN) {
      ret = 0;
      continue;
    }
    if (ret < 0) break;
    how.media_tai_ns = in.media_tai_ns; /* derived RTP = input RTP if it was compliant */
    how.hold = in.lease;                /* the RX slot stays until each TX unit is sent */
    for (uint32_t q = 0; (in.flags & MTL_UNITF_TAI_VALID) && q < QUADS; q++)
      mtl_tx_send_slot(tx[q], in.slot, &how, 0); /* -MTL_EAGAIN: still in flight, drop */
    ret = mtl_rx_release(rx, in.lease); /* the slot is free once every hold completed */
  }

  if (ret < 0) ex_fail("forward", ret);
  mtl_session_stop(&rx, 1, MTL_STOP_FLUSH, 0);
  mtl_session_stop(tx, QUADS, MTL_STOP_DRAIN, MTL_MS(100)); /* holds end with the sends */
  return ret;
}
