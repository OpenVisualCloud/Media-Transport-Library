/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — v210 utility helpers
 */

#ifndef POC_V210_UTILS_H
#define POC_V210_UTILS_H

#include <stddef.h>
#include <stdint.h>

/*
 * Compute the v210 line stride in bytes for a given width.
 * This matches MXL's getV210LineLength():
 *   stride = ((width + 47) / 48) * 128
 */
uint32_t poc_v210_line_stride(uint32_t width);

/* Compute total v210 frame size: stride * height */
uint32_t poc_v210_frame_size(uint32_t width, uint32_t height);

#endif /* POC_V210_UTILS_H */
