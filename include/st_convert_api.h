/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

/**
 * @file st_convert_api.h
 *
 * This header define the public interfaces of streaming(st2110) format conversion
 * toolkit
 *
 */

#include "st_convert_internal.h"

#ifndef _ST_CONVERT_API_HEAD_H_
#define _ST_CONVERT_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * Convert rfc4175_422be10 to yuv422p10le with the max optimized SIMD level.
 *
 * @param pg
 *   Point to pg(rfc4175_422be10) data.
 * @param y
 *   Point to Y(yuv422p10le) vector.
 * @param b
 *   Point to b(yuv422p10le) vector.
 * @param r
 *   Point to r(yuv422p10le) vector.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_422be10_to_yuv422p10le(
    struct st20_rfc4175_422_10_pg2_be* pg, uint16_t* y, uint16_t* b, uint16_t* r,
    uint32_t w, uint32_t h) {
  return st20_rfc4175_422be10_to_yuv422p10le_simd(pg, y, b, r, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_422be10 to rfc4175_422le10 with the max optimized SIMD level.
 *
 * @param pg_be
 *   Point to pg(rfc4175_422be10) data.
 * @param pg_le
 *   Point to pg(rfc4175_422le10) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_422be10_to_422le10(
    struct st20_rfc4175_422_10_pg2_be* pg_be, struct st20_rfc4175_422_10_pg2_le* pg_le,
    uint32_t w, uint32_t h) {
  return st20_rfc4175_422be10_to_422le10_simd(pg_be, pg_le, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_422be10 to v210 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param pg_be
 *   Point to pg(rfc4175_422be10) data.
 * @param pg_v210
 *   Point to pg(v210) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_422be10_to_v210(struct st20_rfc4175_422_10_pg2_be* pg_be,
                                               uint8_t* pg_v210, uint32_t w, uint32_t h) {
  return st20_rfc4175_422be10_to_v210_simd(pg_be, pg_v210, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_422be10 to rfc4175_422le8(packed UYVY) with the max optimized SIMD
 * level.
 *
 * @param pg_10
 *   Point to pg(rfc4175_422be10) data.
 * @param pg_8
 *   Point to pg(rfc4175_422le8) packed UYVY data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_422be10_to_422le8(struct st20_rfc4175_422_10_pg2_be* pg_10,
                                                 struct st20_rfc4175_422_8_pg2_le* pg_8,
                                                 uint32_t w, uint32_t h) {
  return st20_rfc4175_422be10_to_422le8_simd(pg_10, pg_8, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_422be10 to yuv422p8 with the max optimized SIMD level.
 *
 * @param pg
 *   Point to pg(rfc4175_422be10) data.
 * @param y
 *   Point to Y(yuv422p8) vector.
 * @param b
 *   Point to b(yuv422p8) vector.
 * @param r
 *   Point to r(yuv422p8) vector.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_422be10_to_yuv422p8(struct st20_rfc4175_422_10_pg2_be* pg,
                                                   uint8_t* y, uint8_t* b, uint8_t* r,
                                                   uint32_t w, uint32_t h) {
  return st20_rfc4175_422be10_to_yuv422p8_simd(pg, y, b, r, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_422be10 to yuv420p8 with the max optimized SIMD level.
 *
 * @param pg
 *   Point to pg(rfc4175_422be10) data.
 * @param y
 *   Point to Y(yuv420p8) vector.
 * @param b
 *   Point to b(yuv420p8) vector.
 * @param r
 *   Point to r(yuv420p8) vector.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_422be10_to_yuv420p8(struct st20_rfc4175_422_10_pg2_be* pg,
                                                   uint8_t* y, uint8_t* b, uint8_t* r,
                                                   uint32_t w, uint32_t h) {
  return st20_rfc4175_422be10_to_yuv420p8_simd(pg, y, b, r, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_422be12 to yuv422p12le with the max optimized SIMD level.
 *
 * @param pg
 *   Point to pg(rfc4175_422be12) data.
 * @param y
 *   Point to Y(yuv422p12le) vector.
 * @param b
 *   Point to b(yuv422p12le) vector.
 * @param r
 *   Point to r(yuv422p12le) vector.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_422be12_to_yuv422p12le(
    struct st20_rfc4175_422_12_pg2_be* pg, uint16_t* y, uint16_t* b, uint16_t* r,
    uint32_t w, uint32_t h) {
  return st20_rfc4175_422be12_to_yuv422p12le_simd(pg, y, b, r, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_422be12 to rfc4175_422le12 with the max optimized SIMD level.
 *
 * @param pg_be
 *   Point to pg(rfc4175_422be12) data.
 * @param pg_le
 *   Point to pg(rfc4175_422le12) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_422be12_to_422le12(
    struct st20_rfc4175_422_12_pg2_be* pg_be, struct st20_rfc4175_422_12_pg2_le* pg_le,
    uint32_t w, uint32_t h) {
  return st20_rfc4175_422be12_to_422le12_simd(pg_be, pg_le, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_444be10 to yuv444p10le with the max optimized SIMD level.
 *
 * @param pg
 *   Point to pg(rfc4175_444be10) data.
 * @param y
 *   Point to Y(yuv444p10le) vector.
 * @param b
 *   Point to b(yuv444p10le) vector.
 * @param r
 *   Point to r(yuv444p10le) vector.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_444be10_to_yuv444p10le(
    struct st20_rfc4175_444_10_pg4_be* pg, uint16_t* y, uint16_t* b, uint16_t* r,
    uint32_t w, uint32_t h) {
  return st20_rfc4175_444be10_to_444p10le_simd(pg, y, b, r, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_444be10 to gbrp10le with the max optimized SIMD level.
 *
 * @param pg
 *   Point to pg(rfc4175_444be10) data.
 * @param g
 *   Point to g(gbrp10le) vector.
 * @param b
 *   Point to b(gbrp10le) vector.
 * @param r
 *   Point to r(gbrp10le) vector.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_444be10_to_gbrp10le(struct st20_rfc4175_444_10_pg4_be* pg,
                                                   uint16_t* g, uint16_t* b, uint16_t* r,
                                                   uint32_t w, uint32_t h) {
  return st20_rfc4175_444be10_to_444p10le_simd(pg, g, r, b, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_444be10 to rfc4175_444le10 with the max optimized SIMD level.
 *
 * @param pg_be
 *   Point to pg(rfc4175_444be10) data.
 * @param pg_le
 *   Point to pg(rfc4175_444le10) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_444be10_to_444le10(
    struct st20_rfc4175_444_10_pg4_be* pg_be, struct st20_rfc4175_444_10_pg4_le* pg_le,
    uint32_t w, uint32_t h) {
  return st20_rfc4175_444be10_to_444le10_simd(pg_be, pg_le, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_444be12 to yuv444p12le with the max optimized SIMD level.
 *
 * @param pg
 *   Point to pg(rfc4175_444be12) data.
 * @param y
 *   Point to Y(yuv444p12le) vector.
 * @param b
 *   Point to b(yuv444p12le) vector.
 * @param r
 *   Point to r(yuv444p12le) vector.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_444be12_to_yuv444p12le(
    struct st20_rfc4175_444_12_pg2_be* pg, uint16_t* y, uint16_t* b, uint16_t* r,
    uint32_t w, uint32_t h) {
  return st20_rfc4175_444be12_to_444p12le_simd(pg, y, b, r, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_444be12 to gbrp12le with the max optimized SIMD level.
 *
 * @param pg
 *   Point to pg(rfc4175_444be12) data.
 * @param g
 *   Point to g(gbrp12le) vector.
 * @param b
 *   Point to b(gbrp12le) vector.
 * @param r
 *   Point to r(gbrp12le) vector.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_444be12_to_gbrp12le(struct st20_rfc4175_444_12_pg2_be* pg,
                                                   uint16_t* g, uint16_t* b, uint16_t* r,
                                                   uint32_t w, uint32_t h) {
  return st20_rfc4175_444be12_to_444p12le_simd(pg, g, r, b, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_444be12 to rfc4175_444le12 with the max optimized SIMD level.
 *
 * @param pg_be
 *   Point to pg(rfc4175_444be12) data.
 * @param pg_le
 *   Point to pg(rfc4175_444le12) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_444be12_to_444le12(
    struct st20_rfc4175_444_12_pg2_be* pg_be, struct st20_rfc4175_444_12_pg2_le* pg_le,
    uint32_t w, uint32_t h) {
  return st20_rfc4175_444be12_to_444le12_simd(pg_be, pg_le, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert yuv422p10le to rfc4175_422be10.
 *
 * @param y
 *   Point to Y(yuv422p10le) vector.
 * @param b
 *   Point to b(yuv422p10le) vector.
 * @param r
 *   Point to r(yuv422p10le) vector.
 * @param pg
 *   Point to pg(rfc4175_422be10) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_yuv422p10le_to_rfc4175_422be10(
    uint16_t* y, uint16_t* b, uint16_t* r, struct st20_rfc4175_422_10_pg2_be* pg,
    uint32_t w, uint32_t h) {
  return st20_yuv422p10le_to_rfc4175_422be10_simd(y, b, r, pg, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert v210 to rfc4175_422be10 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param pg_v210
 *   Point to pg(v210) data.
 * @param pg_be
 *   Point to pg(rfc4175_422be10) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_v210_to_rfc4175_422be10(uint8_t* pg_v210,
                                               struct st20_rfc4175_422_10_pg2_be* pg_be,
                                               uint32_t w, uint32_t h) {
  return st20_v210_to_rfc4175_422be10_simd(pg_v210, pg_be, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_422le10 to rfc4175_422be10.
 *
 * @param pg_le
 *   Point to pg(rfc4175_422le10) data.
 * @param pg_be
 *   Point to pg(rfc4175_422be10) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_422le10_to_422be10(
    struct st20_rfc4175_422_10_pg2_le* pg_le, struct st20_rfc4175_422_10_pg2_be* pg_be,
    uint32_t w, uint32_t h) {
  return st20_rfc4175_422le10_to_422be10_simd(pg_le, pg_be, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert yuv422p10le to rfc4175_422le10.
 *
 * @param y
 *   Point to Y(yuv422p10le) vector.
 * @param b
 *   Point to b(yuv422p10le) vector.
 * @param r
 *   Point to r(yuv422p10le) vector.
 * @param pg
 *   Point to pg(rfc4175_422le10) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_yuv422p10le_to_rfc4175_422le10(uint16_t* y, uint16_t* b, uint16_t* r,
                                        struct st20_rfc4175_422_10_pg2_le* pg, uint32_t w,
                                        uint32_t h);

/**
 * Convert rfc4175_422le10 to yuv422p10le.
 *
 * @param pg
 *   Point to pg(rfc4175_422le10) data.
 * @param y
 *   Point to Y(yuv422p10le) vector.
 * @param b
 *   Point to b(yuv422p10le) vector.
 * @param r
 *   Point to r(yuv422p10le) vector.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_422le10_to_yuv422p10le(struct st20_rfc4175_422_10_pg2_le* pg,
                                        uint16_t* y, uint16_t* b, uint16_t* r, uint32_t w,
                                        uint32_t h);

/**
 * Convert rfc4175_422le10 to v210 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param pg_le
 *   Point to pg(rfc4175_422le10) data.
 * @param pg_v210
 *   Point to pg(v210) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_422le10_to_v210(uint8_t* pg_le, uint8_t* pg_v210,
                                               uint32_t w, uint32_t h) {
  return st20_rfc4175_422le10_to_v210_simd(pg_le, pg_v210, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_422be10 to y210 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param pg_be
 *   Point to pg(rfc4175_422be10) data.
 * @param pg_y210
 *   Point to pg(y210) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_422be10_to_y210(struct st20_rfc4175_422_10_pg2_be* pg_be,
                                               uint16_t* pg_y210, uint32_t w,
                                               uint32_t h) {
  return st20_rfc4175_422be10_to_y210_simd(pg_be, pg_y210, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert y210 to rfc4175_422be10 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param pg_y210
 *   Point to pg(y210) data.
 * @param pg_be
 *   Point to pg(rfc4175_422be10) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_y210_to_rfc4175_422be10(uint16_t* pg_y210,
                                               struct st20_rfc4175_422_10_pg2_be* pg_be,
                                               uint32_t w, uint32_t h) {
  return st20_y210_to_rfc4175_422be10_simd(pg_y210, pg_be, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert v210 to rfc4175_422le10.
 *
 * @param pg_v210
 *   Point to pg(v210) data.
 * @param pg_le
 *   Point to pg(rfc4175_422le10) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_v210_to_rfc4175_422le10(uint8_t* pg_v210, uint8_t* pg_le, uint32_t w,
                                 uint32_t h);

/**
 * Convert yuv422p12le to rfc4175_422be12.
 *
 * @param y
 *   Point to Y(yuv422p12le) vector.
 * @param b
 *   Point to b(yuv422p12le) vector.
 * @param r
 *   Point to r(yuv422p12le) vector.
 * @param pg
 *   Point to pg(rfc4175_422be12) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_yuv422p12le_to_rfc4175_422be12(
    uint16_t* y, uint16_t* b, uint16_t* r, struct st20_rfc4175_422_12_pg2_be* pg,
    uint32_t w, uint32_t h) {
  return st20_yuv422p12le_to_rfc4175_422be12_simd(y, b, r, pg, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_422le12 to rfc4175_422be12.
 *
 * @param pg_le
 *   Point to pg(rfc4175_422le12) data.
 * @param pg_be
 *   Point to pg(rfc4175_422be12) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_422le12_to_422be12(
    struct st20_rfc4175_422_12_pg2_le* pg_le, struct st20_rfc4175_422_12_pg2_be* pg_be,
    uint32_t w, uint32_t h) {
  return st20_rfc4175_422le12_to_422be12_simd(pg_le, pg_be, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert yuv422p12le to rfc4175_422le12.
 *
 * @param y
 *   Point to Y(yuv422p12le) vector.
 * @param b
 *   Point to b(yuv422p12le) vector.
 * @param r
 *   Point to r(yuv422p12le) vector.
 * @param pg
 *   Point to pg(rfc4175_422le12) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_yuv422p12le_to_rfc4175_422le12(uint16_t* y, uint16_t* b, uint16_t* r,
                                        struct st20_rfc4175_422_12_pg2_le* pg, uint32_t w,
                                        uint32_t h);

/**
 * Convert rfc4175_422le12 to yuv422p12le.
 *
 * @param pg
 *   Point to pg(rfc4175_422le12) data.
 * @param y
 *   Point to Y(yuv422p12le) vector.
 * @param b
 *   Point to b(yuv422p12le) vector.
 * @param r
 *   Point to r(yuv422p12le) vector.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_422le12_to_yuv422p12le(struct st20_rfc4175_422_12_pg2_le* pg,
                                        uint16_t* y, uint16_t* b, uint16_t* r, uint32_t w,
                                        uint32_t h);

/**
 * Convert yuv444p10le to rfc4175_444be10.
 *
 * @param y
 *   Point to Y(yuv444p10le) vector.
 * @param b
 *   Point to b(yuv444p10le) vector.
 * @param r
 *   Point to r(yuv444p10le) vector.
 * @param pg
 *   Point to pg(rfc4175_444be10) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_yuv444p10le_to_rfc4175_444be10(
    uint16_t* y, uint16_t* b, uint16_t* r, struct st20_rfc4175_444_10_pg4_be* pg,
    uint32_t w, uint32_t h) {
  return st20_444p10le_to_rfc4175_444be10_simd(y, b, r, pg, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_444le10 to rfc4175_444be10.
 *
 * @param pg_le
 *   Point to pg(rfc4175_444le10) data.
 * @param pg_be
 *   Point to pg(rfc4175_444be10) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_444le10_to_444be10(
    struct st20_rfc4175_444_10_pg4_le* pg_le, struct st20_rfc4175_444_10_pg4_be* pg_be,
    uint32_t w, uint32_t h) {
  return st20_rfc4175_444le10_to_444be10_simd(pg_le, pg_be, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert yuv444p10le to rfc4175_444le10.
 *
 * @param y
 *   Point to Y(yuv444p10le) vector.
 * @param b
 *   Point to b(yuv444p10le) vector.
 * @param r
 *   Point to r(yuv444p10le) vector.
 * @param pg
 *   Point to pg(rfc4175_444le10) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_yuv444p10le_to_rfc4175_444le10(
    uint16_t* y, uint16_t* b, uint16_t* r, struct st20_rfc4175_444_10_pg4_le* pg,
    uint32_t w, uint32_t h) {
  return st20_444p10le_to_rfc4175_444le10(y, b, r, pg, w, h);
}

/**
 * Convert rfc4175_444le10 to yuv444p10le.
 *
 * @param pg
 *   Point to pg(rfc4175_444le10) data.
 * @param y
 *   Point to Y(yuv444p10le) vector.
 * @param b
 *   Point to b(yuv444p10le) vector.
 * @param r
 *   Point to r(yuv444p10le) vector.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_444le10_to_yuv444p10le(
    struct st20_rfc4175_444_10_pg4_le* pg, uint16_t* y, uint16_t* b, uint16_t* r,
    uint32_t w, uint32_t h) {
  return st20_rfc4175_444le10_to_444p10le(pg, y, b, r, w, h);
}

/**
 * Convert gbrp10le to rfc4175_444be10.
 *
 * @param g
 *   Point to g(gbrp10le) vector.
 * @param b
 *   Point to b(gbrp10le) vector.
 * @param r
 *   Point to r(gbrp10le) vector.
 * @param pg
 *   Point to pg(rfc4175_444be10) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_gbrp10le_to_rfc4175_444be10(uint16_t* g, uint16_t* b, uint16_t* r,
                                                   struct st20_rfc4175_444_10_pg4_be* pg,
                                                   uint32_t w, uint32_t h) {
  return st20_444p10le_to_rfc4175_444be10_simd(g, r, b, pg, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert gbrp10le to rfc4175_444le10.
 *
 * @param g
 *   Point to g(gbrp10le) vector.
 * @param b
 *   Point to b(gbrp10le) vector.
 * @param r
 *   Point to r(gbrp10le) vector.
 * @param pg
 *   Point to pg(rfc4175_444le10) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_gbrp10le_to_rfc4175_444le10(uint16_t* g, uint16_t* b, uint16_t* r,
                                                   struct st20_rfc4175_444_10_pg4_le* pg,
                                                   uint32_t w, uint32_t h) {
  return st20_444p10le_to_rfc4175_444le10(g, r, b, pg, w, h);
}

/**
 * Convert rfc4175_444le10 to gbrp10le.
 *
 * @param pg
 *   Point to pg(rfc4175_444le10) data.
 * @param g
 *   Point to g(gbrp10le) vector.
 * @param b
 *   Point to b(gbrp10le) vector.
 * @param r
 *   Point to r(gbrp10le) vector.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_444le10_to_gbrp10le(struct st20_rfc4175_444_10_pg4_le* pg,
                                                   uint16_t* g, uint16_t* b, uint16_t* r,
                                                   uint32_t w, uint32_t h) {
  return st20_rfc4175_444le10_to_444p10le(pg, g, r, b, w, h);
}

/**
 * Convert yuv444p12le to rfc4175_444be12.
 *
 * @param y
 *   Point to Y(yuv444p12le) vector.
 * @param b
 *   Point to b(yuv444p12le) vector.
 * @param r
 *   Point to r(yuv444p12le) vector.
 * @param pg
 *   Point to pg(rfc4175_444be12) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_yuv444p12le_to_rfc4175_444be12(
    uint16_t* y, uint16_t* b, uint16_t* r, struct st20_rfc4175_444_12_pg2_be* pg,
    uint32_t w, uint32_t h) {
  return st20_444p12le_to_rfc4175_444be12_simd(y, b, r, pg, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_444le12 to rfc4175_444be12.
 *
 * @param pg_le
 *   Point to pg(rfc4175_444le12) data.
 * @param pg_be
 *   Point to pg(rfc4175_444be12) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_444le12_to_444be12(
    struct st20_rfc4175_444_12_pg2_le* pg_le, struct st20_rfc4175_444_12_pg2_be* pg_be,
    uint32_t w, uint32_t h) {
  return st20_rfc4175_444le12_to_444be12_simd(pg_le, pg_be, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert yuv444p12le to rfc4175_444le12.
 *
 * @param y
 *   Point to Y(yuv444p12le) vector.
 * @param b
 *   Point to b(yuv444p12le) vector.
 * @param r
 *   Point to r(yuv444p12le) vector.
 * @param pg
 *   Point to pg(rfc4175_444le12) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_yuv444p12le_to_rfc4175_444le12(
    uint16_t* y, uint16_t* b, uint16_t* r, struct st20_rfc4175_444_12_pg2_le* pg,
    uint32_t w, uint32_t h) {
  return st20_444p12le_to_rfc4175_444le12(y, b, r, pg, w, h);
}

/**
 * Convert rfc4175_444le12 to yuv444p12le.
 *
 * @param pg
 *   Point to pg(rfc4175_444le12) data.
 * @param y
 *   Point to Y(yuv444p12le) vector.
 * @param b
 *   Point to b(yuv444p12le) vector.
 * @param r
 *   Point to r(yuv444p12le) vector.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_444le12_to_yuv444p12le(
    struct st20_rfc4175_444_12_pg2_le* pg, uint16_t* y, uint16_t* b, uint16_t* r,
    uint32_t w, uint32_t h) {
  return st20_rfc4175_444le12_to_444p12le(pg, y, b, r, w, h);
}

/**
 * Convert gbrp12le to rfc4175_444be12.
 *
 * @param g
 *   Point to g(gbrp12le) vector.
 * @param b
 *   Point to b(gbrp12le) vector.
 * @param r
 *   Point to r(gbrp12le) vector.
 * @param pg
 *   Point to pg(rfc4175_444be12) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_gbrp12le_to_rfc4175_444be12(uint16_t* g, uint16_t* b, uint16_t* r,
                                                   struct st20_rfc4175_444_12_pg2_be* pg,
                                                   uint32_t w, uint32_t h) {
  return st20_444p12le_to_rfc4175_444be12_simd(g, r, b, pg, w, h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert gbrp12le to rfc4175_444le12.
 *
 * @param g
 *   Point to g(gbrp12le) vector.
 * @param b
 *   Point to b(gbrp12le) vector.
 * @param r
 *   Point to r(gbrp12le) vector.
 * @param pg
 *   Point to pg(rfc4175_444le12) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_gbrp12le_to_rfc4175_444le12(uint16_t* g, uint16_t* b, uint16_t* r,
                                                   struct st20_rfc4175_444_12_pg2_le* pg,
                                                   uint32_t w, uint32_t h) {
  return st20_444p12le_to_rfc4175_444le12(g, r, b, pg, w, h);
}

/**
 * Convert rfc4175_444le12 to gbrp12le.
 *
 * @param pg
 *   Point to pg(rfc4175_444le12) data.
 * @param g
 *   Point to g(gbrp12le) vector.
 * @param b
 *   Point to b(gbrp12le) vector.
 * @param r
 *   Point to r(gbrp12le) vector.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_444le12_to_gbrp12le(struct st20_rfc4175_444_12_pg2_le* pg,
                                                   uint16_t* g, uint16_t* b, uint16_t* r,
                                                   uint32_t w, uint32_t h) {
  return st20_rfc4175_444le12_to_444p12le(pg, g, r, b, w, h);
}

/**
 * Convert AM824 subframe to AES3 subframe.
 *
 * @param sf_am824
 *   Point to AM824 data.
 * @param sf_aes3
 *   Point to AES3 data.
 * @param subframes
 *   The subframes number
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st31_am824_to_aes3(struct st31_am824* sf_am824, struct st31_aes3* sf_aes3,
                       uint16_t subframes);

/**
 * Convert AES3 subframe to AM824 subframe.
 *
 * @param sf_aes3
 *   Point to AES3 data.
 * @param sf_am824
 *   Point to AM824 data.
 * @param subframes
 *   The subframes number
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st31_aes3_to_am824(struct st31_aes3* sf_aes3, struct st31_am824* sf_am824,
                       uint16_t subframes);

#if defined(__cplusplus)
}
#endif

#endif
