/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

/**
 * @file st_convert_api.h
 *
 * Interfaces to Intel(R) Media Streaming Library Format Conversion Toolkit
 *
 * This header define the public interfaces of Intel(R) Media Streaming Library Format
 * Conversion Toolkit
 *
 */

#include <st_dpdk_api.h>

#ifndef _ST_CONVERT_API_HEAD_H_
#define _ST_CONVERT_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * Convert yuv422p10le to rfc4175_422be10.
 *
 * @param y
 *   Piont to Y(yuv422p10le) vector.
 * @param b
 *   Piont to b(yuv422p10le) vector.
 * @param r
 *   Piont to r(yuv422p10le) vector.
 * @param pg
 *   Piont to pg(rfc4175_422be10) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_yuv422p10le_to_rfc4175_422be10(uint16_t* y, uint16_t* b, uint16_t* r,
                                        struct st20_rfc4175_422_10_pg2_be* pg, uint32_t w,
                                        uint32_t h);

/**
 * Convert rfc4175_422be10 to yuv422p10le with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param pg
 *   Piont to pg(rfc4175_422be10) data.
 * @param y
 *   Piont to Y(yuv422p10le) vector.
 * @param b
 *   Piont to b(yuv422p10le) vector.
 * @param r
 *   Piont to r(yuv422p10le) vector.
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
 * Convert rfc4175_422be10 to yuv422p10le with the max optimised SIMD level.
 *
 * @param pg
 *   Piont to pg(rfc4175_422be10) data.
 * @param y
 *   Piont to Y(yuv422p10le) vector.
 * @param b
 *   Piont to b(yuv422p10le) vector.
 * @param r
 *   Piont to r(yuv422p10le) vector.
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
  return st20_rfc4175_422be10_to_yuv422p10le_simd(pg, y, b, r, w, h, ST_SIMD_LEVEL_MAX);
}

/**
 * Convert yuv422p10le to rfc4175_422le10.
 *
 * @param y
 *   Piont to Y(yuv422p10le) vector.
 * @param b
 *   Piont to b(yuv422p10le) vector.
 * @param r
 *   Piont to r(yuv422p10le) vector.
 * @param pg
 *   Piont to pg(rfc4175_422le10) data.
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
 *   Piont to pg(rfc4175_422le10) data.
 * @param y
 *   Piont to Y(yuv422p10le) vector.
 * @param b
 *   Piont to b(yuv422p10le) vector.
 * @param r
 *   Piont to r(yuv422p10le) vector.
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
 * Convert rfc4175_422be10 to rfc4175_422le10 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param pg_be
 *   Piont to pg(rfc4175_422be10) data.
 * @param pg_le
 *   Piont to pg(rfc4175_422le10) data.
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
 * Convert rfc4175_422be10 to rfc4175_422le10 with the max optimised SIMD level.
 *
 * @param pg_be
 *   Piont to pg(rfc4175_422be10) data.
 * @param pg_le
 *   Piont to pg(rfc4175_422le10) data.
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
  return st20_rfc4175_422be10_to_422le10_simd(pg_be, pg_le, w, h, ST_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_422le10 to rfc4175_422be10.
 *
 * @param pg_le
 *   Piont to pg(rfc4175_422le10) data.
 * @param pg_be
 *   Piont to pg(rfc4175_422be10) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
int st20_rfc4175_422le10_to_422be10(struct st20_rfc4175_422_10_pg2_le* pg_le,
                                    struct st20_rfc4175_422_10_pg2_be* pg_be, uint32_t w,
                                    uint32_t h);

/**
 * Convert rfc4175_422le10 to v210 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param pg_le
 *   Piont to pg(rfc4175_422le10) data.
 * @param pg_v210
 *   Piont to pg(v210) data.
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
 * Convert rfc4175_422le10 to v210 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param pg_le
 *   Piont to pg(rfc4175_422le10) data.
 * @param pg_v210
 *   Piont to pg(v210) data.
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
  return st20_rfc4175_422le10_to_v210_simd(pg_le, pg_v210, w, h, ST_SIMD_LEVEL_MAX);
}

/**
 * Convert v210 to rfc4175_422le10.
 *
 * @param pg_v210
 *   Piont to pg(v210) data.
 * @param pg_le
 *   Piont to pg(rfc4175_422le10) data.
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
 * Convert rfc4175_422be10 to v210 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param pg_be
 *   Piont to pg(rfc4175_422be10) data.
 * @param pg_v210
 *   Piont to pg(v210) data.
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
int st20_rfc4175_422be10_to_v210_simd(uint8_t* pg_be, uint8_t* pg_v210, uint32_t w,
                                      uint32_t h, enum st_simd_level level);

/**
 * Convert rfc4175_422be10 to v210 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param pg_be
 *   Piont to pg(rfc4175_422be10) data.
 * @param pg_v210
 *   Piont to pg(v210) data.
 * @param w
 *   The st2110-20(video) width.
 * @param h
 *   The st2110-20(video) height.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if convert fail.
 */
static inline int st20_rfc4175_422be10_to_v210(uint8_t* pg_be, uint8_t* pg_v210,
                                               uint32_t w, uint32_t h) {
  return st20_rfc4175_422be10_to_v210_simd(pg_be, pg_v210, w, h, ST_SIMD_LEVEL_MAX);
}

/**
 * Convert rfc4175_422be10 to rfc4175_422le8 with required SIMD level.
 * Note the level may downgrade to the SIMD which system really support.
 *
 * @param pg_10
 *   Piont to pg(rfc4175_422be10) data.
 * @param pg_8
 *   Piont to pg(rfc4175_422le8) data.
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
 * Convert rfc4175_422be10 to rfc4175_422le8 with the max optimised SIMD level.
 *
 * @param pg_10
 *   Piont to pg(rfc4175_422be10) data.
 * @param pg_8
 *   Piont to pg(rfc4175_422le8) data.
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
  return st20_rfc4175_422be10_to_422le8_simd(pg_10, pg_8, w, h, ST_SIMD_LEVEL_MAX);
}

#if defined(__cplusplus)
}
#endif

#endif