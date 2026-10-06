/* ex10 — a time-preserving processor: video, audio and ANC in, transformed, out, each
   with the input's media time. Output sessions use media_mode TAI with min_tx_delay_ns =
   the pipeline budget: one frame period (RX completion) + processing + the pick-up lead,
   so the delay is fixed and, for a compliant input, every output RTP equals the input
   RTP. A received unit is a valid send template: its RX-only flags are ignored. Needs:
   MS4a (process_video alone: MS1). */
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

/* Audio: the copy path takes the received unit as the template (its media time). */
int pass_audio(mtl_session_h rx, mtl_session_h tx) {
  struct mtl_unit in;
  MTL_INIT(&in);
  int ret = mtl_rx_dequeue(rx, &in, MTL_MS(20));
  if (ret < 0) return ret;
  ret = mtl_tx_write(tx, in.plane[0].addr, in.used, &in, MTL_MS(10));
  int r = mtl_rx_release(rx, in.lease);
  return ret < 0 ? ret : r;
}

/* ANC: every received entry is a valid TX entry (RX marks the first entry of each RTP
   packet MTL_ANCF_NEW_RTP, so the boundaries stay as they arrived). Both sessions have
   the same word mode; with 8-bit words RX has already skipped what the mode cannot carry.
 */
int pass_anc(mtl_session_h rx, mtl_session_h tx) {
  struct mtl_unit in, out;
  MTL_INIT(&in);
  MTL_INIT(&out);
  int ret = mtl_rx_dequeue(rx, &in, MTL_MS(20));
  if (ret < 0) return ret;
  ret = mtl_tx_acquire(tx, &out, MTL_MS(10));
  if (ret == 0) {
    const struct mtl_anc_packet* t = mtl_anc_table(&in);
    for (uint32_t i = 0; i < in.used && ret == 0; i++)
      ret = mtl_anc_put(&out, &t[i], mtl_anc_words(&in, &t[i]),
                        (size_t)t[i].udw_count * in.plane[1].stride,
                        mtl_anc_raw_hdr(&in, i));
    mtl_unit_from_template(&out, &in); /* the input's media time */
    if (ret == 0)
      ret = mtl_tx_submit(tx, &out);
    else
      mtl_tx_release(tx, out.lease);
  }
  int r = mtl_rx_release(rx, in.lease);
  return ret < 0 ? ret : r;
}
