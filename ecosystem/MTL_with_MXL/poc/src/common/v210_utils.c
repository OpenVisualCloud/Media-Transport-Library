/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — v210 utility helpers
 */

#include "poc_v210_utils.h"

uint32_t poc_v210_line_stride(uint32_t width) {
  /* Matches MXL's getV210LineLength():
   *   stride = ((width + 47) / 48) * 128
   */
  return ((width + 47) / 48) * 128;
}

uint32_t poc_v210_frame_size(uint32_t width, uint32_t height) {
  return poc_v210_line_stride(width) * height;
}
