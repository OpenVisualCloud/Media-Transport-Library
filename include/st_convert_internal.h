/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

/**
 * @file st_convert_internal.h
 *
 * Interfaces to Intel(R) Media Streaming Library Format Conversion Toolkit
 *
 * This header define the public interfaces of Intel(R) Media Streaming Library Format
 * Conversion Toolkit
 *
 */

#include <st_dpdk_api.h>

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
                                             enum st_simd_level level);

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
 *   The st_iova_t address of the pg_be buffer.
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
int st20_rfc4175_422be10_to_yuv422p10le_simd_dma(st_udma_handle udma,
                                                 struct st20_rfc4175_422_10_pg2_be* pg_be,
                                                 st_iova_t pg_be_iova, uint16_t* y,
                                                 uint16_t* b, uint16_t* r, uint32_t w,
                                                 uint32_t h, enum st_simd_level level);

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
                                         enum st_simd_level level);

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
 *   The st_iova_t address of the pg_be buffer.
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
int st20_rfc4175_422be10_to_422le10_simd_dma(st_udma_handle udma,
                                             struct st20_rfc4175_422_10_pg2_be* pg_be,
                                             st_iova_t pg_be_iova,
                                             struct st20_rfc4175_422_10_pg2_le* pg_le,
                                             uint32_t w, uint32_t h,
                                             enum st_simd_level level);

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
                                      enum st_simd_level level);

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
 *   The st_iova_t address of the pg_be buffer.
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
int st20_rfc4175_422be10_to_v210_simd_dma(st_udma_handle udma,
                                          struct st20_rfc4175_422_10_pg2_be* pg_be,
                                          st_iova_t pg_be_iova, uint8_t* pg_v210,
                                          uint32_t w, uint32_t h,
                                          enum st_simd_level level);

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
                                        uint32_t w, uint32_t h, enum st_simd_level level);

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
 *   The st_iova_t address of the pg_10 buffer.
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
int st20_rfc4175_422be10_to_422le8_simd_dma(st_udma_handle udma,
                                            struct st20_rfc4175_422_10_pg2_be* pg_10,
                                            st_iova_t pg_10_iova,
                                            struct st20_rfc4175_422_8_pg2_le* pg_8,
                                            uint32_t w, uint32_t h,
                                            enum st_simd_level level);

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
                                      uint32_t h, enum st_simd_level level);

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
                                             enum st_simd_level level);

/**
 * Convert yuv422p10le to rfc4175_422be10 with required SIMD level and DMA helper.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param udma
 *   Point to dma engine.
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
int st20_yuv422p10le_to_rfc4175_422be10_simd_dma(
    st_udma_handle udma, uint16_t* y, st_iova_t y_iova, uint16_t* b, st_iova_t b_iova,
    uint16_t* r, st_iova_t r_iova, struct st20_rfc4175_422_10_pg2_be* pg, uint32_t w,
    uint32_t h, enum st_simd_level level);

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
                                      uint32_t w, uint32_t h, enum st_simd_level level);

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
                                         enum st_simd_level level);

#if defined(__cplusplus)
}
#endif

#endif