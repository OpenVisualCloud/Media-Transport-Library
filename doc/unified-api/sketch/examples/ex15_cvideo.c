/* ex15 — compressed video (ST 2110-22, JPEG XS) with the application's codec or a codec
   plugin. A unit is one codestream per frame (per field when interlaced): `used` counts
   its bytes. ST 2110-22 sends a constant number of bytes per frame (MTL_CVIDEO_CBR, the
   default): codestream_bytes is the ceiling, MTL prepends its box header and pads a
   shorter codestream, and a longer one fails at submit (-MTL_ENOSPC,
   CODESTREAM_OVERSIZE). With app_format set the session takes and gives raw frames, and a
   codec plugin loaded into the instance encodes or decodes them off the pinned cores.
   Needs: MS4 (MS4b). */
#include <mtl/experimental/mtl_format.h>
#include <mtl/experimental/mtl_plugin.h>

#include "ex_common.h"

size_t encode_frame(int64_t frame, void* dst, size_t cap); /* bytes, 0 = error */
void decode_frame(const void* codestream, size_t bytes, int complete);
void draw(struct mtl_unit* u);

/* 1080p59.94 JPEG XS at about 6:1. app_format 0: the application gives or takes the
   codestream; MTL_APP_*: raw frames, a plugin does the codec. */
static void jpegxs(struct mtl_session_config* sc, uint32_t dir, uint32_t app_format) {
  MTL_INIT(sc);
  sc->direction = dir;
  sc->essence = MTL_CVIDEO;
  mtl_flow_ipv4(&sc->flows[0], 239, 168, 85, 60, 20000);
  sc->cvideo.raster.width = 1920;
  sc->cvideo.raster.height = 1080;
  sc->cvideo.raster.fps = mtl_fps_rational(MTL_FPS_59_94);
  sc->cvideo.codec = MTL_CODEC_JPEGXS;
  sc->cvideo.codestream_bytes = 864000; /* per frame, the box header included */
  sc->cvideo.app_format = app_format;
}

/* 1. Your encoder writes into the slot. */
int open_encoded_tx(mtl_instance_h mt, mtl_session_h* s) {
  struct mtl_session_config sc;
  jpegxs(&sc, MTL_TX, 0);
  return mtl_session_open(mt, &sc, s);
}
int send_encoded(mtl_session_h s, int64_t frame) {
  struct mtl_unit u;
  MTL_INIT(&u);
  int ret = mtl_tx_acquire(s, &u, MTL_MS(20));
  if (ret < 0) return ret;
  u.used = (uint32_t)encode_frame(frame, u.plane[0].addr, u.plane[0].row_bytes);
  if (u.used == 0) {
    mtl_tx_release(s, u.lease);
    return -MTL_EINVAL;
  }
  return mtl_tx_submit(s, &u); /* the slot goes back on a failure, without a result */
}

/* 2. A plugin encodes: the session takes 4:2:2 10-bit planar frames, like a video one.
   The plugin stays loaded while a session uses it; unload it after the close. */
int open_plugin_tx(mtl_instance_h mt, const char* so, mtl_plugin_h* p, mtl_session_h* s) {
  struct mtl_session_config sc;
  jpegxs(&sc, MTL_TX, MTL_APP_YUV422P10LE);
  int ret = mtl_plugin_open(mt, so, NULL, p); /* the codec's .so, e.g. SVT JPEG XS */
  if (ret >= 0) ret = mtl_session_open(mt, &sc, s);
  return ret < 0 ? ex_fail("plugin tx", ret) : 0;
}
int send_raw(mtl_session_h s) {
  struct mtl_unit u;
  MTL_INIT(&u);
  int ret = mtl_tx_acquire(s, &u, MTL_MS(20));
  if (ret < 0) return ret;
  draw(&u); /* the raw planes; the plugin encodes after submit */
  return mtl_tx_submit(s, &u);
}

/* 3. RX: the codestream as received; a frame with lost packets is MTL_RX_INCOMPLETE and
   its gaps read as zero, which a decoder may conceal. With app_format and a plugin,
   dequeue gives decoded frames instead. */
int receive_encoded(mtl_session_h s) {
  struct mtl_unit u;
  MTL_INIT(&u);
  int ret = mtl_rx_dequeue(s, &u, MTL_MS(100));
  if (ret < 0) return ret;
  decode_frame(u.plane[0].addr, u.used, u.status == MTL_RX_COMPLETE);
  return mtl_rx_release(s, u.lease);
}
