/* ex23 — a processor that keeps the input's timing: video, audio and ANC in, processed,
   out, each with the input's media time and a fixed delay. Needs: MS6 (audio in TAI mode;
   process_video alone MS4, pass_anc MS4a2). */
#include <mtl/experimental/mtl_format.h>
#include <mtl/experimental/mtl_util.h>

#include "ex_common.h"

void count(const char* what, int64_t n);

/* The output of one input: TAI mode, and min_tx_delay_ns = the pipeline budget: the
   input's delivery after its media time (rx's latency_min_ns, for a sender that launches
   at its media time), the input's own launch delay (input_launch_ns: whole frames, 0 for
   playback, one for a camera or another processor; its SDP's TSDELAY less its TROFFSET,
   or ex09's measure), the processing, and the pick-up lead. For a compliant input every
   output RTP equals the input's. tsmode: SAMP when the input's SDP says SAMP, else PRES;
   an input off the grid is re-stamped, or keeps its phase with MTL_SUBMIT_RTP_TS and
   out.rtp = in.rtp (MS3). */
int open_output(mtl_instance_h mt, mtl_session_h rx, int64_t input_launch_ns,
                int64_t process_ns, struct mtl_session_config* sc, mtl_session_h* tx) {
  struct mtl_session_info info;
  int ret = mtl_session_get_info(rx, &info, sizeof(info));
  if (ret < 0) return ret;
  sc->media_mode = MTL_MEDIA_TAI;
  if (!sc->tsmode) sc->tsmode = MTL_TSMODE_PRES;
  sc->flags |= MTL_SESSION_RESULTS;
  sc->min_tx_delay_ns = info.latency_min_ns + input_launch_ns + process_ns + MTL_US(500);
  ret = mtl_session_open(mt, sc, tx);
  return ret < 0 ? ex_fail("output", ret) : 0;
}

/* Each frame's outcome: ON_TIME (margin_ns), or DROPPED with TOO_LATE, SNAP_COLLISION,
   WAITING_NEIGHBOUR, LINK_DOWN or RECOVERY. */
static int reap(mtl_session_h s) {
  struct mtl_tx_result r[8];
  int n;
  while ((n = mtl_tx_reap(s, r, 8, 0)) > 0)
    for (int i = 0; i < n; i++)
      count(r[i].status == MTL_TX_ON_TIME ? "margin ns" : mtl_reason_name(r[i].reason),
            r[i].status == MTL_TX_ON_TIME ? r[i].margin_ns : 1);
  return n == -MTL_EAGAIN ? 0 : n;
}

/* Every path ends here: the input goes back and the output's results are read (unread
   results would block its acquire). A unit with no output slot in time is lost. */
static int done(mtl_session_h rx, mtl_lease_h in, mtl_session_h tx, int ret) {
  int r = mtl_rx_release(rx, in);
  if (ret == -MTL_EAGAIN) count("output full: unit lost", 1);
  if (ret >= 0 || ret == -MTL_EAGAIN) ret = reap(tx);
  return ret < 0 ? ret : r;
}

/* Video: here a UHD-to-HD down-converter (mtl_convert, half scale, in this thread: its
   time is in the budget); a GPU or AI stage takes its place. rx: 2160p, tx: 1080p. */
int process_video(mtl_session_h rx, mtl_session_h tx) {
  struct mtl_unit in, out;
  struct mtl_convert_desc d;
  MTL_INIT(&in);
  MTL_INIT(&out);
  MTL_INIT(&d);
  int ret = mtl_rx_dequeue(rx, &in, MTL_MS(50));
  if (ret < 0) return ret;
  if (in.flags & MTL_UNITF_TAI_VALID)
    ret = mtl_tx_acquire(tx, &out, MTL_MS(10));
  else
    ret = -MTL_EINVAL; /* a mediaclk:sender input has no TAI relation */
  if (ret == 0) {
    d.width = 3840; /* the source's */
    d.height = 2160;
    d.src.format = d.dst.format = MTL_YUV422_10;
    d.src.kind = d.dst.kind = MTL_FORMAT_TRANSPORT;
    d.src.plane_count = in.plane_count;
    d.dst.plane_count = out.plane_count;
    memcpy(d.src.plane, in.plane, sizeof(d.src.plane));
    memcpy(d.dst.plane, out.plane, sizeof(d.dst.plane));
    d.flags = MTL_CONVERT_HALF_SCALE;
    ret = mtl_convert(&d);
    /* the input's media time and its meta records (MTL_META_USER, mtl_meta_find) */
    mtl_unit_from_template(&out, &in);
    if (ret == 0)
      ret = mtl_tx_submit(tx, &out);
    else
      mtl_tx_release(tx, out.lease);
  }
  return done(rx, in.lease, tx, ret); /* over budget: DROPPED, TOO_LATE, never slid */
}

/* Audio: the copy path with the received unit as the template (its media time); a partial
   write continues at the next sample, as in ex22. */
int pass_audio(mtl_session_h rx, mtl_session_h tx) {
  struct mtl_unit in, how;
  struct mtl_tx_next nx;
  MTL_INIT(&in);
  int ret = mtl_rx_dequeue(rx, &in, MTL_MS(20));
  if (ret < 0) return ret;
  how = in;
  for (uint32_t off = 0; ret >= 0 && off < in.used && g_running;) {
    int n = mtl_tx_write(tx, (const uint8_t*)in.plane[0].addr + off, in.used - off, &how,
                         MTL_MS(10));
    if (n == -MTL_EAGAIN) { /* the pool is full: read results, wait */
      ret = reap(tx);
      continue;
    }
    if (n < 0) {
      ret = n;
      break;
    }
    off += (uint32_t)n;
    if (off < in.used && (ret = mtl_tx_get_next(tx, &nx, sizeof(nx))) >= 0)
      how.media_tai_ns = nx.next_media_tai_ns;
  }
  return done(rx, in.lease, tx, ret);
}

/* ANC: every received entry is a valid TX entry (RX marks the first entry of each RTP
   packet MTL_ANCF_NEW_RTP, so the boundaries stay as they arrived). Both sessions have
   the same word mode, MTL_ANC_WORDS_RAW included (a relay that changes no bit); with
   8-bit words RX has already skipped what the mode cannot carry. A relay that must not
   forward a damaged message (SCTE-104 split over RTP packets) skips the entries from one
   with MTL_ANCF_GAP_BEFORE on. */
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
  return done(rx, in.lease, tx, ret);
}
