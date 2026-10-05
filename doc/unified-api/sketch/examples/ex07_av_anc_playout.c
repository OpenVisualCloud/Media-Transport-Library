/* ex07 — video, audio and captions from one file, in sync by construction: three sessions
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
/* writes captions for frame pts as ANC: a meta header + packets, and the UDW run */
size_t captions_for(int64_t pts, void* meta, void* udw);

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
  } /* ANC: raster and slot delay come from the first video of its start */
  return mtl_session_create(mt, &sc, s);
}

/* Acquires a unit, waiting while the pool is full: the file is read ahead of the wire. */
static int acquire(mtl_session_h s, struct mtl_unit* u) {
  int ret;
  do {
    ret = mtl_tx_acquire(s, u, MTL_MS(20));
  } while (ret == -MTL_EAGAIN && g_running);
  return ret;
}

/* The copy path: the sample count follows from the bytes. A full pool (-MTL_EAGAIN) is
   waited out; a partial write continues at the next index (mtl_tx_next_slot). */
static int write_audio(mtl_session_h s, int64_t first, const void* data, size_t size) {
  const uint8_t* p = (const uint8_t*)data;
  struct mtl_unit how;
  MTL_INIT(&how);
  how.media_index = first;
  while (size > 0 && g_running) {
    int n = mtl_tx_write(s, p, size, &how, MTL_MS(20));
    if (n == -MTL_EAGAIN) continue;
    if (n < 0) return n;
    p += n;
    size -= (size_t)n;
    if (size > 0) {
      struct mtl_slot_hint h;
      int ret = mtl_tx_next_slot(s, &h, sizeof(h));
      if (ret < 0) return ret;
      how.media_index = h.next_media_index;
    }
  }
  return 0;
}

int av_anc_playout(mtl_instance_h mt) {
  mtl_session_h s[3] = {MTL_NULL(mtl_session_h), MTL_NULL(mtl_session_h),
                        MTL_NULL(mtl_session_h)};
  /* T0: the first feasible instant plus a preroll, so the first units of all three are
     submitted before it; media index 0 of all three is T0 */
  const struct mtl_when origin = {
      .kind = MTL_NOW, .flags = MTL_WHEN_ORIGIN, .preroll_ns = MTL_MS(100)};
  struct mtl_unit u;
  MTL_INIT(&u);

  int ret = open_tx(mt, MTL_VIDEO, 20000, &s[VIDEO]);
  if (ret >= 0) ret = open_tx(mt, MTL_AUDIO, 30000, &s[AUDIO]);
  if (ret >= 0) ret = open_tx(mt, MTL_ANC, 40000, &s[CAPTIONS]);
  if (ret >= 0) ret = mtl_session_start(s, 3, &origin, NULL); /* all three or none */

  int st;
  int64_t pts;
  const void* data;
  size_t size;
  while (ret >= 0 && g_running && demux_next(&st, &pts, &data, &size) == 0) {
    if (st == AUDIO) {
      ret = write_audio(s[AUDIO], pts, data, size);
      continue;
    }
    ret = acquire(s[CAPTIONS], &u); /* frames without captions get the empty ANC packet */
    if (ret < 0) break;
    u.used = (uint32_t)captions_for(pts, u.meta, u.plane[0].addr);
    u.media_index = pts;
    ret = mtl_tx_submit(s[CAPTIONS], &u);
    if (ret < 0) break;

    ret = acquire(s[VIDEO], &u);
    if (ret < 0) break;
    decode_into(&u, data, size);
    u.media_index = pts;
    ret = mtl_tx_submit(s[VIDEO], &u);
  }

  if (ret < 0 && ret != -MTL_EAGAIN) ex_fail("playout", ret); /* -MTL_EAGAIN: stopped */
  mtl_session_stop(s, 3, MTL_STOP_DRAIN, MTL_SEC(2)); /* all three finish together */
  for (int i = 0; i < 3; i++) mtl_session_close(s[i], 0);
  return ret;
}
