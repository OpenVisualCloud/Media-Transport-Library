/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — RFC 4175 → JPEG thumbnail generator (async triple-buffered)
 *
 * Triple-buffered design (ported from poc_14):
 *   Hot path:  nearest-neighbour downscale RFC 4175 → RGB into hot_buf,
 *              swap hot ↔ ready, signal encoder thread (~100-200 µs).
 *   Background: JPEG compress + atomic-rename write (off hot path).
 *
 * RFC 4175 pgroup layout for 4:2:2 10-bit (5 bytes per 2 pixels):
 *   Byte 0: Cb[9:2]
 *   Byte 1: Cb[1:0] | Y0[9:4]
 *   Byte 2: Y0[3:0] | Cr[9:6]
 *   Byte 3: Cr[5:0] | Y1[9:8]
 *   Byte 4: Y1[7:0]
 *
 * This is the format delivered by MTL's st20_rx (raw API) into
 * the MXL grain buffers, and subsequently RDMA'd to the receiver.
 */

#include <jpeglib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "poc_thumbnail.h"

/* ── Module state ── */
static bool s_inited = false;
static char s_path_final[512]; /* /path/to/thumb.jpg */
static char s_path_tmp[512];   /* /path/to/.thumb.jpg.tmp */
static uint32_t s_src_w, s_src_h;
static uint32_t s_src_stride; /* RFC 4175: width * 5 / 2 */
static uint32_t s_thumb_w, s_thumb_h;

/* ── Triple buffers ── */
static uint8_t* s_hot_buf = NULL;   /* hot-path writes here */
static uint8_t* s_ready_buf = NULL; /* latest completed frame */
static uint8_t* s_enc_buf = NULL;   /* encoder thread's private buffer */

/* ── Background encoder thread ── */
static pthread_t s_thread;
static pthread_mutex_t s_mutex;
static pthread_cond_t s_cond;
static bool s_pending; /* ready_buf has a new frame */
static bool s_running; /* false = thread should exit */
static bool s_thread_started;

/* ── BT.709 YCbCr → RGB (limited range 10-bit → 8-bit) ── */
static inline void ycbcr_to_rgb(int y10, int cb10, int cr10, uint8_t* r, uint8_t* g,
                                uint8_t* b) {
  /* Scale 10-bit [64..940]/[64..960] to float */
  double y = ((double)y10 - 64.0) / 876.0;    /* 940-64 */
  double cb = ((double)cb10 - 512.0) / 896.0; /* 960-64 */
  double cr = ((double)cr10 - 512.0) / 896.0;

  /* BT.709 matrix */
  double rf = y + 1.5748 * cr;
  double gf = y - 0.1873 * cb - 0.4681 * cr;
  double bf = y + 1.8556 * cb;

  /* Clamp and convert to 8-bit */
  int ri = (int)(rf * 255.0 + 0.5);
  int gi = (int)(gf * 255.0 + 0.5);
  int bi = (int)(bf * 255.0 + 0.5);
  *r = (uint8_t)(ri < 0 ? 0 : (ri > 255 ? 255 : ri));
  *g = (uint8_t)(gi < 0 ? 0 : (gi > 255 ? 255 : gi));
  *b = (uint8_t)(bi < 0 ? 0 : (bi > 255 ? 255 : bi));
}

/* ── Extract pixel (x) from an RFC 4175 422 10-bit scanline ── */
static inline void rfc4175_get_pixel(const uint8_t* line, uint32_t x, int* y, int* cb,
                                     int* cr) {
  uint32_t pair = x / 2;
  uint32_t pos = x & 1;
  const uint8_t* pg = line + pair * 5;

  *cb = ((int)pg[0] << 2) | (pg[1] >> 6);
  int y0 = ((pg[1] & 0x3F) << 4) | (pg[2] >> 4);
  *cr = ((pg[2] & 0x0F) << 6) | (pg[3] >> 2);
  int y1 = ((pg[3] & 0x03) << 8) | pg[4];

  *y = (pos == 0) ? y0 : y1;
}

/* ── JPEG encode + atomic rename (runs in background thread) ── */
static int write_jpeg(const uint8_t* rgb, uint32_t w, uint32_t h) {
  FILE* fp = fopen(s_path_tmp, "wb");
  if (!fp) {
    static int err_count = 0;
    if (++err_count <= 3) fprintf(stderr, "[THUMB] Cannot open %s: %m\n", s_path_tmp);
    return -1;
  }

  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);
  jpeg_stdio_dest(&cinfo, fp);

  cinfo.image_width = w;
  cinfo.image_height = h;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, POC_THUMB_QUALITY, TRUE);

  jpeg_start_compress(&cinfo, TRUE);
  while (cinfo.next_scanline < cinfo.image_height) {
    JSAMPROW row = (JSAMPROW)(rgb + cinfo.next_scanline * w * 3);
    jpeg_write_scanlines(&cinfo, &row, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  fclose(fp);

  if (rename(s_path_tmp, s_path_final) != 0) {
    static int err_count = 0;
    if (++err_count <= 3) fprintf(stderr, "[THUMB] rename failed: %m\n");
    return -1;
  }
  return 0;
}

/* ── Hand off hot_buf → ready_buf (called after downscale completes) ── */
static inline void thumb_submit(void) {
  pthread_mutex_lock(&s_mutex);
  uint8_t* tmp = s_hot_buf;
  s_hot_buf = s_ready_buf;
  s_ready_buf = tmp;
  s_pending = true;
  pthread_cond_signal(&s_cond);
  pthread_mutex_unlock(&s_mutex);
}

/* ── Background encoder thread ── */
static void* thumb_encode_thread(void* arg) {
  (void)arg;

  while (1) {
    pthread_mutex_lock(&s_mutex);
    while (!s_pending && s_running) pthread_cond_wait(&s_cond, &s_mutex);

    if (!s_running && !s_pending) {
      pthread_mutex_unlock(&s_mutex);
      break;
    }

    /* Take ownership: swap enc_buf ↔ ready_buf */
    uint8_t* tmp = s_enc_buf;
    s_enc_buf = s_ready_buf;
    s_ready_buf = tmp;
    s_pending = false;
    pthread_mutex_unlock(&s_mutex);

    /* JPEG compress + write — entirely off the hot path */
    write_jpeg(s_enc_buf, s_thumb_w, s_thumb_h);
  }

  /* Drain any final pending frame */
  if (s_pending) {
    uint8_t* tmp = s_enc_buf;
    s_enc_buf = s_ready_buf;
    s_ready_buf = tmp;
    s_pending = false;
    write_jpeg(s_enc_buf, s_thumb_w, s_thumb_h);
  }

  return NULL;
}

/* ════════════════════════════════════════════ */

int poc_thumbnail_init(const char* thumb_dir, uint32_t src_width, uint32_t src_height) {
  if (s_inited) return 0;

  s_src_w = src_width;
  s_src_h = src_height;
  s_src_stride = (src_width * 5) / 2; /* RFC 4175 422 10-bit: 5 bytes per 2 pixels */
  s_thumb_w = POC_THUMB_WIDTH;
  s_thumb_h = POC_THUMB_HEIGHT;

  /* Ensure thumbnail fits within source */
  if (s_thumb_w > s_src_w) s_thumb_w = s_src_w;
  if (s_thumb_h > s_src_h) s_thumb_h = s_src_h;

  /* Allocate triple buffers */
  size_t rgb_size = POC_THUMB_RGB_SIZE;
  s_hot_buf = malloc(rgb_size);
  s_ready_buf = malloc(rgb_size);
  s_enc_buf = malloc(rgb_size);
  if (!s_hot_buf || !s_ready_buf || !s_enc_buf) {
    free(s_hot_buf);
    free(s_ready_buf);
    free(s_enc_buf);
    s_hot_buf = s_ready_buf = s_enc_buf = NULL;
    fprintf(stderr, "[THUMB] malloc failed for %ux%u triple buffers\n", s_thumb_w,
            s_thumb_h);
    return -1;
  }

  snprintf(s_path_final, sizeof(s_path_final), "%s/thumb.jpg", thumb_dir);
  snprintf(s_path_tmp, sizeof(s_path_tmp), "%s/.thumb.jpg.tmp", thumb_dir);

  /* Start background encoder thread */
  pthread_mutex_init(&s_mutex, NULL);
  pthread_cond_init(&s_cond, NULL);
  s_pending = false;
  s_running = true;

  if (pthread_create(&s_thread, NULL, thumb_encode_thread, NULL) != 0) {
    fprintf(stderr, "[THUMB] pthread_create failed\n");
    free(s_hot_buf);
    free(s_ready_buf);
    free(s_enc_buf);
    s_hot_buf = s_ready_buf = s_enc_buf = NULL;
    pthread_mutex_destroy(&s_mutex);
    pthread_cond_destroy(&s_cond);
    return -1;
  }
  s_thread_started = true;

  s_inited = true;
  fprintf(stderr,
          "[THUMB] Initialised: %ux%u → %ux%u (RFC 4175, stride=%u), "
          "output %s  [triple-buffered, async]\n",
          s_src_w, s_src_h, s_thumb_w, s_thumb_h, s_src_stride, s_path_final);
  return 0;
}

int poc_thumbnail_write(const uint8_t* rfc4175_data, uint32_t frame_size) {
  if (!s_inited || !rfc4175_data) return -1;

  /* Quick sanity check — RFC 4175 frame = stride * height */
  uint32_t expected = s_src_stride * s_src_h;
  if (frame_size < expected) {
    return -1; /* truncated frame, skip silently */
  }

  /* ── Downscale RFC 4175 → RGB into hot_buf ──
   * Nearest-neighbour: pick every Nth pixel.  Good enough for a monitoring
   * thumbnail and avoids any filtering overhead. */
  double x_scale = (double)s_src_w / (double)s_thumb_w;
  double y_scale = (double)s_src_h / (double)s_thumb_h;

  for (uint32_t ty = 0; ty < s_thumb_h; ty++) {
    uint32_t sy = (uint32_t)(ty * y_scale);
    if (sy >= s_src_h) sy = s_src_h - 1;
    const uint8_t* src_line = rfc4175_data + (uint64_t)sy * s_src_stride;

    uint8_t* dst = s_hot_buf + ty * s_thumb_w * 3;

    for (uint32_t tx = 0; tx < s_thumb_w; tx++) {
      uint32_t sx = (uint32_t)(tx * x_scale);
      if (sx >= s_src_w) sx = s_src_w - 1;

      int y_val, cb_val, cr_val;
      rfc4175_get_pixel(src_line, sx, &y_val, &cb_val, &cr_val);
      ycbcr_to_rgb(y_val, cb_val, cr_val, &dst[tx * 3 + 0], &dst[tx * 3 + 1],
                   &dst[tx * 3 + 2]);
    }
  }

  /* Hand off to background encoder thread (swap hot ↔ ready, signal) */
  thumb_submit();

  return 0;
}

void poc_thumbnail_cleanup(void) {
  /* Stop the background encoder thread */
  if (s_thread_started) {
    pthread_mutex_lock(&s_mutex);
    s_running = false;
    pthread_cond_signal(&s_cond);
    pthread_mutex_unlock(&s_mutex);
    pthread_join(s_thread, NULL);
    s_thread_started = false;
  }

  pthread_mutex_destroy(&s_mutex);
  pthread_cond_destroy(&s_cond);

  free(s_hot_buf);
  s_hot_buf = NULL;
  free(s_ready_buf);
  s_ready_buf = NULL;
  free(s_enc_buf);
  s_enc_buf = NULL;
  s_inited = false;
}
