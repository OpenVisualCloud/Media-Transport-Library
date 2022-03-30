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

#include "st_avx512_vbmi.h"

#include "st_log.h"
#include "st_simd.h"

#ifdef ST_HAS_AVX512_VBMI2
static uint8_t b2l_permute_mask_table_512[16 * 4] = {
    /* b0 - b7 */
    1,
    0,
    6,
    5,
    1 + 10,
    0 + 10,
    6 + 10,
    5 + 10,
    1 + 20,
    0 + 20,
    6 + 20,
    5 + 20,
    1 + 30,
    0 + 30,
    6 + 30,
    5 + 30,
    /* r0 - r7 */
    3,
    2,
    8,
    7,
    3 + 10,
    2 + 10,
    8 + 10,
    7 + 10,
    3 + 20,
    2 + 20,
    8 + 20,
    7 + 20,
    3 + 30,
    2 + 30,
    8 + 30,
    7 + 30,
    /* y0 - y7 */
    2,
    1,
    4,
    3,
    7,
    6,
    9,
    8,
    2 + 10,
    1 + 10,
    4 + 10,
    3 + 10,
    7 + 10,
    6 + 10,
    9 + 10,
    8 + 10,
    /* y8 - y15 */
    2 + 20,
    1 + 20,
    4 + 20,
    3 + 20,
    7 + 20,
    6 + 20,
    9 + 20,
    8 + 20,
    2 + 30,
    1 + 30,
    4 + 30,
    3 + 30,
    7 + 30,
    6 + 30,
    9 + 30,
    8 + 30,
};

static uint16_t b2l_srlv_mask_table_512[8 * 4] = {
    /* b0 - b7 */
    0x0006,
    0x0006,
    0x0006,
    0x0006,
    0x0006,
    0x0006,
    0x0006,
    0x0006,
    /* r0 - r7 */
    0x0002,
    0x0002,
    0x0002,
    0x0002,
    0x0002,
    0x0002,
    0x0002,
    0x0002,
    /* y0 - y7 */
    0x0004,
    0x0000,
    0x0004,
    0x0000,
    0x0004,
    0x0000,
    0x0004,
    0x0000,
    /* y8 - y15 */
    0x0004,
    0x0000,
    0x0004,
    0x0000,
    0x0004,
    0x0000,
    0x0004,
    0x0000,
};

static uint16_t b2l_and_mask_table_512[8 * 4] = {
    /* b0 - b7 */
    0x03ff,
    0x03ff,
    0x03ff,
    0x03ff,
    0x03ff,
    0x03ff,
    0x03ff,
    0x03ff,
    /* r0 - r7 */
    0x03ff,
    0x03ff,
    0x03ff,
    0x03ff,
    0x03ff,
    0x03ff,
    0x03ff,
    0x03ff,
    /* y0 - y7 */
    0x03ff,
    0x03ff,
    0x03ff,
    0x03ff,
    0x03ff,
    0x03ff,
    0x03ff,
    0x03ff,
    /* y8 - y15 */
    0x03ff,
    0x03ff,
    0x03ff,
    0x03ff,
    0x03ff,
    0x03ff,
    0x03ff,
    0x03ff,
};

/* for st20_rfc4175_422be10_to_422le10_avx512_vbmi */
static uint8_t permute_l0_mask_table[64] = {
    1,       0,       3,       2,       /* 4 bytes from pg0 */
    6,       5,       8,       7,       /* 4 bytes from pg1 */
    11,      10,      13,      12,      /* 4 bytes from pg2 */
    0,       5,       10,      63,      /* 5th bytes from pg0,pg1,pg2, and a padding */
    1 + 15,  0 + 15,  3 + 15,  2 + 15,  /* 4 bytes from pg3 */
    6 + 15,  5 + 15,  8 + 15,  7 + 15,  /* 4 bytes from pg4 */
    11 + 15, 10 + 15, 13 + 15, 12 + 15, /* 4 bytes from pg5 */
    0 + 15,  5 + 15,  10 + 15, 63,      /* 5th bytes from pg3,pg4,pg5, and a padding */
    1 + 30,  0 + 30,  3 + 30,  2 + 30,  /* 4 bytes from pg6 */
    6 + 30,  5 + 30,  8 + 30,  7 + 30,  /* 4 bytes from pg7 */
    11 + 30, 10 + 30, 13 + 30, 12 + 30, /* 4 bytes from pg8 */
    0 + 30,  5 + 30,  10 + 30, 63,      /* 5th bytes from pg6,pg7,pg8, and a padding */
    1 + 45,  0 + 45,  3 + 45,  2 + 45,  /* 4 bytes from pg9 */
    6 + 45,  5 + 45,  8 + 45,  7 + 45,  /* 4 bytes from pg10 */
    11 + 45, 10 + 45, 13 + 45, 12 + 45, /* 4 bytes from pg11 */
    0 + 45,  5 + 45,  10 + 45, 63,      /* 5th bytes from pg9,pg10,pg11, and a padding */
};

static uint8_t and_l0_mask_table[64] = {
    0x00, 0xFF, 0xF0, 0x3F, 0x00, 0xFF, 0xF0, 0x3F, 0x00, 0xFF, 0xF0, 0x3F, 0x00,
    0x03, 0x03, 0x03, 0x00, 0xFF, 0xF0, 0x3F, 0x00, 0xFF, 0xF0, 0x3F, 0x00, 0xFF,
    0xF0, 0x3F, 0x00, 0x03, 0x03, 0x03, 0x00, 0xFF, 0xF0, 0x3F, 0x00, 0xFF, 0xF0,
    0x3F, 0x00, 0xFF, 0xF0, 0x3F, 0x00, 0x03, 0x03, 0x03, 0x00, 0xFF, 0xF0, 0x3F,
    0x00, 0xFF, 0xF0, 0x3F, 0x00, 0xFF, 0xF0, 0x3F, 0x00, 0x03, 0x03, 0x03,
};

static uint8_t permute_r0_mask_table[64] = {
    2,       1,       4,       3,       /* 4 bytes from pg0 */
    7,       6,       9,       8,       /* 4 bytes from pg1 */
    12,      11,      14,      13,      /* 4 bytes from pg2 */
    63,      4,       9,       14,      /* 1st bytes from pg0,pg1,pg2, and a padding */
    2 + 15,  1 + 15,  4 + 15,  3 + 15,  /* 4 bytes from pg3 */
    7 + 15,  6 + 15,  9 + 15,  8 + 15,  /* 4 bytes from pg4 */
    12 + 15, 11 + 15, 14 + 15, 13 + 15, /* 4 bytes from pg5 */
    63,      4 + 15,  9 + 15,  14 + 15, /* 1st bytes from pg3,pg4,pg5, and a padding */
    2 + 30,  1 + 30,  4 + 30,  3 + 30,  /* 4 bytes from pg6 */
    7 + 30,  6 + 30,  9 + 30,  8 + 30,  /* 4 bytes from pg7 */
    12 + 30, 11 + 30, 14 + 30, 13 + 30, /* 4 bytes from pg8 */
    63,      4 + 30,  9 + 30,  14 + 30, /* 1st bytes from pg6,pg7,pg8, and a padding */
    2 + 45,  1 + 45,  4 + 45,  3 + 45,  /* 4 bytes from pg6 */
    7 + 45,  6 + 45,  9 + 45,  8 + 45,  /* 4 bytes from pg7 */
    12 + 45, 11 + 45, 14 + 45, 13 + 45, /* 4 bytes from pg8 */
    63,      4 + 45,  9 + 45,  14 + 45, /* 1st bytes from pg9,pg10,pg11, and a padding */
};

static uint8_t and_r0_mask_table[64] = {
    0xFC, 0x0F, 0xFF, 0x00, 0xFC, 0x0F, 0xFF, 0x00, 0xFC, 0x0F, 0xFF, 0x00, 0xC0,
    0xC0, 0xC0, 0x00, 0xFC, 0x0F, 0xFF, 0x00, 0xFC, 0x0F, 0xFF, 0x00, 0xFC, 0x0F,
    0xFF, 0x00, 0xC0, 0xC0, 0xC0, 0x00, 0xFC, 0x0F, 0xFF, 0x00, 0xFC, 0x0F, 0xFF,
    0x00, 0xFC, 0x0F, 0xFF, 0x00, 0xC0, 0xC0, 0xC0, 0x00, 0xFC, 0x0F, 0xFF, 0x00,
    0xFC, 0x0F, 0xFF, 0x00, 0xFC, 0x0F, 0xFF, 0x00, 0xC0, 0xC0, 0xC0, 0x00,
};

static uint8_t permute_l1_mask_table[64] = {
    1,      13,      2,       3,       0,      /* pg0 */
    5,      14,      6,       7,       4,      /* pg1 */
    9,      15,      10,      11,      8,      /* pg2 */
    1 + 16, 13 + 16, 2 + 16,  3 + 16,  0 + 16, /* pg3 */
    5 + 16, 14 + 16, 6 + 16,  7 + 16,  4 + 16, /* pg4 */
    9 + 16, 15 + 16, 10 + 16, 11 + 16, 8 + 16, /* pg5 */
    1 + 32, 13 + 32, 2 + 32,  3 + 32,  0 + 32, /* pg6 */
    5 + 32, 14 + 32, 6 + 32,  7 + 32,  4 + 32, /* pg7 */
    9 + 32, 15 + 32, 10 + 32, 11 + 32, 8 + 32, /* pg8 */
    1 + 48, 13 + 48, 2 + 48,  3 + 48,  0 + 48, /* pg9 */
    5 + 48, 14 + 48, 6 + 48,  7 + 48,  4 + 48, /* pg10 */
    9 + 48, 15 + 48, 10 + 48, 11 + 48, 8 + 48, /* pg11 */
    60,     60,      60,      60,              /* zeros */
};

static uint8_t permute_r1_mask_table[64] = {
    3,       0,      1,      12,      2,       /* pg0 */
    7,       4,      5,      13,      6,       /* pg1 */
    11,      8,      9,      14,      10,      /* pg2 */
    3 + 16,  0 + 16, 1 + 16, 12 + 16, 2 + 16,  /* pg3 */
    7 + 16,  4 + 16, 5 + 16, 13 + 16, 6 + 16,  /* pg4 */
    11 + 16, 8 + 16, 9 + 16, 14 + 16, 10 + 16, /* pg5 */
    3 + 32,  0 + 32, 1 + 32, 12 + 32, 2 + 32,  /* pg6 */
    7 + 32,  4 + 32, 5 + 32, 13 + 32, 6 + 32,  /* pg7 */
    11 + 32, 8 + 32, 9 + 32, 14 + 32, 10 + 32, /* pg8 */
    3 + 48,  0 + 48, 1 + 48, 12 + 48, 2 + 48,  /* pg9 */
    7 + 48,  4 + 48, 5 + 48, 13 + 48, 6 + 48,  /* pg10 */
    11 + 48, 8 + 48, 9 + 48, 14 + 48, 10 + 48, /* pg11 */
    63,      63,     63,     63,               /* zeros */
};
/* end st20_rfc4175_422be10_to_422le10_avx512_vbmi */

/* for st20_rfc4175_422be10_to_422le8_avx512_vbmi */
static uint8_t word_permute_mask_table_512[16 * 4] = {
    1,      0,      2,      1,      3,      2,      4,      3,      /* pg0 */
    1 + 5,  0 + 5,  2 + 5,  1 + 5,  3 + 5,  2 + 5,  4 + 5,  3 + 5,  /* pg1 */
    1 + 10, 0 + 10, 2 + 10, 1 + 10, 3 + 10, 2 + 10, 4 + 10, 3 + 10, /* pg2 */
    1 + 15, 0 + 15, 2 + 15, 1 + 15, 3 + 15, 2 + 15, 4 + 15, 3 + 15, /* pg3 */
    1 + 20, 0 + 20, 2 + 20, 1 + 20, 3 + 20, 2 + 20, 4 + 20, 3 + 20, /* pg4 */
    1 + 25, 0 + 25, 2 + 25, 1 + 25, 3 + 25, 2 + 25, 4 + 25, 3 + 25, /* pg5 */
    1 + 30, 0 + 30, 2 + 30, 1 + 30, 3 + 30, 2 + 30, 4 + 30, 3 + 30, /* pg6 */
    1 + 35, 0 + 35, 2 + 35, 1 + 35, 3 + 35, 2 + 35, 4 + 35, 3 + 35, /* pg7 */
};
static uint16_t word_srlv_mask_table_512[8 * 4] = {
    6, 4, 2, 0, 6, 4, 2, 0, 6, 4, 2, 0, 6, 4, 2, 0,
    6, 4, 2, 0, 6, 4, 2, 0, 6, 4, 2, 0, 6, 4, 2, 0,
};
/* end st20_rfc4175_422be10_to_422le8_avx512_vbmi */

/* for st20_rfc4175_422le10_to_v210_avx512_vbmi */
static uint8_t permute_mask_table_512[16 * 4] = {
    0,      1,      2,      3,       4,       5,       6,       7,
    7,      8,      9,      10,      11,      12,      13,      14, /* pg0-2 */
    0 + 15, 1 + 15, 2 + 15, 3 + 15,  4 + 15,  5 + 15,  6 + 15,  7 + 15,
    7 + 15, 8 + 15, 9 + 15, 10 + 15, 11 + 15, 12 + 15, 13 + 15, 14 + 15, /* pg3-5 */
    0 + 30, 1 + 30, 2 + 30, 3 + 30,  4 + 30,  5 + 30,  6 + 30,  7 + 30,
    7 + 30, 8 + 30, 9 + 30, 10 + 30, 11 + 30, 12 + 30, 13 + 30, 14 + 30, /* pg6-8 */
    0 + 45, 1 + 45, 2 + 45, 3 + 45,  4 + 45,  5 + 45,  6 + 45,  7 + 45,
    7 + 45, 8 + 45, 9 + 45, 10 + 45, 11 + 45, 12 + 45, 13 + 45, 14 + 45, /* pg9-11 */
};

static uint8_t multishift_mask_table_512[16 * 4] = {
    0, 8, 16, 24, 30, 38, 46, 54, 4, 12, 20, 28, 34, 42, 50, 58,
    0, 8, 16, 24, 30, 38, 46, 54, 4, 12, 20, 28, 34, 42, 50, 58,
    0, 8, 16, 24, 30, 38, 46, 54, 4, 12, 20, 28, 34, 42, 50, 58,
    0, 8, 16, 24, 30, 38, 46, 54, 4, 12, 20, 28, 34, 42, 50, 58,
};
static uint8_t padding_mask_table_512[16 * 4] = {
    0xFF, 0xFF, 0xFF, 0x3F, 0xFF, 0xFF, 0xFF, 0x3F, 0xFF, 0xFF, 0xFF, 0x3F, 0xFF,
    0xFF, 0xFF, 0x3F, 0xFF, 0xFF, 0xFF, 0x3F, 0xFF, 0xFF, 0xFF, 0x3F, 0xFF, 0xFF,
    0xFF, 0x3F, 0xFF, 0xFF, 0xFF, 0x3F, 0xFF, 0xFF, 0xFF, 0x3F, 0xFF, 0xFF, 0xFF,
    0x3F, 0xFF, 0xFF, 0xFF, 0x3F, 0xFF, 0xFF, 0xFF, 0x3F, 0xFF, 0xFF, 0xFF, 0x3F,
    0xFF, 0xFF, 0xFF, 0x3F, 0xFF, 0xFF, 0xFF, 0x3F, 0xFF, 0xFF, 0xFF, 0x3F,
};
/* end st20_rfc4175_422le10_to_v210_avx512_vbmi */

/* for st20_rfc4175_422be10_to_v210_avx512_vbmi */
static uint8_t permute0_mask_table_512[16 * 4] = {
    1,      0,      3,       2,       4,       3,       7,       6,
    8,      7,      11,      10,      12,      11,      14,      13, /* pg 0-2 */
    1 + 15, 0 + 15, 3 + 15,  2 + 15,  4 + 15,  3 + 15,  7 + 15,  6 + 15,
    8 + 15, 7 + 15, 11 + 15, 10 + 15, 12 + 15, 11 + 15, 14 + 15, 13 + 15, /* pg 3-5 */
    1 + 30, 0 + 30, 3 + 30,  2 + 30,  4 + 30,  3 + 30,  7 + 30,  6 + 30,
    8 + 30, 7 + 30, 11 + 30, 10 + 30, 12 + 30, 11 + 30, 14 + 30, 13 + 30, /* pg 6-8 */
    1 + 45, 0 + 45, 3 + 45,  2 + 45,  4 + 45,  3 + 45,  7 + 45,  6 + 45,
    8 + 45, 7 + 45, 11 + 45, 10 + 45, 12 + 45, 11 + 45, 14 + 45, 13 + 45, /* pg 9-11 */
};
static uint8_t multishift0_mask_table_512[16 * 4] = {
    6, 14, 14, 22, 32, 40, 48, 56, 2, 10, 18, 26, 36, 44, 44, 52, /* pg 0-2 */
    6, 14, 14, 22, 32, 40, 48, 56, 2, 10, 18, 26, 36, 44, 44, 52, /* pg 3-5 */
    6, 14, 14, 22, 32, 40, 48, 56, 2, 10, 18, 26, 36, 44, 44, 52, /* pg 6-8 */
    6, 14, 14, 22, 32, 40, 48, 56, 2, 10, 18, 26, 36, 44, 44, 52, /* pg 9-11 */
};
static uint8_t and0_mask_table_512[16 * 4] = {
    0xFF, 0x03, 0xF0, 0x3F, 0xFF, 0x03, 0xF0, 0x3F, 0xFF, 0x03, 0xF0, 0x3F, 0xFF,
    0x03, 0xF0, 0x3F, 0xFF, 0x03, 0xF0, 0x3F, 0xFF, 0x03, 0xF0, 0x3F, 0xFF, 0x03,
    0xF0, 0x3F, 0xFF, 0x03, 0xF0, 0x3F, 0xFF, 0x03, 0xF0, 0x3F, 0xFF, 0x03, 0xF0,
    0x3F, 0xFF, 0x03, 0xF0, 0x3F, 0xFF, 0x03, 0xF0, 0x3F, 0xFF, 0x03, 0xF0, 0x3F,
    0xFF, 0x03, 0xF0, 0x3F, 0xFF, 0x03, 0xF0, 0x3F, 0xFF, 0x03, 0xF0, 0x3F,
};
static uint8_t permute1_mask_table_512[16 * 4] = {
    63, 2,      1,      63, 63, 6,       5,       63,
    63, 9,      8,      63, 63, 13,      12,      63, /* pg 0-2 */
    63, 2 + 15, 1 + 15, 63, 63, 6 + 15,  5 + 15,  63,
    63, 9 + 15, 8 + 15, 63, 63, 13 + 15, 12 + 15, 63, /* pg 3-5 */
    63, 2 + 30, 1 + 30, 63, 63, 6 + 30,  5 + 30,  63,
    63, 9 + 30, 8 + 30, 63, 63, 13 + 30, 12 + 30, 63, /* pg 6-8 */
    63, 2 + 45, 1 + 45, 63, 63, 6 + 45,  5 + 45,  63,
    63, 9 + 45, 8 + 45, 63, 63, 13 + 45, 12 + 45, 63, /* pg 9-11 */
};
static uint8_t multishift1_mask_table_512[16 * 4] = {
    0, 10, 18, 0, 0, 44, 52, 0, 0, 6, 14, 0, 0, 40, 48, 0, /* pg 0-2 */
    0, 10, 18, 0, 0, 44, 52, 0, 0, 6, 14, 0, 0, 40, 48, 0, /* pg 3-5 */
    0, 10, 18, 0, 0, 44, 52, 0, 0, 6, 14, 0, 0, 40, 48, 0, /* pg 6-8 */
    0, 10, 18, 0, 0, 44, 52, 0, 0, 6, 14, 0, 0, 40, 48, 0, /* pg 9-11 */
};
static uint8_t and1_mask_table_512[16 * 4] = {
    0x00, 0xFC, 0x0F, 0x00, 0x00, 0xFC, 0x0F, 0x00, 0x00, 0xFC, 0x0F, 0x00, 0x00,
    0xFC, 0x0F, 0x00, 0x00, 0xFC, 0x0F, 0x00, 0x00, 0xFC, 0x0F, 0x00, 0x00, 0xFC,
    0x0F, 0x00, 0x00, 0xFC, 0x0F, 0x00, 0x00, 0xFC, 0x0F, 0x00, 0x00, 0xFC, 0x0F,
    0x00, 0x00, 0xFC, 0x0F, 0x00, 0x00, 0xFC, 0x0F, 0x00, 0x00, 0xFC, 0x0F, 0x00,
    0x00, 0xFC, 0x0F, 0x00, 0x00, 0xFC, 0x0F, 0x00, 0x00, 0xFC, 0x0F, 0x00,
};
/* end st20_rfc4175_422be10_to_v210_avx512_vbmi */

ST_TARGET_CODE_START_AVX512_VBMI2
int st20_rfc4175_422be10_to_yuv422p10le_avx512_vbmi(struct st20_rfc4175_422_10_pg2_be* pg,
                                                    uint16_t* y, uint16_t* b, uint16_t* r,
                                                    uint32_t w, uint32_t h) {
  __m512i permute_le_mask = _mm512_loadu_si512(b2l_permute_mask_table_512);
  __m512i srlv_le_mask = _mm512_loadu_si512(b2l_srlv_mask_table_512);
  __m512i srlv_and_mask = _mm512_loadu_si512(b2l_and_mask_table_512);
  __mmask64 k = 0xFFFFFFFFFF; /* each __m512i with 2*4 pg group, 40 bytes */

  int pg_cnt = w * h / 2;
  dbg("%s, pg_cnt %d\n", __func__, pg_cnt);

#if 0
  while (pg_cnt >= 32) {
    __m512i stage_m512i[4];
    for (int j = 0; j < 4; j++) {
      __m512i input = _mm512_maskz_loadu_epi8(k, pg);
      __m512i permute_le_result = _mm512_permutexvar_epi8(permute_le_mask, input);
      __m512i srlv_le_result = _mm512_srlv_epi16(permute_le_result, srlv_le_mask);
      stage_m512i[j] = _mm512_and_si512(srlv_le_result, srlv_and_mask);
      pg += 8;
    }

    /* shift m128i to m512i */
    /* {B0, R0, Y0, Y1}, {B1, R1, Y2, Y3}, {B2, R2, Y4, Y5}, {B3, R3, Y6, Y7} */
    __m512i result_m512i[4];
    /* {B0, R0, B1, R1} */
    result_m512i[0] = _mm512_shuffle_i32x4(stage_m512i[0], stage_m512i[1], 0b01000100);
    /* {Y0, Y1, Y2, Y3} */
    result_m512i[1] = _mm512_shuffle_i32x4(stage_m512i[0], stage_m512i[1], 0b11101110);
    _mm512_storeu_si512((__m512i*)y, result_m512i[1]);
    y += 32;
    /* {B2, R2, B3, R3} */
    result_m512i[2] = _mm512_shuffle_i32x4(stage_m512i[2], stage_m512i[3], 0b01000100);
    /* {Y4, Y5, Y6, Y7} */
    result_m512i[3] = _mm512_shuffle_i32x4(stage_m512i[2], stage_m512i[3], 0b11101110);
    _mm512_storeu_si512((__m512i*)y, result_m512i[3]);
    y += 32;
    __m512i b_result_m512i =
        _mm512_shuffle_i32x4(result_m512i[0], result_m512i[2], 0b10001000);
    _mm512_storeu_si512((__m512i*)b, b_result_m512i);
    b += 32;
    __m512i r_result_m512i =
        _mm512_shuffle_i32x4(result_m512i[0], result_m512i[2], 0b11011101);
    _mm512_storeu_si512((__m512i*)r, r_result_m512i);
    r += 32;

    pg_cnt -= 32;
  }
#endif

  /* each __m512i batch handle 8 pg groups */
  while (pg_cnt >= 8) {
    __m512i input = _mm512_maskz_loadu_epi8(k, pg);
    __m512i permute_le_result = _mm512_permutexvar_epi8(permute_le_mask, input);
    __m512i srlv_le_result = _mm512_srlv_epi16(permute_le_result, srlv_le_mask);
    __m512i stage_m512i = _mm512_and_si512(srlv_le_result, srlv_and_mask);

    pg += 8;

    __m128i result_B = _mm512_extracti32x4_epi32(stage_m512i, 0);
    __m128i result_R = _mm512_extracti32x4_epi32(stage_m512i, 1);
    __m128i result_Y0 = _mm512_extracti32x4_epi32(stage_m512i, 2);
    __m128i result_Y1 = _mm512_extracti32x4_epi32(stage_m512i, 3);

    _mm_storeu_si128((__m128i*)b, result_B);
    b += 2 * 4;
    _mm_storeu_si128((__m128i*)r, result_R);
    r += 2 * 4;
    _mm_storeu_si128((__m128i*)y, result_Y0);
    y += 2 * 4;
    _mm_storeu_si128((__m128i*)y, result_Y1);
    y += 2 * 4;

    pg_cnt -= 8;
  }

  while (pg_cnt > 0) {
    st20_unpack_pg2be_422le10(pg, b, y, r, y + 1);
    b++;
    r++;
    y += 2;
    pg++;

    pg_cnt--;
  }

  return 0;
}

int st20_rfc4175_422be10_to_422le10_avx512_vbmi(struct st20_rfc4175_422_10_pg2_be* pg_be,
                                                struct st20_rfc4175_422_10_pg2_le* pg_le,
                                                uint32_t w, uint32_t h) {
  __m512i permute_l0 = _mm512_loadu_si512((__m512i*)permute_l0_mask_table);
  __m512i permute_r0 = _mm512_loadu_si512((__m512i*)permute_r0_mask_table);
  __m512i and_l0 = _mm512_loadu_si512((__m512i*)and_l0_mask_table);
  __m512i and_r0 = _mm512_loadu_si512((__m512i*)and_r0_mask_table);
  __m512i permute_l1 = _mm512_loadu_si512((__m512i*)permute_l1_mask_table);
  __m512i permute_r1 = _mm512_loadu_si512((__m512i*)permute_r1_mask_table);
  __mmask16 k = 0x7FFF; /* each __m512i with 12 pg group, 60 bytes */

  int pg_cnt = w * h / 2;
  int batch = pg_cnt / 12;

  for (int i = 0; i < batch; i++) {
    __m512i input = _mm512_maskz_loadu_epi32(k, (__m512i*)pg_be);
    __m512i permute_l0_result = _mm512_permutexvar_epi8(permute_l0, input);
    __m512i permute_r0_result = _mm512_permutexvar_epi8(permute_r0, input);
    __m512i rl_result = _mm512_and_si512(_mm512_rol_epi32(permute_l0_result, 2), and_l0);
    __m512i rr_result = _mm512_and_si512(_mm512_ror_epi32(permute_r0_result, 2), and_r0);
    __m512i rl_result_shuffle = _mm512_permutexvar_epi8(permute_l1, rl_result);
    __m512i rr_result_shuffle = _mm512_permutexvar_epi8(permute_r1, rr_result);
    __m512i result = _mm512_or_si512(rl_result_shuffle, rr_result_shuffle);

    /* store to the first 60 bytes after dest address */
    _mm512_mask_storeu_epi32((__m512i*)pg_le, k, result);

    pg_be += 12;
    pg_le += 12;
  }

  int left = pg_cnt % 12;
  while (left) {
    uint16_t cb, y0, cr, y1;

    cb = (pg_be->Cb00 << 2) + pg_be->Cb00_;
    y0 = (pg_be->Y00 << 4) + pg_be->Y00_;
    cr = (pg_be->Cr00 << 6) + pg_be->Cr00_;
    y1 = (pg_be->Y01 << 8) + pg_be->Y01_;

    pg_le->Cb00 = cb;
    pg_le->Cb00_ = cb >> 8;
    pg_le->Y00 = y0;
    pg_le->Y00_ = y0 >> 6;
    pg_le->Cr00 = cr;
    pg_le->Cr00_ = cr >> 4;
    pg_le->Y01 = y1;
    pg_le->Y01_ = y1 >> 2;
    pg_be++;
    pg_le++;
    left--;
  }

  return 0;
}

int st20_rfc4175_422be10_to_422le8_avx512_vbmi(struct st20_rfc4175_422_10_pg2_be* pg_10,
                                               struct st20_rfc4175_422_8_pg2_le* pg_8,
                                               uint32_t w, uint32_t h) {
  __m512i permute_mask = _mm512_loadu_si512((__m512i*)word_permute_mask_table_512);
  __m512i srlv_mask = _mm512_loadu_si512((__m512i*)word_srlv_mask_table_512);
  __mmask64 k_load = 0xFFFFFFFFFF; /* each __m512i with 2*4 pg group, 40 bytes */
  __mmask64 k_store = 0x5555555555555555;
  int pg_cnt = w * h / 2;
  dbg("%s, pg_cnt %d\n", __func__, pg_cnt);

  /* each __m512i batch handle 8 pg groups */
  while (pg_cnt >= 8) {
    __m512i input = _mm512_maskz_loadu_epi8(k_load, pg_10);
    __m512i permute_result = _mm512_permutexvar_epi8(permute_mask, input);
    __m512i srlv_result = _mm512_srlv_epi16(permute_result, srlv_mask);
    __m512i result = _mm512_srli_epi16(srlv_result, 2);

    _mm512_mask_compressstoreu_epi8(pg_8, k_store, result);

    pg_10 += 8;
    pg_8 += 8;
    pg_cnt -= 8;
  }

  while (pg_cnt > 0) {
    pg_8->Cb00 = pg_10->Cb00;
    pg_8->Y00 = (pg_10->Y00 << 2) + (pg_10->Y00_ >> 2);
    pg_8->Cr00 = (pg_10->Cr00 << 4) + (pg_10->Cr00_ >> 2);
    pg_8->Y01 = (pg_10->Y01 << 6) + (pg_10->Y01_ >> 2);

    pg_10++;
    pg_8++;

    pg_cnt--;
  }

  return 0;
}

int st20_rfc4175_422le10_to_v210_avx512_vbmi(uint8_t* pg_le, uint8_t* pg_v210, uint32_t w,
                                             uint32_t h) {
  __m512i permute_mask = _mm512_loadu_si512((__m512i*)permute_mask_table_512);
  __m512i multishift_mask = _mm512_loadu_si512((__m512i*)multishift_mask_table_512);
  __m512i padding_mask = _mm512_loadu_si512((__m512i*)padding_mask_table_512);
  __mmask16 k = 0x7FFF; /* each __m512i with 12 pg group, 60 bytes */

  int pg_cnt = w * h / 2;
  if (pg_cnt % 12 != 0) {
    err("%s, invalid pg_cnt %d, pixel group number must be multiple of 12!\n", __func__,
        pg_cnt);
    return -EINVAL;
  }

  int batch = pg_cnt / 12;
  for (int i = 0; i < batch; i++) {
    __m512i input = _mm512_maskz_loadu_epi32(k, (__m512i*)pg_le);
    __m512i permute_result = _mm512_permutexvar_epi8(permute_mask, input);
    __m512i multishift_result =
        _mm512_multishift_epi64_epi8(multishift_mask, permute_result);
    __m512i result = _mm512_and_si512(multishift_result, padding_mask);

    _mm512_storeu_si512((__m512i*)pg_v210, result);

    pg_le += 60;
    pg_v210 += 64;
  }

  return 0;
}

int st20_rfc4175_422be10_to_v210_avx512_vbmi(uint8_t* pg_be, uint8_t* pg_v210, uint32_t w,
                                             uint32_t h) {
  __m512i permute0_mask = _mm512_loadu_si512((__m512i*)permute0_mask_table_512);
  __m512i multishift0_mask = _mm512_loadu_si512((__m512i*)multishift0_mask_table_512);
  __m512i and0_mask = _mm512_loadu_si512((__m512i*)and0_mask_table_512);
  __m512i permute1_mask = _mm512_loadu_si512((__m512i*)permute1_mask_table_512);
  __m512i multishift1_mask = _mm512_loadu_si512((__m512i*)multishift1_mask_table_512);
  __m512i and1_mask = _mm512_loadu_si512((__m512i*)and1_mask_table_512);
  __mmask16 k = 0x7FFF; /* each __m512i with 12 pg group, 60 bytes */

  int pg_cnt = w * h / 2;
  if (pg_cnt % 12 != 0) {
    err("%s, invalid pg_cnt %d, pixel group number must be multiple of 12!\n", __func__,
        pg_cnt);
    return -EINVAL;
  }

  int batch = pg_cnt / 12;
  for (int i = 0; i < batch; i++) {
    __m512i input = _mm512_maskz_loadu_epi32(k, (__m512i*)pg_be);
    __m512i permute0_result = _mm512_permutexvar_epi8(permute0_mask, input);
    __m512i multishift0_result =
        _mm512_multishift_epi64_epi8(multishift0_mask, permute0_result);
    __m512i and0_result = _mm512_and_si512(multishift0_result, and0_mask);
    __m512i permute1_result = _mm512_permutexvar_epi8(permute1_mask, input);
    __m512i multishift1_result =
        _mm512_multishift_epi64_epi8(multishift1_mask, permute1_result);
    __m512i and1_result = _mm512_and_si512(multishift1_result, and1_mask);
    __m512i result = _mm512_and_si512(and0_result, and1_result);

    _mm512_storeu_si512((__m512i*)pg_v210, result);

    pg_be += 60;
    pg_v210 += 64;
  }

  return 0;
}
ST_TARGET_CODE_STOP
#endif
