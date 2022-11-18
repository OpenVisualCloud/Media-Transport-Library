/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_avx512.h"

#include "../mt_log.h"
#include "st_main.h"

#ifdef MTL_HAS_AVX512
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
static uint8_t rfc4175be10_to_8_shuffle_tbl_128[16] = {
    1,     0,     2,     1,     3,     2,     4,     3,     /* pg0 */
    1 + 5, 0 + 5, 2 + 5, 1 + 5, 3 + 5, 2 + 5, 4 + 5, 3 + 5, /* pg1 */
};
static uint16_t rfc4175be10_to_8_sllv_tbl_128[8] = {
    0, 2, 4, 6, 0, 2, 4, 6,
};
static uint8_t rfc4175be10_to_8_sllv_shuffle_tbl_128[16] = {
    1, 3, 5, 7, 9, 11, 13, 15, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
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
    0x80, 2, 1, 0x80, 0x80, 6, 5, 0x80, 0x80, 0x80, 9, 8, 0x80, 13, 12, 0x80,
};

static uint32_t srlv1_mask_table_128[4] = {
    2,
    4,
    6,
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

int st20_rfc4175_422be10_to_422le10_avx512_dma(struct mtl_dma_lender_dev* dma,
                                               struct st20_rfc4175_422_10_pg2_be* pg_be,
                                               mtl_iova_t pg_be_iova,
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

  int caches_num = 4;
  int cache_pg_cnt = (256 * 1024) / sizeof(*pg_be); /* pg cnt for each cache */
  int align = caches_num * 3; /* align to simd pg groups and caches_num */
  cache_pg_cnt = cache_pg_cnt / align * align;
  size_t cache_size = cache_pg_cnt * sizeof(*pg_be);
  int soc_id = dma->parent->soc_id;

  struct st20_rfc4175_422_10_pg2_be* be_caches =
      st_rte_zmalloc_socket(cache_size * caches_num, soc_id);
  struct st_cvt_dma_ctx* ctx = st_cvt_dma_ctx_init(2 * caches_num, soc_id, 2);
  if (!be_caches || !ctx) {
    err("%s, alloc cache(%d,%" PRIu64 ") fail, %p\n", __func__, cache_pg_cnt, cache_size,
        be_caches);
    if (be_caches) st_rte_free(be_caches);
    if (ctx) st_cvt_dma_ctx_uinit(ctx);
    return st20_rfc4175_422be10_to_422le10_avx512(pg_be, pg_le, w, h);
  }
  rte_iova_t be_caches_iova = rte_malloc_virt2iova(be_caches);

  /* first with caches batch step */
  int cache_batch = pg_cnt / cache_pg_cnt;
  dbg("%s, pg_cnt %d cache_pg_cnt %d caches_num %d cache_batch %d\n", __func__, pg_cnt,
      cache_pg_cnt, caches_num, cache_batch);
  for (int i = 0; i < cache_batch; i++) {
    struct st20_rfc4175_422_10_pg2_be* be_cache =
        be_caches + (i % caches_num) * cache_pg_cnt;
    dbg("%s, cache batch idx %d\n", __func__, i);

    int max_tran = i + caches_num;
    max_tran = RTE_MIN(max_tran, cache_batch);
    int cur_tran = st_cvt_dma_ctx_get_tran(ctx, 0);
    /* push max be dma */
    while (cur_tran < max_tran) {
      rte_iova_t be_cache_iova = be_caches_iova + (cur_tran % caches_num) * cache_size;
      mt_dma_copy_busy(dma, be_cache_iova, pg_be_iova, cache_size);
      pg_be += cache_pg_cnt;
      pg_be_iova += cache_size;
      st_cvt_dma_ctx_push(ctx, 0);
      cur_tran = st_cvt_dma_ctx_get_tran(ctx, 0);
    }
    mt_dma_submit_busy(dma);

    /* wait until current be dma copy done */
    while (st_cvt_dma_ctx_get_done(ctx, 0) < (i + 1)) {
      uint16_t nb_dq = mt_dma_completed(dma, 1, NULL, NULL);
      if (nb_dq) st_cvt_dma_ctx_pop(ctx);
    }

    struct st20_rfc4175_422_10_pg2_be* be = be_cache;
    int batch = cache_pg_cnt / 3;
    for (int j = 0; j < batch; j++) {
      __m128i input = _mm_maskz_loadu_epi8(k, (__m128i*)be);
      __m128i shuffle_l0_result = _mm_shuffle_epi8(input, shuffle_l0);
      __m128i shuffle_r0_result = _mm_shuffle_epi8(input, shuffle_r0);
      __m128i rl_result = _mm_and_si128(_mm_rol_epi32(shuffle_l0_result, 2), and_l0);
      __m128i rr_result = _mm_and_si128(_mm_ror_epi32(shuffle_r0_result, 2), and_r0);
      __m128i rl_result_shuffle = _mm_shuffle_epi8(rl_result, shuffle_l1);
      __m128i rr_result_shuffle = _mm_shuffle_epi8(rr_result, shuffle_r1);
      __m128i result = _mm_or_si128(rl_result_shuffle, rr_result_shuffle);

      /* store to the first 15 bytes after dest address */
      _mm_mask_storeu_epi8((__m128i*)pg_le, k, result);

      be += 3;
      pg_le += 3;
    }
  }

  pg_cnt = pg_cnt % cache_pg_cnt;
  st_rte_free(be_caches);

  /* remaining simd batch */
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

  /* remaining scalar batch */
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

int st20_rfc4175_422be10_to_yuv422p10le_avx512_dma(
    struct mtl_dma_lender_dev* dma, struct st20_rfc4175_422_10_pg2_be* pg_be,
    mtl_iova_t pg_be_iova, uint16_t* y, uint16_t* b, uint16_t* r, uint32_t w,
    uint32_t h) {
  __m128i shuffle_le_mask = _mm_loadu_si128((__m128i*)b2l_shuffle_mask_table);
  __m128i srlv_le_mask = _mm_loadu_si128((__m128i*)b2l_srlv_mask_table);
  __m128i srlv_and_mask = _mm_loadu_si128((__m128i*)b2l_and_mask_table);
  __m512i permute_mask = _mm512_loadu_si512((__m512i*)b2l_permute_mask_table);
  __mmask16 k = 0x3FF; /* each __m128i with 2 pg group, 10 bytes */
  int pg_cnt = w * h / 2;

  int caches_num = 4;
  int cache_pg_cnt = (256 * 1024) / sizeof(*pg_be); /* pg cnt for each cache */
  int align = caches_num * 32; /* align to simd pg groups and caches_num */
  cache_pg_cnt = cache_pg_cnt / align * align;
  size_t cache_size = cache_pg_cnt * sizeof(*pg_be);
  int soc_id = dma->parent->soc_id;

  struct st20_rfc4175_422_10_pg2_be* be_caches =
      st_rte_zmalloc_socket(cache_size * caches_num, soc_id);
  struct st_cvt_dma_ctx* ctx = st_cvt_dma_ctx_init(2 * caches_num, soc_id, 2);
  if (!be_caches || !ctx) {
    err("%s, alloc cache(%d,%" PRIu64 ") fail, %p\n", __func__, cache_pg_cnt, cache_size,
        be_caches);
    if (be_caches) st_rte_free(be_caches);
    if (ctx) st_cvt_dma_ctx_uinit(ctx);
    return st20_rfc4175_422be10_to_yuv422p10le_avx512(pg_be, y, b, r, w, h);
  }
  rte_iova_t be_caches_iova = rte_malloc_virt2iova(be_caches);

  /* first with caches batch step */
  int cache_batch = pg_cnt / cache_pg_cnt;
  dbg("%s, pg_cnt %d cache_pg_cnt %d caches_num %d cache_batch %d\n", __func__, pg_cnt,
      cache_pg_cnt, caches_num, cache_batch);
  for (int i = 0; i < cache_batch; i++) {
    struct st20_rfc4175_422_10_pg2_be* be_cache =
        be_caches + (i % caches_num) * cache_pg_cnt;
    dbg("%s, cache batch idx %d\n", __func__, i);

    int max_tran = i + caches_num;
    max_tran = RTE_MIN(max_tran, cache_batch);
    int cur_tran = st_cvt_dma_ctx_get_tran(ctx, 0);
    /* push max be dma */
    while (cur_tran < max_tran) {
      rte_iova_t be_cache_iova = be_caches_iova + (cur_tran % caches_num) * cache_size;
      mt_dma_copy_busy(dma, be_cache_iova, pg_be_iova, cache_size);
      pg_be += cache_pg_cnt;
      pg_be_iova += cache_size;
      st_cvt_dma_ctx_push(ctx, 0);
      cur_tran = st_cvt_dma_ctx_get_tran(ctx, 0);
    }
    mt_dma_submit_busy(dma);

    /* wait until current be dma copy done */
    while (st_cvt_dma_ctx_get_done(ctx, 0) < (i + 1)) {
      uint16_t nb_dq = mt_dma_completed(dma, 1, NULL, NULL);
      if (nb_dq) st_cvt_dma_ctx_pop(ctx);
    }

    struct st20_rfc4175_422_10_pg2_be* pg = be_cache;
    int batch = cache_pg_cnt / 32;
    for (int j = 0; j < batch; j++) {
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
    }
  }

  pg_cnt = pg_cnt % cache_pg_cnt;
  st_rte_free(be_caches);

  /* remaining simd batch */
  int batch = pg_cnt / 32;
  /* each m512i batch handle 4 __m512i(16 __m128i), each __m128i with 2 pg group */
  for (int i = 0; i < batch; i++) {
    /* cvt the result to __m128i(2 pg group) */
    __m128i stage_m128i[16];
    for (int j = 0; j < 16; j++) {
      __m128i input = _mm_maskz_loadu_epi8(k, (__m128i*)pg_be);
      __m128i shuffle_le_result = _mm_shuffle_epi8(input, shuffle_le_mask);
      __m128i srlv_le_result = _mm_srlv_epi16(shuffle_le_result, srlv_le_mask);
      stage_m128i[j] = _mm_and_si128(srlv_le_result, srlv_and_mask);
      pg_be += 2;
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
  }
  pg_cnt = pg_cnt % 32;

  batch = pg_cnt / 8;
  /* each __m128i batch handle 4 16 __m128i, each __m128i with 2 pg group */
  for (int i = 0; i < batch; i++) {
    __m128i stage_m128i[4];
    for (int j = 0; j < 4; j++) {
      __m128i input = _mm_maskz_loadu_epi8(k, (__m128i*)pg_be);
      __m128i shuffle_le_result = _mm_shuffle_epi8(input, shuffle_le_mask);
      __m128i srlv_le_result = _mm_srlv_epi16(shuffle_le_result, srlv_le_mask);
      stage_m128i[j] = _mm_and_si128(srlv_le_result, srlv_and_mask);
      pg_be += 2;
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
  }
  pg_cnt = pg_cnt % 8;

  /* remaining scalar batch */
  while (pg_cnt > 0) {
    st20_unpack_pg2be_422le10(pg_be, b, y, r, y + 1);
    b++;
    r++;
    y += 2;
    pg_be++;

    pg_cnt--;
  }

  return 0;
}

int st20_rfc4175_422be10_to_422le8_avx512(struct st20_rfc4175_422_10_pg2_be* pg_10,
                                          struct st20_rfc4175_422_8_pg2_le* pg_8,
                                          uint32_t w, uint32_t h) {
  __m128i shuffle_mask = _mm_loadu_si128((__m128i*)rfc4175be10_to_8_shuffle_tbl_128);
  __m128i sllv_mask = _mm_loadu_si128((__m128i*)rfc4175be10_to_8_sllv_tbl_128);
  __m128i sllv_shuffle_mask =
      _mm_loadu_si128((__m128i*)rfc4175be10_to_8_sllv_shuffle_tbl_128);
  __mmask16 k = 0x3FF; /* each __m128i with 2 pg group, 10 bytes */
  int pg_cnt = w * h / 2;
  dbg("%s, pg_cnt %d\n", __func__, pg_cnt);

  while (pg_cnt >= 2) {
    __m128i input = _mm_maskz_loadu_epi8(k, (__m128i*)pg_10);
    __m128i shuffle_result = _mm_shuffle_epi8(input, shuffle_mask);
    __m128i sllv_result = _mm_sllv_epi16(shuffle_result, sllv_mask);
    __m128i result = _mm_shuffle_epi8(sllv_result, sllv_shuffle_mask);

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

int st20_rfc4175_422be10_to_422le8_avx512_dma(struct mtl_dma_lender_dev* dma,
                                              struct st20_rfc4175_422_10_pg2_be* pg_10,
                                              mtl_iova_t pg_10_iova,
                                              struct st20_rfc4175_422_8_pg2_le* pg_8,
                                              uint32_t w, uint32_t h) {
  __m128i shuffle_mask = _mm_loadu_si128((__m128i*)rfc4175be10_to_8_shuffle_tbl_128);
  __m128i sllv_mask = _mm_loadu_si128((__m128i*)rfc4175be10_to_8_sllv_tbl_128);
  __m128i sllv_shuffle_mask =
      _mm_loadu_si128((__m128i*)rfc4175be10_to_8_sllv_shuffle_tbl_128);
  __mmask16 k = 0x3FF; /* each __m128i with 2 pg group, 10 bytes */
  int pg_cnt = w * h / 2;
  dbg("%s, pg_cnt %d\n", __func__, pg_cnt);

  int caches_num = 4;
  int cache_pg_cnt = (256 * 1024) / sizeof(*pg_10); /* pg cnt for each cache */
  int align = caches_num * 2; /* align to simd pg groups and caches_num */
  cache_pg_cnt = cache_pg_cnt / align * align;
  size_t cache_size = cache_pg_cnt * sizeof(*pg_10);
  int soc_id = dma->parent->soc_id;

  struct st20_rfc4175_422_10_pg2_be* be10_caches =
      st_rte_zmalloc_socket(cache_size * caches_num, soc_id);
  /* two type be(0) or le(1) */
  struct st_cvt_dma_ctx* ctx = st_cvt_dma_ctx_init(2 * caches_num, soc_id, 2);
  if (!be10_caches || !ctx) {
    err("%s, alloc cache(%d,%" PRIu64 ") fail, %p\n", __func__, cache_pg_cnt, cache_size,
        be10_caches);
    if (be10_caches) st_rte_free(be10_caches);
    if (ctx) st_cvt_dma_ctx_uinit(ctx);
    return st20_rfc4175_422be10_to_422le8_avx512(pg_10, pg_8, w, h);
  }
  rte_iova_t be10_caches_iova = rte_malloc_virt2iova(be10_caches);

  /* first with caches batch step */
  int cache_batch = pg_cnt / cache_pg_cnt;
  dbg("%s, pg_cnt %d cache_pg_cnt %d caches_num %d cache_batch %d\n", __func__, pg_cnt,
      cache_pg_cnt, caches_num, cache_batch);
  for (int i = 0; i < cache_batch; i++) {
    struct st20_rfc4175_422_10_pg2_be* be10_cache =
        be10_caches + (i % caches_num) * cache_pg_cnt;
    dbg("%s, cache batch idx %d\n", __func__, i);

    int max_tran = i + caches_num;
    max_tran = RTE_MIN(max_tran, cache_batch);
    int cur_tran = st_cvt_dma_ctx_get_tran(ctx, 0);
    /* push max be dma */
    while (cur_tran < max_tran) {
      rte_iova_t be10_cache_iova =
          be10_caches_iova + (cur_tran % caches_num) * cache_size;
      mt_dma_copy_busy(dma, be10_cache_iova, pg_10_iova, cache_size);
      pg_10 += cache_pg_cnt;
      pg_10_iova += cache_size;
      st_cvt_dma_ctx_push(ctx, 0);
      cur_tran = st_cvt_dma_ctx_get_tran(ctx, 0);
    }
    mt_dma_submit_busy(dma);

    /* wait until current be dma copy done */
    while (st_cvt_dma_ctx_get_done(ctx, 0) < (i + 1)) {
      uint16_t nb_dq = mt_dma_completed(dma, 1, NULL, NULL);
      if (nb_dq) st_cvt_dma_ctx_pop(ctx);
    }
    struct st20_rfc4175_422_10_pg2_be* be_10 = be10_cache;
    int batch = cache_pg_cnt / 2;
    for (int j = 0; j < batch; j++) {
      __m128i input = _mm_maskz_loadu_epi8(k, (__m128i*)be_10);
      __m128i shuffle_result = _mm_shuffle_epi8(input, shuffle_mask);
      __m128i sllv_result = _mm_sllv_epi16(shuffle_result, sllv_mask);
      __m128i result = _mm_shuffle_epi8(sllv_result, sllv_shuffle_mask);

      _mm_storel_epi64((__m128i*)pg_8, result);

      be_10 += 2;
      pg_8 += 2;
    }
  }

  pg_cnt = pg_cnt % cache_pg_cnt;
  st_rte_free(be10_caches);
  st_cvt_dma_ctx_uinit(ctx);

  /* remaining simd batch */
  int batch = pg_cnt / 2;
  for (int i = 0; i < batch; i++) {
    __m128i input = _mm_maskz_loadu_epi8(k, (__m128i*)pg_10);
    __m128i shuffle_result = _mm_shuffle_epi8(input, shuffle_mask);
    __m128i sllv_result = _mm_sllv_epi16(shuffle_result, sllv_mask);
    __m128i result = _mm_shuffle_epi8(sllv_result, sllv_shuffle_mask);

    _mm_storel_epi64((__m128i*)pg_8, result);

    pg_10 += 2;
    pg_8 += 2;
  }

  /* remaining scalar batch */
  int left = pg_cnt % 2;
  while (left > 0) {
    pg_8->Cb00 = pg_10->Cb00;
    pg_8->Y00 = (pg_10->Y00 << 2) + (pg_10->Y00_ >> 2);
    pg_8->Cr00 = (pg_10->Cr00 << 4) + (pg_10->Cr00_ >> 2);
    pg_8->Y01 = (pg_10->Y01 << 6) + (pg_10->Y01_ >> 2);

    pg_10++;
    pg_8++;

    left--;
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

int st20_rfc4175_422be10_to_v210_avx512(struct st20_rfc4175_422_10_pg2_be* pg_be,
                                        uint8_t* pg_v210, uint32_t w, uint32_t h) {
  __m128i shuffle0_mask = _mm_loadu_si128((__m128i*)shuffle0_mask_table_128);
  __m128i sllv0_mask = _mm_loadu_si128((__m128i*)sllv0_mask_table_128);
  __m128i srlv0_mask = _mm_loadu_si128((__m128i*)srlv0_mask_table_128);
  __m128i and0_mask = _mm_loadu_si128((__m128i*)and0_mask_table_128);
  __m128i shuffle1_mask = _mm_loadu_si128((__m128i*)shuffle1_mask_table_128);
  __m128i srlv1_mask = _mm_loadu_si128((__m128i*)srlv1_mask_table_128);
  __m128i and1_mask = _mm_loadu_si128((__m128i*)and1_mask_table_128);

  __mmask16 k_load = 0x7FFF; /* each __m128i with 3 pg group, 15 bytes */

  int pg_cnt = w * h / 2;
  if (pg_cnt % 3 != 0) {
    err("%s, invalid pg_cnt %d, pixel group number must be multiple of 3!\n", __func__,
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
    __m128i srlv1_result = _mm_srlv_epi32(shuffle1_result, srlv1_mask);
    __m128i and1_result = _mm_and_si128(srlv1_result, and1_mask);
    __m128i result = _mm_or_si128(and0_result, and1_result);

    _mm_store_si128((__m128i*)pg_v210, result);

    pg_be += 3;
    pg_v210 += 16;
  }

  return 0;
}

int st20_rfc4175_422be10_to_v210_avx512_dma(struct mtl_dma_lender_dev* dma,
                                            struct st20_rfc4175_422_10_pg2_be* pg_be,
                                            mtl_iova_t pg_be_iova, uint8_t* pg_v210,
                                            uint32_t w, uint32_t h) {
  __m128i shuffle0_mask = _mm_loadu_si128((__m128i*)shuffle0_mask_table_128);
  __m128i sllv0_mask = _mm_loadu_si128((__m128i*)sllv0_mask_table_128);
  __m128i srlv0_mask = _mm_loadu_si128((__m128i*)srlv0_mask_table_128);
  __m128i and0_mask = _mm_loadu_si128((__m128i*)and0_mask_table_128);
  __m128i shuffle1_mask = _mm_loadu_si128((__m128i*)shuffle1_mask_table_128);
  __m128i srlv1_mask = _mm_loadu_si128((__m128i*)srlv1_mask_table_128);
  __m128i and1_mask = _mm_loadu_si128((__m128i*)and1_mask_table_128);

  __mmask16 k_load = 0x7FFF; /* each __m128i with 3 pg group, 15 bytes */

  int pg_cnt = w * h / 2;
  if (pg_cnt % 3 != 0) {
    err("%s, invalid pg_cnt %d, pixel group number must be multiple of 3!\n", __func__,
        pg_cnt);
    return -EINVAL;
  }

  int caches_num = 4;
  int cache_pg_cnt = (256 * 1024) / sizeof(*pg_be); /* pg cnt for each cache */
  int align = caches_num * 3; /* align to simd pg groups and caches_num */
  cache_pg_cnt = cache_pg_cnt / align * align;
  size_t cache_size = cache_pg_cnt * sizeof(*pg_be);
  int soc_id = dma->parent->soc_id;

  struct st20_rfc4175_422_10_pg2_be* be_caches =
      st_rte_zmalloc_socket(cache_size * caches_num, soc_id);
  struct st_cvt_dma_ctx* ctx = st_cvt_dma_ctx_init(2 * caches_num, soc_id, 2);
  if (!be_caches || !ctx) {
    err("%s, alloc cache(%d,%" PRIu64 ") fail, %p\n", __func__, cache_pg_cnt, cache_size,
        be_caches);
    if (be_caches) st_rte_free(be_caches);
    if (ctx) st_cvt_dma_ctx_uinit(ctx);
    return st20_rfc4175_422be10_to_v210_avx512(pg_be, pg_v210, w, h);
  }
  rte_iova_t be_caches_iova = rte_malloc_virt2iova(be_caches);

  /* first with caches batch step */
  int cache_batch = pg_cnt / cache_pg_cnt;
  dbg("%s, pg_cnt %d cache_pg_cnt %d caches_num %d cache_batch %d\n", __func__, pg_cnt,
      cache_pg_cnt, caches_num, cache_batch);
  for (int i = 0; i < cache_batch; i++) {
    struct st20_rfc4175_422_10_pg2_be* be_cache =
        be_caches + (i % caches_num) * cache_pg_cnt;
    dbg("%s, cache batch idx %d\n", __func__, i);

    int max_tran = i + caches_num;
    max_tran = RTE_MIN(max_tran, cache_batch);
    int cur_tran = st_cvt_dma_ctx_get_tran(ctx, 0);
    /* push max be dma */
    while (cur_tran < max_tran) {
      rte_iova_t be_cache_iova = be_caches_iova + (cur_tran % caches_num) * cache_size;
      mt_dma_copy_busy(dma, be_cache_iova, pg_be_iova, cache_size);
      pg_be += cache_pg_cnt;
      pg_be_iova += cache_size;
      st_cvt_dma_ctx_push(ctx, 0);
      cur_tran = st_cvt_dma_ctx_get_tran(ctx, 0);
    }
    mt_dma_submit_busy(dma);

    /* wait until current be dma copy done */
    while (st_cvt_dma_ctx_get_done(ctx, 0) < (i + 1)) {
      uint16_t nb_dq = mt_dma_completed(dma, 1, NULL, NULL);
      if (nb_dq) st_cvt_dma_ctx_pop(ctx);
    }

    struct st20_rfc4175_422_10_pg2_be* be = be_cache;
    int batch = cache_pg_cnt / 3;
    for (int j = 0; j < batch; j++) {
      __m128i input = _mm_maskz_loadu_epi8(k_load, (__m128i*)be);
      __m128i shuffle0_result = _mm_shuffle_epi8(input, shuffle0_mask);
      __m128i sllv0_result = _mm_sllv_epi16(shuffle0_result, sllv0_mask);
      __m128i srlv0_result = _mm_srlv_epi16(sllv0_result, srlv0_mask);
      __m128i and0_result = _mm_and_si128(srlv0_result, and0_mask);
      __m128i shuffle1_result = _mm_shuffle_epi8(input, shuffle1_mask);
      __m128i srlv1_result = _mm_srlv_epi32(shuffle1_result, srlv1_mask);
      __m128i and1_result = _mm_and_si128(srlv1_result, and1_mask);
      __m128i result = _mm_or_si128(and0_result, and1_result);

      _mm_store_si128((__m128i*)pg_v210, result);

      be += 3;
      pg_v210 += 16;
    }
  }

  pg_cnt = pg_cnt % cache_pg_cnt;
  st_rte_free(be_caches);

  /* remaining simd batch */
  int batch = pg_cnt / 3;
  for (int i = 0; i < batch; i++) {
    __m128i input = _mm_maskz_loadu_epi8(k_load, (__m128i*)pg_be);
    __m128i shuffle0_result = _mm_shuffle_epi8(input, shuffle0_mask);
    __m128i sllv0_result = _mm_sllv_epi16(shuffle0_result, sllv0_mask);
    __m128i srlv0_result = _mm_srlv_epi16(sllv0_result, srlv0_mask);
    __m128i and0_result = _mm_and_si128(srlv0_result, and0_mask);
    __m128i shuffle1_result = _mm_shuffle_epi8(input, shuffle1_mask);
    __m128i srlv1_result = _mm_srlv_epi32(shuffle1_result, srlv1_mask);
    __m128i and1_result = _mm_and_si128(srlv1_result, and1_mask);
    __m128i result = _mm_or_si128(and0_result, and1_result);

    _mm_store_si128((__m128i*)pg_v210, result);

    pg_be += 3;
    pg_v210 += 16;
  }

  return 0;
}

/* b0, r0, y0, y1, b1, r1, y2, y3 */
static uint16_t l2b_sllv_mask_table[8] = {
    0x0006, 0x0002, 0x0004, 0x0000, 0x0006, 0x0002, 0x0004, 0x0000,
};

static uint8_t l2b_shuffle_hi_mask_table[16] = {
    1,    0,    4,    2,    6,  /* pg0 */
    9,    8,    12,   10,   14, /* pg1*/
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
};

static uint8_t l2b_shuffle_lo_mask_table[16] = {
    0x80, 5,    3,    7,    0x80, /* pg0 */
    0x80, 13,   11,   15,   0x80, /* pg1 */
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
};

int st20_yuv422p10le_to_rfc4175_422be10_avx512(uint16_t* y, uint16_t* b, uint16_t* r,
                                               struct st20_rfc4175_422_10_pg2_be* pg,
                                               uint32_t w, uint32_t h) {
  uint32_t pg_cnt = w * h / 2; /* two pgs in one convert */
  uint16_t cb, y0, cr, y1;
  __m128i sllv_le_mask = _mm_loadu_si128((__m128i*)l2b_sllv_mask_table);
  __m128i shuffle_hi_mask = _mm_loadu_si128((__m128i*)l2b_shuffle_hi_mask_table);
  __m128i shuffle_lo_mask = _mm_loadu_si128((__m128i*)l2b_shuffle_lo_mask_table);
  __mmask16 k = 0x3FF; /* each __m128i with 2 pg group, 10 bytes */

  /* each __m128i batch handle 4 __m128i, each __m128i with 2 pg group */
  while (pg_cnt >= 8) {
    __m128i src_y0 = _mm_loadu_si128((__m128i*)y); /* y0-y7 */
    y += 8;
    __m128i src_y8 = _mm_loadu_si128((__m128i*)y); /* y8-y15 */
    y += 8;
    __m128i src_b = _mm_loadu_si128((__m128i*)b); /* b0-b7 */
    b += 8;
    __m128i src_r = _mm_loadu_si128((__m128i*)r); /* r0-r7 */
    r += 8;

    __m128i src_br_lo =
        _mm_unpacklo_epi16(src_b, src_r); /* b0, r0, b1, r1, b2, r2, b3, r3 */
    __m128i src_br_hi =
        _mm_unpackhi_epi16(src_b, src_r); /* b4, r4, b5, r5, b6, r6, b7, r7 */

    __m128i src[4];
    /* b0, r0, y0, y1, b1, r1, y2, y3 */
    src[0] = _mm_unpacklo_epi32(src_br_lo, src_y0);
    /* b2, r2, y4, y5, b3, r3, y6, y7 */
    src[1] = _mm_unpackhi_epi32(src_br_lo, src_y0);
    src[2] = _mm_unpacklo_epi32(src_br_hi, src_y8);
    src[3] = _mm_unpackhi_epi32(src_br_hi, src_y8);

    for (int j = 0; j < 4; j++) {
      /* convert to PGs in __m128i */
      __m128i srlv_le_result = _mm_sllv_epi16(src[j], sllv_le_mask);
      __m128i shuffle_hi_result = _mm_shuffle_epi8(srlv_le_result, shuffle_hi_mask);
      __m128i shuffle_lo_result = _mm_shuffle_epi8(srlv_le_result, shuffle_lo_mask);
      __m128i result = _mm_or_si128(shuffle_hi_result, shuffle_lo_result);
      _mm_mask_storeu_epi8(pg, k, result);
      pg += 2;
    }

    pg_cnt -= 8;
  }

  dbg("%s, remaining pg_cnt %d\n", __func__, pg_cnt);
  while (pg_cnt > 0) {
    cb = *b++;
    y0 = *y++;
    cr = *r++;
    y1 = *y++;

    pg->Cb00 = cb >> 2;
    pg->Cb00_ = cb;
    pg->Y00 = y0 >> 4;
    pg->Y00_ = y0;
    pg->Cr00 = cr >> 6;
    pg->Cr00_ = cr;
    pg->Y01 = y1 >> 8;
    pg->Y01_ = y1;
    pg++;

    pg_cnt--;
  }

  return 0;
}

int st20_yuv422p10le_to_rfc4175_422be10_avx512_dma(struct mtl_dma_lender_dev* dma,
                                                   uint16_t* y, mtl_iova_t y_iova,
                                                   uint16_t* b, mtl_iova_t b_iova,
                                                   uint16_t* r, mtl_iova_t r_iova,
                                                   struct st20_rfc4175_422_10_pg2_be* pg,
                                                   uint32_t w, uint32_t h) {
  uint32_t pg_cnt = w * h / 2; /* two pgs in one convert */
  uint16_t cb, y0, cr, y1;
  __m128i sllv_le_mask = _mm_loadu_si128((__m128i*)l2b_sllv_mask_table);
  __m128i shuffle_hi_mask = _mm_loadu_si128((__m128i*)l2b_shuffle_hi_mask_table);
  __m128i shuffle_lo_mask = _mm_loadu_si128((__m128i*)l2b_shuffle_lo_mask_table);
  __mmask16 k = 0x3FF; /* each __m128i with 2 pg group, 10 bytes */

  int caches_num = 4;
  int le_size_per_pg = (2 + 1 + 1) * sizeof(uint16_t); /* 2y 1b 1r per pg */
  int cache_pg_cnt = (256 * 1024) / le_size_per_pg;    /* pg cnt for each cache */
  int align = caches_num * 8;                          /* align to caches_num and simd */
  cache_pg_cnt = cache_pg_cnt / align * align;
  size_t cache_size = cache_pg_cnt * le_size_per_pg;
  int soc_id = dma->parent->soc_id;

  uint16_t* le_caches = st_rte_zmalloc_socket(cache_size * caches_num, soc_id);
  struct st_cvt_dma_ctx* ctx = st_cvt_dma_ctx_init(3 * caches_num, soc_id, 3);
  if (!le_caches || !ctx) {
    err("%s, alloc cache(%d,%" PRIu64 ") fail, %p\n", __func__, cache_pg_cnt, cache_size,
        le_caches);
    if (le_caches) st_rte_free(le_caches);
    if (ctx) st_cvt_dma_ctx_uinit(ctx);
    return st20_yuv422p10le_to_rfc4175_422be10_avx512(y, b, r, pg, w, h);
  }
  rte_iova_t le_caches_iova = rte_malloc_virt2iova(le_caches);

  /* first with caches batch step */
  int cache_batch = pg_cnt / cache_pg_cnt;
  dbg("%s, pg_cnt %d cache_pg_cnt %d caches_num %d cache_batch %d\n", __func__, pg_cnt,
      cache_pg_cnt, caches_num, cache_batch);
  for (int i = 0; i < cache_batch; i++) {
    uint16_t* le_cache = le_caches + (i % caches_num) * cache_size / sizeof(*le_cache);
    dbg("%s, cache batch idx %d le_cache %p\n", __func__, i, le_cache);

    int max_tran = i + caches_num;
    max_tran = RTE_MIN(max_tran, cache_batch);
    int cur_tran = st_cvt_dma_ctx_get_tran(ctx, 2);
    /* push max be dma */
    while (cur_tran < max_tran) {
      rte_iova_t cache_iova = le_caches_iova + (cur_tran % caches_num) * cache_size;

      mt_dma_copy_busy(dma, cache_iova, y_iova, cache_size / 2);
      y += (cache_pg_cnt * 2); /* two y in one pg */
      y_iova += cache_size / 2;
      st_cvt_dma_ctx_push(ctx, 0);
      cache_iova += cache_size / 2;

      mt_dma_copy_busy(dma, cache_iova, b_iova, cache_size / 4);
      b += cache_pg_cnt;
      b_iova += cache_size / 4;
      st_cvt_dma_ctx_push(ctx, 1);
      cache_iova += cache_size / 4;

      mt_dma_copy_busy(dma, cache_iova, r_iova, cache_size / 4);
      r += cache_pg_cnt;
      r_iova += cache_size / 4;
      st_cvt_dma_ctx_push(ctx, 2);

      cur_tran = st_cvt_dma_ctx_get_tran(ctx, 2);
    }
    mt_dma_submit_busy(dma);

    /* wait until current be dma copy done */
    while (st_cvt_dma_ctx_get_done(ctx, 2) < (i + 1)) {
      uint16_t nb_dq = mt_dma_completed(dma, 1, NULL, NULL);
      if (nb_dq) st_cvt_dma_ctx_pop(ctx);
    }

    int batch = cache_pg_cnt / 8;
    uint16_t* y_cache = le_cache;
    uint16_t* b_cache = y_cache + cache_size / 2 / sizeof(uint16_t);
    uint16_t* r_cache = b_cache + cache_size / 4 / sizeof(uint16_t);
    dbg("%s, cache batch idx %d cache y %p b %p r %p\n", __func__, i, y_cache, b_cache,
        r_cache);
    for (int j = 0; j < batch; j++) {
      __m128i src_y0 = _mm_loadu_si128((__m128i*)y_cache); /* y0-y7 */
      y_cache += 8;
      __m128i src_y8 = _mm_loadu_si128((__m128i*)y_cache); /* y8-y15 */
      y_cache += 8;
      __m128i src_b = _mm_loadu_si128((__m128i*)b_cache); /* b0-b7 */
      b_cache += 8;
      __m128i src_r = _mm_loadu_si128((__m128i*)r_cache); /* r0-r7 */
      r_cache += 8;

      __m128i src_br_lo =
          _mm_unpacklo_epi16(src_b, src_r); /* b0, r0, b1, r1, b2, r2, b3, r3 */
      __m128i src_br_hi =
          _mm_unpackhi_epi16(src_b, src_r); /* b4, r4, b5, r5, b6, r6, b7, r7 */

      __m128i src[4];
      /* b0, r0, y0, y1, b1, r1, y2, y3 */
      src[0] = _mm_unpacklo_epi32(src_br_lo, src_y0);
      /* b2, r2, y4, y5, b3, r3, y6, y7 */
      src[1] = _mm_unpackhi_epi32(src_br_lo, src_y0);
      src[2] = _mm_unpacklo_epi32(src_br_hi, src_y8);
      src[3] = _mm_unpackhi_epi32(src_br_hi, src_y8);

      for (int j = 0; j < 4; j++) {
        /* convert to PGs in __m128i */
        __m128i srlv_le_result = _mm_sllv_epi16(src[j], sllv_le_mask);
        __m128i shuffle_hi_result = _mm_shuffle_epi8(srlv_le_result, shuffle_hi_mask);
        __m128i shuffle_lo_result = _mm_shuffle_epi8(srlv_le_result, shuffle_lo_mask);
        __m128i result = _mm_or_si128(shuffle_hi_result, shuffle_lo_result);
        _mm_mask_storeu_epi8(pg, k, result);
        pg += 2;
      }
    }
  }

  pg_cnt = pg_cnt % cache_pg_cnt;
  st_rte_free(le_caches);

  /* each __m128i batch handle 4 __m128i, each __m128i with 2 pg group */
  while (pg_cnt >= 8) {
    __m128i src_y0 = _mm_loadu_si128((__m128i*)y); /* y0-y7 */
    y += 8;
    __m128i src_y8 = _mm_loadu_si128((__m128i*)y); /* y8-y15 */
    y += 8;
    __m128i src_b = _mm_loadu_si128((__m128i*)b); /* b0-b7 */
    b += 8;
    __m128i src_r = _mm_loadu_si128((__m128i*)r); /* r0-r7 */
    r += 8;

    __m128i src_br_lo =
        _mm_unpacklo_epi16(src_b, src_r); /* b0, r0, b1, r1, b2, r2, b3, r3 */
    __m128i src_br_hi =
        _mm_unpackhi_epi16(src_b, src_r); /* b4, r4, b5, r5, b6, r6, b7, r7 */

    __m128i src[4];
    /* b0, r0, y0, y1, b1, r1, y2, y3 */
    src[0] = _mm_unpacklo_epi32(src_br_lo, src_y0);
    /* b2, r2, y4, y5, b3, r3, y6, y7 */
    src[1] = _mm_unpackhi_epi32(src_br_lo, src_y0);
    src[2] = _mm_unpacklo_epi32(src_br_hi, src_y8);
    src[3] = _mm_unpackhi_epi32(src_br_hi, src_y8);

    for (int j = 0; j < 4; j++) {
      /* convert to PGs in __m128i */
      __m128i srlv_le_result = _mm_sllv_epi16(src[j], sllv_le_mask);
      __m128i shuffle_hi_result = _mm_shuffle_epi8(srlv_le_result, shuffle_hi_mask);
      __m128i shuffle_lo_result = _mm_shuffle_epi8(srlv_le_result, shuffle_lo_mask);
      __m128i result = _mm_or_si128(shuffle_hi_result, shuffle_lo_result);
      _mm_mask_storeu_epi8(pg, k, result);
      pg += 2;
    }

    pg_cnt -= 8;
  }

  dbg("%s, remaining pg_cnt %d\n", __func__, pg_cnt);
  while (pg_cnt > 0) {
    cb = *b++;
    y0 = *y++;
    cr = *r++;
    y1 = *y++;

    pg->Cb00 = cb >> 2;
    pg->Cb00_ = cb;
    pg->Y00 = y0 >> 4;
    pg->Y00_ = y0;
    pg->Cr00 = cr >> 6;
    pg->Cr00_ = cr;
    pg->Y01 = y1 >> 8;
    pg->Y01_ = y1;
    pg++;

    pg_cnt--;
  }

  return 0;
}

#if 0
static uint8_t rfc4175_l2b_shuffle_l0_tbl[16] = {
    0x00, 0x01, 0x01, 0x02, 0x04, 0x80, /* pg0 */
    0x05, 0x06, 0x06, 0x07, 0x09, 0x80, /* pg1 */
    0x80, 0x80, 0x80, 0x80,
};

static uint8_t rfc4175_l2b_and_l0_tbl[16] = {
    0xFF, 0x03, 0xFC, 0x0F, 0xC0, 0x00, /* pg0 */
    0xFF, 0x03, 0xFC, 0x0F, 0xC0, 0x00, /* pg1 */
    0x00, 0x00, 0x00, 0x00,
};

static uint16_t rfc4175_l2b_sllv0_tbl[8] = {
    6, 2, 2, /* pg0 */
    6, 2, 2, /* pg1 */
    0, 0,
};

static uint8_t rfc4175_l2b_shuffle_l1_tbl[16] = {
    0x01, 0x03, 0x02, 0x05, 0x80, /* pg0 */
    0x07, 0x09, 0x08, 0x0B, 0x80, /* pg1 */
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
};

static uint8_t rfc4175_l2b_shuffle_r0_tbl[16] = {
    0x80, 0x00, 0x02, 0x03, 0x03, 0x04, /* pg0 */
    0x80, 0x05, 0x07, 0x08, 0x08, 0x09, /* pg1 */
    0x80, 0x80, 0x80, 0x80,
};

static uint8_t rfc4175_l2b_and_r0_tbl[16] = {
    0x00, 0x03, 0xF0, 0x3F, 0xC0, 0xFF, /* pg0 */
    0x00, 0x03, 0xF0, 0x3F, 0xC0, 0xFF, /* pg1 */
    0x00, 0x00, 0x00, 0x00,
};

static uint16_t rfc4175_l2b_slrv0_tbl[8] = {
    2, 2, 6, /* pg0 */
    2, 2, 6, /* pg1 */
    0, 0,
};

static uint8_t rfc4175_l2b_shuffle_r1_tbl[16] = {
    0x80, 0x00, 0x03, 0x02, 0x04, /* pg0 */
    0x80, 0x06, 0x09, 0x08, 0x0A, /* pg1 */
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
};

int st20_rfc4175_422le10_to_422be10_avx512(struct st20_rfc4175_422_10_pg2_le* pg_le,
                                           struct st20_rfc4175_422_10_pg2_be* pg_be,
                                           uint32_t w, uint32_t h) {
  __m128i shuffle_l0 = _mm_loadu_si128((__m128i*)rfc4175_l2b_shuffle_l0_tbl);
  __m128i shuffle_r0 = _mm_loadu_si128((__m128i*)rfc4175_l2b_shuffle_r0_tbl);
  __m128i and_l0 = _mm_loadu_si128((__m128i*)rfc4175_l2b_and_l0_tbl);
  __m128i and_r0 = _mm_loadu_si128((__m128i*)rfc4175_l2b_and_r0_tbl);
  __m128i sllv_l0 = _mm_loadu_si128((__m128i*)rfc4175_l2b_sllv0_tbl);
  __m128i slrv_l0 = _mm_loadu_si128((__m128i*)rfc4175_l2b_slrv0_tbl);
  __m128i shuffle_l1 = _mm_loadu_si128((__m128i*)rfc4175_l2b_shuffle_l1_tbl);
  __m128i shuffle_r1 = _mm_loadu_si128((__m128i*)rfc4175_l2b_shuffle_r1_tbl);
  __mmask16 k = 0x3FF; /* each __m128i with 2 pg group, 10 bytes */

  int pg_cnt = w * h / 2;
  dbg("%s, pg_cnt %d\n", __func__, pg_cnt);
  int batch = pg_cnt / 2;

  for (int i = 0; i < batch; i++) {
    __m128i input = _mm_maskz_loadu_epi8(k, (__m128i*)pg_le);
    __m128i shuffle_l0_result = _mm_shuffle_epi8(input, shuffle_l0);
    __m128i shuffle_r0_result = _mm_shuffle_epi8(input, shuffle_r0);
    __m128i and_l0_result = _mm_and_si128(shuffle_l0_result, and_l0);
    __m128i and_r0_result = _mm_and_si128(shuffle_r0_result, and_r0);
    __m128i sllv_result = _mm_sllv_epi16(and_l0_result, sllv_l0);
    __m128i srlv_result = _mm_srlv_epi16(and_r0_result, slrv_l0);
    __m128i rl_result_shuffle = _mm_shuffle_epi8(sllv_result, shuffle_l1);
    __m128i rr_result_shuffle = _mm_shuffle_epi8(srlv_result, shuffle_r1);
    __m128i result = _mm_or_si128(rl_result_shuffle, rr_result_shuffle);

    _mm_mask_storeu_epi8((__m128i*)pg_be, k, result);

    pg_be += 2;
    pg_le += 2;
  }

  int left = pg_cnt % 2;
  dbg("%s, left %d\n", __func__, left);
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
#endif

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

int st20_rfc4175_422le10_to_422be10_avx512(struct st20_rfc4175_422_10_pg2_le* pg_le,
                                           struct st20_rfc4175_422_10_pg2_be* pg_be,
                                           uint32_t w, uint32_t h) {
  __mmask16 k = 0x7FFF; /* each __m128i with 3 pg group, 15 bytes */
  __m128i shuffle_l0 = _mm_loadu_si128((__m128i*)rfc4175_l2b_shuffle_l0_tbl);
  __m128i shuffle_r0 = _mm_loadu_si128((__m128i*)rfc4175_l2b_shuffle_r0_tbl);
  __m128i and_l0 = _mm_loadu_si128((__m128i*)rfc4175_l2b_and_l0_tbl);
  __m128i and_r0 = _mm_loadu_si128((__m128i*)rfc4175_l2b_and_r0_tbl);
  __m128i shuffle_l1 = _mm_loadu_si128((__m128i*)rfc4175_l2b_shuffle_l1_tbl);
  __m128i shuffle_r1 = _mm_loadu_si128((__m128i*)rfc4175_l2b_shuffle_r1_tbl);

  int pg_cnt = w * h / 2;
  dbg("%s, pg_cnt %d\n", __func__, pg_cnt);
  int batch = pg_cnt / 3;

  for (int i = 0; i < batch; i++) {
    __m128i input = _mm_maskz_loadu_epi8(k, (__m128i*)pg_le);
    __m128i shuffle_l0_result = _mm_shuffle_epi8(input, shuffle_l0);
    __m128i shuffle_r0_result = _mm_shuffle_epi8(input, shuffle_r0);
    __m128i rl_result = _mm_and_si128(_mm_rol_epi32(shuffle_l0_result, 2), and_l0);
    __m128i rr_result = _mm_and_si128(_mm_ror_epi32(shuffle_r0_result, 2), and_r0);
    __m128i rl_result_shuffle = _mm_shuffle_epi8(rl_result, shuffle_l1);
    __m128i rr_result_shuffle = _mm_shuffle_epi8(rr_result, shuffle_r1);
    __m128i result = _mm_or_si128(rl_result_shuffle, rr_result_shuffle);

    /* store to the first 15 bytes after dest address */
    _mm_mask_storeu_epi8((__m128i*)pg_be, k, result);

    pg_be += 3;
    pg_le += 3;
  }

  int left = pg_cnt % 3;
  dbg("%s, left %d\n", __func__, left);
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

int st20_rfc4175_422le10_to_422be10_avx512_dma(struct mtl_dma_lender_dev* dma,
                                               struct st20_rfc4175_422_10_pg2_le* pg_le,
                                               mtl_iova_t pg_le_iova,
                                               struct st20_rfc4175_422_10_pg2_be* pg_be,
                                               uint32_t w, uint32_t h) {
  __m128i shuffle_l0 = _mm_loadu_si128((__m128i*)rfc4175_l2b_shuffle_l0_tbl);
  __m128i shuffle_r0 = _mm_loadu_si128((__m128i*)rfc4175_l2b_shuffle_r0_tbl);
  __m128i and_l0 = _mm_loadu_si128((__m128i*)rfc4175_l2b_and_l0_tbl);
  __m128i and_r0 = _mm_loadu_si128((__m128i*)rfc4175_l2b_and_r0_tbl);
  __m128i shuffle_l1 = _mm_loadu_si128((__m128i*)rfc4175_l2b_shuffle_l1_tbl);
  __m128i shuffle_r1 = _mm_loadu_si128((__m128i*)rfc4175_l2b_shuffle_r1_tbl);
  __mmask16 k = 0x7FFF; /* each __m128i with 3 pg group, 15 bytes */
  int pg_cnt = w * h / 2;

  int caches_num = 4;
  int cache_pg_cnt = (256 * 1024) / sizeof(*pg_le); /* pg cnt for each cache */
  int align = caches_num * 3; /* align to simd pg groups and caches_num */
  cache_pg_cnt = cache_pg_cnt / align * align;
  size_t cache_size = cache_pg_cnt * sizeof(*pg_le);
  int soc_id = dma->parent->soc_id;

  struct st20_rfc4175_422_10_pg2_le* le_caches =
      st_rte_zmalloc_socket(cache_size * caches_num, soc_id);
  struct st_cvt_dma_ctx* ctx = st_cvt_dma_ctx_init(2 * caches_num, soc_id, 2);
  if (!le_caches || !ctx) {
    err("%s, alloc cache(%d,%" PRIu64 ") fail, %p\n", __func__, cache_pg_cnt, cache_size,
        le_caches);
    if (le_caches) st_rte_free(le_caches);
    if (ctx) st_cvt_dma_ctx_uinit(ctx);
    return st20_rfc4175_422le10_to_422be10_avx512(pg_le, pg_be, w, h);
  }
  rte_iova_t le_caches_iova = rte_malloc_virt2iova(le_caches);

  /* first with caches batch step */
  int cache_batch = pg_cnt / cache_pg_cnt;
  dbg("%s, pg_cnt %d cache_pg_cnt %d caches_num %d cache_batch %d\n", __func__, pg_cnt,
      cache_pg_cnt, caches_num, cache_batch);
  for (int i = 0; i < cache_batch; i++) {
    struct st20_rfc4175_422_10_pg2_le* le_cache =
        le_caches + (i % caches_num) * cache_pg_cnt;
    dbg("%s, cache batch idx %d\n", __func__, i);

    int max_tran = i + caches_num;
    max_tran = RTE_MIN(max_tran, cache_batch);
    int cur_tran = st_cvt_dma_ctx_get_tran(ctx, 0);
    /* push max be dma */
    while (cur_tran < max_tran) {
      rte_iova_t le_cache_iova = le_caches_iova + (cur_tran % caches_num) * cache_size;
      mt_dma_copy_busy(dma, le_cache_iova, pg_le_iova, cache_size);
      pg_le += cache_pg_cnt;
      pg_le_iova += cache_size;
      st_cvt_dma_ctx_push(ctx, 0);
      cur_tran = st_cvt_dma_ctx_get_tran(ctx, 0);
    }
    mt_dma_submit_busy(dma);

    /* wait until current be dma copy done */
    while (st_cvt_dma_ctx_get_done(ctx, 0) < (i + 1)) {
      uint16_t nb_dq = mt_dma_completed(dma, 1, NULL, NULL);
      if (nb_dq) st_cvt_dma_ctx_pop(ctx);
    }

    struct st20_rfc4175_422_10_pg2_le* le = le_cache;
    int batch = cache_pg_cnt / 3;
    for (int j = 0; j < batch; j++) {
      __m128i input = _mm_maskz_loadu_epi8(k, (__m128i*)le);
      __m128i shuffle_l0_result = _mm_shuffle_epi8(input, shuffle_l0);
      __m128i shuffle_r0_result = _mm_shuffle_epi8(input, shuffle_r0);
      __m128i rl_result = _mm_and_si128(_mm_rol_epi32(shuffle_l0_result, 2), and_l0);
      __m128i rr_result = _mm_and_si128(_mm_ror_epi32(shuffle_r0_result, 2), and_r0);
      __m128i rl_result_shuffle = _mm_shuffle_epi8(rl_result, shuffle_l1);
      __m128i rr_result_shuffle = _mm_shuffle_epi8(rr_result, shuffle_r1);
      __m128i result = _mm_or_si128(rl_result_shuffle, rr_result_shuffle);

      /* store to the first 15 bytes after dest address */
      _mm_mask_storeu_epi8((__m128i*)pg_be, k, result);

      le += 3;
      pg_be += 3;
    }
  }

  pg_cnt = pg_cnt % cache_pg_cnt;
  st_rte_free(le_caches);

  /* remaining simd batch */
  int batch = pg_cnt / 3;
  for (int i = 0; i < batch; i++) {
    __m128i input = _mm_maskz_loadu_epi8(k, (__m128i*)pg_le);
    __m128i shuffle_l0_result = _mm_shuffle_epi8(input, shuffle_l0);
    __m128i shuffle_r0_result = _mm_shuffle_epi8(input, shuffle_r0);
    __m128i rl_result = _mm_and_si128(_mm_rol_epi32(shuffle_l0_result, 2), and_l0);
    __m128i rr_result = _mm_and_si128(_mm_ror_epi32(shuffle_r0_result, 2), and_r0);
    __m128i rl_result_shuffle = _mm_shuffle_epi8(rl_result, shuffle_l1);
    __m128i rr_result_shuffle = _mm_shuffle_epi8(rr_result, shuffle_r1);
    __m128i result = _mm_or_si128(rl_result_shuffle, rr_result_shuffle);

    /* store to the first 15 bytes after dest address */
    _mm_mask_storeu_epi8((__m128i*)pg_be, k, result);

    pg_be += 3;
    pg_le += 3;
  }

  /* remaining scalar batch */
  int left = pg_cnt % 3;
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

/* begin st20_v210_to_rfc4175_422be10_avx512 */
static uint8_t v210_to_rfc4175be_shuffle_l0_tbl_128[16] = {
    0, 1, 1, 2, 5, 6, 8, 9, 5, 9, 10, 11, 12, 13, 13, 15,
};

static uint16_t v210_to_rfc4175be_sllv_tbl_128[8] = {
    6, 2, 4, 2, 0, 2, 4, 0,
};

static uint8_t v210_to_rfc4175be_shuffle_l1_tbl_128[16] = {
    1, 0, 2, 8, 4, 5, 4, 7, 6, 9, 11, 10, 12, 14, 14, 15,
};

static uint8_t v210_to_rfc4175be_mask_l_tbl_128[16] = {
    0xFF, 0xC0, 0xF0, 0x03, 0x00, 0xFF, 0xC0, 0x0F,
    0xFC, 0x00, 0xFF, 0xC0, 0xF0, 0xFC, 0x00, 0x00,
};

static uint8_t v210_to_rfc4175be_shuffle_r0_tbl_128[16] = {
    1, 2, 2, 3, 4, 5, 7, 6, 9, 10, 12, 13, 14, 13, 14, 15,
};

static uint16_t v210_to_rfc4175be_srlv_tbl_128[8] = {
    6, 2, 0, 0, 2, 4, 0, 4,
};

static uint8_t v210_to_rfc4175be_shuffle_r1_tbl_128[16] = {
    0, 0, 3, 2, 4, 5, 6, 7, 9, 8, 10, 10, 12, 15, 14, 15,
};

static uint8_t v210_to_rfc4175be_mask_r_tbl_128[16] = {
    0x00, 0x3F, 0x0F, 0xFC, 0xFF, 0x00, 0x3F, 0xF0,
    0x03, 0xFF, 0x00, 0x3F, 0x0F, 0x03, 0xFF, 0x00,
};

int st20_v210_to_rfc4175_422be10_avx512(uint8_t* pg_v210,
                                        struct st20_rfc4175_422_10_pg2_be* pg_be,
                                        uint32_t w, uint32_t h) {
  __m128i shuffle_l0 = _mm_loadu_si128((__m128i*)v210_to_rfc4175be_shuffle_l0_tbl_128);
  __m128i sllv = _mm_loadu_si128((__m128i*)v210_to_rfc4175be_sllv_tbl_128);
  __m128i shuffle_l1 = _mm_loadu_si128((__m128i*)v210_to_rfc4175be_shuffle_l1_tbl_128);
  __m128i mask_l = _mm_loadu_si128((__m128i*)v210_to_rfc4175be_mask_l_tbl_128);
  __m128i shuffle_r0 = _mm_loadu_si128((__m128i*)v210_to_rfc4175be_shuffle_r0_tbl_128);
  __m128i srlv = _mm_loadu_si128((__m128i*)v210_to_rfc4175be_srlv_tbl_128);
  __m128i shuffle_r1 = _mm_loadu_si128((__m128i*)v210_to_rfc4175be_shuffle_r1_tbl_128);
  __m128i mask_r = _mm_loadu_si128((__m128i*)v210_to_rfc4175be_mask_r_tbl_128);

  __mmask16 k_store = 0x7FFF; /* each __m128i with 3 pg group, 15 bytes */

  int pg_cnt = w * h / 2;
  if (pg_cnt % 3 != 0) {
    err("%s, invalid pg_cnt %d, pixel group number must be multiple of 3!\n", __func__,
        pg_cnt);
    return -EINVAL;
  }

  int batch = pg_cnt / 3;
  for (int i = 0; i < batch; i++) {
    __m128i input = _mm_loadu_si128((__m128i*)pg_v210);
    __m128i shuffle_l0_result = _mm_shuffle_epi8(input, shuffle_l0);
    __m128i sllv_result = _mm_sllv_epi16(shuffle_l0_result, sllv);
    __m128i shuffle_l1_result = _mm_shuffle_epi8(sllv_result, shuffle_l1);
    __m128i mask_l_result = _mm_and_si128(shuffle_l1_result, mask_l);
    __m128i shuffle_r0_result = _mm_shuffle_epi8(input, shuffle_r0);
    __m128i srlv_result = _mm_srlv_epi16(shuffle_r0_result, srlv);
    __m128i shuffle_r1_result = _mm_shuffle_epi8(srlv_result, shuffle_r1);
    __m128i mask_r_result = _mm_and_si128(shuffle_r1_result, mask_r);
    __m128i result = _mm_or_si128(mask_l_result, mask_r_result);

    _mm_mask_storeu_epi8((__m128i*)pg_be, k_store, result);

    pg_be += 3;
    pg_v210 += 16;
  }

  return 0;
}

int st20_v210_to_rfc4175_422be10_avx512_dma(struct mtl_dma_lender_dev* dma,
                                            uint8_t* pg_v210, mtl_iova_t pg_v210_iova,
                                            struct st20_rfc4175_422_10_pg2_be* pg_be,
                                            uint32_t w, uint32_t h) {
  __m128i shuffle_l0 = _mm_loadu_si128((__m128i*)v210_to_rfc4175be_shuffle_l0_tbl_128);
  __m128i sllv = _mm_loadu_si128((__m128i*)v210_to_rfc4175be_sllv_tbl_128);
  __m128i shuffle_l1 = _mm_loadu_si128((__m128i*)v210_to_rfc4175be_shuffle_l1_tbl_128);
  __m128i mask_l = _mm_loadu_si128((__m128i*)v210_to_rfc4175be_mask_l_tbl_128);
  __m128i shuffle_r0 = _mm_loadu_si128((__m128i*)v210_to_rfc4175be_shuffle_r0_tbl_128);
  __m128i srlv = _mm_loadu_si128((__m128i*)v210_to_rfc4175be_srlv_tbl_128);
  __m128i shuffle_r1 = _mm_loadu_si128((__m128i*)v210_to_rfc4175be_shuffle_r1_tbl_128);
  __m128i mask_r = _mm_loadu_si128((__m128i*)v210_to_rfc4175be_mask_r_tbl_128);

  __mmask16 k_store = 0x7FFF; /* each __m128i with 3 pg group, 15 bytes */

  int pg_cnt = w * h / 2;
  if (pg_cnt % 3 != 0) {
    err("%s, invalid pg_cnt %d, pixel group number must be multiple of 3!\n", __func__,
        pg_cnt);
    return -EINVAL;
  }

  int caches_num = 4;
  int sz_v210_3be = 16;                              /* 16 to 15(3 pgs) */
  int cache_pg_cnt = (256 * 1024) / sz_v210_3be * 3; /* pg cnt for each cache */
  int align = caches_num * 3; /* align to simd pg groups and caches_num */
  cache_pg_cnt = cache_pg_cnt / align * align;
  size_t cache_size = cache_pg_cnt * sz_v210_3be / 3;
  int soc_id = dma->parent->soc_id;

  uint8_t* v210_caches = st_rte_zmalloc_socket(cache_size * caches_num, soc_id);
  struct st_cvt_dma_ctx* ctx = st_cvt_dma_ctx_init(2 * caches_num, soc_id, 2);
  if (!v210_caches || !ctx) {
    err("%s, alloc cache(%d,%" PRIu64 ") fail, %p\n", __func__, cache_pg_cnt, cache_size,
        v210_caches);
    if (v210_caches) st_rte_free(v210_caches);
    if (ctx) st_cvt_dma_ctx_uinit(ctx);
    return st20_v210_to_rfc4175_422be10_avx512(pg_v210, pg_be, w, h);
  }
  rte_iova_t v210_caches_iova = rte_malloc_virt2iova(v210_caches);

  /* first with caches batch step */
  int cache_batch = pg_cnt / cache_pg_cnt;
  dbg("%s, pg_cnt %d cache_pg_cnt %d caches_num %d cache_batch %d\n", __func__, pg_cnt,
      cache_pg_cnt, caches_num, cache_batch);
  for (int i = 0; i < cache_batch; i++) {
    uint8_t* v210_cache = v210_caches + (i % caches_num) * cache_size;
    dbg("%s, cache batch idx %d\n", __func__, i);

    int max_tran = i + caches_num;
    max_tran = RTE_MIN(max_tran, cache_batch);
    int cur_tran = st_cvt_dma_ctx_get_tran(ctx, 0);
    /* push max be dma */
    while (cur_tran < max_tran) {
      rte_iova_t v210_cache_iova =
          v210_caches_iova + (cur_tran % caches_num) * cache_size;
      mt_dma_copy_busy(dma, v210_cache_iova, pg_v210_iova, cache_size);
      pg_v210 += cache_size;
      pg_v210_iova += cache_size;
      st_cvt_dma_ctx_push(ctx, 0);
      cur_tran = st_cvt_dma_ctx_get_tran(ctx, 0);
    }
    mt_dma_submit_busy(dma);

    /* wait until current be dma copy done */
    while (st_cvt_dma_ctx_get_done(ctx, 0) < (i + 1)) {
      uint16_t nb_dq = mt_dma_completed(dma, 1, NULL, NULL);
      if (nb_dq) st_cvt_dma_ctx_pop(ctx);
    }

    uint8_t* v210 = v210_cache;
    int batch = cache_pg_cnt / 3;
    for (int i = 0; i < batch; i++) {
      __m128i input = _mm_loadu_si128((__m128i*)v210);
      __m128i shuffle_l0_result = _mm_shuffle_epi8(input, shuffle_l0);
      __m128i sllv_result = _mm_sllv_epi16(shuffle_l0_result, sllv);
      __m128i shuffle_l1_result = _mm_shuffle_epi8(sllv_result, shuffle_l1);
      __m128i mask_l_result = _mm_and_si128(shuffle_l1_result, mask_l);
      __m128i shuffle_r0_result = _mm_shuffle_epi8(input, shuffle_r0);
      __m128i srlv_result = _mm_srlv_epi16(shuffle_r0_result, srlv);
      __m128i shuffle_r1_result = _mm_shuffle_epi8(srlv_result, shuffle_r1);
      __m128i mask_r_result = _mm_and_si128(shuffle_r1_result, mask_r);
      __m128i result = _mm_or_si128(mask_l_result, mask_r_result);

      _mm_mask_storeu_epi8((__m128i*)pg_be, k_store, result);

      v210 += 16;
      pg_be += 3;
    }
  }

  pg_cnt = pg_cnt % cache_pg_cnt;
  st_rte_free(v210_caches);

  /* remaining simd batch */
  int batch = pg_cnt / 3;
  for (int i = 0; i < batch; i++) {
    __m128i input = _mm_loadu_si128((__m128i*)pg_v210);
    __m128i shuffle_l0_result = _mm_shuffle_epi8(input, shuffle_l0);
    __m128i sllv_result = _mm_sllv_epi16(shuffle_l0_result, sllv);
    __m128i shuffle_l1_result = _mm_shuffle_epi8(sllv_result, shuffle_l1);
    __m128i mask_l_result = _mm_and_si128(shuffle_l1_result, mask_l);
    __m128i shuffle_r0_result = _mm_shuffle_epi8(input, shuffle_r0);
    __m128i srlv_result = _mm_srlv_epi16(shuffle_r0_result, srlv);
    __m128i shuffle_r1_result = _mm_shuffle_epi8(srlv_result, shuffle_r1);
    __m128i mask_r_result = _mm_and_si128(shuffle_r1_result, mask_r);
    __m128i result = _mm_or_si128(mask_l_result, mask_r_result);

    _mm_mask_storeu_epi8((__m128i*)pg_be, k_store, result);

    pg_be += 3;
    pg_v210 += 16;
  }

  return 0;
}
/* end st20_v210_to_rfc4175_422be10_avx512 */

/* begin st20_rfc4175_422be10_to_y210_avx512 */
static uint8_t rfc4175be_to_y210_shuffle_tbl_128[16] = {
    2,     1,     1,     0,     4,     3,     3,     2,     /* pg0 */
    2 + 5, 1 + 5, 1 + 5, 0 + 5, 4 + 5, 3 + 5, 3 + 5, 2 + 5, /* pg1 */
};

static uint16_t rfc4175be_to_y210_sllv_tbl_128[8] = {
    2, 0, 6, 4, 2, 0, 6, 4,
};

static uint16_t rfc4175be_to_y210_and_tbl_128[8] = {
    0xFFC0, 0xFFC0, 0xFFC0, 0xFFC0, 0xFFC0, 0xFFC0, 0xFFC0, 0xFFC0,
};

int st20_rfc4175_422be10_to_y210_avx512(struct st20_rfc4175_422_10_pg2_be* pg_be,
                                        uint16_t* pg_y210, uint32_t w, uint32_t h) {
  __m128i shuffle_mask = _mm_loadu_si128((__m128i*)rfc4175be_to_y210_shuffle_tbl_128);
  __m128i sllv_mask = _mm_loadu_si128((__m128i*)rfc4175be_to_y210_sllv_tbl_128);
  __m128i and_mask = _mm_loadu_si128((__m128i*)rfc4175be_to_y210_and_tbl_128);

  __mmask16 k_load = 0x3FF; /* each __m128i with 2 pg group, 10 bytes */

  int pg_cnt = w * h / 2;

  int batch = pg_cnt / 2;
  for (int i = 0; i < batch; i++) {
    __m128i input = _mm_maskz_loadu_epi8(k_load, (__m128i*)pg_be);
    __m128i shuffle_result = _mm_shuffle_epi8(input, shuffle_mask);
    __m128i sllv_result = _mm_sllv_epi16(shuffle_result, sllv_mask);
    __m128i result = _mm_and_si128(sllv_result, and_mask);

    _mm_storeu_si128((__m128i*)pg_y210, result);

    pg_be += 2;
    pg_y210 += 8;
  }

  int left = pg_cnt % 2;
  while (left > 0) {
    *pg_y210 = (pg_be->Y00 << 10) + (pg_be->Y00_ << 6);
    *(pg_y210 + 1) = (pg_be->Cb00 << 8) + (pg_be->Cb00_ << 6);
    *(pg_y210 + 2) = (pg_be->Y01 << 14) + (pg_be->Y01_ << 6);
    *(pg_y210 + 3) = (pg_be->Cr00 << 12) + (pg_be->Cr00_ << 6);
    pg_be++;
    pg_y210 += 4;

    left--;
  }

  return 0;
}

int st20_rfc4175_422be10_to_y210_avx512_dma(struct mtl_dma_lender_dev* dma,
                                            struct st20_rfc4175_422_10_pg2_be* pg_be,
                                            mtl_iova_t pg_be_iova, uint16_t* pg_y210,
                                            uint32_t w, uint32_t h) {
  __m128i shuffle_mask = _mm_loadu_si128((__m128i*)rfc4175be_to_y210_shuffle_tbl_128);
  __m128i sllv_mask = _mm_loadu_si128((__m128i*)rfc4175be_to_y210_sllv_tbl_128);
  __m128i and_mask = _mm_loadu_si128((__m128i*)rfc4175be_to_y210_and_tbl_128);

  __mmask16 k_load = 0x3FF; /* each __m128i with 2 pg group, 10 bytes */

  int pg_cnt = w * h / 2;
  dbg("%s, pg_cnt %d\n", __func__, pg_cnt);

  int caches_num = 4;
  int cache_pg_cnt = (256 * 1024) / sizeof(*pg_be); /* pg cnt for each cache */
  int align = caches_num * 2; /* align to simd pg groups and caches_num */
  cache_pg_cnt = cache_pg_cnt / align * align;
  size_t cache_size = cache_pg_cnt * sizeof(*pg_be);
  int soc_id = dma->parent->soc_id;

  struct st20_rfc4175_422_10_pg2_be* be_caches =
      st_rte_zmalloc_socket(cache_size * caches_num, soc_id);
  struct st_cvt_dma_ctx* ctx = st_cvt_dma_ctx_init(2 * caches_num, soc_id, 2);
  if (!be_caches || !ctx) {
    err("%s, alloc cache(%d,%" PRIu64 ") fail, %p\n", __func__, cache_pg_cnt, cache_size,
        be_caches);
    if (be_caches) st_rte_free(be_caches);
    if (ctx) st_cvt_dma_ctx_uinit(ctx);
    return st20_rfc4175_422be10_to_y210_avx512(pg_be, pg_y210, w, h);
  }
  rte_iova_t be_caches_iova = rte_malloc_virt2iova(be_caches);

  /* first with caches batch step */
  int cache_batch = pg_cnt / cache_pg_cnt;
  dbg("%s, pg_cnt %d cache_pg_cnt %d caches_num %d cache_batch %d\n", __func__, pg_cnt,
      cache_pg_cnt, caches_num, cache_batch);
  for (int i = 0; i < cache_batch; i++) {
    struct st20_rfc4175_422_10_pg2_be* be_cache =
        be_caches + (i % caches_num) * cache_pg_cnt;
    dbg("%s, cache batch idx %d\n", __func__, i);

    int max_tran = i + caches_num;
    max_tran = RTE_MIN(max_tran, cache_batch);
    int cur_tran = st_cvt_dma_ctx_get_tran(ctx, 0);
    /* push max be dma */
    while (cur_tran < max_tran) {
      rte_iova_t be_cache_iova = be_caches_iova + (cur_tran % caches_num) * cache_size;
      mt_dma_copy_busy(dma, be_cache_iova, pg_be_iova, cache_size);
      pg_be += cache_pg_cnt;
      pg_be_iova += cache_size;
      st_cvt_dma_ctx_push(ctx, 0);
      cur_tran = st_cvt_dma_ctx_get_tran(ctx, 0);
    }
    mt_dma_submit_busy(dma);

    /* wait until current be dma copy done */
    while (st_cvt_dma_ctx_get_done(ctx, 0) < (i + 1)) {
      uint16_t nb_dq = mt_dma_completed(dma, 1, NULL, NULL);
      if (nb_dq) st_cvt_dma_ctx_pop(ctx);
    }
    struct st20_rfc4175_422_10_pg2_be* be = be_cache;
    int batch = cache_pg_cnt / 2;
    for (int j = 0; j < batch; j++) {
      __m128i input = _mm_maskz_loadu_epi8(k_load, (__m128i*)be);
      __m128i shuffle_result = _mm_shuffle_epi8(input, shuffle_mask);
      __m128i sllv_result = _mm_sllv_epi16(shuffle_result, sllv_mask);
      __m128i result = _mm_and_si128(sllv_result, and_mask);

      _mm_storeu_si128((__m128i*)pg_y210, result);

      be += 2;
      pg_y210 += 8;
    }
  }

  pg_cnt = pg_cnt % cache_pg_cnt;
  st_rte_free(be_caches);
  st_cvt_dma_ctx_uinit(ctx);

  /* remaining simd batch */
  int batch = pg_cnt / 2;
  for (int i = 0; i < batch; i++) {
    __m128i input = _mm_maskz_loadu_epi8(k_load, (__m128i*)pg_be);
    __m128i shuffle_result = _mm_shuffle_epi8(input, shuffle_mask);
    __m128i sllv_result = _mm_sllv_epi16(shuffle_result, sllv_mask);
    __m128i result = _mm_and_si128(sllv_result, and_mask);

    _mm_storeu_si128((__m128i*)pg_y210, result);

    pg_be += 2;
    pg_y210 += 8;
  }

  /* remaining scalar batch */
  int left = pg_cnt % 2;
  while (left > 0) {
    *pg_y210 = (pg_be->Y00 << 10) + (pg_be->Y00_ << 6);
    *(pg_y210 + 1) = (pg_be->Cb00 << 8) + (pg_be->Cb00_ << 6);
    *(pg_y210 + 2) = (pg_be->Y01 << 14) + (pg_be->Y01_ << 6);
    *(pg_y210 + 3) = (pg_be->Cr00 << 12) + (pg_be->Cr00_ << 6);
    pg_be++;
    pg_y210 += 4;

    left--;
  }

  return 0;
}
/* end st20_rfc4175_422be10_to_y210_avx512 */

/* begin st20_y210_to_rfc4175_422be10_avx512 */
static uint8_t y210_to_rfc4175be_shuffle0_tbl_128[16] = {
    /*k: 000000111101111b */
    3,     2,     7,     6,     0, /* pg0: cb,cr */
    3 + 8, 2 + 8, 7 + 8, 6 + 8, 0, /* pg1: cb,cr */
    0,     0,     0,     0,     0, 0,
};

static uint8_t y210_to_rfc4175be_shuffle1_tbl_128[16] = {
    /*k: 000001111011110b */
    0, 1,     0,     5,     4,     /* pg0: y0,y1 */
    0, 1 + 8, 0 + 8, 5 + 8, 4 + 8, /* pg1: y0,y1 */
    0, 0,     0,     0,     0,     0,
};

int st20_y210_to_rfc4175_422be10_avx512(uint16_t* pg_y210,
                                        struct st20_rfc4175_422_10_pg2_be* pg_be,
                                        uint32_t w, uint32_t h) {
  __m128i srlv_mask = _mm_loadu_si128(
      (__m128i*)rfc4175be_to_y210_sllv_tbl_128); /* reverse of be to y210 */
  __m128i shuffle0_mask = _mm_loadu_si128((__m128i*)y210_to_rfc4175be_shuffle0_tbl_128);
  __m128i shuffle1_mask = _mm_loadu_si128((__m128i*)y210_to_rfc4175be_shuffle1_tbl_128);

  __mmask16 k_store = 0x3FF; /* each __m128i with 2 pg group, 10 bytes */

  int pg_cnt = w * h / 2;

  int batch = pg_cnt / 2;
  for (int i = 0; i < batch; i++) {
    __m128i input = _mm_loadu_si128((__m128i*)pg_y210);
    __m128i srlv_result = _mm_srlv_epi16(input, srlv_mask);
    __m128i shuffle0_result = _mm_maskz_shuffle_epi8(0x1EF, srlv_result, shuffle0_mask);
    __m128i shuffle1_result = _mm_maskz_shuffle_epi8(0x3DE, srlv_result, shuffle1_mask);
    __m128i result = _mm_or_si128(shuffle0_result, shuffle1_result);

    _mm_mask_storeu_epi8((__m128i*)pg_be, k_store, result);

    pg_be += 2;
    pg_y210 += 8;
  }

  int left = pg_cnt % 2;
  while (left > 0) {
    pg_be->Cb00 = *(pg_y210 + 1) >> 8;
    pg_be->Cb00_ = (*(pg_y210 + 1) >> 6) & 0x3;
    pg_be->Y00 = *pg_y210 >> 10;
    pg_be->Y00_ = (*pg_y210 >> 6) & 0xF;
    pg_be->Cr00 = *(pg_y210 + 3) >> 12;
    pg_be->Cr00_ = (*(pg_y210 + 3) >> 6) & 0x3F;
    pg_be->Y01 = *(pg_y210 + 2) >> 14;
    pg_be->Y01_ = (*(pg_y210 + 2) >> 6) & 0xFF;

    pg_y210 += 4;
    pg_be++;
    left--;
  }

  return 0;
}

int st20_y210_to_rfc4175_422be10_avx512_dma(struct mtl_dma_lender_dev* dma,
                                            uint16_t* pg_y210, mtl_iova_t pg_y210_iova,
                                            struct st20_rfc4175_422_10_pg2_be* pg_be,
                                            uint32_t w, uint32_t h) {
  __m128i srlv_mask = _mm_loadu_si128(
      (__m128i*)rfc4175be_to_y210_sllv_tbl_128); /* reverse of be to y210 */
  __m128i shuffle0_mask = _mm_loadu_si128((__m128i*)y210_to_rfc4175be_shuffle0_tbl_128);
  __m128i shuffle1_mask = _mm_loadu_si128((__m128i*)y210_to_rfc4175be_shuffle1_tbl_128);

  __mmask16 k_store = 0x3FF; /* each __m128i with 2 pg group, 10 bytes */

  int pg_cnt = w * h / 2;
  dbg("%s, pg_cnt %d\n", __func__, pg_cnt);

  int caches_num = 4;
  int cache_pg_cnt = (256 * 1024) / 8; /* pg cnt for each cache */
  int align = caches_num * 2;          /* align to simd pg groups and caches_num */
  cache_pg_cnt = cache_pg_cnt / align * align;
  size_t cache_size = cache_pg_cnt * 8;
  int soc_id = dma->parent->soc_id;

  uint16_t* y210_caches = st_rte_zmalloc_socket(cache_size * caches_num, soc_id);
  struct st_cvt_dma_ctx* ctx = st_cvt_dma_ctx_init(2 * caches_num, soc_id, 2);
  if (!y210_caches || !ctx) {
    err("%s, alloc cache(%d,%" PRIu64 ") fail, %p\n", __func__, cache_pg_cnt, cache_size,
        y210_caches);
    if (y210_caches) st_rte_free(y210_caches);
    if (ctx) st_cvt_dma_ctx_uinit(ctx);
    return st20_y210_to_rfc4175_422be10_avx512(pg_y210, pg_be, w, h);
  }
  rte_iova_t y210_caches_iova = rte_malloc_virt2iova(y210_caches);

  /* first with caches batch step */
  int cache_batch = pg_cnt / cache_pg_cnt;
  dbg("%s, pg_cnt %d cache_pg_cnt %d caches_num %d cache_batch %d\n", __func__, pg_cnt,
      cache_pg_cnt, caches_num, cache_batch);
  for (int i = 0; i < cache_batch; i++) {
    uint16_t* y210_cache = y210_caches + (i % caches_num) * cache_pg_cnt * 4;
    dbg("%s, cache batch idx %d\n", __func__, i);

    int max_tran = i + caches_num;
    max_tran = RTE_MIN(max_tran, cache_batch);
    int cur_tran = st_cvt_dma_ctx_get_tran(ctx, 0);
    /* push max be dma */
    while (cur_tran < max_tran) {
      rte_iova_t y210_cache_iova =
          y210_caches_iova + (cur_tran % caches_num) * cache_size;
      mt_dma_copy_busy(dma, y210_cache_iova, pg_y210_iova, cache_size);
      pg_y210 += 4 * cache_pg_cnt;
      pg_y210_iova += cache_size;
      st_cvt_dma_ctx_push(ctx, 0);
      cur_tran = st_cvt_dma_ctx_get_tran(ctx, 0);
    }
    mt_dma_submit_busy(dma);

    /* wait until current be dma copy done */
    while (st_cvt_dma_ctx_get_done(ctx, 0) < (i + 1)) {
      uint16_t nb_dq = mt_dma_completed(dma, 1, NULL, NULL);
      if (nb_dq) st_cvt_dma_ctx_pop(ctx);
    }
    uint16_t* y210 = y210_cache;
    int batch = cache_pg_cnt / 2;
    for (int j = 0; j < batch; j++) {
      __m128i input = _mm_loadu_si128((__m128i*)y210);
      __m128i srlv_result = _mm_srlv_epi16(input, srlv_mask);
      __m128i shuffle0_result = _mm_maskz_shuffle_epi8(0x1EF, srlv_result, shuffle0_mask);
      __m128i shuffle1_result = _mm_maskz_shuffle_epi8(0x3DE, srlv_result, shuffle1_mask);
      __m128i result = _mm_or_si128(shuffle0_result, shuffle1_result);

      _mm_mask_storeu_epi8((__m128i*)pg_be, k_store, result);

      pg_be += 2;
      y210 += 8;
    }
  }

  pg_cnt = pg_cnt % cache_pg_cnt;
  st_rte_free(y210_caches);
  st_cvt_dma_ctx_uinit(ctx);

  /* remaining simd batch */
  int batch = pg_cnt / 2;
  for (int i = 0; i < batch; i++) {
    __m128i input = _mm_loadu_si128((__m128i*)pg_y210);
    __m128i srlv_result = _mm_srlv_epi16(input, srlv_mask);
    __m128i shuffle0_result = _mm_maskz_shuffle_epi8(0x1EF, srlv_result, shuffle0_mask);
    __m128i shuffle1_result = _mm_maskz_shuffle_epi8(0x3DE, srlv_result, shuffle1_mask);
    __m128i result = _mm_or_si128(shuffle0_result, shuffle1_result);

    _mm_mask_storeu_epi8((__m128i*)pg_be, k_store, result);

    pg_be += 2;
    pg_y210 += 8;
  }

  /* remaining scalar batch */
  int left = pg_cnt % 2;
  while (left > 0) {
    pg_be->Cb00 = *(pg_y210 + 1) >> 8;
    pg_be->Cb00_ = (*(pg_y210 + 1) >> 6) & 0x3;
    pg_be->Y00 = *pg_y210 >> 10;
    pg_be->Y00_ = (*pg_y210 >> 6) & 0xF;
    pg_be->Cr00 = *(pg_y210 + 3) >> 12;
    pg_be->Cr00_ = (*(pg_y210 + 3) >> 6) & 0x3F;
    pg_be->Y01 = *(pg_y210 + 2) >> 14;
    pg_be->Y01_ = (*(pg_y210 + 2) >> 6) & 0xFF;

    pg_y210 += 4;
    pg_be++;
    left--;
  }

  return 0;
}
/* end st20_y210_to_rfc4175_422be10_avx512 */

ST_TARGET_CODE_STOP
#endif
