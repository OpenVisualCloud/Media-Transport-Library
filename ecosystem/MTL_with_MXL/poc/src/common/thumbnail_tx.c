/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — TX-side YUV422P10LE → JPEG thumbnails with packet-drop masking
 *
 * Triple-buffered async design (ported from poc_14):
 *   Hot path:  downscale + RGB + packet-drop masking into hot_buf_p / hot_buf_r
 *              then swap hot ↔ ready and signal the background thread (~200 µs).
 *   Background: JPEG compress + atomic-rename write to tmpfs (off hot path).
 *
 * Packet mapping:
 *   RFC 4175 4:2:2 10-bit: 1 pgroup = 2 pixels = 5 bytes.
 *   BPM packing with ~1260 B payload → ~252 pgroups/packet.
 *   Pixels are packed in raster order (left→right, top→bottom).
 *   For pixel at (sx, sy): pgroup_idx = sy * (width/2) + sx/2
 *                           pkt_idx   = pgroup_idx / pgroups_per_pkt
 *   Drop pattern uses pkt_drop_hash(pkt_idx) — pseudo-random but
 *   perfectly complementary between P and R.
 */

#include "poc_thumbnail_tx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <jpeglib.h>

/* murmurhash3-style finalizer — same function used in MTL
 * tv_runtime_packet_loss() so the visual pattern matches the real drops. */
static inline uint32_t pkt_drop_hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x45d9f3bU;
    x ^= x >> 16;
    x *= 0x45d9f3bU;
    x ^= x >> 16;
    return x;
}

/* ── Module state ── */
static bool     s_inited;
static char     s_path_p[512], s_path_p_tmp[512];
static char     s_path_r[512], s_path_r_tmp[512];
static uint32_t s_src_w, s_src_h;
static uint32_t s_thumb_w, s_thumb_h;
static uint32_t s_pgroups_per_pkt;   /* for packet-drop pixel mapping */
static uint32_t s_frame_counter;     /* rotates drop pattern each frame */

/* ── Triple buffers for P and R paths ── */
static uint8_t *s_hot_p,   *s_ready_p,   *s_enc_p;
static uint8_t *s_hot_r,   *s_ready_r,   *s_enc_r;

/* ── Background encoder thread ── */
static pthread_t       s_thread;
static pthread_mutex_t s_mutex;
static pthread_cond_t  s_cond;
static bool            s_pending;        /* ready buffers have a new frame */
static bool            s_running;        /* false = thread should exit     */
static bool            s_thread_started;

/* ── BT.709 YCbCr 10-bit → 8-bit RGB (same as thumbnail.c) ── */
static inline void ycbcr10_to_rgb(int y10, int cb10, int cr10,
                                  uint8_t *r, uint8_t *g, uint8_t *b)
{
    double y  = ((double)y10  - 64.0) / 876.0;   /* 940 - 64 */
    double cb = ((double)cb10 - 512.0) / 896.0;   /* 960 - 64 */
    double cr = ((double)cr10 - 512.0) / 896.0;

    double rf = y + 1.5748 * cr;
    double gf = y - 0.1873 * cb - 0.4681 * cr;
    double bf = y + 1.8556 * cb;

    int ri = (int)(rf * 255.0 + 0.5);
    int gi = (int)(gf * 255.0 + 0.5);
    int bi = (int)(bf * 255.0 + 0.5);
    *r = (uint8_t)(ri < 0 ? 0 : (ri > 255 ? 255 : ri));
    *g = (uint8_t)(gi < 0 ? 0 : (gi > 255 ? 255 : gi));
    *b = (uint8_t)(bi < 0 ? 0 : (bi > 255 ? 255 : bi));
}

/* ── JPEG encode + atomic rename (runs in background thread) ── */
static int write_jpeg(const char *path_tmp, const char *path_final,
                      const uint8_t *rgb, uint32_t w, uint32_t h)
{
    FILE *fp = fopen(path_tmp, "wb");
    if (!fp) {
        static int err_count = 0;
        if (++err_count <= 3)
            fprintf(stderr, "[TX_THUMB] Cannot open %s: %m\n", path_tmp);
        return -1;
    }

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);

    cinfo.image_width      = w;
    cinfo.image_height     = h;
    cinfo.input_components = 3;
    cinfo.in_color_space   = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, POC_TX_THUMB_QUALITY, TRUE);

    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = (JSAMPROW)(rgb + cinfo.next_scanline * w * 3);
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(fp);

    if (rename(path_tmp, path_final) != 0) {
        static int err_count = 0;
        if (++err_count <= 3)
            fprintf(stderr, "[TX_THUMB] rename failed: %m\n");
        return -1;
    }
    return 0;
}

/* ── Hand off hot_buf → ready_buf for both P and R (called after downscale) ── */
static inline void thumb_tx_submit(void)
{
    pthread_mutex_lock(&s_mutex);
    /* Swap hot ↔ ready for both P and R paths */
    uint8_t *tmp;
    tmp = s_hot_p;  s_hot_p  = s_ready_p;  s_ready_p = tmp;
    tmp = s_hot_r;  s_hot_r  = s_ready_r;  s_ready_r = tmp;
    s_pending = true;
    pthread_cond_signal(&s_cond);
    pthread_mutex_unlock(&s_mutex);
}

/* ── Background encoder thread — encodes both P and R JPEGs ── */
static void *thumb_tx_encode_thread(void *arg)
{
    (void)arg;

    while (1) {
        pthread_mutex_lock(&s_mutex);
        while (!s_pending && s_running)
            pthread_cond_wait(&s_cond, &s_mutex);

        if (!s_running && !s_pending) {
            pthread_mutex_unlock(&s_mutex);
            break;
        }

        /* Take ownership: swap enc ↔ ready for both paths */
        uint8_t *tmp;
        tmp = s_enc_p;  s_enc_p  = s_ready_p;  s_ready_p = tmp;
        tmp = s_enc_r;  s_enc_r  = s_ready_r;  s_ready_r = tmp;
        s_pending = false;
        pthread_mutex_unlock(&s_mutex);

        /* JPEG compress + write — entirely off the hot path */
        write_jpeg(s_path_p_tmp, s_path_p, s_enc_p, s_thumb_w, s_thumb_h);
        write_jpeg(s_path_r_tmp, s_path_r, s_enc_r, s_thumb_w, s_thumb_h);
    }

    /* Drain any final pending frame */
    if (s_pending) {
        uint8_t *tmp;
        tmp = s_enc_p;  s_enc_p  = s_ready_p;  s_ready_p = tmp;
        tmp = s_enc_r;  s_enc_r  = s_ready_r;  s_ready_r = tmp;
        s_pending = false;
        write_jpeg(s_path_p_tmp, s_path_p, s_enc_p, s_thumb_w, s_thumb_h);
        write_jpeg(s_path_r_tmp, s_path_r, s_enc_r, s_thumb_w, s_thumb_h);
    }

    return NULL;
}

/* ════════════════════════════════════════════ */

int poc_thumb_tx_init(const char *thumb_dir,
                      uint32_t src_w, uint32_t src_h)
{
    if (s_inited) return 0;

    s_src_w  = src_w;
    s_src_h  = src_h;
    s_thumb_w = POC_TX_THUMB_WIDTH;
    s_thumb_h = POC_TX_THUMB_HEIGHT;
    if (s_thumb_w > src_w) s_thumb_w = src_w;
    if (s_thumb_h > src_h) s_thumb_h = src_h;

    /* Compute pgroups per packet for the BPM packetisation mapping.
     *
     * RFC 4175 422 10-bit: 1 pgroup = 2 pixels = 5 bytes.
     * With standard MTU (~1500), after IP/UDP/RTP/SRD headers the payload
     * is ~1260 bytes.  From MTL stats: 1920×1080 yields ~4111 pkts/frame
     * with ~1261 bytes/pkt → 252 pgroups/pkt.
     *
     * We compute it dynamically so it works for any resolution. */
    uint32_t total_pgroups   = (src_w * src_h) / 2;
    uint32_t transport_bytes = total_pgroups * 5;
    uint32_t approx_payload  = 1260;   /* empirical from MTL stats */
    uint32_t pkts_per_frame  = (transport_bytes + approx_payload - 1) / approx_payload;
    s_pgroups_per_pkt = (total_pgroups + pkts_per_frame - 1) / pkts_per_frame;

    /* Allocate triple buffers for both P and R paths (6 buffers total) */
    size_t rgb_size = POC_TX_THUMB_RGB_SIZE;
    s_hot_p   = malloc(rgb_size);
    s_ready_p = malloc(rgb_size);
    s_enc_p   = malloc(rgb_size);
    s_hot_r   = malloc(rgb_size);
    s_ready_r = malloc(rgb_size);
    s_enc_r   = malloc(rgb_size);
    if (!s_hot_p || !s_ready_p || !s_enc_p ||
        !s_hot_r || !s_ready_r || !s_enc_r) {
        free(s_hot_p); free(s_ready_p); free(s_enc_p);
        free(s_hot_r); free(s_ready_r); free(s_enc_r);
        s_hot_p = s_ready_p = s_enc_p = NULL;
        s_hot_r = s_ready_r = s_enc_r = NULL;
        fprintf(stderr, "[TX_THUMB] malloc failed for %ux%u triple buffers\n",
                s_thumb_w, s_thumb_h);
        return -1;
    }

    snprintf(s_path_p,     sizeof(s_path_p),     "%s/thumb_p.jpg",      thumb_dir);
    snprintf(s_path_r,     sizeof(s_path_r),     "%s/thumb_r.jpg",      thumb_dir);
    snprintf(s_path_p_tmp, sizeof(s_path_p_tmp), "%s/.thumb_p.jpg.tmp", thumb_dir);
    snprintf(s_path_r_tmp, sizeof(s_path_r_tmp), "%s/.thumb_r.jpg.tmp", thumb_dir);

    /* Start background encoder thread */
    pthread_mutex_init(&s_mutex, NULL);
    pthread_cond_init(&s_cond, NULL);
    s_pending = false;
    s_running = true;

    if (pthread_create(&s_thread, NULL, thumb_tx_encode_thread, NULL) != 0) {
        fprintf(stderr, "[TX_THUMB] pthread_create failed\n");
        free(s_hot_p); free(s_ready_p); free(s_enc_p);
        free(s_hot_r); free(s_ready_r); free(s_enc_r);
        s_hot_p = s_ready_p = s_enc_p = NULL;
        s_hot_r = s_ready_r = s_enc_r = NULL;
        pthread_mutex_destroy(&s_mutex);
        pthread_cond_destroy(&s_cond);
        return -1;
    }
    s_thread_started = true;

    s_inited = true;
    fprintf(stderr, "[TX_THUMB] Initialised: %ux%u → %ux%u, pgroups/pkt=%u, "
            "pkts/frame≈%u  [triple-buffered, async]\n",
            src_w, src_h, s_thumb_w, s_thumb_h, s_pgroups_per_pkt, pkts_per_frame);
    return 0;
}

int poc_thumb_tx_write(void *const planes[3], bool corrupt,
                       bool mute_p, bool mute_r)
{
    if (!s_inited || !planes[0]) return -1;

    /* ── Muted paths → solid black thumbnail ── */
    if (mute_p) memset(s_hot_p, 0, POC_TX_THUMB_RGB_SIZE);
    if (mute_r) memset(s_hot_r, 0, POC_TX_THUMB_RGB_SIZE);

    /* If both paths are muted, skip the pixel loop entirely */
    if (mute_p && mute_r) goto submit;

    const uint16_t *y_plane  = (const uint16_t *)planes[0];
    const uint16_t *cb_plane = (const uint16_t *)planes[1];
    const uint16_t *cr_plane = (const uint16_t *)planes[2];

    const uint32_t src_w  = s_src_w;
    const uint32_t src_h  = s_src_h;
    const uint32_t half_w = src_w / 2;
    const double x_scale  = (double)src_w / s_thumb_w;
    const double y_scale  = (double)src_h / s_thumb_h;
    const uint32_t ppkt   = s_pgroups_per_pkt;

    for (uint32_t ty = 0; ty < s_thumb_h; ty++) {
        uint32_t sy = (uint32_t)(ty * y_scale);
        if (sy >= src_h) sy = src_h - 1;

        uint8_t *dst_p = mute_p ? NULL : (s_hot_p + (size_t)ty * s_thumb_w * 3);
        uint8_t *dst_r = mute_r ? NULL : (s_hot_r + (size_t)ty * s_thumb_w * 3);

        for (uint32_t tx = 0; tx < s_thumb_w; tx++) {
            uint32_t sx = (uint32_t)(tx * x_scale);
            if (sx >= src_w) sx = src_w - 1;

            /* Read YCbCr from YUV422P10LE planar layout */
            int y_val  = y_plane [(size_t)sy * src_w  + sx]     & 0x3FF;
            int cb_val = cb_plane[(size_t)sy * half_w + sx / 2] & 0x3FF;
            int cr_val = cr_plane[(size_t)sy * half_w + sx / 2] & 0x3FF;

            uint8_t r, g, b;
            ycbcr10_to_rgb(y_val, cb_val, cr_val, &r, &g, &b);

            size_t off = (size_t)tx * 3;

            if (corrupt) {
                /* Map this pixel to its ST 2110-20 packet index.
                 * Hash the packet index for a pseudo-random drop pattern
                 * that is still perfectly complementary between P and R. */
                uint64_t pgroup_idx = (uint64_t)sy * half_w + sx / 2;
                uint32_t pkt_idx = (uint32_t)(pgroup_idx / ppkt);
                uint32_t seed = pkt_idx ^ s_frame_counter;
                bool drop_p = (pkt_drop_hash(seed) & 1) == 0;
                bool drop_r = !drop_p;

                if (dst_p) {
                    dst_p[off + 0] = drop_p ? 0 : r;
                    dst_p[off + 1] = drop_p ? 0 : g;
                    dst_p[off + 2] = drop_p ? 0 : b;
                }
                if (dst_r) {
                    dst_r[off + 0] = drop_r ? 0 : r;
                    dst_r[off + 1] = drop_r ? 0 : g;
                    dst_r[off + 2] = drop_r ? 0 : b;
                }
            } else {
                if (dst_p) {
                    dst_p[off + 0] = r;
                    dst_p[off + 1] = g;
                    dst_p[off + 2] = b;
                }
                if (dst_r) {
                    dst_r[off + 0] = r;
                    dst_r[off + 1] = g;
                    dst_r[off + 2] = b;
                }
            }
        }
    }

    s_frame_counter++;

submit:
    /* Hand off to background encoder thread (swap hot ↔ ready, signal) */
    thumb_tx_submit();

    return 0;
}

void poc_thumb_tx_cleanup(void)
{
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

    free(s_hot_p);   s_hot_p   = NULL;
    free(s_ready_p); s_ready_p = NULL;
    free(s_enc_p);   s_enc_p   = NULL;
    free(s_hot_r);   s_hot_r   = NULL;
    free(s_ready_r); s_ready_r = NULL;
    free(s_enc_r);   s_enc_r   = NULL;
    s_inited = false;
}
