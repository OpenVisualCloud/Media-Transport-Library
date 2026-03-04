/* SPDX-License-Identifier: BSD-3-Clause
 * poc_14 — Multi-instance thumbnail generator (triple-buffered)
 *
 * Hot path: nearest-neighbour downscale → RGB24 into hot_buf (~173 KB).
 *           Swap hot_buf ↔ ready_buf under mutex, signal encoder thread.
 *           Total hot-path cost: ~100-200 µs (pure math + 1 µs mutex).
 *
 * Background thread: swap enc_buf ↔ ready_buf, then JPEG compress +
 *                    atomic-rename write to /dev/shm/poc14_thumbs/.
 *                    No dirty pages (tmpfs), no hot-path blocking.
 *
 * RFC 4175 pgroup layout for 4:2:2 10-bit (5 bytes per 2 pixels):
 *   Byte 0: Cb[9:2]
 *   Byte 1: Cb[1:0] | Y0[9:4]
 *   Byte 2: Y0[3:0] | Cr[9:6]
 *   Byte 3: Cr[5:0] | Y1[9:8]
 *   Byte 4: Y1[7:0]
 */

#include "thumbnail_14.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <jpeglib.h>

/* ── RFC 4175 pixel extraction ──
 *
 * Each pgroup = 2 pixels = 5 bytes.
 * Pixel pair index = x / 2.
 * Within the pair: pos 0 = left pixel (Y0), pos 1 = right pixel (Y1).
 * Both share the same Cb/Cr.
 */
static inline void rfc4175_get_pixel(const uint8_t *line, int x,
                                     int *y_out, int *cb_out, int *cr_out)
{
    int pair = x / 2;
    int pos  = x & 1;
    const uint8_t *pg = line + pair * 5;

    *cb_out = ((int)pg[0] << 2) | (pg[1] >> 6);
    int y0  = ((pg[1] & 0x3F) << 4) | (pg[2] >> 4);
    *cr_out = ((pg[2] & 0x0F) << 6) | (pg[3] >> 2);
    int y1  = ((pg[3] & 0x03) << 8) | pg[4];

    *y_out = (pos == 0) ? y0 : y1;
}

/* ── BT.709 YCbCr → RGB (limited range 10-bit → 8-bit) ── */
static inline void ycbcr10_to_rgb(int y10, int cb10, int cr10,
                                  uint8_t *r, uint8_t *g, uint8_t *b)
{
    /* Scale 10-bit [64..940]/[64..960] to float */
    double y  = ((double)y10  - 64.0) / 876.0;   /* 940-64 */
    double cb = ((double)cb10 - 512.0) / 896.0;   /* 960-64 */
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

/* ── JPEG encode + atomic-rename write (runs in background thread) ── */
static void write_jpeg(poc14_thumb_ctx_t *ctx, const uint8_t *rgb)
{
    char tmp_path[520];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", ctx->path);

    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) return;

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);

    cinfo.image_width      = (JDIMENSION)ctx->dst_w;
    cinfo.image_height     = (JDIMENSION)ctx->dst_h;
    cinfo.input_components = 3;
    cinfo.in_color_space   = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, POC14_THUMB_QUALITY, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    JSAMPROW row;
    while (cinfo.next_scanline < cinfo.image_height) {
        row = (JSAMPROW)&rgb[cinfo.next_scanline * (unsigned)ctx->dst_w * 3];
        jpeg_write_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(fp);

    rename(tmp_path, ctx->path);
}

/* ── Hand off hot_buf → ready_buf (called after downscale completes) ── */
static inline void thumb_submit(poc14_thumb_ctx_t *ctx)
{
    pthread_mutex_lock(&ctx->mutex);
    /* Swap hot_buf ↔ ready_buf: hand off freshly-written pixels,
     * get back the buffer the thread has already consumed (or is
     * about to discard if it was still pending). */
    uint8_t *tmp    = ctx->hot_buf;
    ctx->hot_buf    = ctx->ready_buf;
    ctx->ready_buf  = tmp;
    ctx->pending    = true;
    pthread_cond_signal(&ctx->cond);
    pthread_mutex_unlock(&ctx->mutex);
}

/* ── Background encoder thread ── */
static void *thumb_encode_thread(void *arg)
{
    poc14_thumb_ctx_t *ctx = (poc14_thumb_ctx_t *)arg;

    while (1) {
        pthread_mutex_lock(&ctx->mutex);
        while (!ctx->pending && ctx->running)
            pthread_cond_wait(&ctx->cond, &ctx->mutex);

        if (!ctx->running && !ctx->pending) {
            pthread_mutex_unlock(&ctx->mutex);
            break;
        }

        /* Take ownership: swap enc_buf ↔ ready_buf.
         * After this, enc_buf has the latest frame pixels and
         * ready_buf is recycled for the hot path's next swap. */
        uint8_t *tmp    = ctx->enc_buf;
        ctx->enc_buf    = ctx->ready_buf;
        ctx->ready_buf  = tmp;
        ctx->pending    = false;
        pthread_mutex_unlock(&ctx->mutex);

        /* JPEG compress + write — entirely off the hot path */
        write_jpeg(ctx, ctx->enc_buf);
    }

    /* Drain any final pending frame */
    if (ctx->pending) {
        uint8_t *tmp    = ctx->enc_buf;
        ctx->enc_buf    = ctx->ready_buf;
        ctx->ready_buf  = tmp;
        ctx->pending    = false;
        write_jpeg(ctx, ctx->enc_buf);
    }

    return NULL;
}

/* ════════════════════════════════════════════════════════════════
 *  PUBLIC API
 * ════════════════════════════════════════════════════════════════ */

int poc14_thumb_init(poc14_thumb_ctx_t *ctx, int stream_id,
                     const char *role, int src_w, int src_h)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->src_w = src_w;
    ctx->src_h = src_h;
    ctx->dst_w = POC14_THUMB_WIDTH;
    ctx->dst_h = POC14_THUMB_HEIGHT;

    /* Ensure output directory exists (tmpfs — no dirty pages) */
    mkdir(POC14_THUMB_DIR, 0755);
    snprintf(ctx->path, sizeof(ctx->path),
             POC14_THUMB_DIR "/thumb_%s_%d.jpg", role, stream_id);

    /* Allocate triple buffers */
    ctx->hot_buf   = malloc(POC14_THUMB_RGB_SIZE);
    ctx->ready_buf = malloc(POC14_THUMB_RGB_SIZE);
    ctx->enc_buf   = malloc(POC14_THUMB_RGB_SIZE);
    if (!ctx->hot_buf || !ctx->ready_buf || !ctx->enc_buf) {
        fprintf(stderr, "[THUMB-%d] malloc failed for RGB buffers\n", stream_id);
        free(ctx->hot_buf);
        free(ctx->ready_buf);
        free(ctx->enc_buf);
        ctx->hot_buf = ctx->ready_buf = ctx->enc_buf = NULL;
        return -1;
    }

    /* Start background encoder thread */
    pthread_mutex_init(&ctx->mutex, NULL);
    pthread_cond_init(&ctx->cond, NULL);
    ctx->pending = false;
    ctx->running = true;

    if (pthread_create(&ctx->thread, NULL, thumb_encode_thread, ctx) != 0) {
        fprintf(stderr, "[THUMB-%d] pthread_create failed\n", stream_id);
        free(ctx->hot_buf);
        free(ctx->ready_buf);
        free(ctx->enc_buf);
        ctx->hot_buf = ctx->ready_buf = ctx->enc_buf = NULL;
        pthread_mutex_destroy(&ctx->mutex);
        pthread_cond_destroy(&ctx->cond);
        return -1;
    }
    ctx->thread_started = true;

    printf("[THUMB-%d] Initialized (%s) → %s  [triple-buffered, async]\n",
           stream_id, role, ctx->path);
    return 0;
}

int poc14_thumb_write(poc14_thumb_ctx_t *ctx,
                      const uint8_t *frame_data, size_t frame_len)
{
    if (!ctx->hot_buf || !frame_data)
        return -1;

    /* RFC 4175 422 10-bit stride: 5 bytes per 2 pixels */
    int stride = (ctx->src_w * 5) / 2;

    /* Quick sanity check */
    size_t expected = (size_t)stride * ctx->src_h;
    if (frame_len < expected)
        return -1;  /* truncated frame, skip */

    /* Nearest-neighbour downscale + RFC 4175 → RGB into hot_buf */
    for (int dy = 0; dy < ctx->dst_h; dy++) {
        int sy = dy * ctx->src_h / ctx->dst_h;
        if (sy >= ctx->src_h) sy = ctx->src_h - 1;
        const uint8_t *line = frame_data + (size_t)sy * stride;

        for (int dx = 0; dx < ctx->dst_w; dx++) {
            int sx = dx * ctx->src_w / ctx->dst_w;
            if (sx >= ctx->src_w) sx = ctx->src_w - 1;

            int Y, Cb, Cr;
            rfc4175_get_pixel(line, sx, &Y, &Cb, &Cr);

            uint8_t *pixel = ctx->hot_buf + (dy * ctx->dst_w + dx) * 3;
            ycbcr10_to_rgb(Y, Cb, Cr, &pixel[0], &pixel[1], &pixel[2]);
        }
    }

    /* Hand off to background encoder thread */
    thumb_submit(ctx);
    return 0;
}

int poc14_thumb_write_planar(poc14_thumb_ctx_t *ctx,
                             void *const planes[3])
{
    if (!ctx->hot_buf || !planes[0])
        return -1;

    const uint16_t *y_plane  = (const uint16_t *)planes[0];
    const uint16_t *cb_plane = (const uint16_t *)planes[1];
    const uint16_t *cr_plane = (const uint16_t *)planes[2];

    const int src_w  = ctx->src_w;
    const int src_h  = ctx->src_h;
    const int half_w = src_w / 2;

    /* Nearest-neighbour downscale + YUV422P10LE → RGB into hot_buf */
    for (int dy = 0; dy < ctx->dst_h; dy++) {
        int sy = dy * src_h / ctx->dst_h;
        if (sy >= src_h) sy = src_h - 1;

        for (int dx = 0; dx < ctx->dst_w; dx++) {
            int sx = dx * src_w / ctx->dst_w;
            if (sx >= src_w) sx = src_w - 1;

            int y_val  = y_plane [(size_t)sy * src_w  + sx]     & 0x3FF;
            int cb_val = cb_plane[(size_t)sy * half_w + sx / 2] & 0x3FF;
            int cr_val = cr_plane[(size_t)sy * half_w + sx / 2] & 0x3FF;

            uint8_t *pixel = ctx->hot_buf + (dy * ctx->dst_w + dx) * 3;
            ycbcr10_to_rgb(y_val, cb_val, cr_val,
                           &pixel[0], &pixel[1], &pixel[2]);
        }
    }

    /* Hand off to background encoder thread */
    thumb_submit(ctx);
    return 0;
}

void poc14_thumb_cleanup(poc14_thumb_ctx_t *ctx)
{
    /* Stop the background encoder thread */
    if (ctx->thread_started) {
        pthread_mutex_lock(&ctx->mutex);
        ctx->running = false;
        pthread_cond_signal(&ctx->cond);
        pthread_mutex_unlock(&ctx->mutex);
        pthread_join(ctx->thread, NULL);
        ctx->thread_started = false;
    }

    pthread_mutex_destroy(&ctx->mutex);
    pthread_cond_destroy(&ctx->cond);

    free(ctx->hot_buf);   ctx->hot_buf   = NULL;
    free(ctx->ready_buf); ctx->ready_buf = NULL;
    free(ctx->enc_buf);   ctx->enc_buf   = NULL;
}
