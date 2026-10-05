/* ex10 — a time-preserving processor: video, audio and ANC in, transformed, out, each
   with the input's media time. Output sessions use media_mode TAI with min_tx_delay_ns =
   the pipeline budget: one frame period (RX completion) + processing + the pick-up lead,
   so the delay is fixed and, for a compliant input, every output RTP equals the input
   RTP. A received unit is a valid send
   template: its RX-only flags are ignored. */
#include <mtl/experimental/mtl_util.h>

#include "ex_common.h"

void transform(const struct mtl_unit* in, struct mtl_unit* out);

int process_video(mtl_session_h rx, mtl_session_h tx) {
  struct mtl_unit in, out;
  MTL_INIT(&in);
  MTL_INIT(&out);
  int ret = mtl_rx_dequeue(rx, &in, MTL_MS(50));
  if (ret < 0) return ret;
  if (!(in.flags &
        MTL_UNITF_TAI_VALID)) { /* a mediaclk:sender input has no TAI relation */
    mtl_rx_release(rx, in.lease);
    return -MTL_EINVAL;
  }
  ret = mtl_tx_acquire(tx, &out, MTL_MS(10));
  if (ret == 0) {
    transform(&in, &out);
    out.media_tai_ns = in.media_tai_ns; /* the input's media time, not "now" */
    ret = mtl_tx_submit(tx, &out);
  }
  int r = mtl_rx_release(rx, in.lease);
  return ret < 0 ? ret : r;
}

/* Audio and ANC: the copy path takes the received unit as the template (media time, and
   for ANC the packet table in its meta area). */
int pass_through(mtl_session_h rx, mtl_session_h tx) {
  struct mtl_unit in;
  MTL_INIT(&in);
  int ret = mtl_rx_dequeue(rx, &in, MTL_MS(20));
  if (ret < 0) return ret;
  ret = mtl_tx_write(tx, in.plane[0].addr, in.used, &in, MTL_MS(10));
  int r = mtl_rx_release(rx, in.lease);
  return ret < 0 ? ret : r;
}
