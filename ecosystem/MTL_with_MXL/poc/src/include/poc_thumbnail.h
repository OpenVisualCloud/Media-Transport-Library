/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — RFC 4175 → JPEG thumbnail generator (async triple-buffered)
 *
 * Triple-buffered design (ported from poc_14):
 *   Hot path:  nearest-neighbour downscale RFC 4175 → RGB into hot_buf,
 *              swap hot ↔ ready, signal encoder thread.
 *   Background: JPEG compress + atomic-rename write to disk.
 */

#ifndef POC_THUMBNAIL_H
#define POC_THUMBNAIL_H

#include <stdint.h>
#include <stdbool.h>

/* Default thumbnail dimensions (downscaled from source) */
#define POC_THUMB_WIDTH   480
#define POC_THUMB_HEIGHT  270
#define POC_THUMB_QUALITY 75      /* JPEG quality 1..100 */
#define POC_THUMB_RGB_SIZE ((size_t)POC_THUMB_WIDTH * POC_THUMB_HEIGHT * 3)

/* Initialise the thumbnail subsystem.
 * Allocates triple buffers and starts the background encoder thread.
 * thumb_dir: directory where thumb.jpg will be written (must exist).
 * src_width/src_height: source frame dimensions.
 * Returns 0 on success.  Safe to call multiple times (no-op after first). */
int poc_thumbnail_init(const char *thumb_dir,
                       uint32_t src_width, uint32_t src_height);

/* Generate a JPEG thumbnail from an RFC 4175 422 10-bit frame.
 * Hot-path cost: nearest-neighbour downscale + RGB conversion (~100-200 µs).
 * JPEG encoding + file I/O happen asynchronously in the background thread.
 * rfc4175_data: pointer to raw RFC 4175 frame (stride = width * 5 / 2).
 * frame_size: total bytes (for bounds checking).
 * Returns 0 on success, -1 on error (logged to stderr). */
int poc_thumbnail_write(const uint8_t *rfc4175_data, uint32_t frame_size);

/* Stop background encoder thread and free resources. */
void poc_thumbnail_cleanup(void);

#endif /* POC_THUMBNAIL_H */
