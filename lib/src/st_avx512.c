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

#include "st_avx512.h"

#include "st_log.h"
#include "st_simd.h"

#ifdef ST_HAS_AVX512
static uint8_t b2l_shuffle_mask_table[16] = {
    0x01, 0x00, 0x06, 0x05, 0x03, 0x02, 0x08, 0x07, /* b0, b1, r0, r1 */
    0x02, 0x01, 0x04, 0x03, 0x07, 0x06, 0x09, 0x08, /* y0, y1, y2, y3 */
};

static uint16_t b2l_srlv_mask_table[8] = {
    0x0006, 0x0006, 0x0002, 0x0002, 0x0004, 0x0000, 0x0004, 0x0000,
};

static uint16_t b2l_and_mask_table[8] = {
    0x03ff, 0x03ff, 0x03ff, 0x03ff, 0x03ff, 0x03ff, 0x03ff, 0x03ff,
};

/*
 * {B0, R0, Y0, Y1}, {B1, R1, Y2, Y3}, {B2, R2, Y4, Y5}, {B3, R3, Y6, Y7}
 * to
 * {B0, B1, B2, B3}, {R0, R1, R2, R3}, {Y0, Y1, Y2, Y3}, {Y4, Y5, Y6, Y7},
 */
static uint32_t b2l_permute_mask_table[16] = {
    0, 4, 8, 12, 1, 5, 9, 13, 2, 3, 6, 7, 10, 11, 14, 15,
};

/* for st20_rfc4175_422be10_to_422le10_avx512 */
static uint8_t shuffle_l0_mask_table[16] = {
    1,  0,  3,  2,    /* 4 bytes from pg0 */
    6,  5,  8,  7,    /* 4 bytes from pg1 */
    11, 10, 13, 12,   /* 4 bytes from pg2 */
    0,  5,  10, 0x80, /* 5th bytes from pg0,pg1,pg2, and a padding */
};

static uint8_t and_l0_mask_table[16] = {
    0x00, 0xFF, 0xF0, 0x3F, 0x00, 0xFF, 0xF0, 0x3F,
    0x00, 0xFF, 0xF0, 0x3F, 0x00, 0x03, 0x03, 0x03,
};

static uint8_t shuffle_r0_mask_table[16] = {
    2,    1,  4,  3,  /* 4 bytes from pg0 */
    7,    6,  9,  8,  /* 4 bytes from pg1 */
    12,   11, 14, 13, /* 4 bytes from pg2 */
    0x80, 4,  9,  14, /* 1st bytes from pg0,pg1,pg2, and a padding */
};

static uint8_t and_r0_mask_table[16] = {
    0xFC, 0x0F, 0xFF, 0x00, 0xFC, 0x0F, 0xFF, 0x00,
    0xFC, 0x0F, 0xFF, 0x00, 0xC0, 0xC0, 0xC0, 0x00,
};

static uint8_t shuffle_l1_mask_table[16] = {
    1,    13, 2,  3,  0x80, /* pg0 */
    5,    14, 6,  7,  0x80, /* pg1 */
    9,    15, 10, 11, 0x80, /* pg2 */
    0x80,                   /* zeros */
};

static uint8_t shuffle_r1_mask_table[16] = {
    0x80, 0, 1, 12, 2,  /* pg0 */
    0x80, 4, 5, 13, 6,  /* pg1 */
    0x80, 8, 9, 14, 10, /* pg2 */
    0x80,               /* zeros */
};
/* end st20_rfc4175_422be10_to_422le10_avx512 */

/* for st20_rfc4175_422be10_to_422le8_avx512 */
static uint8_t word_shuffle_mask_table_128[16] = {
    1,     0,     2,     1,     3,     2,     4,     3,     /* pg0 */
    1 + 5, 0 + 5, 2 + 5, 1 + 5, 3 + 5, 2 + 5, 4 + 5, 3 + 5, /* pg1 */
};
static uint16_t word_srlv_mask_table_128[8] = {
    6, 4, 2, 0, 6, 4, 2, 0,
};
static uint8_t word_srlv_shuffle_mask_table_128[16] = {
    0, 2, 4, 6, 8, 10, 12, 14, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
};
/* end st20_rfc4175_422be10_to_422le8_avx512 */

/* for st20_rfc4175_422le10_to_v210_avx512 */
static uint8_t shuffle_r_mask_table_128[16] = {
    0, 1, 2, 3, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
};
static uint32_t srlv_mask_table_128[4] = {
    0,
    6,
    4,
    2,
};
static uint32_t sllv_mask_table_128[4] = {
    0,
    2,
    4,
    0,
};
static uint8_t padding_mask_table_128[16] = {
    0xFF, 0xFF, 0xFF, 0x3F, 0xFF, 0xFF, 0xFF, 0x3F,
    0xFF, 0xFF, 0xFF, 0x3F, 0xFF, 0xFF, 0xFF, 0x3F,
};
/* end st20_rfc4175_422le10_to_v210_avx512 */

/* for st20_rfc4175_422be10_to_v210_avx512 */
static uint8_t shuffle0_mask_table_128[16] = {
    1, 0, 3, 2, 4, 3, 7, 6, 8, 7, 11, 10, 12, 11, 14, 13,
};

static uint16_t sllv0_mask_table_128[8] = {
    0, 2, 0, 0, 0, 0, 0, 4,
};

static uint16_t srlv0_mask_table_128[8] = {
    6, 0, 0, 0, 2, 2, 4, 0,
};

static uint8_t and0_mask_table_128[16] = {
    0xFF, 0x03, 0xF0, 0x3F, 0xFF, 0x03, 0xF0, 0x3F,
    0xFF, 0x03, 0xF0, 0x3F, 0xFF, 0x03, 0xF0, 0x3F,
};

static uint8_t shuffle1_mask_table_128[16] = {
    0x80, 2, 1, 0x80, 0x80, 6, 5, 0x80, 0x80, 9, 8, 0x80, 0x80, 13, 12, 0x80,
};

static uint32_t srlv1_mask_table_128[4] = {
    2,
    4,
    0,
    0,
};

static uint8_t and1_mask_table_128[16] = {
    0x00, 0xFC, 0x0F, 0x00, 0x00, 0xFC, 0x0F, 0x00,
    0x00, 0xFC, 0x0F, 0x00, 0x00, 0xFC, 0x0F, 0x00,
};
/* end st20_rfc4175_422be10_to_v210_avx512 */

ST_TARGET_CODE_START_AVX512
int st20_rfc4175_422be10_to_422le10_avx512(struct st20_rfc4175_422_10_pg2_be* pg_be,
                                           struct st20_rfc4175_422_10_pg2_le* pg_le,
                                           uint32_t w, uint32_t h) {
  __m128i shuffle_l0 = _mm_loadu_si128((__m128i*)shuffle_l0_mask_table);
  __m128i shuffle_r0 = _mm_loadu_si128((__m128i*)shuffle_r0_mask_table);
  __m128i and_l0 = _mm_loadu_si128((__m128i*)and_l0_mask_table);
  __m128i and_r0 = _mm_loadu_si128((__m128i*)and_r0_mask_table);
  __m128i shuffle_l1 = _mm_loadu_si128((__m128i*)shuffle_l1_mask_table);
  __m128i shuffle_r1 = _mm_loadu_si128((__m128i*)shuffle_r1_mask_table);
  __mmask16 k = 0x7FFF; /* each __m128i with 3 pg group, 15 bytes */

  int pg_cnt = w * h / 2;
  dbg("%s, pg_cnt %d\n", __func__, pg_cnt);
  int batch = pg_cnt / 3;

  for (int i = 0; i < batch; i++) {
    __m128i input = _mm_maskz_loadu_epi8(k, (__m128i*)pg_be);
    __m128i shuffle_l0_result = _mm_shuffle_epi8(input, shuffle_l0);
    __m128i shuffle_r0_result = _mm_shuffle_epi8(input, shuffle_r0);
    __m128i rl_result = _mm_and_si128(_mm_rol_epi32(shuffle_l0_result, 2), and_l0);
    __m128i rr_result = _mm_and_si128(_mm_ror_epi32(shuffle_r0_result, 2), and_r0);
    __m128i rl_result_shuffle = _mm_shuffle_epi8(rl_result, shuffle_l1);
    __m128i rr_result_shuffle = _mm_shuffle_epi8(rr_result, shuffle_r1);
    __m128i result = _mm_or_si128(rl_result_shuffle, rr_result_shuffle);

    /* store to the first 15 bytes after dest address */
    _mm_mask_storeu_epi8((__m128i*)pg_le, k, result);

    pg_be += 3;
    pg_le += 3;
  }

  int left = pg_cnt % 3;
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

int st20_rfc4175_422be10_to_yuv422p10le_avx512(struct st20_rfc4175_422_10_pg2_be* pg,
                                               uint16_t* y, uint16_t* b, uint16_t* r,
                                               uint32_t w, uint32_t h) {
  __m128i shuffle_le_mask = _mm_loadu_si128((__m128i*)b2l_shuffle_mask_table);
  __m128i srlv_le_mask = _mm_loadu_si128((__m128i*)b2l_srlv_mask_table);
  __m128i srlv_and_mask = _mm_loadu_si128((__m128i*)b2l_and_mask_table);
  __m512i permute_mask = _mm512_loadu_si512((__m512i*)b2l_permute_mask_table);
  __mmask16 k = 0x3FF; /* each __m128i with 2 pg group, 10 bytes */
  int pg_cnt = w * h / 2;
  dbg("%s, pg_cnt %d\n", __func__, pg_cnt);

  /* each m512i batch handle 4 __m512i(16 __m128i), each __m128i with 2 pg group */
  while (pg_cnt >= 32) {
    /* cvt the result to __m128i(2 pg group) */
    __m128i stage_m128i[16];
    for (int j = 0; j < 16; j++) {
      __m128i input = _mm_maskz_loadu_epi8(k, (__m128i*)pg);
      __m128i shuffle_le_result = _mm_shuffle_epi8(input, shuffle_le_mask);
      __m128i srlv_le_result = _mm_srlv_epi16(shuffle_le_result, srlv_le_mask);
      stage_m128i[j] = _mm_and_si128(srlv_le_result, srlv_and_mask);
      pg += 2;
    }
    /* shift result to m128i */
    __m512i stage_m512i[4];
    for (int j = 0; j < 4; j++) {
      /* {B0, R0, Y0, Y1}, {B1, R1, Y2, Y3}, {B2, R2, Y4, Y5}, {B3, R3, Y6, Y7} */
      __m512i input_m512i = _mm512_loadu_si512((__m512i*)&stage_m128i[j * 4]);
      /* {B0, B1, B2, B3}, {R0, R1, R2, R3}, {Y0, Y1, Y2, Y3}, {Y4, Y5, Y6, Y7} */
      stage_m512i[j] = _mm512_permutexvar_epi32(permute_mask, input_m512i);
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

  /* each __m128i batch handle 4 16 __m128i, each __m128i with 2 pg group */
  while (pg_cnt >= 8) {
    __m128i stage_m128i[4];
    for (int j = 0; j < 4; j++) {
      __m128i input = _mm_maskz_loadu_epi8(k, (__m128i*)pg);
      __m128i shuffle_le_result = _mm_shuffle_epi8(input, shuffle_le_mask);
      __m128i srlv_le_result = _mm_srlv_epi16(shuffle_le_result, srlv_le_mask);
      stage_m128i[j] = _mm_and_si128(srlv_le_result, srlv_and_mask);
      pg += 2;
    }
    // {B0, R0, Y0, Y1}, {B1, R1, Y2, Y3}, {B2, R2, Y4, Y5}, {B3, R3, Y6, Y7}
    __m512i stage_m512i = _mm512_loadu_si512((__m512i*)&stage_m128i[0]);
    /* {B0, B1, B2, B3}, {R0, R1, R2, R3}, {Y0, Y1, Y2, Y3}, {Y4, Y5, Y6, Y7} */
    __m512i permute = _mm512_permutexvar_epi32(permute_mask, stage_m512i);

    __m128i result_B = _mm512_extracti32x4_epi32(permute, 0);
    __m128i result_R = _mm512_extracti32x4_epi32(permute, 1);
    __m128i result_Y0 = _mm512_extracti32x4_epi32(permute, 2);
    __m128i result_Y1 = _mm512_extracti32x4_epi32(permute, 3);

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

  dbg("%s, remaining pg_cnt %d\n", __func__, pg_cnt);
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

int st20_rfc4175_422be10_to_422le8_avx512(struct st20_rfc4175_422_10_pg2_be* pg_10,
                                          struct st20_rfc4175_422_8_pg2_le* pg_8,
                                          uint32_t w, uint32_t h) {
  __m128i shuffle_mask = _mm_loadu_si128((__m128i*)word_shuffle_mask_table_128);
  __m128i srlv_mask = _mm_loadu_si128((__m128i*)word_srlv_mask_table_128);
  __m128i srlv_shuffle_mask = _mm_loadu_si128((__m128i*)word_srlv_shuffle_mask_table_128);
  __mmask16 k = 0x3FF; /* each __m128i with 2 pg group, 10 bytes */
  int pg_cnt = w * h / 2;
  dbg("%s, pg_cnt %d\n", __func__, pg_cnt);

  while (pg_cnt >= 2) {
    __m128i input = _mm_maskz_loadu_epi8(k, (__m128i*)pg_10);
    __m128i shuffle_result = _mm_shuffle_epi8(input, shuffle_mask);
    __m128i srlv_result = _mm_srlv_epi16(shuffle_result, srlv_mask);
    __m128i srlv_srli_result = _mm_srli_epi16(srlv_result, 2);
    __m128i result = _mm_shuffle_epi8(srlv_srli_result, srlv_shuffle_mask);

    _mm_storel_epi64((__m128i*)pg_8, result);

    pg_10 += 2;
    pg_8 += 2;
    pg_cnt -= 2;
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

int st20_rfc4175_422le10_to_v210_avx512(uint8_t* pg_le, uint8_t* pg_v210, uint32_t w,
                                        uint32_t h) {
  __m128i shuffle_r_mask = _mm_loadu_si128((__m128i*)shuffle_r_mask_table_128);
  __m128i srlv_mask = _mm_loadu_si128((__m128i*)srlv_mask_table_128);
  __m128i sllv_mask = _mm_loadu_si128((__m128i*)sllv_mask_table_128);
  __m128i padding_mask = _mm_loadu_si128((__m128i*)padding_mask_table_128);
  __mmask16 k = 0x7FFF; /* each __m128i with 3 pg group, 15 bytes */
  __mmask16 k_mov = 0x0880;

  int pg_cnt = w * h / 2;
  if (pg_cnt % 3 != 0) {
    err("%s, invalid pg_cnt %d, pixel group number must be multiple of 3!\n", __func__,
        pg_cnt);
    return -EINVAL;
  }

  int batch = pg_cnt / 3;
  for (int i = 0; i < batch; i++) {
    __m128i input = _mm_maskz_loadu_epi8(k, (__m128i*)pg_le);
    __m128i shuffle_l_result = _mm_maskz_mov_epi8(k_mov, input);
    __m128i shuffle_r_result = _mm_shuffle_epi8(input, shuffle_r_mask);
    __m128i sllv_result = _mm_sllv_epi32(shuffle_l_result, sllv_mask);
    __m128i srlv_result = _mm_srlv_epi32(shuffle_r_result, srlv_mask);
    __m128i result = _mm_and_si128(_mm_or_si128(sllv_result, srlv_result), padding_mask);

    _mm_store_si128((__m128i*)pg_v210, result);

    pg_le += 15;
    pg_v210 += 16;
  }

  return 0;
}

int st20_rfc4175_422be10_to_v210_avx512(uint8_t* pg_be, uint8_t* pg_v210, uint32_t w,
                                        uint32_t h) {
  __m128i shuffle0_mask = _mm_loadu_si128((__m128i*)shuffle0_mask_table_128);
  __m128i sllv0_mask = _mm_loadu_si128((__m128i*)sllv0_mask_table_128);
  __m128i srlv0_mask = _mm_loadu_si128((__m128i*)srlv0_mask_table_128);
  __m128i and0_mask = _mm_loadu_si128((__m128i*)and0_mask_table_128);
  __m128i shuffle1_mask = _mm_loadu_si128((__m128i*)shuffle1_mask_table_128);
  __m128i srlv1_mask = _mm_loadu_si128((__m128i*)srlv1_mask_table_128);
  __m128i and1_mask = _mm_loadu_si128((__m128i*)and1_mask_table_128);

  __mmask16 k_load = 0x7FFF; /* each __m128i with 3 pg group, 15 bytes */
  __mmask8 k_l1 = 0x04;      /* shift left for shuffle1_result(epi32)[2] */

  int pg_cnt = w * h / 2;
  if (pg_cnt % 3 != 0) {
    printf("%s, invalid pg_cnt %d, pixel group number must be multiple of 3!\n", __func__,
           pg_cnt);
    return -EINVAL;
  }

  int batch = pg_cnt / 3;
  for (int i = 0; i < batch; i++) {
    __m128i input = _mm_maskz_loadu_epi8(k_load, (__m128i*)pg_be);
    __m128i shuffle0_result = _mm_shuffle_epi8(input, shuffle0_mask);
    __m128i sllv0_result = _mm_sllv_epi16(shuffle0_result, sllv0_mask);
    __m128i srlv0_result = _mm_srlv_epi16(sllv0_result, srlv0_mask);
    __m128i and0_result = _mm_and_si128(srlv0_result, and0_mask);
    __m128i shuffle1_result = _mm_shuffle_epi8(input, shuffle1_mask);
    __m128i slli1_result = _mm_mask_slli_epi32(shuffle1_result, k_l1, shuffle1_result, 2);
    __m128i srlv1_result = _mm_srlv_epi32(slli1_result, srlv1_mask);
    __m128i and1_result = _mm_and_si128(srlv1_result, and1_mask);
    __m128i result = _mm_or_si128(and0_result, and1_result);

    _mm_store_si128((__m128i*)pg_v210, result);

    pg_be += 15;
    pg_v210 += 16;
  }

  return 0;
}
ST_TARGET_CODE_STOP
#endif
