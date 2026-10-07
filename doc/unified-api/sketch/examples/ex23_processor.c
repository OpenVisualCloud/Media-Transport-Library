/* ex23 — a processor that keeps the input's timing: video, audio and ANC in, processed,
   out, each with the input's media time and a fixed delay. Needs: MS6. */
#include <mtl/experimental/mtl_format.h>
#include <mtl/experimental/mtl_observe.h>
#include <mtl/experimental/mtl_util.h>

#include "ex_common.h"

void count(const char* what, int64_t n);

/* The output of one input: TAI mode, min_tx_delay_ns = the pipeline budget (examples.md
   derives it), the pick-up lead read from MTL. input_launch_ns: the input's own launch
   delay, whole frames. tsmode: SAMP when the input's SDP says SAMP, else PRES. */
int open_output(mtl_instance_h mt, mtl_session_h rx, int64_t input_launch_ns,
                int64_t process_ns, struct mtl_session_config* sc, mtl_session_h* tx) {
  struct mtl_session_info info;
  int64_t lead = 0;
  int ret = mtl_session_get_info(rx, &info, sizeof(info));
  if (ret < 0) return ret;
  sc->media_mode = MTL_MEDIA_TAI;
  if (!sc->tsmode) sc->tsmode = MTL_TSMODE_PRES;
  sc->flags |= MTL_SESSION_RESULTS;
  ret = mtl_session_create(mt, sc, tx);
  if (ret < 0) return ex_fail("output", ret);
  ret = mtl_stat_get(MTL_OBJ_OF_SESSION(*tx), "info.pickup_lead_ns", &lead);
  sc->min_tx_delay_ns = info.latency_min_ns /* the input arrives this late */
                        + input_launch_ns   /* its sender's launch delay */
                        + process_ns        /* our work */
                        + lead;             /* MTL takes the unit this early */
  if (ret >= 0) ret = mtl_session_update(*tx, sc, MTL_UPDATE_MEDIA, NULL, NULL);
  if (ret >= 0) ret = mtl_session_start(tx, 1, NULL, NULL);
  if (ret < 0) {
    ex_fail("output", ret);
    mtl_session_close(*tx, 0); /* not left CREATED */
    *tx = MTL_NULL(mtl_session_h);
  }
  return ret < 0 ? ret : 0;
}

/* Each frame's outcome: ON_TIME with its margin, or DROPPED with TOO_LATE,
   SNAP_COLLISION, WAITING_NEIGHBOUR, LINK_DOWN or RECOVERY (ex06's results). */
static void on_result(void* priv, const struct mtl_tx_result* r) {
  (void)priv;
  if (r->status != MTL_TX_ON_TIME)
    count(mtl_reason_name(r->reason), 1);
  else if (r->flags & MTL_TXR_MARGIN_VALID)
    count("margin ns", r->margin_ns);
}

/* Video: here a UHD-to-HD down-converter (mtl_convert, half scale, in this thread: its
   time is in the budget); a GPU or AI stage takes its place. rx: 2160p, tx: 1080p. A
   unit with no output slot in time is lost; one over budget is DROPPED (TOO_LATE), never
   slid. */
int process_video(mtl_session_h rx, mtl_session_h tx) {
  struct mtl_unit in, out;
  struct mtl_convert_desc d;
  MTL_INIT(&in);
  MTL_INIT(&out);
  MTL_INIT(&d);
  int ret = mtl_rx_dequeue(rx, &in, MTL_FOREVER);
  if (ret < 0) return ret;
  if (!(in.flags & MTL_UNITF_TAI_VALID)) { /* a mediaclk:sender input: no TAI relation */
    mtl_rx_release(rx, in.lease);
    return -MTL_EINVAL;
  }
  ret = mtl_tx_acquire(tx, &out, MTL_MS(10));
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
  mtl_rx_release(rx, in.lease);
  if (ret == -MTL_EAGAIN) count("output full: unit lost", 1);
  if (ret < 0 && ret != -MTL_EAGAIN) return ret;
  return mtl_tx_reap_each(tx, on_result, NULL); /* unread, results hold slots */
}

/* Audio: the copy path with the received unit as the template (its media time). A TAI
   template writes one unit per call, so the rest continues at the input's media time plus
   the samples written: exact, never the next feasible sample. rate: the sample rate. */
int pass_audio(mtl_session_h rx, mtl_session_h tx, uint32_t rate) {
  struct mtl_unit in, tmpl;
  MTL_INIT(&in);
  int ret = mtl_rx_dequeue(rx, &in, MTL_FOREVER);
  if (ret < 0) return ret;
  if (!(in.flags & MTL_UNITF_TAI_VALID)) { /* a mediaclk:sender input: no TAI relation */
    mtl_rx_release(rx, in.lease);
    return -MTL_EINVAL;
  }
  const uint8_t* pcm = (const uint8_t*)in.plane[0].addr;
  tmpl = in;
  for (uint32_t off = 0; ret >= 0 && off < in.used;) {
    int n = mtl_tx_write(tx, pcm + off, in.used - off, &tmpl, MTL_MS(10));
    if (n < 0) {
      ret = n == -MTL_EAGAIN ? mtl_tx_reap_each(tx, on_result, NULL) : n; /* pool full */
      continue;
    }
    off += (uint32_t)n;
    int64_t samples = off / in.plane[0].row_bytes;
    tmpl.media_tai_ns = in.media_tai_ns + samples * MTL_SEC(1) / rate;
  }
  mtl_rx_release(rx, in.lease);
  return ret < 0 ? ret : mtl_tx_reap_each(tx, on_result, NULL);
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
  int ret = mtl_rx_dequeue(rx, &in, MTL_FOREVER);
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
  mtl_rx_release(rx, in.lease);
  if (ret == -MTL_EAGAIN) count("output full: unit lost", 1);
  if (ret < 0 && ret != -MTL_EAGAIN) return ret;
  return mtl_tx_reap_each(tx, on_result, NULL);
}
