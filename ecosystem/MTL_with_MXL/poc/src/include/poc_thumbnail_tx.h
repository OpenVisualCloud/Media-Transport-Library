/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — TX-side YUV422P10LE → JPEG thumbnails with packet-drop masking
 *
 * Triple-buffered async design (ported from poc_14):
 *   Hot path:  downscale + RGB + packet-drop masking into hot_buf_p / hot_buf_r
 *              then swap hot ↔ ready and signal the background thread.
 *   Background: JPEG compress + atomic-rename write to tmpfs.
 *
 * Generates P-path and R-path preview thumbnails.  When packet corruption
 * (drop) is active, blanks out the pixel regions corresponding to dropped
 * packets on each path, using the exact ST 2110-20 BPM packetisation mapping.
 */

#ifndef POC_THUMBNAIL_TX_H
#define POC_THUMBNAIL_TX_H

#include <stdbool.h>
#include <stdint.h>

#define POC_TX_THUMB_WIDTH 480
#define POC_TX_THUMB_HEIGHT 270
#define POC_TX_THUMB_QUALITY 75
#define POC_TX_THUMB_RGB_SIZE ((size_t)POC_TX_THUMB_WIDTH * POC_TX_THUMB_HEIGHT * 3)

/**
 * Initialise the TX-side thumbnail generator.
 * Allocates triple buffers for P and R paths and starts the background
 * JPEG encoder thread.
 * @param thumb_dir  Directory where thumb_p.jpg / thumb_r.jpg are written.
 * @param src_w      Source frame width in pixels.
 * @param src_h      Source frame height in pixels.
 * @return 0 on success.
 */
int poc_thumb_tx_init(const char* thumb_dir, uint32_t src_w, uint32_t src_h);

/**
 * Generate P and R preview thumbnails from YUV422P10LE planar data.
 *
 * Hot-path cost: nearest-neighbour downscale + RGB conversion + packet-drop
 * masking (~200 µs).  JPEG compression + file I/O happen asynchronously
 * in the background encoder thread.
 *
 * planes[0] = Y  plane (uint16_t[], width * height values)
 * planes[1] = Cb plane (uint16_t[], width/2 * height values)
 * planes[2] = Cr plane (uint16_t[], width/2 * height values)
 *
 * When @p corrupt is true, the thumbnails show which pixels each path
 * actually delivers (blanking dropped-packet regions).  When false, both
 * thumbnails show the full image.
 *
 * When @p mute_p / @p mute_r is true the corresponding thumbnail is
 * rendered as a solid black frame (path is muted).
 *
 * @param planes  Array of 3 plane pointers (Y, Cb, Cr).
 * @param corrupt Whether packet-drop masking is active.
 * @param mute_p  Whether the primary path is muted.
 * @param mute_r  Whether the redundant path is muted.
 * @return 0 on success.
 */
int poc_thumb_tx_write(void* const planes[3], bool corrupt, bool mute_p, bool mute_r);

/** Stop background thread and free resources. */
void poc_thumb_tx_cleanup(void);

#endif /* POC_THUMBNAIL_TX_H */
