/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#ifndef _ST_LIB_AVX512_VBMI_H_
#define _ST_LIB_AVX512_VBMI_H_

#include "st_main.h"

int st20_rfc4175_422be10_to_yuv422p10le_avx512_vbmi(struct st20_rfc4175_422_10_pg2_be* pg,
                                                    uint16_t* y, uint16_t* b, uint16_t* r,
                                                    uint32_t w, uint32_t h);

int st20_rfc4175_422be10_to_422le10_avx512_vbmi(struct st20_rfc4175_422_10_pg2_be* pg_be,
                                                struct st20_rfc4175_422_10_pg2_le* pg_le,
                                                uint32_t w, uint32_t h);

int st20_rfc4175_422be10_to_422le8_avx512_vbmi(struct st20_rfc4175_422_10_pg2_be* pg_10,
                                               struct st20_rfc4175_422_8_pg2_le* pg_8,
                                               uint32_t w, uint32_t h);

int st20_rfc4175_422be10_to_422le8_avx512_vbmi_dma(
    struct mtl_dma_lender_dev* dma, struct st20_rfc4175_422_10_pg2_be* pg_10,
    mtl_iova_t pg_10_iova, struct st20_rfc4175_422_8_pg2_le* pg_8, uint32_t w,
    uint32_t h);

int st20_rfc4175_422le10_to_v210_avx512_vbmi(uint8_t* pg_le, uint8_t* pg_v210, uint32_t w,
                                             uint32_t h);

int st20_rfc4175_422be10_to_v210_avx512_vbmi(struct st20_rfc4175_422_10_pg2_be* pg_be,
                                             uint8_t* pg_v210, uint32_t w, uint32_t h);

int st20_rfc4175_422be10_to_422le10_avx512_vbmi_dma(
    struct mtl_dma_lender_dev* dma, struct st20_rfc4175_422_10_pg2_be* pg_be,
    mtl_iova_t pg_be_iova, struct st20_rfc4175_422_10_pg2_le* pg_le, uint32_t w,
    uint32_t h);

int st20_rfc4175_422be10_to_yuv422p10le_avx512_vbmi_dma(
    struct mtl_dma_lender_dev* dma, struct st20_rfc4175_422_10_pg2_be* pg_be,
    mtl_iova_t pg_be_iova, uint16_t* y, uint16_t* b, uint16_t* r, uint32_t w, uint32_t h);

int st20_rfc4175_422be10_to_v210_avx512_vbmi_dma(struct mtl_dma_lender_dev* dma,
                                                 struct st20_rfc4175_422_10_pg2_be* pg_be,
                                                 mtl_iova_t pg_be_iova, uint8_t* pg_v210,
                                                 uint32_t w, uint32_t h);

int st20_yuv422p10le_to_rfc4175_422be10_vbmi(uint16_t* y, uint16_t* b, uint16_t* r,
                                             struct st20_rfc4175_422_10_pg2_be* pg,
                                             uint32_t w, uint32_t h);

int st20_rfc4175_422le10_to_422be10_vbmi(struct st20_rfc4175_422_10_pg2_le* pg_le,
                                         struct st20_rfc4175_422_10_pg2_be* pg_be,
                                         uint32_t w, uint32_t h);

int st20_rfc4175_422le10_to_422be10_avx512_vbmi_dma(
    struct mtl_dma_lender_dev* dma, struct st20_rfc4175_422_10_pg2_le* pg_le,
    mtl_iova_t pg_le_iova, struct st20_rfc4175_422_10_pg2_be* pg_be, uint32_t w,
    uint32_t h);

int st20_v210_to_rfc4175_422be10_avx512_vbmi(uint8_t* pg_v210,
                                             struct st20_rfc4175_422_10_pg2_be* pg_be,
                                             uint32_t w, uint32_t h);

int st20_v210_to_rfc4175_422be10_avx512_vbmi_dma(struct mtl_dma_lender_dev* dma,
                                                 uint8_t* pg_v210,
                                                 mtl_iova_t pg_v210_iova,
                                                 struct st20_rfc4175_422_10_pg2_be* pg_be,
                                                 uint32_t w, uint32_t h);

int st20_rfc4175_422be12_to_yuv422p12le_avx512_vbmi(struct st20_rfc4175_422_12_pg2_be* pg,
                                                    uint16_t* y, uint16_t* b, uint16_t* r,
                                                    uint32_t w, uint32_t h);

int st20_rfc4175_422be12_to_yuv422p12le_avx512_vbmi_dma(
    struct mtl_dma_lender_dev* dma, struct st20_rfc4175_422_12_pg2_be* pg_be,
    mtl_iova_t pg_be_iova, uint16_t* y, uint16_t* b, uint16_t* r, uint32_t w, uint32_t h);

int st20_downsample_rfc4175_422be10_wh_half_avx512_vbmi(uint8_t* pg_old, uint8_t* pg_new,
                                                        uint32_t w, uint32_t h,
                                                        uint32_t linesize_old,
                                                        uint32_t linesize_new);

#endif