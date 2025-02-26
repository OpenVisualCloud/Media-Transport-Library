/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

/**
 * @file st_convert_internal.h
 *
 * This header define the internal interfaces of streaming(st2110) format conversion
 * toolkit. Please note the APIs below is for internal test usage only.
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
int st20_rfc4175_422be10_to_yuv422p10le_simd(struct st20_rfc4175_422_10_pg2_be *pg,
                                             uint16_t *y, uint16_t *b, uint16_t *r,
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
                                                 struct st20_rfc4175_422_10_pg2_be *pg_be,
                                                 mtl_iova_t pg_be_iova, uint16_t *y,
                                                 uint16_t *b, uint16_t *r, uint32_t w,
                                                 uint32_t h, enum mtl_simd_level level);

/* the SIMD level version of st20_rfc4175_422be10_to_yuv422p10le_2way */
int st20_rfc4175_422be10_to_yuv422p10le_simd_2way(
    struct st20_rfc4175_422_10_pg2_be *pg_be, uint16_t *y_full, uint16_t *b_full,
    uint16_t *r_full, uint32_t w, uint32_t h, uint16_t *y_decimated,
    uint16_t *b_decimated, uint16_t *r_decimated, int decimator,
    enum mtl_simd_level level);

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
int st20_rfc4175_422be10_to_422le10_simd(struct st20_rfc4175_422_10_pg2_be *pg_be,
                                         struct st20_rfc4175_422_10_pg2_le *pg_le,
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
                                             struct st20_rfc4175_422_10_pg2_be *pg_be,
                                             mtl_iova_t pg_be_iova,
                                             struct st20_rfc4175_422_10_pg2_le *pg_le,
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
int st20_rfc4175_422be10_to_v210_simd(struct st20_rfc4175_422_10_pg2_be *pg_be,
                                      uint8_t *pg_v210, uint32_t w, uint32_t h,
                                      enum mtl_simd_level level);

/**
 * Convert rfc4175_422be10 to dual v210 streams(one full and one decimated) with required
 * SIMD level. Note the level may downgrade to the SIMD which system really support.
 *
 * @param pg_be
 *   Point to pg(rfc4175_422be10) data.
 * @param pg_v210_full
 *   Point to full pg(v210) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @param pg_v210_decimated
 *   Point to decimated pg(v210) data.
 * @param decimator
 *   The decimated ration, 2 or 4.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_422be10_to_v210_simd_2way(struct st20_rfc4175_422_10_pg2_be *pg_be,
                                           uint8_t *pg_v210_full, uint32_t w, uint32_t h,
                                           uint8_t *pg_v210_decimated, int decimator,
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
                                          struct st20_rfc4175_422_10_pg2_be *pg_be,
                                          mtl_iova_t pg_be_iova, uint8_t *pg_v210,
                                          uint32_t w, uint32_t h,
                                          enum mtl_simd_level level);

/**
 * Convert rfc4175_422be10 to rfc4175_422le8(packed UYVY) with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param pg_10
 *   Point to pg(rfc4175_422be10) data.
 * @param pg_8
 *   Point to pg(rfc4175_422le8) packed UYVY data data.
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
int st20_rfc4175_422be10_to_422le8_simd(struct st20_rfc4175_422_10_pg2_be *pg_10,
                                        struct st20_rfc4175_422_8_pg2_le *pg_8,
                                        uint32_t w, uint32_t h,
                                        enum mtl_simd_level level);

/**
 * Convert rfc4175_422be10 to rfc4175_422le8 packed UYVY with required SIMD level and DMA
 * helper. Profiling shows gain with 4k/8k solution due to LLC cache miss migration, thus
 * pls only applied with 4k/8k.
 *
 * @param udma
 *   Point to dma engine.
 * @param pg_10
 *   Point to pg(rfc4175_422be10) data.
 * @param pg_10_iova
 *   The mtl_iova_t address of the pg_10 buffer.
 * @param pg_8
 *   Point to pg(rfc4175_422le8) packed UYVY data.
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
                                            struct st20_rfc4175_422_10_pg2_be *pg_10,
                                            mtl_iova_t pg_10_iova,
                                            struct st20_rfc4175_422_8_pg2_le *pg_8,
                                            uint32_t w, uint32_t h,
                                            enum mtl_simd_level level);

/**
 * Convert rfc4175_422be10 to yuv422p8 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
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
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_422be10_to_yuv422p8_simd(struct st20_rfc4175_422_10_pg2_be *pg,
                                          uint8_t *y, uint8_t *b, uint8_t *r, uint32_t w,
                                          uint32_t h, enum mtl_simd_level level);

/**
 * Convert rfc4175_422be10 to yuv420p8 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
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
 * @param level
 *   simd level.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_422be10_to_yuv420p8_simd(struct st20_rfc4175_422_10_pg2_be *pg,
                                          uint8_t *y, uint8_t *b, uint8_t *r, uint32_t w,
                                          uint32_t h, enum mtl_simd_level level);

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
int st20_rfc4175_422be12_to_yuv422p12le_simd(struct st20_rfc4175_422_12_pg2_be *pg,
                                             uint16_t *y, uint16_t *b, uint16_t *r,
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
                                                 struct st20_rfc4175_422_12_pg2_be *pg_be,
                                                 mtl_iova_t pg_be_iova, uint16_t *y,
                                                 uint16_t *b, uint16_t *r, uint32_t w,
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
int st20_rfc4175_422be12_to_422le12_simd(struct st20_rfc4175_422_12_pg2_be *pg_be,
                                         struct st20_rfc4175_422_12_pg2_le *pg_le,
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
                                             struct st20_rfc4175_422_12_pg2_be *pg_be,
                                             mtl_iova_t pg_be_iova,
                                             struct st20_rfc4175_422_12_pg2_le *pg_le,
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
int st20_rfc4175_444be10_to_444p10le_simd(struct st20_rfc4175_444_10_pg4_be *pg,
                                          uint16_t *y_g, uint16_t *b_r, uint16_t *r_b,
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
int st20_rfc4175_444be10_to_444le10_simd(struct st20_rfc4175_444_10_pg4_be *pg_be,
                                         struct st20_rfc4175_444_10_pg4_le *pg_le,
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
int st20_rfc4175_444be12_to_444p12le_simd(struct st20_rfc4175_444_12_pg2_be *pg,
                                          uint16_t *y_g, uint16_t *b_r, uint16_t *r_b,
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
int st20_rfc4175_444be12_to_444le12_simd(struct st20_rfc4175_444_12_pg2_be *pg_be,
                                         struct st20_rfc4175_444_12_pg2_le *pg_le,
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
int st20_yuv422p10le_to_rfc4175_422be10_simd(uint16_t *y, uint16_t *b, uint16_t *r,
                                             struct st20_rfc4175_422_10_pg2_be *pg,
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
    mtl_udma_handle udma, uint16_t *y, mtl_iova_t y_iova, uint16_t *b, mtl_iova_t b_iova,
    uint16_t *r, mtl_iova_t r_iova, struct st20_rfc4175_422_10_pg2_be *pg, uint32_t w,
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
int st20_v210_to_rfc4175_422be10_simd(uint8_t *pg_v210,
                                      struct st20_rfc4175_422_10_pg2_be *pg_be,
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
int st20_v210_to_rfc4175_422be10_simd_dma(mtl_udma_handle udma, uint8_t *pg_v210,
                                          mtl_iova_t pg_v210_iova,
                                          struct st20_rfc4175_422_10_pg2_be *pg_be,
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
int st20_yuv422p12le_to_rfc4175_422be12_simd(uint16_t *y, uint16_t *b, uint16_t *r,
                                             struct st20_rfc4175_422_12_pg2_be *pg,
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
int st20_444p10le_to_rfc4175_444be10_simd(uint16_t *y_g, uint16_t *b_r, uint16_t *r_b,
                                          struct st20_rfc4175_444_10_pg4_be *pg,
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
int st20_444p12le_to_rfc4175_444be12_simd(uint16_t *y_g, uint16_t *b_r, uint16_t *r_b,
                                          struct st20_rfc4175_444_12_pg2_be *pg,
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
int st20_rfc4175_422le10_to_422be10_simd(struct st20_rfc4175_422_10_pg2_le *pg_le,
                                         struct st20_rfc4175_422_10_pg2_be *pg_be,
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
                                             struct st20_rfc4175_422_10_pg2_le *pg_le,
                                             mtl_iova_t pg_le_iova,
                                             struct st20_rfc4175_422_10_pg2_be *pg_be,
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
int st20_rfc4175_422le10_to_v210_simd(uint8_t *pg_le, uint8_t *pg_v210, uint32_t w,
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
int st20_rfc4175_422be10_to_y210_simd(struct st20_rfc4175_422_10_pg2_be *pg_be,
                                      uint16_t *pg_y210, uint32_t w, uint32_t h,
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
                                          struct st20_rfc4175_422_10_pg2_be *pg_be,
                                          mtl_iova_t pg_be_iova, uint16_t *pg_y210,
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
int st20_y210_to_rfc4175_422be10_simd(uint16_t *pg_y210,
                                      struct st20_rfc4175_422_10_pg2_be *pg_be,
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
int st20_y210_to_rfc4175_422be10_simd_dma(mtl_udma_handle udma, uint16_t *pg_y210,
                                          mtl_iova_t pg_y210_iova,
                                          struct st20_rfc4175_422_10_pg2_be *pg_be,
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
int st20_rfc4175_422le12_to_422be12_simd(struct st20_rfc4175_422_12_pg2_le *pg_le,
                                         struct st20_rfc4175_422_12_pg2_be *pg_be,
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
int st20_rfc4175_444le10_to_444be10_simd(struct st20_rfc4175_444_10_pg4_le *pg_le,
                                         struct st20_rfc4175_444_10_pg4_be *pg_be,
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
int st20_444p10le_to_rfc4175_444le10(uint16_t *y_g, uint16_t *b_r, uint16_t *r_b,
                                     struct st20_rfc4175_444_10_pg4_le *pg, uint32_t w,
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
int st20_rfc4175_444le10_to_444p10le(struct st20_rfc4175_444_10_pg4_le *pg, uint16_t *y_g,
                                     uint16_t *b_r, uint16_t *r_b, uint32_t w,
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
int st20_rfc4175_444le12_to_444be12_simd(struct st20_rfc4175_444_12_pg2_le *pg_le,
                                         struct st20_rfc4175_444_12_pg2_be *pg_be,
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
int st20_444p12le_to_rfc4175_444le12(uint16_t *y_g, uint16_t *b_r, uint16_t *r_b,
                                     struct st20_rfc4175_444_12_pg2_le *pg, uint32_t w,
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
int st20_rfc4175_444le12_to_444p12le(struct st20_rfc4175_444_12_pg2_le *pg, uint16_t *y_g,
                                     uint16_t *b_r, uint16_t *r_b, uint32_t w,
                                     uint32_t h);

/**
 * Convert rfc4175_422be10 to yuv422p10le with the max optimized SIMD level and DMA
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
    mtl_udma_handle udma, struct st20_rfc4175_422_10_pg2_be *pg_be, mtl_iova_t pg_be_iova,
    uint16_t *y, uint16_t *b, uint16_t *r, uint32_t w, uint32_t h) {
  return st20_rfc4175_422be10_to_yuv422p10le_simd_dma(udma, pg_be, pg_be_iova, y, b, r, w,
                                                      h, MTL_SIMD_LEVEL_MAX);
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
    mtl_udma_handle udma, struct st20_rfc4175_422_10_pg2_be *pg_be, mtl_iova_t pg_be_iova,
    struct st20_rfc4175_422_10_pg2_le *pg_le, uint32_t w, uint32_t h) {
  return st20_rfc4175_422be10_to_422le10_simd_dma(udma, pg_be, pg_be_iova, pg_le, w, h,
                                                  MTL_SIMD_LEVEL_MAX);
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
    mtl_udma_handle udma, struct st20_rfc4175_422_10_pg2_be *pg_be, mtl_iova_t pg_be_iova,
    uint8_t *pg_v210, uint32_t w, uint32_t h) {
  return st20_rfc4175_422be10_to_v210_simd_dma(udma, pg_be, pg_be_iova, pg_v210, w, h,
                                               MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_422be10 to rfc4175_422le8(packed UYVY) with max SIMD level and DMA
 * helper. Profiling shows gain with 4k/8k solution due to LLC cache miss migration, thus
 * pls only applied with 4k/8k.
 *
 * @param udma
 *   Point to dma engine.
 * @param pg_10
 *   Point to pg(rfc4175_422be10) data.
 * @param pg_10_iova
 *   The mtl_iova_t address of the pg_10 buffer.
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
static inline int st20_rfc4175_422be10_to_422le8_dma(
    mtl_udma_handle udma, struct st20_rfc4175_422_10_pg2_be *pg_10, mtl_iova_t pg_10_iova,
    struct st20_rfc4175_422_8_pg2_le *pg_8, uint32_t w, uint32_t h) {
  return st20_rfc4175_422be10_to_422le8_simd_dma(udma, pg_10, pg_10_iova, pg_8, w, h,
                                                 MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_422be12 to yuv422p12le with the max optimized SIMD level and DMA
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
    mtl_udma_handle udma, struct st20_rfc4175_422_12_pg2_be *pg_be, mtl_iova_t pg_be_iova,
    uint16_t *y, uint16_t *b, uint16_t *r, uint32_t w, uint32_t h) {
  return st20_rfc4175_422be12_to_yuv422p12le_simd_dma(udma, pg_be, pg_be_iova, y, b, r, w,
                                                      h, MTL_SIMD_LEVEL_MAX);
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
    mtl_udma_handle udma, struct st20_rfc4175_422_12_pg2_be *pg_be, mtl_iova_t pg_be_iova,
    struct st20_rfc4175_422_12_pg2_le *pg_le, uint32_t w, uint32_t h) {
  return st20_rfc4175_422be12_to_422le12_simd_dma(udma, pg_be, pg_be_iova, pg_le, w, h,
                                                  MTL_SIMD_LEVEL_MAX);
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
    mtl_udma_handle udma, uint16_t *y, mtl_iova_t y_iova, uint16_t *b, mtl_iova_t b_iova,
    uint16_t *r, mtl_iova_t r_iova, struct st20_rfc4175_422_10_pg2_be *pg, uint32_t w,
    uint32_t h) {
  return st20_yuv422p10le_to_rfc4175_422be10_simd_dma(
      udma, y, y_iova, b, b_iova, r, r_iova, pg, w, h, MTL_SIMD_LEVEL_MAX);
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
    mtl_udma_handle udma, uint8_t *pg_v210, mtl_iova_t pg_v210_iova,
    struct st20_rfc4175_422_10_pg2_be *pg_be, uint32_t w, uint32_t h) {
  return st20_v210_to_rfc4175_422be10_simd_dma(udma, pg_v210, pg_v210_iova, pg_be, w, h,
                                               MTL_SIMD_LEVEL_MAX);
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
    mtl_udma_handle udma, struct st20_rfc4175_422_10_pg2_le *pg_le, mtl_iova_t pg_le_iova,
    struct st20_rfc4175_422_10_pg2_be *pg_be, uint32_t w, uint32_t h) {
  return st20_rfc4175_422le10_to_422be10_simd_dma(udma, pg_le, pg_le_iova, pg_be, w, h,
                                                  MTL_SIMD_LEVEL_MAX);
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
    mtl_udma_handle udma, struct st20_rfc4175_422_10_pg2_be *pg_be, mtl_iova_t pg_be_iova,
    uint16_t *pg_y210, uint32_t w, uint32_t h) {
  return st20_rfc4175_422be10_to_y210_simd_dma(udma, pg_be, pg_be_iova, pg_y210, w, h,
                                               MTL_SIMD_LEVEL_MAX);
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
    mtl_udma_handle udma, uint16_t *pg_y210, mtl_iova_t pg_y210_iova,
    struct st20_rfc4175_422_10_pg2_be *pg_be, uint32_t w, uint32_t h) {
  return st20_y210_to_rfc4175_422be10_simd_dma(udma, pg_y210, pg_y210_iova, pg_be, w, h,
                                               MTL_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_422be10 to dual v210 streams(one full and one decimated) with required
 * SIMD level. Note the level may downgrade to the SIMD which system really support.
 *
 * Todo: add SIMD implementation
 *
 * @param pg_be
 *   Point to pg(rfc4175_422be10) data.
 * @param pg_v210_full
 *   Point to full pg(v210) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @param pg_v210_decimated
 *   Point to decimated pg(v210) data.
 * @param decimator
 *   The decimated ration, 2 or 4.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_422be10_to_v210_2way(
    struct st20_rfc4175_422_10_pg2_be *pg_be, uint8_t *pg_v210_full, uint32_t w,
    uint32_t h, uint8_t *pg_v210_decimated, int decimator) {
  return st20_rfc4175_422be10_to_v210_simd_2way(
      pg_be, pg_v210_full, w, h, pg_v210_decimated, decimator, MTL_SIMD_LEVEL_MAX);
}

/** helper to call st20_rfc4175_422be10_to_v210_2way with mtl_cpuva_t type for python
 * binding */
static inline int st20_rfc4175_422be10_to_v210_2way_cpuva(mtl_cpuva_t pg_be,
                                                          mtl_cpuva_t pg_v210_full,
                                                          uint32_t w, uint32_t h,
                                                          mtl_cpuva_t pg_v210_decimated,
                                                          int decimator) {
  return st20_rfc4175_422be10_to_v210_2way((struct st20_rfc4175_422_10_pg2_be *)pg_be,
                                           (uint8_t *)pg_v210_full, w, h,
                                           (uint8_t *)pg_v210_decimated, decimator);
}

/**
 * Convert rfc4175_422be10 to dual yuv422p10le streams(one full and one decimated) with
 * required SIMD level. Note the level may downgrade to the SIMD which system really
 * support.
 *
 * Todo: add SIMD implementation
 *
 * @param pg_be
 *   Point to pg(rfc4175_422be10) data.
 * @param y_full
 *   Point to full Y(yuv422p10le) vector.
 * @param b_full
 *   Point to full b(yuv422p10le) vector.
 * @param r_full
 *   Point to full r(yuv422p10le) vector.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @param y_decimated
 *   Point to decimated Y(yuv422p10le) vector.
 * @param b_decimated
 *   Point to decimated b(yuv422p10le) vector.
 * @param r_decimated
 *   Point to decimated r(yuv422p10le) vector.
 * @param decimator
 *   The decimated ration, 2 or 4.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_422be10_to_yuv422p10le_2way(
    struct st20_rfc4175_422_10_pg2_be *pg_be, uint16_t *y_full, uint16_t *b_full,
    uint16_t *r_full, uint32_t w, uint32_t h, uint16_t *y_decimated,
    uint16_t *b_decimated, uint16_t *r_decimated, int decimator) {
  return st20_rfc4175_422be10_to_yuv422p10le_simd_2way(
      pg_be, y_full, b_full, r_full, w, h, y_decimated, b_decimated, r_decimated,
      decimator, MTL_SIMD_LEVEL_MAX);
}

/** helper to call st20_rfc4175_422be10_to_yuv422p10le_2way with mtl_cpuva_t type for
 * python binding */
static inline int st20_rfc4175_422be10_to_yuv422p10le_2way_cpuva(
    mtl_cpuva_t pg_be, mtl_cpuva_t y_full, mtl_cpuva_t b_full, mtl_cpuva_t r_full,
    uint32_t w, uint32_t h, mtl_cpuva_t y_decimated, mtl_cpuva_t b_decimated,
    mtl_cpuva_t r_decimated, int decimator) {
  return st20_rfc4175_422be10_to_yuv422p10le_2way(
      (struct st20_rfc4175_422_10_pg2_be *)pg_be, (uint16_t *)y_full, (uint16_t *)b_full,
      (uint16_t *)r_full, w, h, (uint16_t *)y_decimated, (uint16_t *)b_decimated,
      (uint16_t *)r_decimated, decimator);
}

#if defined(__cplusplus)
}
#endif

#endif
