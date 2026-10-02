/* ex07 — video, audio and captions from one file, in sync by construction: three sessions
   on one timeline, started together; each unit says which frame or sample it is, so every
   RTP timestamp is exact and ANC frame k carries video frame k's timestamp. */
#include <mtl/experimental/mtl_sync.h>
#include <mtl/experimental/mtl_util.h>

#include "ex_common.h"

#define FILE_FPS MTL_FPS_59_94

enum { VIDEO, AUDIO, CAPTIONS };
/* the demuxer: video pts in frames, audio pts in samples, both from 0; 0 = a packet */
int demux_next(int* stream, int64_t* pts, const void** data, size_t* size);
void decode_into(struct mtl_unit* u, const void* data, size_t size);
/* writes captions for frame pts as ANC: a meta header + packets, and the UDW run */
size_t captions_for(int64_t pts, void* meta, void* udw);

/* One TX session of the file on the shared timeline; port: the UDP port of its flow. */
static int open_tx(mtl_instance_h mt, uint32_t essence, uint16_t port, mtl_timeline_h tl,
                   mtl_session_h* s) {
  struct mtl_session_config sc;
  MTL_INIT(&sc);
  sc.direction = MTL_TX;
  sc.essence = essence;
  mtl_flow_ipv4(&sc.flows[0], 239, 168, 85, 20, port);
  sc.timeline = tl;
  sc.media_mode = MTL_MEDIA_INDEX; /* unit.media_index = frame or first sample */
  if (essence == MTL_VIDEO) {
    sc.video.raster.width = 1920;
    sc.video.raster.height = 1080;
    sc.video.raster.rate = FILE_FPS;
    sc.video.format = MTL_YUV422_10;
  } else if (essence == MTL_AUDIO) {
    sc.audio.format = MTL_PCM24;
    sc.audio.sample_rate = 48000;
    sc.audio.channels = 2;
  } /* ANC: its raster comes from the video it starts with */
  return mtl_session_create(mt, &sc, s);
}

int av_anc_playout(mtl_instance_h mt) {
  mtl_session_h s[3] = {MTL_NULL(mtl_session_h), MTL_NULL(mtl_session_h),
                        MTL_NULL(mtl_session_h)};
  mtl_timeline_h tl;
  struct mtl_timeline_config tc;
  struct mtl_unit u;
  MTL_INIT(&tc); /* anchor AT_START: T0 is chosen when the sessions start */
  MTL_INIT(&u);

  int ret = mtl_timeline_create(mt, &tc, &tl);
  if (ret >= 0) ret = open_tx(mt, MTL_VIDEO, 20000, tl, &s[VIDEO]);
  if (ret >= 0) ret = open_tx(mt, MTL_AUDIO, 30000, tl, &s[AUDIO]);
  if (ret >= 0) ret = open_tx(mt, MTL_ANC, 40000, tl, &s[CAPTIONS]);
  if (ret >= 0) ret = mtl_session_start(s, 3, NULL, NULL); /* all three or none */

  int st;
  int64_t pts;
  const void* data;
  size_t size;
  while (ret >= 0 && g_running && demux_next(&st, &pts, &data, &size) == 0) {
    if (st == AUDIO) { /* the copy path: sample count from the bytes */
      struct mtl_unit how;
      MTL_INIT(&how);
      how.media_index = pts;
      ret = mtl_tx_write(s[AUDIO], data, size, &how, MTL_MS(20));
      continue;
    }
    ret = mtl_tx_acquire(s[CAPTIONS], &u, MTL_MS(20)); /* frames without captions get */
    if (ret < 0) break;                                /* the empty ANC packet anyway */
    u.used = (uint32_t)captions_for(pts, u.meta, u.plane[0].addr);
    u.media_index = pts;
    ret = mtl_tx_submit(s[CAPTIONS], &u);
    if (ret < 0) break;

    ret = mtl_tx_acquire(s[VIDEO], &u, MTL_MS(40));
    if (ret < 0) break;
    decode_into(&u, data, size);
    u.media_index = pts;
    ret = mtl_tx_submit(s[VIDEO], &u);
  }

  if (ret < 0) ex_fail("playout", ret);
  mtl_session_stop(s, 3, MTL_STOP_DRAIN, MTL_SEC(2)); /* all three finish together */
  for (int i = 0; i < 3; i++) mtl_session_close(s[i], 0);
  mtl_timeline_close(tl);
  return ret;
}
