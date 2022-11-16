/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_avx2.h"

#include "st_dma.h"
#include "st_log.h"
#include "st_simd.h"
#include "st_util.h"

#ifdef MTL_HAS_AVX2
ST_TARGET_CODE_START_AVX2
/* begin st20_rfc4175_422be10_to_422le10_avx2 */
static uint8_t rfc4175_b2l_shuffle_l0_tbl[16] = {
    1,  0,  3,  2,    /* 4 bytes from pg0 */
    6,  5,  8,  7,    /* 4 bytes from pg1 */
    11, 10, 13, 12,   /* 4 bytes from pg2 */
    0,  5,  10, 0x80, /* 5th bytes from pg0,pg1,pg2, and a padding */
};

static uint8_t rfc4175_b2l_and_l0_tbl[16] = {
    0x00, 0xFF, 0xF0, 0x3F, 0x00, 0xFF, 0xF0, 0x3F,
    0x00, 0xFF, 0xF0, 0x3F, 0x00, 0x03, 0x03, 0x03,
};

static uint8_t rfc4175_b2l_shuffle_r0_tbl[16] = {
    2,    1,  4,  3,  /* 4 bytes from pg0 */
    7,    6,  9,  8,  /* 4 bytes from pg1 */
    12,   11, 14, 13, /* 4 bytes from pg2 */
    0x80, 4,  9,  14, /* 1st bytes from pg0,pg1,pg2, and a padding */
};

static uint8_t rfc4175_b2l_and_r0_tbl[16] = {
    0xFC, 0x0F, 0xFF, 0x00, 0xFC, 0x0F, 0xFF, 0x00,
    0xFC, 0x0F, 0xFF, 0x00, 0xC0, 0xC0, 0xC0, 0x00,
};

static uint8_t rfc4175_b2l_shuffle_l1_tbl[16] = {
    1,    13, 2,  3,  0x80, /* pg0 */
    5,    14, 6,  7,  0x80, /* pg1 */
    9,    15, 10, 11, 0x80, /* pg2 */
    0x80,                   /* zeros */
};

static uint8_t rfc4175_b2l_shuffle_r1_tbl[16] = {
    0x80, 0, 1, 12, 2,  /* pg0 */
    0x80, 4, 5, 13, 6,  /* pg1 */
    0x80, 8, 9, 14, 10, /* pg2 */
    0x80,               /* zeros */
};

int st20_rfc4175_422be10_to_422le10_avx2(struct st20_rfc4175_422_10_pg2_be* pg_be,
                                         struct st20_rfc4175_422_10_pg2_le* pg_le,
                                         uint32_t w, uint32_t h) {
  __m128i shuffle_l0 = _mm_loadu_si128((__m128i*)rfc4175_b2l_shuffle_l0_tbl);
  __m128i shuffle_r0 = _mm_loadu_si128((__m128i*)rfc4175_b2l_shuffle_r0_tbl);
  __m128i and_l0 = _mm_loadu_si128((__m128i*)rfc4175_b2l_and_l0_tbl);
  __m128i and_r0 = _mm_loadu_si128((__m128i*)rfc4175_b2l_and_r0_tbl);
  __m128i shuffle_l1 = _mm_loadu_si128((__m128i*)rfc4175_b2l_shuffle_l1_tbl);
  __m128i shuffle_r1 = _mm_loadu_si128((__m128i*)rfc4175_b2l_shuffle_r1_tbl);

  int pg_cnt = w * h / 2;
  int batch = pg_cnt / 3;
  int left = pg_cnt % 3;
  dbg("%s, pg_cnt %d batch %d left %d\n", __func__, pg_cnt, batch, left);
  /* jump the last batch, for the Xmm may access invalid memory in the last byte */
  if (batch != 0 && left == 0) {
    left = 3;
    batch -= 1;
  }

  /* jump the last batch, for the Xmm may access invalid memory in the last byte */
  for (int i = 0; i < batch; i++) {
    __m128i input = _mm_loadu_si128((__m128i*)pg_be);
    __m128i shuffle_l0_result = _mm_shuffle_epi8(input, shuffle_l0);
    __m128i shuffle_r0_result = _mm_shuffle_epi8(input, shuffle_r0);
    __m128i sl_result = _mm_and_si128(_mm_slli_epi32(shuffle_l0_result, 2), and_l0);
    __m128i sr_result = _mm_and_si128(_mm_srli_epi32(shuffle_r0_result, 2), and_r0);
    __m128i sl_result_shuffle = _mm_shuffle_epi8(sl_result, shuffle_l1);
    __m128i sr_result_shuffle = _mm_shuffle_epi8(sr_result, shuffle_r1);
    __m128i result = _mm_or_si128(sl_result_shuffle, sr_result_shuffle);

    _mm_storeu_si128((__m128i*)pg_le, result);

    pg_be += 3;
    pg_le += 3;
  }

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
/* end st20_rfc4175_422be10_to_422le10_avx2 */

/* begin st20_rfc4175_422le10_to_422be10_avx2 */
static uint8_t rfc4175_l2b_shuffle_l0_tbl[16] = {
    0x01, 0x02, 0x03, 0x04, /* 4 bytes from pg0 */
    0x06, 0x07, 0x08, 0x09, /* 4 bytes from pg1 */
    0x0B, 0x0C, 0x0D, 0x0E, /* 4 bytes from pg2 */
    0x04, 0x09, 0x0E, 0x80, /* 5th bytes from pg0,pg1,pg2 */
};

static uint8_t rfc4175_l2b_and_l0_tbl[16] = {
    0xF0, 0x3F, 0x00, 0xFF, /* pg0 */
    0xF0, 0x3F, 0x00, 0xFF, /* pg1 */
    0xF0, 0x3F, 0x00, 0xFF, /* pg2 */
    0x00, 0x03, 0x03, 0x03, /* 5th bytes from pg0,pg1,pg2 */
};

static uint8_t rfc4175_l2b_shuffle_l1_tbl[16] = {
    0x80, 0x01, 0x00, 0x0D, 0x03, /* pg0 */
    0x80, 0x05, 0x04, 0x0E, 0x07, /* pg1 */
    0x80, 0x09, 0x08, 0x0F, 0x0B, /* pg2 */
    0x80,                         /* zeros */
};

static uint8_t rfc4175_l2b_shuffle_r0_tbl[16] = {
    0x00, 0x01, 0x02, 0x03, /* 4 bytes from pg0 */
    0x05, 0x06, 0x07, 0x08, /* 4 bytes from pg1 */
    0x0A, 0x0B, 0x0C, 0x0D, /* 4 bytes from pg2 */
    0x80, 0x00, 0x05, 0x0A, /* 5th bytes from pg0,pg1,pg2 */
};

static uint8_t rfc4175_l2b_and_r0_tbl[16] = {
    0xFF, 0x00, 0xFC, 0x0F, /* pg0 */
    0xFF, 0x00, 0xFC, 0x0F, /* pg1 */
    0xFF, 0x00, 0xFC, 0x0F, /* pg2 */
    0xC0, 0xC0, 0xC0, 0x00, /* 5th bytes from pg0,pg1,pg2 */
};

static uint8_t rfc4175_l2b_shuffle_r1_tbl[16] = {
    0x00, 0x0C, 0x03, 0x02, 0x80, /* pg0 */
    0x04, 0x0D, 0x07, 0x06, 0x80, /* pg1 */
    0x08, 0x0E, 0x0B, 0x0A, 0x80, /* pg2 */
    0x80,                         /* zeros */
};

int st20_rfc4175_422le10_to_422be10_avx2(struct st20_rfc4175_422_10_pg2_le* pg_le,
                                         struct st20_rfc4175_422_10_pg2_be* pg_be,
                                         uint32_t w, uint32_t h) {
  __m128i shuffle_l0 = _mm_loadu_si128((__m128i*)rfc4175_l2b_shuffle_l0_tbl);
  __m128i shuffle_r0 = _mm_loadu_si128((__m128i*)rfc4175_l2b_shuffle_r0_tbl);
  __m128i and_l0 = _mm_loadu_si128((__m128i*)rfc4175_l2b_and_l0_tbl);
  __m128i and_r0 = _mm_loadu_si128((__m128i*)rfc4175_l2b_and_r0_tbl);
  __m128i shuffle_l1 = _mm_loadu_si128((__m128i*)rfc4175_l2b_shuffle_l1_tbl);
  __m128i shuffle_r1 = _mm_loadu_si128((__m128i*)rfc4175_l2b_shuffle_r1_tbl);

  int pg_cnt = w * h / 2;
  int batch = pg_cnt / 3;
  int left = pg_cnt % 3;
  dbg("%s, pg_cnt %d batch %d left %d\n", __func__, pg_cnt, batch, left);
  /* jump the last batch, for the Xmm may access invalid memory in the last byte */
  if (batch != 0 && left == 0) {
    left = 3;
    batch -= 1;
  }

  /* jump the last batch, for the Xmm may access invalid memory in the last byte */
  for (int i = 0; i < batch; i++) {
    __m128i input = _mm_loadu_si128((__m128i*)pg_le);
    __m128i shuffle_l0_result = _mm_shuffle_epi8(input, shuffle_l0);
    __m128i shuffle_r0_result = _mm_shuffle_epi8(input, shuffle_r0);
    __m128i sl_result = _mm_and_si128(_mm_slli_epi32(shuffle_l0_result, 2), and_l0);
    __m128i sr_result = _mm_and_si128(_mm_srli_epi32(shuffle_r0_result, 2), and_r0);
    __m128i sl_result_shuffle = _mm_shuffle_epi8(sl_result, shuffle_l1);
    __m128i sr_result_shuffle = _mm_shuffle_epi8(sr_result, shuffle_r1);
    __m128i result = _mm_or_si128(sl_result_shuffle, sr_result_shuffle);

    _mm_storeu_si128((__m128i*)pg_be, result);

    pg_be += 3;
    pg_le += 3;
  }

  while (left) {
    uint16_t cb, y0, cr, y1;

    cb = pg_le->Cb00 + (pg_le->Cb00_ << 8);
    y0 = pg_le->Y00 + (pg_le->Y00_ << 6);
    cr = pg_le->Cr00 + (pg_le->Cr00_ << 4);
    y1 = pg_le->Y01 + (pg_le->Y01_ << 2);

    pg_be->Cb00 = cb >> 2;
    pg_be->Cb00_ = cb;
    pg_be->Y00 = y0 >> 4;
    pg_be->Y00_ = y0;
    pg_be->Cr00 = cr >> 6;
    pg_be->Cr00_ = cr;
    pg_be->Y01 = y1 >> 8;
    pg_be->Y01_ = y1;

    pg_be++;
    pg_le++;
    left--;
  }

  return 0;
}
/* end st20_rfc4175_422le10_to_422be10_avx2 */
ST_TARGET_CODE_STOP
#endif
