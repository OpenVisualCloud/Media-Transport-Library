/* 10 §12 — time-preserving processor (ST 2110-10 §7.9): media_mode TAI with the input's
   media time, source_kind CAPTURE, min_tx_delay_ns = the pipeline budget. The slot delay
   L is fixed, the derived RTP equals the input RTP when the input was compliant, and ANC
   gets the video's RTP because it carries the same media time. PASSTHROUGH + AUTO is
   rejected (-MTL_EINVAL, reason PASSTHROUGH_AUTO). */
#include "ex_common.h"

void transform(const struct mtl_buffer_view* in, const struct mtl_buffer_view* out);

void processor_timing(struct mtl_session_config* sc, int64_t pipeline_budget_ns) {
  sc->timing.media_mode = MTL_MEDIA_TAI;
  sc->timing.source_kind = MTL_SOURCE_CAPTURE; /* snap_mode 0 = NEAREST */
  /* fixed L: one bound, not 2-or-3 slots */
  sc->timing.min_tx_delay_ns = pipeline_budget_ns;
}

int process_video(mtl_session_h rx, mtl_session_h tx) {
  mtl_lease_h in, out;
  struct mtl_buffer_view vin, vout;
  struct mtl_rx_unit u;
  mtl_buffer_view_init(&vin);
  mtl_buffer_view_init(&vout);
  int ret = mtl_rx_dequeue(rx, &in, &vin, &u, sizeof(u), MTL_MS(50));
  if (ret < 0) return ret;
  /* a mediaclk:sender input has no TAI relation */
  if (!(u.hdr.time_valid & MTL_RT_MEDIA)) {
    mtl_rx_release(rx, in);
    return -MTL_EINVAL;
  }
  ret = mtl_tx_acquire(tx, &out, &vout, NULL, MTL_MS(10));
  if (ret == 0) {
    transform(&vin, &vout);
    struct mtl_tx_submission sub;
    mtl_tx_submission_init(&sub);
    sub.media_tai_ns = u.timing.media_tai_ns; /* the input's media time, not "now" */
    ret = mtl_tx_submit(tx, out, &sub);
    if (ret < 0) mtl_tx_release(tx, out);
  }
  int r = mtl_rx_release(rx, in);
  return ret < 0 ? ret : r;
}

/* Audio passthrough: the first sample keeps its media time; later packets follow at +S. A
   first sample off the output packet grid is re-phased with a DISCONTINUITY. */
int pass_audio(mtl_session_h rx, mtl_session_h tx) {
  mtl_lease_h in;
  struct mtl_buffer_view v;
  struct mtl_rx_unit u;
  mtl_buffer_view_init(&v);
  int ret = mtl_rx_dequeue(rx, &in, &v, &u, sizeof(u), MTL_MS(20));
  if (ret < 0) return ret;
  struct mtl_tx_submission sub;
  mtl_tx_submission_init(&sub);
  sub.media_tai_ns = u.timing.media_tai_ns;
  sub.sample_count = u.sample_count;
  ret = mtl_tx_write(tx, v.plane[0].addr, u.valid_bytes, &sub, MTL_MS(10));
  int r = mtl_rx_release(rx, in);
  return ret < 0 ? ret : r;
}

/* ANC passthrough: UDW run in plane 0, packet table in the meta area. */
int pass_anc(mtl_session_h rx, mtl_session_h tx) {
  mtl_lease_h in;
  struct mtl_buffer_view v;
  struct mtl_rx_unit u;
  mtl_buffer_view_init(&v);
  int ret = mtl_rx_dequeue(rx, &in, &v, &u, sizeof(u), MTL_MS(20));
  if (ret < 0) return ret;
  struct mtl_tx_submission sub;
  mtl_tx_submission_init(&sub);
  sub.media_tai_ns = u.timing.media_tai_ns; /* same media time as its video: same RTP */
  sub.anc = (const struct mtl_anc_packet*)v.meta;
  sub.anc_count = u.anc_count;
  ret = mtl_tx_write(tx, v.plane[0].addr, u.valid_bytes, &sub, MTL_MS(10));
  int r = mtl_rx_release(rx, in);
  return ret < 0 ? ret : r;
}
