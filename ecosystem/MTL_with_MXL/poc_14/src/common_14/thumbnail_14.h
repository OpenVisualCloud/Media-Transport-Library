/* SPDX-License-Identifier: BSD-3-Clause
 * poc_14 — Multi-instance thumbnail generator (no static state)
 *
 * Triple-buffered design: the hot path does only a fast downscale+RGB
 * conversion (~173 KB output) and hands the result to a background
 * encoder thread that performs JPEG compression + file I/O.
 *
 * Output directory: /dev/shm/poc14_thumbs  (tmpfs — no dirty pages).
 */

#ifndef POC14_THUMBNAIL_H
#define POC14_THUMBNAIL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

#define POC14_THUMB_WIDTH    320
#define POC14_THUMB_HEIGHT   180
#define POC14_THUMB_QUALITY  70
#define POC14_THUMB_RGB_SIZE ((size_t)POC14_THUMB_WIDTH * POC14_THUMB_HEIGHT * 3)
#define POC14_THUMB_DIR      "/dev/shm/poc14_thumbs"

typedef struct {
    /* Triple-buffered RGB24 thumbnails (downscaled) */
    uint8_t *hot_buf;           /* hot-path writes here — never touched by thread */
    uint8_t *ready_buf;         /* latest completed frame — swapped under mutex  */
    uint8_t *enc_buf;           /* encode thread's private buffer               */

    int      src_w, src_h;      /* original frame dimensions */
    int      dst_w, dst_h;      /* thumbnail dimensions */
    char     path[512];         /* output JPEG path on tmpfs */

    /* Background encoder thread */
    pthread_t       thread;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    bool            pending;        /* true  = ready_buf has a new frame  */
    bool            running;        /* false = thread should exit         */
    bool            thread_started; /* guard for cleanup                  */
} poc14_thumb_ctx_t;

/**
 * Initialize a thumbnail context for a specific stream.
 * Creates the output directory on /dev/shm and starts the background
 * encoder thread.
 *   role: "tx", "rx", or "sender"
 *   stream_id: 0..15
 *   src_w, src_h: source frame dimensions
 */
int  poc14_thumb_init(poc14_thumb_ctx_t *ctx, int stream_id,
                      const char *role, int src_w, int src_h);

/**
 * Submit an RFC 4175 YUV 422 10-bit frame for thumbnail generation.
 * Hot-path cost: nearest-neighbour downscale + RGB conversion (~100-200 µs).
 * JPEG encoding + file I/O happen asynchronously in the background thread.
 * Returns 0 on success, -1 on error (invalid input).
 */
int  poc14_thumb_write(poc14_thumb_ctx_t *ctx,
                       const uint8_t *frame_data, size_t frame_len);

/**
 * Submit a YUV422P10LE planar frame for thumbnail generation.
 * planes[0] = Y (uint16_t, width*height), planes[1] = Cb, planes[2] = Cr.
 * Hot-path cost: nearest-neighbour downscale + RGB conversion.
 * JPEG encoding + file I/O happen asynchronously in the background thread.
 * Returns 0 on success, -1 on error.
 */
int  poc14_thumb_write_planar(poc14_thumb_ctx_t *ctx,
                              void *const planes[3]);

/**
 * Stop the background encoder thread, free all buffers.
 */
void poc14_thumb_cleanup(poc14_thumb_ctx_t *ctx);

#endif /* POC14_THUMBNAIL_H */
