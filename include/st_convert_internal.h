/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

/**
 * @file st_convert_internal.h
 *
 * This header define the internal interfaces of Media Transport Library Format
 * Conversion Toolkit
 *
 */

#include "st20_api.h"
#include "st30_api.h"

#ifndef _ST_CONVERT_INTERNAL_HEAD_H_
#define _ST_CONVERT_INTERNAL_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * Convert rfc4175_422be10 to yuv422p10le with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
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
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_422be10_to_yuv422p10le_simd(struct st20_rfc4175_422_10_pg2_be* pg,
                                             uint16_t* y, uint16_t* b, uint16_t* r,
                                             uint32_t w, uint32_t h,
                                             enum mtl_simd_level level);

/**
 * Convert rfc4175_422be10 to yuv422p10le with required SIMD level and DMA helper.
 * Note the level may downgrade to the SIMD which system really support.
 * Profiling shows gain with 4k/8k solution due to LLC cache miss migration, thus pls
 * only applied with 4k/8k.
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
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_422be10_to_yuv422p10le_simd_dma(mtl_udma_handle udma,
                                                 struct st20_rfc4175_422_10_pg2_be* pg_be,
                                                 mtl_iova_t pg_be_iova, uint16_t* y,
                                                 uint16_t* b, uint16_t* r, uint32_t w,
                                                 uint32_t h, enum mtl_simd_level level);

/**
 * Convert rfc4175_422be10 to rfc4175_422le10 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param pg_be
 *   Point to pg(rfc4175_422be10) data.
 * @param pg_le
 *   Point to pg(rfc4175_422le10) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_422be10_to_422le10_simd(struct st20_rfc4175_422_10_pg2_be* pg_be,
                                         struct st20_rfc4175_422_10_pg2_le* pg_le,
                                         uint32_t w, uint32_t h,
                                         enum mtl_simd_level level);

/**
 * Convert rfc4175_422be10 to rfc4175_422le10 with required SIMD level and DMA helper.
 * Note the level may downgrade to the SIMD which system really support.
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
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_422be10_to_422le10_simd_dma(mtl_udma_handle udma,
                                             struct st20_rfc4175_422_10_pg2_be* pg_be,
                                             mtl_iova_t pg_be_iova,
                                             struct st20_rfc4175_422_10_pg2_le* pg_le,
                                             uint32_t w, uint32_t h,
                                             enum mtl_simd_level level);

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
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_422be10_to_v210_simd(struct st20_rfc4175_422_10_pg2_be* pg_be,
                                      uint8_t* pg_v210, uint32_t w, uint32_t h,
                                      enum mtl_simd_level level);

/**
 * Convert rfc4175_422be10 to v210 with required SIMD level and DMA helper.
 * Note the level may downgrade to the SIMD which system really support.
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
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_422be10_to_v210_simd_dma(mtl_udma_handle udma,
                                          struct st20_rfc4175_422_10_pg2_be* pg_be,
                                          mtl_iova_t pg_be_iova, uint8_t* pg_v210,
                                          uint32_t w, uint32_t h,
                                          enum mtl_simd_level level);

/**
 * Convert rfc4175_422be10 to rfc4175_422le8 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param pg_10
 *   Point to pg(rfc4175_422be10) data.
 * @param pg_8
 *   Point to pg(rfc4175_422le8) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_422be10_to_422le8_simd(struct st20_rfc4175_422_10_pg2_be* pg_10,
                                        struct st20_rfc4175_422_8_pg2_le* pg_8,
                                        uint32_t w, uint32_t h,
                                        enum mtl_simd_level level);

/**
 * Convert rfc4175_422be10 to rfc4175_422le8 with required SIMD level and DMA helper.
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
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_422be10_to_422le8_simd_dma(mtl_udma_handle udma,
                                            struct st20_rfc4175_422_10_pg2_be* pg_10,
                                            mtl_iova_t pg_10_iova,
                                            struct st20_rfc4175_422_8_pg2_le* pg_8,
                                            uint32_t w, uint32_t h,
                                            enum mtl_simd_level level);

/**
 * Convert rfc4175_422be12 to yuv422p12le with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
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
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_422be12_to_yuv422p12le_simd(struct st20_rfc4175_422_12_pg2_be* pg,
                                             uint16_t* y, uint16_t* b, uint16_t* r,
                                             uint32_t w, uint32_t h,
                                             enum mtl_simd_level level);

/**
 * Convert rfc4175_422be12 to yuv422p12le with required SIMD level and DMA helper.
 * Note the level may downgrade to the SIMD which system really support.
 * Profiling shows gain with 4k/8k solution due to LLC cache miss migration, thus pls
 * only applied with 4k/8k.
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
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_422be12_to_yuv422p12le_simd_dma(mtl_udma_handle udma,
                                                 struct st20_rfc4175_422_12_pg2_be* pg_be,
                                                 mtl_iova_t pg_be_iova, uint16_t* y,
                                                 uint16_t* b, uint16_t* r, uint32_t w,
                                                 uint32_t h, enum mtl_simd_level level);

/**
 * Convert rfc4175_422be12 to rfc4175_422le12 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param pg_be
 *   Point to pg(rfc4175_422be12) data.
 * @param pg_le
 *   Point to pg(rfc4175_422le12) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_422be12_to_422le12_simd(struct st20_rfc4175_422_12_pg2_be* pg_be,
                                         struct st20_rfc4175_422_12_pg2_le* pg_le,
                                         uint32_t w, uint32_t h,
                                         enum mtl_simd_level level);

/**
 * Convert rfc4175_422be12 to rfc4175_422le12 with required SIMD level and DMA helper.
 * Note the level may downgrade to the SIMD which system really support.
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
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_422be12_to_422le12_simd_dma(mtl_udma_handle udma,
                                             struct st20_rfc4175_422_12_pg2_be* pg_be,
                                             mtl_iova_t pg_be_iova,
                                             struct st20_rfc4175_422_12_pg2_le* pg_le,
                                             uint32_t w, uint32_t h,
                                             enum mtl_simd_level level);

/**
 * Convert rfc4175_444be10 to yuv444p10le/gbrp10le with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param pg
 *   Point to pg(rfc4175_444be10) data.
 * @param y_g
 *   Point to Y(yuv444p10le) or g(gbrp10le) vector.
 * @param b_r
 *   Point to b(yuv444p10le) or r(gbrp10le) vector.
 * @param r_b
 *   Point to r(yuv444p10le) or b(gbrp10le) vector.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_444be10_to_444p10le_simd(struct st20_rfc4175_444_10_pg4_be* pg,
                                          uint16_t* y_g, uint16_t* b_r, uint16_t* r_b,
                                          uint32_t w, uint32_t h,
                                          enum mtl_simd_level level);

/**
 * Convert rfc4175_444be10 to rfc4175_444le10 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param pg_be
 *   Point to pg(rfc4175_444be10) data.
 * @param pg_le
 *   Point to pg(rfc4175_444le10) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_444be10_to_444le10_simd(struct st20_rfc4175_444_10_pg4_be* pg_be,
                                         struct st20_rfc4175_444_10_pg4_le* pg_le,
                                         uint32_t w, uint32_t h,
                                         enum mtl_simd_level level);

/**
 * Convert rfc4175_444be12 to yuv444p12le/gbrp12le with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param pg
 *   Point to pg(rfc4175_444be12) data.
 * @param y_g
 *   Point to Y(yuv444p12le) or g(gbrp12le) vector.
 * @param b_r
 *   Point to b(yuv444p12le) or r(gbrp12le) vector.
 * @param r_b
 *   Point to r(yuv444p12le) or b(gbrp12le) vector.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_444be12_to_444p12le_simd(struct st20_rfc4175_444_12_pg2_be* pg,
                                          uint16_t* y_g, uint16_t* b_r, uint16_t* r_b,
                                          uint32_t w, uint32_t h,
                                          enum mtl_simd_level level);

/**
 * Convert rfc4175_444be12 to rfc4175_444le12 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param pg_be
 *   Point to pg(rfc4175_444be12) data.
 * @param pg_le
 *   Point to pg(rfc4175_444le12) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_444be12_to_444le12_simd(struct st20_rfc4175_444_12_pg2_be* pg_be,
                                         struct st20_rfc4175_444_12_pg2_le* pg_le,
                                         uint32_t w, uint32_t h,
                                         enum mtl_simd_level level);

/**
 * Convert yuv422p10le to rfc4175_422be10 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
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
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_yuv422p10le_to_rfc4175_422be10_simd(uint16_t* y, uint16_t* b, uint16_t* r,
                                             struct st20_rfc4175_422_10_pg2_be* pg,
                                             uint32_t w, uint32_t h,
                                             enum mtl_simd_level level);

/**
 * Convert yuv422p10le to rfc4175_422be10 with required SIMD level and DMA helper.
 * Note the level may downgrade to the SIMD which system really support.
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
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_yuv422p10le_to_rfc4175_422be10_simd_dma(
    mtl_udma_handle udma, uint16_t* y, mtl_iova_t y_iova, uint16_t* b, mtl_iova_t b_iova,
    uint16_t* r, mtl_iova_t r_iova, struct st20_rfc4175_422_10_pg2_be* pg, uint32_t w,
    uint32_t h, enum mtl_simd_level level);

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
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_v210_to_rfc4175_422be10_simd(uint8_t* pg_v210,
                                      struct st20_rfc4175_422_10_pg2_be* pg_be,
                                      uint32_t w, uint32_t h, enum mtl_simd_level level);

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
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_v210_to_rfc4175_422be10_simd_dma(mtl_udma_handle udma, uint8_t* pg_v210,
                                          mtl_iova_t pg_v210_iova,
                                          struct st20_rfc4175_422_10_pg2_be* pg_be,
                                          uint32_t w, uint32_t h,
                                          enum mtl_simd_level level);

/**
 * Convert yuv422p12le to rfc4175_422be12 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
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
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_yuv422p12le_to_rfc4175_422be12_simd(uint16_t* y, uint16_t* b, uint16_t* r,
                                             struct st20_rfc4175_422_12_pg2_be* pg,
                                             uint32_t w, uint32_t h,
                                             enum mtl_simd_level level);

/**
 * Convert yuv444p10le or gbrp10le to rfc4175_444be10 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param y_g
 *   Point to Y(yuv444p10le) or g(gbrp10le) vector.
 * @param b_r
 *   Point to b(yuv444p10le) or r(gbrp10le) vector.
 * @param r_b
 *   Point to r(yuv444p10le) or b(gbrp10le) vector.
 * @param pg
 *   Point to pg(rfc4175_444be10) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_444p10le_to_rfc4175_444be10_simd(uint16_t* y_g, uint16_t* b_r, uint16_t* r_b,
                                          struct st20_rfc4175_444_10_pg4_be* pg,
                                          uint32_t w, uint32_t h,
                                          enum mtl_simd_level level);

/**
 * Convert yuv444p12le or gbrp12le to rfc4175_444be12 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param y_g
 *   Point to Y(yuv444p12le) or g(gbrp12le) vector.
 * @param b_r
 *   Point to b(yuv444p12le) or r(gbrp12le) vector.
 * @param r_b
 *   Point to r(yuv444p12le) or b(gbrp12le) vector.
 * @param pg
 *   Point to pg(rfc4175_444be10) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_444p12le_to_rfc4175_444be12_simd(uint16_t* y_g, uint16_t* b_r, uint16_t* r_b,
                                          struct st20_rfc4175_444_12_pg2_be* pg,
                                          uint32_t w, uint32_t h,
                                          enum mtl_simd_level level);

/**
 * Convert rfc4175_422le10 to rfc4175_422be10 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param pg_le
 *   Point to pg(rfc4175_422le10) data.
 * @param pg_be
 *   Point to pg(rfc4175_422be10) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_422le10_to_422be10_simd(struct st20_rfc4175_422_10_pg2_le* pg_le,
                                         struct st20_rfc4175_422_10_pg2_be* pg_be,
                                         uint32_t w, uint32_t h,
                                         enum mtl_simd_level level);

/**
 * Convert rfc4175_422le10 to rfc4175_422be10 with required SIMD level and DMA helper.
 * Note the level may downgrade to the SIMD which system really support.
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
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_422le10_to_422be10_simd_dma(mtl_udma_handle udma,
                                             struct st20_rfc4175_422_10_pg2_le* pg_le,
                                             mtl_iova_t pg_le_iova,
                                             struct st20_rfc4175_422_10_pg2_be* pg_be,
                                             uint32_t w, uint32_t h,
                                             enum mtl_simd_level level);

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
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_422le10_to_v210_simd(uint8_t* pg_le, uint8_t* pg_v210, uint32_t w,
                                      uint32_t h, enum mtl_simd_level level);

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
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_422be10_to_y210_simd(struct st20_rfc4175_422_10_pg2_be* pg_be,
                                      uint16_t* pg_y210, uint32_t w, uint32_t h,
                                      enum mtl_simd_level level);

/**
 * Convert rfc4175_422be10 to y210 with required SIMD level and DMA helper.
 * Note the level may downgrade to the SIMD which system really support.
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
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_422be10_to_y210_simd_dma(mtl_udma_handle udma,
                                          struct st20_rfc4175_422_10_pg2_be* pg_be,
                                          mtl_iova_t pg_be_iova, uint16_t* pg_y210,
                                          uint32_t w, uint32_t h,
                                          enum mtl_simd_level level);

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
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_y210_to_rfc4175_422be10_simd(uint16_t* pg_y210,
                                      struct st20_rfc4175_422_10_pg2_be* pg_be,
                                      uint32_t w, uint32_t h, enum mtl_simd_level level);

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
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_y210_to_rfc4175_422be10_simd_dma(mtl_udma_handle udma, uint16_t* pg_y210,
                                          mtl_iova_t pg_y210_iova,
                                          struct st20_rfc4175_422_10_pg2_be* pg_be,
                                          uint32_t w, uint32_t h,
                                          enum mtl_simd_level level);

/**
 * Convert rfc4175_422le12 to rfc4175_422be12 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param pg_le
 *   Point to pg(rfc4175_422le12) data.
 * @param pg_be
 *   Point to pg(rfc4175_422be12) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_422le12_to_422be12_simd(struct st20_rfc4175_422_12_pg2_le* pg_le,
                                         struct st20_rfc4175_422_12_pg2_be* pg_be,
                                         uint32_t w, uint32_t h,
                                         enum mtl_simd_level level);

/**
 * Convert rfc4175_444le10 to rfc4175_444be10 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param pg_le
 *   Point to pg(rfc4175_444le10) data.
 * @param pg_be
 *   Point to pg(rfc4175_444be10) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_444le10_to_444be10_simd(struct st20_rfc4175_444_10_pg4_le* pg_le,
                                         struct st20_rfc4175_444_10_pg4_be* pg_be,
                                         uint32_t w, uint32_t h,
                                         enum mtl_simd_level level);
/**
 * Convert yuv444p10le or gbrp10le to rfc4175_444le10.
 *
 * @param y_g
 *   Point to Y(yuv444p10le) or g(gbrp10le) vector.
 * @param b_r
 *   Point to b(yuv444p10le) or r(gbrp10le) vector.
 * @param r_b
 *   Point to r(yuv444p10le) or b(gbrp10le) vector.
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
int st20_444p10le_to_rfc4175_444le10(uint16_t* y_g, uint16_t* b_r, uint16_t* r_b,
                                     struct st20_rfc4175_444_10_pg4_le* pg, uint32_t w,
                                     uint32_t h);

/**
 * Convert rfc4175_444le10 to yuv444p10le or gbrp10le.
 *
 * @param pg
 *   Point to pg(rfc4175_444le10) data.
 * @param y_g
 *   Point to Y(yuv444p10le) or g(gbrp10le) vector.
 * @param b_r
 *   Point to b(yuv444p10le) or r(gbrp10le) vector.
 * @param r_b
 *   Point to r(yuv444p10le) or b(gbrp10le) vector.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_444le10_to_444p10le(struct st20_rfc4175_444_10_pg4_le* pg, uint16_t* y_g,
                                     uint16_t* b_r, uint16_t* r_b, uint32_t w,
                                     uint32_t h);

/**
 * Convert rfc4175_444le12 to rfc4175_444be12 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param pg_le
 *   Point to pg(rfc4175_444le12) data.
 * @param pg_be
 *   Point to pg(rfc4175_444be12) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_444le12_to_444be12_simd(struct st20_rfc4175_444_12_pg2_le* pg_le,
                                         struct st20_rfc4175_444_12_pg2_be* pg_be,
                                         uint32_t w, uint32_t h,
                                         enum mtl_simd_level level);
/**
 * Convert yuv444p12le or gbrp12le to rfc4175_444le12.
 *
 * @param y_g
 *   Point to Y(yuv444p12le) or g(gbrp12le) vector.
 * @param b_r
 *   Point to b(yuv444p12le) or r(gbrp12le) vector.
 * @param r_b
 *   Point to r(yuv444p12le) or b(gbrp12le) vector.
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
int st20_444p12le_to_rfc4175_444le12(uint16_t* y_g, uint16_t* b_r, uint16_t* r_b,
                                     struct st20_rfc4175_444_12_pg2_le* pg, uint32_t w,
                                     uint32_t h);

/**
 * Convert rfc4175_444le12 to yuv444p12le or gbrp12le.
 *
 * @param pg
 *   Point to pg(rfc4175_444le12) data.
 * @param y_g
 *   Point to Y(yuv444p12le) or g(gbrp12le) vector.
 * @param b_r
 *   Point to b(yuv444p12le) or r(gbrp12le) vector.
 * @param r_b
 *   Point to r(yuv444p12le) or b(gbrp12le) vector.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_444le12_to_444p12le(struct st20_rfc4175_444_12_pg2_le* pg, uint16_t* y_g,
                                     uint16_t* b_r, uint16_t* r_b, uint32_t w,
                                     uint32_t h);

#if defined(__cplusplus)
}
#endif

#endif
