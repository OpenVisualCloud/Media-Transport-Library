/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

/**
 * @file st_convert_api.h
 *
 * This header define the public interfaces of Media Transport Library Format
 * Conversion Toolkit
 *
 */

#include "st_convert_internal.h"

#ifndef _ST_CONVERT_API_HEAD_H_
#define _ST_CONVERT_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * Convert rfc4175_422be10 to yuv422p10le with the max optimised SIMD level.
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
 * Convert rfc4175_422be10 to yuv422p10le with the max optimised SIMD level and DMA
 * helper. Profiling shows gain with 4k/8k solution due to LLC cache miss migration, thus
 * pls only applied with 4k/8k.
 *
 * @param udma
 *   Point to dma engine.
 * @param pg_be
 *   Point to pg(rfc4175_422be10) data.
 * @param pg_be_iova
 *   The mtl_iova_t address of the pg_be buffer.
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
static inline int st20_rfc4175_422be10_to_yuv422p10le_dma(
    mtl_udma_handle udma, struct st20_rfc4175_422_10_pg2_be* pg_be, mtl_iova_t pg_be_iova,
    uint16_t* y, uint16_t* b, uint16_t* r, uint32_t w, uint32_t h) {
  return st20_rfc4175_422be10_to_yuv422p10le_simd_dma(udma, pg_be, pg_be_iova, y, b, r, w,
                                                      h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_422be10 to rfc4175_422le10 with the max optimised SIMD level.
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
 * Convert rfc4175_422be10 to rfc4175_422le10 with max SIMD level and DMA helper.
 * Profiling shows gain with 4k/8k solution due to LLC cache miss migration, thus pls
 * only applied with 4k/8k.
 *
 * @param udma
 *   Point to dma engine.
 * @param pg_be
 *   Point to pg(rfc4175_422be10) data.
 * @param pg_be_iova
 *   The mtl_iova_t address of the pg_be buffer.
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
static inline int st20_rfc4175_422be10_to_422le10_dma(
    mtl_udma_handle udma, struct st20_rfc4175_422_10_pg2_be* pg_be, mtl_iova_t pg_be_iova,
    struct st20_rfc4175_422_10_pg2_le* pg_le, uint32_t w, uint32_t h) {
  return st20_rfc4175_422be10_to_422le10_simd_dma(udma, pg_be, pg_be_iova, pg_le, w, h,
                                                  MTL_SIMD_LEVEL_MAX);
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
 * Convert rfc4175_422be10 to v210 with max SIMD level and DMA helper.
 * Profiling shows gain with 4k/8k solution due to LLC cache miss migration, thus pls
 * only applied with 4k/8k.
 *
 * @param udma
 *   Point to dma engine.
 * @param pg_be
 *   Point to pg(rfc4175_422be10) data.
 * @param pg_be_iova
 *   The mtl_iova_t address of the pg_be buffer.
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
static inline int st20_rfc4175_422be10_to_v210_dma(
    mtl_udma_handle udma, struct st20_rfc4175_422_10_pg2_be* pg_be, mtl_iova_t pg_be_iova,
    uint8_t* pg_v210, uint32_t w, uint32_t h) {
  return st20_rfc4175_422be10_to_v210_simd_dma(udma, pg_be, pg_be_iova, pg_v210, w, h,
                                               MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_422be10 to rfc4175_422le8 with the max optimised SIMD level.
 *
 * @param pg_10
 *   Point to pg(rfc4175_422be10) data.
 * @param pg_8
 *   Point to pg(rfc4175_422le8) data.
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
 * Convert rfc4175_422be10 to rfc4175_422le8 with max SIMD level and DMA helper.
 * Profiling shows gain with 4k/8k solution due to LLC cache miss migration, thus pls
 * only applied with 4k/8k.
 *
 * @param udma
 *   Point to dma engine.
 * @param pg_10
 *   Point to pg(rfc4175_422be10) data.
 * @param pg_10_iova
 *   The mtl_iova_t address of the pg_10 buffer.
 * @param pg_8
 *   Point to pg(rfc4175_422le8) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_422be10_to_422le8_dma(
    mtl_udma_handle udma, struct st20_rfc4175_422_10_pg2_be* pg_10, mtl_iova_t pg_10_iova,
    struct st20_rfc4175_422_8_pg2_le* pg_8, uint32_t w, uint32_t h) {
  return st20_rfc4175_422be10_to_422le8_simd_dma(udma, pg_10, pg_10_iova, pg_8, w, h,
                                                 MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_422be12 to yuv422p12le with the max optimised SIMD level.
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
 * Convert rfc4175_422be12 to yuv422p12le with the max optimised SIMD level and DMA
 * helper. Profiling shows gain with 4k/8k solution due to LLC cache miss migration, thus
 * pls only applied with 4k/8k.
 *
 * @param udma
 *   Point to dma engine.
 * @param pg_be
 *   Point to pg(rfc4175_422be12) data.
 * @param pg_be_iova
 *   The mtl_iova_t address of the pg_be buffer.
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
static inline int st20_rfc4175_422be12_to_yuv422p12le_dma(
    mtl_udma_handle udma, struct st20_rfc4175_422_12_pg2_be* pg_be, mtl_iova_t pg_be_iova,
    uint16_t* y, uint16_t* b, uint16_t* r, uint32_t w, uint32_t h) {
  return st20_rfc4175_422be12_to_yuv422p12le_simd_dma(udma, pg_be, pg_be_iova, y, b, r, w,
                                                      h, MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_422be12 to rfc4175_422le12 with the max optimised SIMD level.
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
 * Convert rfc4175_422be12 to rfc4175_422le12 with max SIMD level and DMA helper.
 * Profiling shows gain with 4k/8k solution due to LLC cache miss migration, thus pls
 * only applied with 4k/8k.
 *
 * @param udma
 *   Point to dma engine.
 * @param pg_be
 *   Point to pg(rfc4175_422be12) data.
 * @param pg_be_iova
 *   The mtl_iova_t address of the pg_be buffer.
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
static inline int st20_rfc4175_422be12_to_422le12_dma(
    mtl_udma_handle udma, struct st20_rfc4175_422_12_pg2_be* pg_be, mtl_iova_t pg_be_iova,
    struct st20_rfc4175_422_12_pg2_le* pg_le, uint32_t w, uint32_t h) {
  return st20_rfc4175_422be12_to_422le12_simd_dma(udma, pg_be, pg_be_iova, pg_le, w, h,
                                                  MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_444be10 to yuv444p10le with the max optimised SIMD level.
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
 * Convert rfc4175_444be10 to gbrp10le with the max optimised SIMD level.
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
 * Convert rfc4175_444be10 to rfc4175_444le10 with the max optimised SIMD level.
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
 * Convert rfc4175_444be12 to yuv444p12le with the max optimised SIMD level.
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
 * Convert rfc4175_444be12 to gbrp12le with the max optimised SIMD level.
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
 * Convert rfc4175_444be12 to rfc4175_444le12 with the max optimised SIMD level.
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
 * Convert yuv422p10le to rfc4175_422be10 with DMA helper.
 *
 * @param udma
 *   Point to dma engine.
 * @param y
 *   Point to Y(yuv422p10le) vector.
 * @param y_iova
 *   IOVA address of the y buffer.
 * @param b
 *   Point to b(yuv422p10le) vector.
 * @param b_iova
 *   IOVA address of the b buffer.
 * @param r
 *   Point to r(yuv422p10le) vector.
 * @param r_iova
 *   IOVA address of the r buffer.
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
static inline int st20_yuv422p10le_to_rfc4175_422be10_dma(
    mtl_udma_handle udma, uint16_t* y, mtl_iova_t y_iova, uint16_t* b, mtl_iova_t b_iova,
    uint16_t* r, mtl_iova_t r_iova, struct st20_rfc4175_422_10_pg2_be* pg, uint32_t w,
    uint32_t h) {
  return st20_yuv422p10le_to_rfc4175_422be10_simd_dma(
      udma, y, y_iova, b, b_iova, r, r_iova, pg, w, h, MTL_SIMD_LEVEL_MAX);
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
 * Convert v210 to rfc4175_422be10 with required SIMD level and DMA helper.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param udma
 *   Point to dma engine.
 * @param pg_v210
 *   Point to pg(v210) data.
 * @param pg_v210_iova
 *   IOVA address for pg_v210 data.
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
static inline int st20_v210_to_rfc4175_422be10_dma(
    mtl_udma_handle udma, uint8_t* pg_v210, mtl_iova_t pg_v210_iova,
    struct st20_rfc4175_422_10_pg2_be* pg_be, uint32_t w, uint32_t h) {
  return st20_v210_to_rfc4175_422be10_simd_dma(udma, pg_v210, pg_v210_iova, pg_be, w, h,
                                               MTL_SIMD_LEVEL_MAX);
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
 * Convert rfc4175_422le10 to rfc4175_422be10 with max SIMD level and DMA helper.
 * Profiling shows gain with 4k/8k solution due to LLC cache miss migration, thus pls
 * only applied with 4k/8k.
 *
 * @param udma
 *   Point to dma engine.
 * @param pg_le
 *   Point to pg(rfc4175_422le10) data.
 * @param pg_le_iova
 *   The mtl_iova_t address of the pg_le buffer.
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
static inline int st20_rfc4175_422le10_to_422be10_dma(
    mtl_udma_handle udma, struct st20_rfc4175_422_10_pg2_le* pg_le, mtl_iova_t pg_le_iova,
    struct st20_rfc4175_422_10_pg2_be* pg_be, uint32_t w, uint32_t h) {
  return st20_rfc4175_422le10_to_422be10_simd_dma(udma, pg_le, pg_le_iova, pg_be, w, h,
                                                  MTL_SIMD_LEVEL_MAX);
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
 * Convert rfc4175_422be10 to y210 with max SIMD level and DMA helper.
 * Profiling shows gain with 4k/8k solution due to LLC cache miss migration, thus pls
 * only applied with 4k/8k.
 *
 * @param udma
 *   Point to dma engine.
 * @param pg_be
 *   Point to pg(rfc4175_422be10) data.
 * @param pg_be_iova
 *   The mtl_iova_t address of the pg_be buffer.
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
static inline int st20_rfc4175_422be10_to_y210_dma(
    mtl_udma_handle udma, struct st20_rfc4175_422_10_pg2_be* pg_be, mtl_iova_t pg_be_iova,
    uint16_t* pg_y210, uint32_t w, uint32_t h) {
  return st20_rfc4175_422be10_to_y210_simd_dma(udma, pg_be, pg_be_iova, pg_y210, w, h,
                                               MTL_SIMD_LEVEL_MAX);
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
 * Convert y210 to rfc4175_422be10 with required SIMD level and DMA helper.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param udma
 *   Point to dma engine.
 * @param pg_y210
 *   Point to pg(y210) data.
 * @param pg_y210_iova
 *   IOVA address for pg_y210 data.
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
static inline int st20_y210_to_rfc4175_422be10_dma(
    mtl_udma_handle udma, uint16_t* pg_y210, mtl_iova_t pg_y210_iova,
    struct st20_rfc4175_422_10_pg2_be* pg_be, uint32_t w, uint32_t h) {
  return st20_y210_to_rfc4175_422be10_simd_dma(udma, pg_y210, pg_y210_iova, pg_be, w, h,
                                               MTL_SIMD_LEVEL_MAX);
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
