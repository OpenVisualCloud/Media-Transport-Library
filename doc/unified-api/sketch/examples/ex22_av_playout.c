/* ex22 — video, audio and captions from one file, in sync by construction: three sessions
   started together with MTL_WHEN_ORIGIN, so the file's frame 0 and sample 0 are at the
   start's T0; each unit says which frame or sample it is, so every RTP timestamp is exact
   and ANC frame k carries video frame k's timestamp. Needs: MS6. */
#include <mtl/experimental/mtl_util.h>

#include "ex_common.h"

#define FILE_FPS MTL_FPS_59_94

enum { VIDEO, AUDIO, CAPTIONS };
/* the demuxer: video pts in frames, audio pts in samples, both from 0; 0 = a packet */
int demux_next(int* stream, int64_t* pts, const void** data, size_t* size);
void decode_into(struct mtl_unit* u, const void* data, size_t size);
/* appends the caption ANC packets of frame pts to u (mtl_anc_put, 8-bit words, in raster
   order): 0, or an MTL_E* code */
int captions_for(int64_t pts, struct mtl_unit* u);

/* One TX session of the file; port: the UDP port of its flow. */
static int open_tx(mtl_instance_h mt, uint32_t essence, uint16_t port, mtl_session_h* s) {
  struct mtl_session_config sc;
  MTL_INIT(&sc);
  sc.direction = MTL_TX;
  sc.essence = essence;
  mtl_flow_ipv4(&sc.flows[0], 239, 168, 85, 20, port);
  sc.media_mode = MTL_MEDIA_INDEX; /* unit.media_index = frame or first sample */
  if (essence == MTL_VIDEO) {
    sc.video.raster.width = 1920;
    sc.video.raster.height = 1080;
    sc.video.raster.fps = mtl_fps_rational(FILE_FPS);
    sc.video.format = MTL_YUV422_10;
  } else if (essence == MTL_AUDIO) {
    sc.audio.format = MTL_PCM24;
    sc.audio.sample_rate = 48000;
    sc.audio.channels = 2;
  } /* ANC: raster and launch delay come from the first video of its start */
  return mtl_session_create(mt, &sc, s);
}

/* Every frame gets an ANC unit, empty when it has no captions: it keeps the stream
   alive. The pool is the read-ahead: acquire waits while it is full. */
static int send_captions(mtl_session_h s, int64_t pts) {
  struct mtl_unit u;
  MTL_INIT(&u);
  int ret = mtl_tx_acquire(s, &u, MTL_FOREVER);
  if (ret < 0) return ret;
  ret = captions_for(pts, &u);
  if (ret < 0) {
    mtl_tx_release(s, u.lease);
    return ret;
  }
  u.media_index = pts;
  return mtl_tx_submit(s, &u);
}

static int send_video(mtl_session_h s, int64_t pts, const void* data, size_t size) {
  struct mtl_unit u;
  MTL_INIT(&u);
  int ret = mtl_tx_acquire(s, &u, MTL_FOREVER);
  if (ret < 0) return ret;
  decode_into(&u, data, size);
  u.media_index = pts;
  return mtl_tx_submit(s, &u);
}

/* The copy path: the sample count follows from the bytes, and every unit continues at
   the sample after the last, so the audio stays exact. */
static int send_audio(mtl_session_h s, int64_t first, const void* data, size_t size) {
  struct mtl_unit tmpl;
  MTL_INIT(&tmpl);
  tmpl.media_index = first;
  int n = mtl_tx_write(s, data, size, &tmpl, MTL_FOREVER);
  return n < 0 ? n : 0; /* fewer bytes only after an error, which the next call returns */
}

int av_anc_playout(mtl_instance_h mt) {
  mtl_session_h s[3] = {MTL_NULL(mtl_session_h), MTL_NULL(mtl_session_h),
                        MTL_NULL(mtl_session_h)};
  /* T0: the first feasible instant plus a preroll, so the first units of all three are
     submitted before it; media index 0 of all three is T0 */
  const struct mtl_when origin = {
      .kind = MTL_NOW, .flags = MTL_WHEN_ORIGIN, .preroll_ns = MTL_MS(100)};

  int ret = open_tx(mt, MTL_VIDEO, 20000, &s[VIDEO]);
  if (ret >= 0) ret = open_tx(mt, MTL_AUDIO, 30000, &s[AUDIO]);
  if (ret >= 0) ret = open_tx(mt, MTL_ANC, 40000, &s[CAPTIONS]);
  if (ret >= 0) ret = mtl_session_start(s, 3, &origin, NULL); /* all three or none */

  int st;
  int64_t pts;
  const void* data;
  size_t size;
  while (ret >= 0 && demux_next(&st, &pts, &data, &size) == 0) {
    if (st == AUDIO) {
      ret = send_audio(s[AUDIO], pts, data, size);
    } else {
      ret = send_captions(s[CAPTIONS], pts); /* ANC frame k before video frame k */
      if (ret >= 0) ret = send_video(s[VIDEO], pts, data, size);
    }
  }

  if (ret < 0 && ret != -MTL_ECANCELED) ex_fail("playout", ret);
  mtl_session_stop(s, 3, MTL_STOP_DRAIN, MTL_SEC(2)); /* all three finish together */
  for (int i = 0; i < 3; i++) mtl_session_close(s[i], 0);
  return ret == -MTL_ECANCELED ? 0 : ret;
}
