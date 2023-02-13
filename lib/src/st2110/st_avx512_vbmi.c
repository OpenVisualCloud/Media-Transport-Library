/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st_avx512_vbmi.h"

#include "../mt_log.h"
#include "st_main.h"

#ifdef MTL_HAS_AVX512_VBMI2
MT_TARGET_CODE_START_AVX512_VBMI2
/* begin st20_rfc4175_422be10_to_yuv422p10le_avx512_vbmi */
static uint8_t be10_to_ple_permute_tbl_512[16 * 4] = {
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

static uint16_t be10_to_ple_srlv_tbl_512[8 * 4] = {
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

static uint16_t be10_to_ple_and_tbl_512[8 * 4] = {
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

int st20_rfc4175_422be10_to_yuv422p10le_avx512_vbmi(struct st20_rfc4175_422_10_pg2_be* pg,
                                                    uint16_t* y, uint16_t* b, uint16_t* r,
                                                    uint32_t w, uint32_t h) {
  __m512i permute_le_mask = _mm512_loadu_si512(be10_to_ple_permute_tbl_512);
  __m512i srlv_le_mask = _mm512_loadu_si512(be10_to_ple_srlv_tbl_512);
  __m512i srlv_and_mask = _mm512_loadu_si512(be10_to_ple_and_tbl_512);
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

int st20_rfc4175_422be10_to_yuv422p10le_avx512_vbmi_dma(
    struct mtl_dma_lender_dev* dma, struct st20_rfc4175_422_10_pg2_be* pg_be,
    mtl_iova_t pg_be_iova, uint16_t* y, uint16_t* b, uint16_t* r, uint32_t w,
    uint32_t h) {
  __m512i permute_le_mask = _mm512_loadu_si512(be10_to_ple_permute_tbl_512);
  __m512i srlv_le_mask = _mm512_loadu_si512(be10_to_ple_srlv_tbl_512);
  __m512i srlv_and_mask = _mm512_loadu_si512(be10_to_ple_and_tbl_512);
  __mmask64 k = 0xFFFFFFFFFF; /* each __m512i with 2*4 pg group, 40 bytes */
  int pg_cnt = w * h / 2;

  int caches_num = 4;
  int cache_pg_cnt = (256 * 1024) / sizeof(*pg_be); /* pg cnt for each cache */
  int align = caches_num * 8; /* align to simd pg groups and caches_num */
  cache_pg_cnt = cache_pg_cnt / align * align;
  size_t cache_size = cache_pg_cnt * sizeof(*pg_be);
  int soc_id = dma->parent->soc_id;

  struct st20_rfc4175_422_10_pg2_be* be_caches =
      mt_rte_zmalloc_socket(cache_size * caches_num, soc_id);
  struct mt_cvt_dma_ctx* ctx = mt_cvt_dma_ctx_init(2 * caches_num, soc_id, 2);
  if (!be_caches || !ctx) {
    err("%s, alloc cache(%d,%" PRIu64 ") fail, %p\n", __func__, cache_pg_cnt, cache_size,
        be_caches);
    if (be_caches) mt_rte_free(be_caches);
    if (ctx) mt_cvt_dma_ctx_uinit(ctx);
    return st20_rfc4175_422be10_to_yuv422p10le_avx512_vbmi(pg_be, y, b, r, w, h);
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
    int cur_tran = mt_cvt_dma_ctx_get_tran(ctx, 0);
    /* push max be dma */
    while (cur_tran < max_tran) {
      rte_iova_t be_cache_iova = be_caches_iova + (cur_tran % caches_num) * cache_size;
      mt_dma_copy_busy(dma, be_cache_iova, pg_be_iova, cache_size);
      pg_be += cache_pg_cnt;
      pg_be_iova += cache_size;
      mt_cvt_dma_ctx_push(ctx, 0);
      cur_tran = mt_cvt_dma_ctx_get_tran(ctx, 0);
    }
    mt_dma_submit_busy(dma);

    /* wait until current be dma copy done */
    while (mt_cvt_dma_ctx_get_done(ctx, 0) < (i + 1)) {
      uint16_t nb_dq = mt_dma_completed(dma, 1, NULL, NULL);
      if (nb_dq) mt_cvt_dma_ctx_pop(ctx);
    }

    struct st20_rfc4175_422_10_pg2_be* pg = be_cache;
    int batch = cache_pg_cnt / 8;
    for (int j = 0; j < batch; j++) {
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
    }
  }

  pg_cnt = pg_cnt % cache_pg_cnt;
  mt_cvt_dma_ctx_uinit(ctx);
  mt_rte_free(be_caches);

  /* remaining simd batch */
  int batch = pg_cnt / 8;
  /* each m512i batch handle 4 __m512i(16 __m128i), each __m128i with 2 pg group */
  for (int i = 0; i < batch; i++) {
    __m512i input = _mm512_maskz_loadu_epi8(k, pg_be);
    __m512i permute_le_result = _mm512_permutexvar_epi8(permute_le_mask, input);
    __m512i srlv_le_result = _mm512_srlv_epi16(permute_le_result, srlv_le_mask);
    __m512i stage_m512i = _mm512_and_si512(srlv_le_result, srlv_and_mask);

    pg_be += 8;

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
/* end st20_rfc4175_422be10_to_yuv422p10le_avx512_vbmi */

/* begin st20_rfc4175_422be10_to_422le10_avx512_vbmi */
static uint8_t be10_to_le_permute_l0_tbl_512[64] = {
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

static uint8_t be10_to_le_and_l0_tbl_512[64] = {
    0x00, 0xFF, 0xF0, 0x3F, 0x00, 0xFF, 0xF0, 0x3F, 0x00, 0xFF, 0xF0, 0x3F, 0x00,
    0x03, 0x03, 0x03, 0x00, 0xFF, 0xF0, 0x3F, 0x00, 0xFF, 0xF0, 0x3F, 0x00, 0xFF,
    0xF0, 0x3F, 0x00, 0x03, 0x03, 0x03, 0x00, 0xFF, 0xF0, 0x3F, 0x00, 0xFF, 0xF0,
    0x3F, 0x00, 0xFF, 0xF0, 0x3F, 0x00, 0x03, 0x03, 0x03, 0x00, 0xFF, 0xF0, 0x3F,
    0x00, 0xFF, 0xF0, 0x3F, 0x00, 0xFF, 0xF0, 0x3F, 0x00, 0x03, 0x03, 0x03,
};

static uint8_t be10_to_le_permute_r0_tbl_512[64] = {
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

static uint8_t be10_to_le_and_r0_tbl_512[64] = {
    0xFC, 0x0F, 0xFF, 0x00, 0xFC, 0x0F, 0xFF, 0x00, 0xFC, 0x0F, 0xFF, 0x00, 0xC0,
    0xC0, 0xC0, 0x00, 0xFC, 0x0F, 0xFF, 0x00, 0xFC, 0x0F, 0xFF, 0x00, 0xFC, 0x0F,
    0xFF, 0x00, 0xC0, 0xC0, 0xC0, 0x00, 0xFC, 0x0F, 0xFF, 0x00, 0xFC, 0x0F, 0xFF,
    0x00, 0xFC, 0x0F, 0xFF, 0x00, 0xC0, 0xC0, 0xC0, 0x00, 0xFC, 0x0F, 0xFF, 0x00,
    0xFC, 0x0F, 0xFF, 0x00, 0xFC, 0x0F, 0xFF, 0x00, 0xC0, 0xC0, 0xC0, 0x00,
};

static uint8_t be10_to_le_permute_l1_tbl_512[64] = {
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

static uint8_t be10_to_le_permute_r1_tbl_512[64] = {
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

int st20_rfc4175_422be10_to_422le10_avx512_vbmi(struct st20_rfc4175_422_10_pg2_be* pg_be,
                                                struct st20_rfc4175_422_10_pg2_le* pg_le,
                                                uint32_t w, uint32_t h) {
  __m512i permute_l0 = _mm512_loadu_si512((__m512i*)be10_to_le_permute_l0_tbl_512);
  __m512i permute_r0 = _mm512_loadu_si512((__m512i*)be10_to_le_permute_r0_tbl_512);
  __m512i and_l0 = _mm512_loadu_si512((__m512i*)be10_to_le_and_l0_tbl_512);
  __m512i and_r0 = _mm512_loadu_si512((__m512i*)be10_to_le_and_r0_tbl_512);
  __m512i permute_l1 = _mm512_loadu_si512((__m512i*)be10_to_le_permute_l1_tbl_512);
  __m512i permute_r1 = _mm512_loadu_si512((__m512i*)be10_to_le_permute_r1_tbl_512);
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

int st20_rfc4175_422be10_to_422le10_avx512_vbmi_dma(
    struct mtl_dma_lender_dev* dma, struct st20_rfc4175_422_10_pg2_be* pg_be,
    mtl_iova_t pg_be_iova, struct st20_rfc4175_422_10_pg2_le* pg_le, uint32_t w,
    uint32_t h) {
  __m512i permute_l0 = _mm512_loadu_si512((__m512i*)be10_to_le_permute_l0_tbl_512);
  __m512i permute_r0 = _mm512_loadu_si512((__m512i*)be10_to_le_permute_r0_tbl_512);
  __m512i and_l0 = _mm512_loadu_si512((__m512i*)be10_to_le_and_l0_tbl_512);
  __m512i and_r0 = _mm512_loadu_si512((__m512i*)be10_to_le_and_r0_tbl_512);
  __m512i permute_l1 = _mm512_loadu_si512((__m512i*)be10_to_le_permute_l1_tbl_512);
  __m512i permute_r1 = _mm512_loadu_si512((__m512i*)be10_to_le_permute_r1_tbl_512);
  __mmask16 k = 0x7FFF; /* each __m512i with 12 pg group, 60 bytes */
  int pg_cnt = w * h / 2;

  int caches_num = 4;
  int cache_pg_cnt = (256 * 1024) / sizeof(*pg_be); /* pg cnt for each cache */
  int align = caches_num * 12; /* align to simd pg groups and caches_num */
  cache_pg_cnt = cache_pg_cnt / align * align;
  size_t cache_size = cache_pg_cnt * sizeof(*pg_be);
  int soc_id = dma->parent->soc_id;

  struct st20_rfc4175_422_10_pg2_be* be_caches =
      mt_rte_zmalloc_socket(cache_size * caches_num, soc_id);
  /* two type be(0) or le(1) */
  struct mt_cvt_dma_ctx* ctx = mt_cvt_dma_ctx_init(2 * caches_num, soc_id, 2);
  if (!be_caches || !ctx) {
    err("%s, alloc cache(%d,%" PRIu64 ") fail, %p\n", __func__, cache_pg_cnt, cache_size,
        be_caches);
    if (be_caches) mt_rte_free(be_caches);
    if (ctx) mt_cvt_dma_ctx_uinit(ctx);
    return st20_rfc4175_422be10_to_422le10_avx512_vbmi(pg_be, pg_le, w, h);
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
    int cur_tran = mt_cvt_dma_ctx_get_tran(ctx, 0);
    /* push max be dma */
    while (cur_tran < max_tran) {
      rte_iova_t be_cache_iova = be_caches_iova + (cur_tran % caches_num) * cache_size;
      mt_dma_copy_busy(dma, be_cache_iova, pg_be_iova, cache_size);
      pg_be += cache_pg_cnt;
      pg_be_iova += cache_size;
      mt_cvt_dma_ctx_push(ctx, 0);
      cur_tran = mt_cvt_dma_ctx_get_tran(ctx, 0);
    }
    mt_dma_submit_busy(dma);

    /* wait until current be dma copy done */
    while (mt_cvt_dma_ctx_get_done(ctx, 0) < (i + 1)) {
      uint16_t nb_dq = mt_dma_completed(dma, 1, NULL, NULL);
      if (nb_dq) mt_cvt_dma_ctx_pop(ctx);
    }

    struct st20_rfc4175_422_10_pg2_be* be = be_cache;
    int batch = cache_pg_cnt / 12;
    for (int j = 0; j < batch; j++) {
      __m512i input = _mm512_maskz_loadu_epi32(k, (__m512i*)be);
      __m512i permute_l0_result = _mm512_permutexvar_epi8(permute_l0, input);
      __m512i permute_r0_result = _mm512_permutexvar_epi8(permute_r0, input);
      __m512i rl_result =
          _mm512_and_si512(_mm512_rol_epi32(permute_l0_result, 2), and_l0);
      __m512i rr_result =
          _mm512_and_si512(_mm512_ror_epi32(permute_r0_result, 2), and_r0);
      __m512i rl_result_shuffle = _mm512_permutexvar_epi8(permute_l1, rl_result);
      __m512i rr_result_shuffle = _mm512_permutexvar_epi8(permute_r1, rr_result);
      __m512i result = _mm512_or_si512(rl_result_shuffle, rr_result_shuffle);

      /* store to the first 60 bytes after dest address */
      _mm512_mask_storeu_epi32((__m512i*)pg_le, k, result);

      be += 12;
      pg_le += 12;
    }
  }

  pg_cnt = pg_cnt % cache_pg_cnt;
  mt_rte_free(be_caches);
  mt_cvt_dma_ctx_uinit(ctx);

  /* remaining simd batch */
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

  /* remaining scalar batch */
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
/* end st20_rfc4175_422be10_to_422le10_avx512_vbmi */

/* begin st20_rfc4175_422be10_to_422le8_avx512_vbmi */
static uint8_t be10_to_le8_permute_tbl_512[16 * 4] = {
    0,      4,      3,      2,      1,      /* pg0 - xmm0 */
    0 + 5,  2 + 5,  1 + 5,                  /* pg1 */
    4 + 5,  3 + 5,  2 + 5,                  /* pg1 across 64 bit lane */
    0 + 10, 4 + 10, 3 + 10, 2 + 10, 1 + 10, /* pg2 */
    0 + 15, 4 + 15, 3 + 15, 2 + 15, 1 + 15, /* pg3 - xmm1 */
    0 + 20, 2 + 20, 1 + 20,                 /* pg4 */
    4 + 20, 3 + 20, 2 + 20,                 /* pg4 across 64 bit lane */
    0 + 25, 4 + 25, 3 + 25, 2 + 25, 1 + 25, /* pg5 */
    0 + 30, 4 + 30, 3 + 30, 2 + 30, 1 + 30, /* pg6 - xmm2 */
    0 + 35, 2 + 35, 1 + 35,                 /* pg7 */
    4 + 35, 3 + 35, 2 + 35,                 /* pg7 across 64 bit lane */
    0 + 40, 4 + 40, 3 + 40, 2 + 40, 1 + 40, /* pg8 */
    0 + 45, 4 + 45, 3 + 45, 2 + 45, 1 + 45, /* pg9 - xmm3 */
    0 + 50, 2 + 50, 1 + 50,                 /* pg10 */
    4 + 50, 3 + 50, 2 + 50,                 /* pg10 across 64 bit lane */
    0 + 55, 4 + 55, 3 + 55, 2 + 55, 1 + 55, /* pg11 */
};
static uint8_t be10_to_le8_multishift_tbl_512[16 * 4] = {
    0,  30, 20, 10, /* pg0 */
    0,  0,  40, 54, /* pg1, first half */
    12, 2,  0,  0,  /* pg1, second half */
    24, 54, 44, 34, /* pg2 */
    0,  30, 20, 10, /* pg3 */
    0,  0,  40, 54, /* pg4, first half */
    12, 2,  0,  0,  /* pg4, second half */
    24, 54, 44, 34, /* pg5 */
    0,  30, 20, 10, /* pg6 */
    0,  0,  40, 54, /* pg7, first half */
    12, 2,  0,  0,  /* pg7, second half */
    24, 54, 44, 34, /* pg8 */
    0,  30, 20, 10, /* pg9 */
    0,  0,  40, 54, /* pg10, first half */
    12, 2,  0,  0,  /* pg10, second half */
    24, 54, 44, 34, /* pg11 */
};

int st20_rfc4175_422be10_to_422le8_avx512_vbmi(struct st20_rfc4175_422_10_pg2_be* pg_10,
                                               struct st20_rfc4175_422_8_pg2_le* pg_8,
                                               uint32_t w, uint32_t h) {
  __m512i permute_mask = _mm512_loadu_si512((__m512i*)be10_to_le8_permute_tbl_512);
  __m512i multishift_mask = _mm512_loadu_si512((__m512i*)be10_to_le8_multishift_tbl_512);
  __mmask16 k_load = 0x7FFF; /* each __m512i with 12 pg group, 60 bytes */
  __mmask32 k_compress = 0xDBDBDBDB;
  int pg_cnt = w * h / 2;
  dbg("%s, pg_cnt %d\n", __func__, pg_cnt);

  /* each __m512i batch handle 12 pg groups */
  int batch = pg_cnt / 12;
  for (int i = 0; i < batch; i++) {
    __m512i input = _mm512_maskz_loadu_epi32(k_load, pg_10);
    __m512i permute_result = _mm512_permutexvar_epi8(permute_mask, input);
    __m512i multishift_result =
        _mm512_multishift_epi64_epi8(multishift_mask, permute_result);

    _mm512_mask_compressstoreu_epi16(pg_8, k_compress, multishift_result);

    pg_10 += 12;
    pg_8 += 12;
  }

  int left = pg_cnt % 12;
  while (left) {
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

int st20_rfc4175_422be10_to_422le8_avx512_vbmi_dma(
    struct mtl_dma_lender_dev* dma, struct st20_rfc4175_422_10_pg2_be* pg_10,
    mtl_iova_t pg_10_iova, struct st20_rfc4175_422_8_pg2_le* pg_8, uint32_t w,
    uint32_t h) {
  __m512i permute_mask = _mm512_loadu_si512((__m512i*)be10_to_le8_permute_tbl_512);
  __m512i multishift_mask = _mm512_loadu_si512((__m512i*)be10_to_le8_multishift_tbl_512);
  __mmask16 k_load = 0x7FFF; /* each __m512i with 12 pg group, 60 bytes */
  __mmask32 k_compress = 0xDBDBDBDB;
  int pg_cnt = w * h / 2;
  dbg("%s, pg_cnt %d\n", __func__, pg_cnt);

  int caches_num = 4;
  int cache_pg_cnt = (256 * 1024) / sizeof(*pg_10); /* pg cnt for each cache */
  int align = caches_num * 12; /* align to simd pg groups and caches_num */
  cache_pg_cnt = cache_pg_cnt / align * align;
  size_t cache_size = cache_pg_cnt * sizeof(*pg_10);
  int soc_id = dma->parent->soc_id;

  struct st20_rfc4175_422_10_pg2_be* be10_caches =
      mt_rte_zmalloc_socket(cache_size * caches_num, soc_id);
  /* two type be(0) or le(1) */
  struct mt_cvt_dma_ctx* ctx = mt_cvt_dma_ctx_init(2 * caches_num, soc_id, 2);
  if (!be10_caches || !ctx) {
    err("%s, alloc cache(%d,%" PRIu64 ") fail, %p\n", __func__, cache_pg_cnt, cache_size,
        be10_caches);
    if (be10_caches) mt_rte_free(be10_caches);
    if (ctx) mt_cvt_dma_ctx_uinit(ctx);
    return st20_rfc4175_422be10_to_422le8_avx512_vbmi(pg_10, pg_8, w, h);
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
    int cur_tran = mt_cvt_dma_ctx_get_tran(ctx, 0);
    /* push max be dma */
    while (cur_tran < max_tran) {
      rte_iova_t be10_cache_iova =
          be10_caches_iova + (cur_tran % caches_num) * cache_size;
      mt_dma_copy_busy(dma, be10_cache_iova, pg_10_iova, cache_size);
      pg_10 += cache_pg_cnt;
      pg_10_iova += cache_size;
      mt_cvt_dma_ctx_push(ctx, 0);
      cur_tran = mt_cvt_dma_ctx_get_tran(ctx, 0);
    }
    mt_dma_submit_busy(dma);

    /* wait until current be dma copy done */
    while (mt_cvt_dma_ctx_get_done(ctx, 0) < (i + 1)) {
      uint16_t nb_dq = mt_dma_completed(dma, 1, NULL, NULL);
      if (nb_dq) mt_cvt_dma_ctx_pop(ctx);
    }
    struct st20_rfc4175_422_10_pg2_be* be_10 = be10_cache;
    int batch = cache_pg_cnt / 12;
    for (int j = 0; j < batch; j++) {
      __m512i input = _mm512_maskz_loadu_epi32(k_load, be_10);
      __m512i permute_result = _mm512_permutexvar_epi8(permute_mask, input);
      __m512i multishift_result =
          _mm512_multishift_epi64_epi8(multishift_mask, permute_result);

      _mm512_mask_compressstoreu_epi16(pg_8, k_compress, multishift_result);

      be_10 += 12;
      pg_8 += 12;
    }
  }

  pg_cnt = pg_cnt % cache_pg_cnt;
  mt_rte_free(be10_caches);
  mt_cvt_dma_ctx_uinit(ctx);

  /* remaining simd batch */
  int batch = pg_cnt / 12;
  for (int i = 0; i < batch; i++) {
    __m512i input = _mm512_maskz_loadu_epi32(k_load, pg_10);
    __m512i permute_result = _mm512_permutexvar_epi8(permute_mask, input);
    __m512i multishift_result =
        _mm512_multishift_epi64_epi8(multishift_mask, permute_result);

    _mm512_mask_compressstoreu_epi16(pg_8, k_compress, multishift_result);

    pg_10 += 12;
    pg_8 += 12;
  }

  /* remaining scalar batch */
  int left = pg_cnt % 12;
  while (left) {
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
/* end st20_rfc4175_422be10_to_422le8_avx512_vbmi */

/* begin st20_rfc4175_422le10_to_v210_avx512_vbmi */
static uint8_t le10_to_v210_permute_tbl_512[16 * 4] = {
    0,      1,      2,      3,       4,       5,       6,       7,
    7,      8,      9,      10,      11,      12,      13,      14, /* pg0-2 */
    0 + 15, 1 + 15, 2 + 15, 3 + 15,  4 + 15,  5 + 15,  6 + 15,  7 + 15,
    7 + 15, 8 + 15, 9 + 15, 10 + 15, 11 + 15, 12 + 15, 13 + 15, 14 + 15, /* pg3-5 */
    0 + 30, 1 + 30, 2 + 30, 3 + 30,  4 + 30,  5 + 30,  6 + 30,  7 + 30,
    7 + 30, 8 + 30, 9 + 30, 10 + 30, 11 + 30, 12 + 30, 13 + 30, 14 + 30, /* pg6-8 */
    0 + 45, 1 + 45, 2 + 45, 3 + 45,  4 + 45,  5 + 45,  6 + 45,  7 + 45,
    7 + 45, 8 + 45, 9 + 45, 10 + 45, 11 + 45, 12 + 45, 13 + 45, 14 + 45, /* pg9-11 */
};

static uint8_t le10_to_v210_multishift_tbl_512[16 * 4] = {
    0, 8, 16, 24, 30, 38, 46, 54, 4, 12, 20, 28, 34, 42, 50, 58,
    0, 8, 16, 24, 30, 38, 46, 54, 4, 12, 20, 28, 34, 42, 50, 58,
    0, 8, 16, 24, 30, 38, 46, 54, 4, 12, 20, 28, 34, 42, 50, 58,
    0, 8, 16, 24, 30, 38, 46, 54, 4, 12, 20, 28, 34, 42, 50, 58,
};
static uint8_t le10_to_v210_and_tbl_512[16 * 4] = {
    0xFF, 0xFF, 0xFF, 0x3F, 0xFF, 0xFF, 0xFF, 0x3F, 0xFF, 0xFF, 0xFF, 0x3F, 0xFF,
    0xFF, 0xFF, 0x3F, 0xFF, 0xFF, 0xFF, 0x3F, 0xFF, 0xFF, 0xFF, 0x3F, 0xFF, 0xFF,
    0xFF, 0x3F, 0xFF, 0xFF, 0xFF, 0x3F, 0xFF, 0xFF, 0xFF, 0x3F, 0xFF, 0xFF, 0xFF,
    0x3F, 0xFF, 0xFF, 0xFF, 0x3F, 0xFF, 0xFF, 0xFF, 0x3F, 0xFF, 0xFF, 0xFF, 0x3F,
    0xFF, 0xFF, 0xFF, 0x3F, 0xFF, 0xFF, 0xFF, 0x3F, 0xFF, 0xFF, 0xFF, 0x3F,
};

int st20_rfc4175_422le10_to_v210_avx512_vbmi(uint8_t* pg_le, uint8_t* pg_v210, uint32_t w,
                                             uint32_t h) {
  __m512i permute_mask = _mm512_loadu_si512((__m512i*)le10_to_v210_permute_tbl_512);
  __m512i multishift_mask = _mm512_loadu_si512((__m512i*)le10_to_v210_multishift_tbl_512);
  __m512i padding_mask = _mm512_loadu_si512((__m512i*)le10_to_v210_and_tbl_512);
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
/* end st20_rfc4175_422le10_to_v210_avx512_vbmi */

/* begin st20_rfc4175_422be10_to_v210_avx512_vbmi */
static uint8_t be10_to_v210_permute0_tbl_512[16 * 4] = {
    1,      0,      3,       2,       4,       3,       7,       6,
    8,      7,      11,      10,      12,      11,      14,      13, /* pg 0-2 */
    1 + 15, 0 + 15, 3 + 15,  2 + 15,  4 + 15,  3 + 15,  7 + 15,  6 + 15,
    8 + 15, 7 + 15, 11 + 15, 10 + 15, 12 + 15, 11 + 15, 14 + 15, 13 + 15, /* pg 3-5 */
    1 + 30, 0 + 30, 3 + 30,  2 + 30,  4 + 30,  3 + 30,  7 + 30,  6 + 30,
    8 + 30, 7 + 30, 11 + 30, 10 + 30, 12 + 30, 11 + 30, 14 + 30, 13 + 30, /* pg 6-8 */
    1 + 45, 0 + 45, 3 + 45,  2 + 45,  4 + 45,  3 + 45,  7 + 45,  6 + 45,
    8 + 45, 7 + 45, 11 + 45, 10 + 45, 12 + 45, 11 + 45, 14 + 45, 13 + 45, /* pg 9-11 */
};
static uint8_t be10_to_v210_multishift0_tbl_512[16 * 4] = {
    6, 14, 14, 22, 32, 40, 48, 56, 2, 10, 18, 26, 36, 44, 44, 52, /* pg 0-2 */
    6, 14, 14, 22, 32, 40, 48, 56, 2, 10, 18, 26, 36, 44, 44, 52, /* pg 3-5 */
    6, 14, 14, 22, 32, 40, 48, 56, 2, 10, 18, 26, 36, 44, 44, 52, /* pg 6-8 */
    6, 14, 14, 22, 32, 40, 48, 56, 2, 10, 18, 26, 36, 44, 44, 52, /* pg 9-11 */
};
static uint8_t be10_to_v210_and0_tbl_512[16 * 4] = {
    0xFF, 0x03, 0xF0, 0x3F, 0xFF, 0x03, 0xF0, 0x3F, 0xFF, 0x03, 0xF0, 0x3F, 0xFF,
    0x03, 0xF0, 0x3F, 0xFF, 0x03, 0xF0, 0x3F, 0xFF, 0x03, 0xF0, 0x3F, 0xFF, 0x03,
    0xF0, 0x3F, 0xFF, 0x03, 0xF0, 0x3F, 0xFF, 0x03, 0xF0, 0x3F, 0xFF, 0x03, 0xF0,
    0x3F, 0xFF, 0x03, 0xF0, 0x3F, 0xFF, 0x03, 0xF0, 0x3F, 0xFF, 0x03, 0xF0, 0x3F,
    0xFF, 0x03, 0xF0, 0x3F, 0xFF, 0x03, 0xF0, 0x3F, 0xFF, 0x03, 0xF0, 0x3F,
};
static uint8_t be10_to_v210_permute1_tbl_512[16 * 4] = {
    63, 2,      1,      63, 63, 6,       5,       63,
    63, 9,      8,      63, 63, 13,      12,      63, /* pg 0-2 */
    63, 2 + 15, 1 + 15, 63, 63, 6 + 15,  5 + 15,  63,
    63, 9 + 15, 8 + 15, 63, 63, 13 + 15, 12 + 15, 63, /* pg 3-5 */
    63, 2 + 30, 1 + 30, 63, 63, 6 + 30,  5 + 30,  63,
    63, 9 + 30, 8 + 30, 63, 63, 13 + 30, 12 + 30, 63, /* pg 6-8 */
    63, 2 + 45, 1 + 45, 63, 63, 6 + 45,  5 + 45,  63,
    63, 9 + 45, 8 + 45, 63, 63, 13 + 45, 12 + 45, 63, /* pg 9-11 */
};
static uint8_t be10_to_v210_multishift1_tbl_512[16 * 4] = {
    0, 10, 18, 0, 0, 44, 52, 0, 0, 6, 14, 0, 0, 40, 48, 0, /* pg 0-2 */
    0, 10, 18, 0, 0, 44, 52, 0, 0, 6, 14, 0, 0, 40, 48, 0, /* pg 3-5 */
    0, 10, 18, 0, 0, 44, 52, 0, 0, 6, 14, 0, 0, 40, 48, 0, /* pg 6-8 */
    0, 10, 18, 0, 0, 44, 52, 0, 0, 6, 14, 0, 0, 40, 48, 0, /* pg 9-11 */
};
static uint8_t be10_to_v210_and1_tbl_512[16 * 4] = {
    0x00, 0xFC, 0x0F, 0x00, 0x00, 0xFC, 0x0F, 0x00, 0x00, 0xFC, 0x0F, 0x00, 0x00,
    0xFC, 0x0F, 0x00, 0x00, 0xFC, 0x0F, 0x00, 0x00, 0xFC, 0x0F, 0x00, 0x00, 0xFC,
    0x0F, 0x00, 0x00, 0xFC, 0x0F, 0x00, 0x00, 0xFC, 0x0F, 0x00, 0x00, 0xFC, 0x0F,
    0x00, 0x00, 0xFC, 0x0F, 0x00, 0x00, 0xFC, 0x0F, 0x00, 0x00, 0xFC, 0x0F, 0x00,
    0x00, 0xFC, 0x0F, 0x00, 0x00, 0xFC, 0x0F, 0x00, 0x00, 0xFC, 0x0F, 0x00,
};

int st20_rfc4175_422be10_to_v210_avx512_vbmi(struct st20_rfc4175_422_10_pg2_be* pg_be,
                                             uint8_t* pg_v210, uint32_t w, uint32_t h) {
  __m512i permute0_mask = _mm512_loadu_si512((__m512i*)be10_to_v210_permute0_tbl_512);
  __m512i multishift0_mask =
      _mm512_loadu_si512((__m512i*)be10_to_v210_multishift0_tbl_512);
  __m512i and0_mask = _mm512_loadu_si512((__m512i*)be10_to_v210_and0_tbl_512);
  __m512i permute1_mask = _mm512_loadu_si512((__m512i*)be10_to_v210_permute1_tbl_512);
  __m512i multishift1_mask =
      _mm512_loadu_si512((__m512i*)be10_to_v210_multishift1_tbl_512);
  __m512i and1_mask = _mm512_loadu_si512((__m512i*)be10_to_v210_and1_tbl_512);
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
    __m512i result = _mm512_or_si512(and0_result, and1_result);

    _mm512_storeu_si512((__m512i*)pg_v210, result);

    pg_be += 12;
    pg_v210 += 64;
  }

  return 0;
}

int st20_rfc4175_422be10_to_v210_avx512_vbmi_dma(struct mtl_dma_lender_dev* dma,
                                                 struct st20_rfc4175_422_10_pg2_be* pg_be,
                                                 mtl_iova_t pg_be_iova, uint8_t* pg_v210,
                                                 uint32_t w, uint32_t h) {
  __m512i permute0_mask = _mm512_loadu_si512((__m512i*)be10_to_v210_permute0_tbl_512);
  __m512i multishift0_mask =
      _mm512_loadu_si512((__m512i*)be10_to_v210_multishift0_tbl_512);
  __m512i and0_mask = _mm512_loadu_si512((__m512i*)be10_to_v210_and0_tbl_512);
  __m512i permute1_mask = _mm512_loadu_si512((__m512i*)be10_to_v210_permute1_tbl_512);
  __m512i multishift1_mask =
      _mm512_loadu_si512((__m512i*)be10_to_v210_multishift1_tbl_512);
  __m512i and1_mask = _mm512_loadu_si512((__m512i*)be10_to_v210_and1_tbl_512);
  __mmask16 k = 0x7FFF; /* each __m512i with 12 pg group, 60 bytes */

  int pg_cnt = w * h / 2;
  if (pg_cnt % 12 != 0) {
    err("%s, invalid pg_cnt %d, pixel group number must be multiple of 12!\n", __func__,
        pg_cnt);
    return -EINVAL;
  }

  int caches_num = 4;
  int cache_pg_cnt = (256 * 1024) / sizeof(*pg_be); /* pg cnt for each cache */
  int align = caches_num * 12; /* align to simd pg groups and caches_num */
  cache_pg_cnt = cache_pg_cnt / align * align;
  size_t cache_size = cache_pg_cnt * sizeof(*pg_be);
  int soc_id = dma->parent->soc_id;

  struct st20_rfc4175_422_10_pg2_be* be_caches =
      mt_rte_zmalloc_socket(cache_size * caches_num, soc_id);
  /* two type be(0) or le(1) */
  struct mt_cvt_dma_ctx* ctx = mt_cvt_dma_ctx_init(2 * caches_num, soc_id, 2);
  if (!be_caches || !ctx) {
    err("%s, alloc cache(%d,%" PRIu64 ") fail, %p\n", __func__, cache_pg_cnt, cache_size,
        be_caches);
    if (be_caches) mt_rte_free(be_caches);
    if (ctx) mt_cvt_dma_ctx_uinit(ctx);
    return st20_rfc4175_422be10_to_v210_avx512_vbmi(pg_be, pg_v210, w, h);
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
    int cur_tran = mt_cvt_dma_ctx_get_tran(ctx, 0);
    /* push max be dma */
    while (cur_tran < max_tran) {
      rte_iova_t be_cache_iova = be_caches_iova + (cur_tran % caches_num) * cache_size;
      mt_dma_copy_busy(dma, be_cache_iova, pg_be_iova, cache_size);
      pg_be += cache_pg_cnt;
      pg_be_iova += cache_size;
      mt_cvt_dma_ctx_push(ctx, 0);
      cur_tran = mt_cvt_dma_ctx_get_tran(ctx, 0);
    }
    mt_dma_submit_busy(dma);

    /* wait until current be dma copy done */
    while (mt_cvt_dma_ctx_get_done(ctx, 0) < (i + 1)) {
      uint16_t nb_dq = mt_dma_completed(dma, 1, NULL, NULL);
      if (nb_dq) mt_cvt_dma_ctx_pop(ctx);
    }

    struct st20_rfc4175_422_10_pg2_be* be = be_cache;
    int batch = cache_pg_cnt / 12;
    for (int j = 0; j < batch; j++) {
      __m512i input = _mm512_maskz_loadu_epi32(k, (__m512i*)be);
      __m512i permute0_result = _mm512_permutexvar_epi8(permute0_mask, input);
      __m512i multishift0_result =
          _mm512_multishift_epi64_epi8(multishift0_mask, permute0_result);
      __m512i and0_result = _mm512_and_si512(multishift0_result, and0_mask);
      __m512i permute1_result = _mm512_permutexvar_epi8(permute1_mask, input);
      __m512i multishift1_result =
          _mm512_multishift_epi64_epi8(multishift1_mask, permute1_result);
      __m512i and1_result = _mm512_and_si512(multishift1_result, and1_mask);
      __m512i result = _mm512_or_si512(and0_result, and1_result);

      _mm512_storeu_si512((__m512i*)pg_v210, result);

      be += 12;
      pg_v210 += 64;
    }
  }

  pg_cnt = pg_cnt % cache_pg_cnt;
  mt_rte_free(be_caches);
  mt_cvt_dma_ctx_uinit(ctx);

  /* remaining simd batch */
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
    __m512i result = _mm512_or_si512(and0_result, and1_result);

    _mm512_storeu_si512((__m512i*)pg_v210, result);

    pg_be += 12;
    pg_v210 += 64;
  }

  return 0;
}
/* end st20_rfc4175_422be10_to_v210_avx512_vbmi */

/* begin st20_yuv422p10le_to_rfc4175_422be10_vbmi */
static uint16_t ple_to_be10_sllv_tbl_512[8 * 4] = {
    /* 0-15, b0 - b7 */
    0x0006,
    0x0006,
    0x0006,
    0x0006,
    0x0006,
    0x0006,
    0x0006,
    0x0006,
    /* 16-31, y0 - y7 */
    0x0004,
    0x0000,
    0x0004,
    0x0000,
    0x0004,
    0x0000,
    0x0004,
    0x0000,
    /* 32-47, r0 - r7 */
    0x0002,
    0x0002,
    0x0002,
    0x0002,
    0x0002,
    0x0002,
    0x0002,
    0x0002,
    /* 48-63, y8 - y15 */
    0x0004,
    0x0000,
    0x0004,
    0x0000,
    0x0004,
    0x0000,
    0x0004,
    0x0000,
};

static uint8_t ple_to_be10_permute_hi_tbl_512[16 * 4] = {
    1 + (2 * 0), 0 + (2 * 0), 16 + (4 * 0), 32 + (2 * 0), 18 + (4 * 0), /* pg0 */
    1 + (2 * 1), 0 + (2 * 1), 16 + (4 * 1), 32 + (2 * 1), 18 + (4 * 1), /* pg1 */
    1 + (2 * 2), 0 + (2 * 2), 16 + (4 * 2), 32 + (2 * 2), 18 + (4 * 2), /* pg2 */
    1 + (2 * 3), 0 + (2 * 3), 16 + (4 * 3), 32 + (2 * 3), 18 + (4 * 3), /* pg3 */
    1 + (2 * 4), 0 + (2 * 4), 48 + (4 * 0), 32 + (2 * 4), 50 + (4 * 0), /* pg4 */
    1 + (2 * 5), 0 + (2 * 5), 48 + (4 * 1), 32 + (2 * 5), 50 + (4 * 1), /* pg5 */
    1 + (2 * 6), 0 + (2 * 6), 48 + (4 * 2), 32 + (2 * 6), 50 + (4 * 2), /* pg6 */
    1 + (2 * 7), 0 + (2 * 7), 48 + (4 * 3), 32 + (2 * 7), 50 + (4 * 3), /* pg7 */
    0x00,        0x00,        0x00,         0x00,         0x00,
    0x00,        0x00,        0x00, /* 40-48 */
    0x00,        0x00,        0x00,         0x00,         0x00,
    0x00,        0x00,        0x00, /* 49-53 */
    0x00,        0x00,        0x00,         0x00,         0x00,
    0x00,        0x00,        0x00, /* 54-63 */
};

static uint8_t ple_to_be10_permute_lo_tbl_512[16 * 4] = {
    0,    17 + (4 * 0), 33 + (2 * 0), 19 + (4 * 0), 0,                      /* pg0 */
    0,    17 + (4 * 1), 33 + (2 * 1), 19 + (4 * 1), 0,                      /* pg1 */
    0,    17 + (4 * 2), 33 + (2 * 2), 19 + (4 * 2), 0,                      /* pg2 */
    0,    17 + (4 * 3), 33 + (2 * 3), 19 + (4 * 3), 0,                      /* pg3 */
    0,    49 + (4 * 0), 33 + (2 * 4), 51 + (4 * 0), 0,                      /* pg4 */
    0,    49 + (4 * 1), 33 + (2 * 5), 51 + (4 * 1), 0,                      /* pg5 */
    0,    49 + (4 * 2), 33 + (2 * 6), 51 + (4 * 2), 0,                      /* pg6 */
    0,    49 + (4 * 3), 33 + (2 * 7), 51 + (4 * 3), 0,                      /* pg7 */
    0x00, 0x00,         0x00,         0x00,         0x00, 0x00, 0x00, 0x00, /* 40-48 */
    0x00, 0x00,         0x00,         0x00,         0x00, 0x00, 0x00, 0x00, /* 49-53 */
    0x00, 0x00,         0x00,         0x00,         0x00, 0x00, 0x00, 0x00, /* 54-63 */
};

static uint8_t ple_to_be10_and_lo_tbl_512[16 * 4] = {
    0x00, 0xFF, 0xFF, 0xFF, 0x00,                   /* pg0 */
    0x00, 0xFF, 0xFF, 0xFF, 0x00,                   /* pg1 */
    0x00, 0xFF, 0xFF, 0xFF, 0x00,                   /* pg2 */
    0x00, 0xFF, 0xFF, 0xFF, 0x00,                   /* pg3 */
    0x00, 0xFF, 0xFF, 0xFF, 0x00,                   /* pg4 */
    0x00, 0xFF, 0xFF, 0xFF, 0x00,                   /* pg5 */
    0x00, 0xFF, 0xFF, 0xFF, 0x00,                   /* pg6 */
    0x00, 0xFF, 0xFF, 0xFF, 0x00,                   /* pg7 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 40-48 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 49-53 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 54-63 */
};

int st20_yuv422p10le_to_rfc4175_422be10_vbmi(uint16_t* y, uint16_t* b, uint16_t* r,
                                             struct st20_rfc4175_422_10_pg2_be* pg,
                                             uint32_t w, uint32_t h) {
  uint32_t pg_cnt = w * h / 2; /* two pgs in one convert */
  uint16_t cb, y0, cr, y1;
  __m512i sllv_le_mask = _mm512_loadu_si512(ple_to_be10_sllv_tbl_512);
  __m512i permute_hi_mask = _mm512_loadu_si512(ple_to_be10_permute_hi_tbl_512);
  __m512i permute_lo_mask = _mm512_loadu_si512(ple_to_be10_permute_lo_tbl_512);
  __m512i and_lo_mask = _mm512_loadu_si512(ple_to_be10_and_lo_tbl_512);
  __mmask64 k = 0xFFFFFFFFFF; /* each __m512i with 2*4 pg group, 40 bytes */
  __m512i zero = _mm512_setzero_si512();

  /* each __m512i batch handle 4 __m128i, each __m128i with 2 pg group */
  while (pg_cnt >= 8) {
    __m128i src_128i[4];
    src_128i[0] = _mm_loadu_si128((__m128i*)b); /* b0-b7 */
    b += 8;
    src_128i[1] = _mm_loadu_si128((__m128i*)y); /* y0-y7 */
    y += 8;
    src_128i[2] = _mm_loadu_si128((__m128i*)r); /* r0-r7 */
    r += 8;
    src_128i[3] = _mm_loadu_si128((__m128i*)y); /* y8-y15 */
    y += 8;

    /* b0-b7, y0-y7, r0-r7, y8-y15 */
    __m512i src = _mm512_inserti64x2(zero, src_128i[0], 0);
    src = _mm512_inserti64x2(src, src_128i[1], 1);
    src = _mm512_inserti64x2(src, src_128i[2], 2);
    src = _mm512_inserti64x2(src, src_128i[3], 3);

    /* b0, r0, y0, y1, b1, r1, y2, y3 */
    __m512i srlv_le_result = _mm512_sllv_epi16(src, sllv_le_mask);
    __m512i permute_hi_result = _mm512_permutexvar_epi8(permute_hi_mask, srlv_le_result);
    __m512i permute_lo_result = _mm512_permutexvar_epi8(permute_lo_mask, srlv_le_result);
    permute_lo_result = _mm512_and_si512(permute_lo_result, and_lo_mask);
    __m512i result = _mm512_or_si512(permute_hi_result, permute_lo_result);
    _mm512_mask_storeu_epi8(pg, k, result);
    pg += 8;

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
/* end st20_yuv422p10le_to_rfc4175_422be10_vbmi */

/* begin st20_rfc4175_422le10_to_422be10_vbmi */
static uint8_t le10_to_be_permute_l0_tbl_512[64] = {
    /* simd 0 */
    0x01, 0x02, 0x03, 0x04, /* 4 bytes from pg0 */
    0x06, 0x07, 0x08, 0x09, /* 4 bytes from pg1 */
    0x0B, 0x0C, 0x0D, 0x0E, /* 4 bytes from pg2 */
    0x04, 0x09, 0x0E, 0x3F, /* 5th bytes from pg0,pg1,pg2 */
    /* simd 1 */
    0x01 + 15, 0x02 + 15, 0x03 + 15, 0x04 + 15, /* 4 bytes from pg0 */
    0x06 + 15, 0x07 + 15, 0x08 + 15, 0x09 + 15, /* 4 bytes from pg1 */
    0x0B + 15, 0x0C + 15, 0x0D + 15, 0x0E + 15, /* 4 bytes from pg2 */
    0x04 + 15, 0x09 + 15, 0x0E + 15, 0x3F,      /* 5th bytes from pg0,pg1,pg2 */
    /* simd 2 */
    0x01 + 30, 0x02 + 30, 0x03 + 30, 0x04 + 30, /* 4 bytes from pg0 */
    0x06 + 30, 0x07 + 30, 0x08 + 30, 0x09 + 30, /* 4 bytes from pg1 */
    0x0B + 30, 0x0C + 30, 0x0D + 30, 0x0E + 30, /* 4 bytes from pg2 */
    0x04 + 30, 0x09 + 30, 0x0E + 30, 0x3F,      /* 5th bytes from pg0,pg1,pg2 */
    /* simd 3 */
    0x01 + 45, 0x02 + 45, 0x03 + 45, 0x04 + 45, /* 4 bytes from pg0 */
    0x06 + 45, 0x07 + 45, 0x08 + 45, 0x09 + 45, /* 4 bytes from pg1 */
    0x0B + 45, 0x0C + 45, 0x0D + 45, 0x0E + 45, /* 4 bytes from pg2 */
    0x04 + 45, 0x09 + 45, 0x0E + 45, 0x3F,      /* 5th bytes from pg0,pg1,pg2 */
};

static uint8_t le10_to_be_and_l0_tbl_512[64] = {
    /* simd 0 */
    0xF0, 0x3F, 0x00, 0xFF, /* pg0 */
    0xF0, 0x3F, 0x00, 0xFF, /* pg1 */
    0xF0, 0x3F, 0x00, 0xFF, /* pg2 */
    0x00, 0x03, 0x03, 0x03, /* 5th bytes from pg0,pg1,pg2 */
    /* simd 1 */
    0xF0, 0x3F, 0x00, 0xFF, /* pg0 */
    0xF0, 0x3F, 0x00, 0xFF, /* pg1 */
    0xF0, 0x3F, 0x00, 0xFF, /* pg2 */
    0x00, 0x03, 0x03, 0x03, /* 5th bytes from pg0,pg1,pg2 */
    /* simd 2 */
    0xF0, 0x3F, 0x00, 0xFF, /* pg0 */
    0xF0, 0x3F, 0x00, 0xFF, /* pg1 */
    0xF0, 0x3F, 0x00, 0xFF, /* pg2 */
    0x00, 0x03, 0x03, 0x03, /* 5th bytes from pg0,pg1,pg2 */
    /* simd 3 */
    0xF0, 0x3F, 0x00, 0xFF, /* pg0 */
    0xF0, 0x3F, 0x00, 0xFF, /* pg1 */
    0xF0, 0x3F, 0x00, 0xFF, /* pg2 */
    0x00, 0x03, 0x03, 0x03, /* 5th bytes from pg0,pg1,pg2 */
};

static uint8_t le10_to_be_permute_l1_tbl_512[64] = {
    /* simd 0 */
    0x02, 0x01, 0x00, 0x0D, 0x03, /* pg0 */
    0x02, 0x05, 0x04, 0x0E, 0x07, /* pg1 */
    0x02, 0x09, 0x08, 0x0F, 0x0B, /* pg2 */
    /* simd 1 */
    0x02, 0x01 + 16, 0x00 + 16, 0x0D + 16, 0x03 + 16, /* pg0 */
    0x02, 0x05 + 16, 0x04 + 16, 0x0E + 16, 0x07 + 16, /* pg1 */
    0x02, 0x09 + 16, 0x08 + 16, 0x0F + 16, 0x0B + 16, /* pg2 */
    /* simd 2 */
    0x02, 0x01 + 32, 0x00 + 32, 0x0D + 32, 0x03 + 32, /* pg0 */
    0x02, 0x05 + 32, 0x04 + 32, 0x0E + 32, 0x07 + 32, /* pg1 */
    0x02, 0x09 + 32, 0x08 + 32, 0x0F + 32, 0x0B + 32, /* pg2 */
    /* simd 3 */
    0x02, 0x01 + 48, 0x00 + 48, 0x0D + 48, 0x03 + 48, /* pg0 */
    0x02, 0x05 + 48, 0x04 + 48, 0x0E + 48, 0x07 + 48, /* pg1 */
    0x02, 0x09 + 48, 0x08 + 48, 0x0F + 48, 0x0B + 48, /* pg2 */
    /* zeros */
    0x02, 0x02, 0x02, 0x02, /* zeros */
};

static uint8_t le10_to_be_permute_r0_tbl_512[64] = {
    /* simd 0 */
    0x00, 0x01, 0x02, 0x03, /* 4 bytes from pg0 */
    0x05, 0x06, 0x07, 0x08, /* 4 bytes from pg1 */
    0x0A, 0x0B, 0x0C, 0x0D, /* 4 bytes from pg2 */
    0x3F, 0x00, 0x05, 0x0A, /* 5th bytes from pg0,pg1,pg2 */
    /* simd 1 */
    0x00 + 15, 0x01 + 15, 0x02 + 15, 0x03 + 15, /* 4 bytes from pg0 */
    0x05 + 15, 0x06 + 15, 0x07 + 15, 0x08 + 15, /* 4 bytes from pg1 */
    0x0A + 15, 0x0B + 15, 0x0C + 15, 0x0D + 15, /* 4 bytes from pg2 */
    0x3F, 0x00 + 15, 0x05 + 15, 0x0A + 15,      /* 5th bytes from pg0,pg1,pg2 */
    /* simd 2 */
    0x00 + 30, 0x01 + 30, 0x02 + 30, 0x03 + 30, /* 4 bytes from pg0 */
    0x05 + 30, 0x06 + 30, 0x07 + 30, 0x08 + 30, /* 4 bytes from pg1 */
    0x0A + 30, 0x0B + 30, 0x0C + 30, 0x0D + 30, /* 4 bytes from pg2 */
    0x3F, 0x00 + 30, 0x05 + 30, 0x0A + 30,      /* 5th bytes from pg0,pg1,pg2 */
    /* simd 3 */
    0x00 + 45, 0x01 + 45, 0x02 + 45, 0x03 + 45, /* 4 bytes from pg0 */
    0x05 + 45, 0x06 + 45, 0x07 + 45, 0x08 + 45, /* 4 bytes from pg1 */
    0x0A + 45, 0x0B + 45, 0x0C + 45, 0x0D + 45, /* 4 bytes from pg2 */
    0x3F, 0x00 + 45, 0x05 + 45, 0x0A + 45,      /* 5th bytes from pg0,pg1,pg2 */
};

static uint8_t le10_to_be_and_r0_tbl_512[64] = {
    /* simd 0 */
    0xFF, 0x00, 0xFC, 0x0F, /* pg0 */
    0xFF, 0x00, 0xFC, 0x0F, /* pg1 */
    0xFF, 0x00, 0xFC, 0x0F, /* pg2 */
    0xC0, 0xC0, 0xC0, 0x00, /* 5th bytes from pg0,pg1,pg2 */
                            /* simd 1 */
    0xFF, 0x00, 0xFC, 0x0F, /* pg0 */
    0xFF, 0x00, 0xFC, 0x0F, /* pg1 */
    0xFF, 0x00, 0xFC, 0x0F, /* pg2 */
    0xC0, 0xC0, 0xC0, 0x00, /* 5th bytes from pg0,pg1,pg2 */
                            /* simd 2 */
    0xFF, 0x00, 0xFC, 0x0F, /* pg0 */
    0xFF, 0x00, 0xFC, 0x0F, /* pg1 */
    0xFF, 0x00, 0xFC, 0x0F, /* pg2 */
    0xC0, 0xC0, 0xC0, 0x00, /* 5th bytes from pg0,pg1,pg2 */
                            /* simd 3 */
    0xFF, 0x00, 0xFC, 0x0F, /* pg0 */
    0xFF, 0x00, 0xFC, 0x0F, /* pg1 */
    0xFF, 0x00, 0xFC, 0x0F, /* pg2 */
    0xC0, 0xC0, 0xC0, 0x00, /* 5th bytes from pg0,pg1,pg2 */
};

static uint8_t le10_to_be_permute_r1_tbl_512[64] = {
    /* simd 0 */
    0x00, 0x0C, 0x03, 0x02, 0x3F, /* pg0 */
    0x04, 0x0D, 0x07, 0x06, 0x3F, /* pg1 */
    0x08, 0x0E, 0x0B, 0x0A, 0x3F, /* pg2 */
    /* simd 1 */
    0x00 + 16, 0x0C + 16, 0x03 + 16, 0x02 + 16, 0x3F, /* pg0 */
    0x04 + 16, 0x0D + 16, 0x07 + 16, 0x06 + 16, 0x3F, /* pg1 */
    0x08 + 16, 0x0E + 16, 0x0B + 16, 0x0A + 16, 0x3F, /* pg2 */
    /* simd 2 */
    0x00 + 32, 0x0C + 32, 0x03 + 32, 0x02 + 32, 0x3F, /* pg0 */
    0x04 + 32, 0x0D + 32, 0x07 + 32, 0x06 + 32, 0x3F, /* pg1 */
    0x08 + 32, 0x0E + 32, 0x0B + 32, 0x0A + 32, 0x3F, /* pg2 */
    /* simd 3 */
    0x00 + 48, 0x0C + 48, 0x03 + 48, 0x02 + 48, 0x3F, /* pg0 */
    0x04 + 48, 0x0D + 48, 0x07 + 48, 0x06 + 48, 0x3F, /* pg1 */
    0x08 + 48, 0x0E + 48, 0x0B + 48, 0x0A + 48, 0x3F, /* pg2 */
    /* zeros */
    0x3F, 0x3F, 0x3F, 0x3F, /* zeros */
};

int st20_rfc4175_422le10_to_422be10_vbmi(struct st20_rfc4175_422_10_pg2_le* pg_le,
                                         struct st20_rfc4175_422_10_pg2_be* pg_be,
                                         uint32_t w, uint32_t h) {
  __m512i permute_l0 = _mm512_loadu_si512((__m512i*)le10_to_be_permute_l0_tbl_512);
  __m512i permute_r0 = _mm512_loadu_si512((__m512i*)le10_to_be_permute_r0_tbl_512);
  __m512i and_l0 = _mm512_loadu_si512((__m512i*)le10_to_be_and_l0_tbl_512);
  __m512i and_r0 = _mm512_loadu_si512((__m512i*)le10_to_be_and_r0_tbl_512);
  __m512i permute_l1 = _mm512_loadu_si512((__m512i*)le10_to_be_permute_l1_tbl_512);
  __m512i permute_r1 = _mm512_loadu_si512((__m512i*)le10_to_be_permute_r1_tbl_512);
  __mmask16 k = 0x7FFF; /* each __m512i with 12 pg group, 60 bytes */

  int pg_cnt = w * h / 2;
  int batch = pg_cnt / 12;

  for (int i = 0; i < batch; i++) {
    __m512i input = _mm512_maskz_loadu_epi32(k, (__m512i*)pg_le);
    __m512i permute_l0_result = _mm512_permutexvar_epi8(permute_l0, input);
    __m512i permute_r0_result = _mm512_permutexvar_epi8(permute_r0, input);
    __m512i rl_result = _mm512_and_si512(_mm512_rol_epi32(permute_l0_result, 2), and_l0);
    __m512i rr_result = _mm512_and_si512(_mm512_ror_epi32(permute_r0_result, 2), and_r0);
    __m512i rl_result_shuffle = _mm512_permutexvar_epi8(permute_l1, rl_result);
    __m512i rr_result_shuffle = _mm512_permutexvar_epi8(permute_r1, rr_result);
    __m512i result = _mm512_or_si512(rl_result_shuffle, rr_result_shuffle);

    /* store to the first 60 bytes after dest address */
    _mm512_mask_storeu_epi32((__m512i*)pg_be, k, result);

    pg_be += 12;
    pg_le += 12;
  }

  int left = pg_cnt % 12;
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

int st20_rfc4175_422le10_to_422be10_avx512_vbmi_dma(
    struct mtl_dma_lender_dev* dma, struct st20_rfc4175_422_10_pg2_le* pg_le,
    mtl_iova_t pg_le_iova, struct st20_rfc4175_422_10_pg2_be* pg_be, uint32_t w,
    uint32_t h) {
  __m512i permute_l0 = _mm512_loadu_si512((__m512i*)le10_to_be_permute_l0_tbl_512);
  __m512i permute_r0 = _mm512_loadu_si512((__m512i*)le10_to_be_permute_r0_tbl_512);
  __m512i and_l0 = _mm512_loadu_si512((__m512i*)le10_to_be_and_l0_tbl_512);
  __m512i and_r0 = _mm512_loadu_si512((__m512i*)le10_to_be_and_r0_tbl_512);
  __m512i permute_l1 = _mm512_loadu_si512((__m512i*)le10_to_be_permute_l1_tbl_512);
  __m512i permute_r1 = _mm512_loadu_si512((__m512i*)le10_to_be_permute_r1_tbl_512);
  __mmask16 k = 0x7FFF; /* each __m512i with 12 pg group, 60 bytes */
  int pg_cnt = w * h / 2;

  int caches_num = 4;
  int cache_pg_cnt = (256 * 1024) / sizeof(*pg_le); /* pg cnt for each cache */
  int align = caches_num * 12; /* align to simd pg groups and caches_num */
  cache_pg_cnt = cache_pg_cnt / align * align;
  size_t cache_size = cache_pg_cnt * sizeof(*pg_le);
  int soc_id = dma->parent->soc_id;

  struct st20_rfc4175_422_10_pg2_le* le_caches =
      mt_rte_zmalloc_socket(cache_size * caches_num, soc_id);
  /* two type be(0) or le(1) */
  struct mt_cvt_dma_ctx* ctx = mt_cvt_dma_ctx_init(2 * caches_num, soc_id, 2);
  if (!le_caches || !ctx) {
    err("%s, alloc cache(%d,%" PRIu64 ") fail, %p\n", __func__, cache_pg_cnt, cache_size,
        le_caches);
    if (le_caches) mt_rte_free(le_caches);
    if (ctx) mt_cvt_dma_ctx_uinit(ctx);
    return st20_rfc4175_422be10_to_422le10_avx512_vbmi(pg_be, pg_le, w, h);
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
    int cur_tran = mt_cvt_dma_ctx_get_tran(ctx, 0);
    /* push max be dma */
    while (cur_tran < max_tran) {
      rte_iova_t be_cache_iova = le_caches_iova + (cur_tran % caches_num) * cache_size;
      mt_dma_copy_busy(dma, be_cache_iova, pg_le_iova, cache_size);
      pg_le += cache_pg_cnt;
      pg_le_iova += cache_size;
      mt_cvt_dma_ctx_push(ctx, 0);
      cur_tran = mt_cvt_dma_ctx_get_tran(ctx, 0);
    }
    mt_dma_submit_busy(dma);

    /* wait until current be dma copy done */
    while (mt_cvt_dma_ctx_get_done(ctx, 0) < (i + 1)) {
      uint16_t nb_dq = mt_dma_completed(dma, 1, NULL, NULL);
      if (nb_dq) mt_cvt_dma_ctx_pop(ctx);
    }

    struct st20_rfc4175_422_10_pg2_le* le = le_cache;
    int batch = cache_pg_cnt / 12;
    for (int j = 0; j < batch; j++) {
      __m512i input = _mm512_maskz_loadu_epi32(k, (__m512i*)le);
      __m512i permute_l0_result = _mm512_permutexvar_epi8(permute_l0, input);
      __m512i permute_r0_result = _mm512_permutexvar_epi8(permute_r0, input);
      __m512i rl_result =
          _mm512_and_si512(_mm512_rol_epi32(permute_l0_result, 2), and_l0);
      __m512i rr_result =
          _mm512_and_si512(_mm512_ror_epi32(permute_r0_result, 2), and_r0);
      __m512i rl_result_shuffle = _mm512_permutexvar_epi8(permute_l1, rl_result);
      __m512i rr_result_shuffle = _mm512_permutexvar_epi8(permute_r1, rr_result);
      __m512i result = _mm512_or_si512(rl_result_shuffle, rr_result_shuffle);

      /* store to the first 60 bytes after dest address */
      _mm512_mask_storeu_epi32((__m512i*)pg_be, k, result);

      le += 12;
      pg_be += 12;
    }
  }

  pg_cnt = pg_cnt % cache_pg_cnt;
  mt_rte_free(le_caches);
  mt_cvt_dma_ctx_uinit(ctx);

  /* remaining simd batch */
  int batch = pg_cnt / 12;
  for (int i = 0; i < batch; i++) {
    __m512i input = _mm512_maskz_loadu_epi32(k, (__m512i*)pg_le);
    __m512i permute_l0_result = _mm512_permutexvar_epi8(permute_l0, input);
    __m512i permute_r0_result = _mm512_permutexvar_epi8(permute_r0, input);
    __m512i rl_result = _mm512_and_si512(_mm512_rol_epi32(permute_l0_result, 2), and_l0);
    __m512i rr_result = _mm512_and_si512(_mm512_ror_epi32(permute_r0_result, 2), and_r0);
    __m512i rl_result_shuffle = _mm512_permutexvar_epi8(permute_l1, rl_result);
    __m512i rr_result_shuffle = _mm512_permutexvar_epi8(permute_r1, rr_result);
    __m512i result = _mm512_or_si512(rl_result_shuffle, rr_result_shuffle);

    /* store to the first 60 bytes after dest address */
    _mm512_mask_storeu_epi32((__m512i*)pg_be, k, result);

    pg_be += 12;
    pg_le += 12;
  }

  /* remaining scalar batch */
  int left = pg_cnt % 12;
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
/* end st20_rfc4175_422le10_to_422be10_vbmi */

/* begin st20_v210_to_rfc4175_422be10_avx512_vbmi */
static uint8_t v210_to_be10_multishift0_tbl_512[16 * 4] = {
    2,  14, 6,  40, 32, /* pg0 - xmm0 */
    44, 56, 48,         /* pg1 */
    18, 10,             /* pg1 */
    22, 36, 48, 60, 52, /* pg2 */
    0,                  /* not used */
    2,  14, 6,  40, 32, /* pg3 - xmm1 */
    44, 56, 48,         /* pg4 */
    18, 10,             /* pg4 */
    22, 36, 48, 60, 52, /* pg5 */
    0,                  /* not used */
    2,  14, 6,  40, 32, /* pg6 - xmm2 */
    44, 56, 48,         /* pg7 */
    18, 10,             /* pg7 */
    22, 36, 48, 60, 52, /* pg8 */
    0,                  /* not used */
    2,  14, 6,  40, 32, /* pg9 - xmm3 */
    44, 56, 48,         /* pg10 */
    18, 10,             /* pg10 */
    22, 36, 48, 60, 52, /* pg11 */
    0,                  /* not used */
};

static uint8_t v210_to_be10_shuffle_tbl_512[16 * 4] = {
    1, 2, 3, 0, 4, 5, 8, 9, 9, 10, 11, 8, 12, 13, 14, 15, /* xmm0 */
    1, 2, 3, 0, 4, 5, 8, 9, 9, 10, 11, 8, 12, 13, 14, 15, /* xmm1 */
    1, 2, 3, 0, 4, 5, 8, 9, 9, 10, 11, 8, 12, 13, 14, 15, /* xmm2 */
    1, 2, 3, 0, 4, 5, 8, 9, 9, 10, 11, 8, 12, 13, 14, 15, /* xmm3 */
};

static uint8_t v210_to_be10_multishift1_tbl_512[16 * 4] = {
    0,  18, 18, 10, 0, /* pg0 - xmm0 */
    0,  36, 54,        /* pg1 */
    22, 0,             /* pg1 */
    0,  6,  28, 40, 0, /* pg2 */
    0,                 /* not used */
    0,  18, 18, 10, 0, /* pg3 - xmm1 */
    0,  36, 54,        /* pg4 */
    22, 0,             /* pg4 */
    0,  6,  28, 40, 0, /* pg5 */
    0,                 /* not used */
    0,  18, 18, 10, 0, /* pg6 - xmm2 */
    0,  36, 54,        /* pg7 */
    22, 0,             /* pg7 */
    0,  6,  28, 40, 0, /* pg8 */
    0,                 /* not used */
    0,  18, 18, 10, 0, /* pg9 - xmm3 */
    0,  36, 54,        /* pg10 */
    22, 0,             /* pg10 */
    0,  6,  28, 40, 0, /* pg11 */
    0,                 /* not used */
};

static uint8_t v210_to_be10_and_tbl_512[16 * 4] = {
    0xFF, 0x3F, 0xF0, 0x03, 0xFF, /* pg0 - xmm0 */
    0xFF, 0x3F, 0xF0,             /* pg1 */
    0x03, 0xFF,                   /* pg1 */
    0xFF, 0x3F, 0x0F, 0x03, 0xFF, /* pg2 */
    0x00,                         /* not used */
    0xFF, 0x3F, 0xF0, 0x03, 0xFF, /* pg3 - xmm1 */
    0xFF, 0x3F, 0xF0,             /* pg4 */
    0x03, 0xFF,                   /* pg4 */
    0xFF, 0x3F, 0x0F, 0x03, 0xFF, /* pg5 */
    0x00,                         /* not used */
    0xFF, 0x3F, 0xF0, 0x03, 0xFF, /* pg6 - xmm2 */
    0xFF, 0x3F, 0xF0,             /* pg7 */
    0x03, 0xFF,                   /* pg7 */
    0xFF, 0x3F, 0x0F, 0x03, 0xFF, /* pg8 */
    0x00,                         /* not used */
    0xFF, 0x3F, 0xF0, 0x03, 0xFF, /* pg9 - xmm3 */
    0xFF, 0x3F, 0xF0,             /* pg10 */
    0x03, 0xFF,                   /* pg10 */
    0xFF, 0x3F, 0x0F, 0x03, 0xFF, /* pg11 */
    0x00,                         /* not used */
};

int st20_v210_to_rfc4175_422be10_avx512_vbmi(uint8_t* pg_v210,
                                             struct st20_rfc4175_422_10_pg2_be* pg_be,
                                             uint32_t w, uint32_t h) {
  __m512i multishift0_mask =
      _mm512_loadu_si512((__m512i*)v210_to_be10_multishift0_tbl_512);
  __m512i shuffle_mask = _mm512_loadu_si512((__m512i*)v210_to_be10_shuffle_tbl_512);
  __m512i multishift1_mask =
      _mm512_loadu_si512((__m512i*)v210_to_be10_multishift1_tbl_512);
  __m512i and_mask = _mm512_loadu_si512((__m512i*)v210_to_be10_and_tbl_512);

  __mmask64 k_store =
      0x7FFF7FFF7FFF7FFF; /* each __m128i with 3 pg group, 15 bytes, 4*xmms*/

  int pg_cnt = w * h / 2;
  if (pg_cnt % 12 != 0) {
    err("%s, invalid pg_cnt %d, pixel group number must be multiple of 12!\n", __func__,
        pg_cnt);
    return -EINVAL;
  }

  int batch = pg_cnt / 12;
  for (int i = 0; i < batch; i++) {
    __m512i input = _mm512_loadu_si512((__m512i*)pg_v210);
    __m512i multishift0_result = _mm512_multishift_epi64_epi8(multishift0_mask, input);
    __m512i shuffle_result = _mm512_shuffle_epi8(input, shuffle_mask);
    __m512i multishift1_result =
        _mm512_multishift_epi64_epi8(multishift1_mask, shuffle_result);
    __m512i and0_result = _mm512_and_si512(multishift0_result, and_mask);
    __m512i and1_result = _mm512_andnot_si512(and_mask, multishift1_result);
    __m512i result = _mm512_or_si512(and0_result, and1_result);

    _mm512_mask_compressstoreu_epi8((__m512i*)pg_be, k_store, result);

    pg_be += 12;
    pg_v210 += 64;
  }

  return 0;
}

int st20_v210_to_rfc4175_422be10_avx512_vbmi_dma(struct mtl_dma_lender_dev* dma,
                                                 uint8_t* pg_v210,
                                                 mtl_iova_t pg_v210_iova,
                                                 struct st20_rfc4175_422_10_pg2_be* pg_be,
                                                 uint32_t w, uint32_t h) {
  __m512i multishift0_mask =
      _mm512_loadu_si512((__m512i*)v210_to_be10_multishift0_tbl_512);
  __m512i shuffle_mask = _mm512_loadu_si512((__m512i*)v210_to_be10_shuffle_tbl_512);
  __m512i multishift1_mask =
      _mm512_loadu_si512((__m512i*)v210_to_be10_multishift1_tbl_512);
  __m512i and_mask = _mm512_loadu_si512((__m512i*)v210_to_be10_and_tbl_512);

  __mmask64 k_store =
      0x7FFF7FFF7FFF7FFF; /* each __m128i with 3 pg group, 15 bytes, 4*xmms */

  int pg_cnt = w * h / 2;
  if (pg_cnt % 12 != 0) {
    err("%s, invalid pg_cnt %d, pixel group number must be multiple of 12!\n", __func__,
        pg_cnt);
    return -EINVAL;
  }

  int caches_num = 4;
  int cache_3_pg_cnt = (256 * 1024) / 16; /* 3pg cnt for each cache */
  int align = caches_num * 4;             /* align to simd pg groups and caches_num */
  cache_3_pg_cnt = cache_3_pg_cnt / align * align;
  size_t cache_size = cache_3_pg_cnt * 16;
  int soc_id = dma->parent->soc_id;

  uint8_t* v210_caches = mt_rte_zmalloc_socket(cache_size * caches_num, soc_id);
  /* two type v210(0) or be(1) */
  struct mt_cvt_dma_ctx* ctx = mt_cvt_dma_ctx_init(2 * caches_num, soc_id, 2);
  if (!v210_caches || !ctx) {
    err("%s, alloc cache(%d,%" PRIu64 ") fail, %p\n", __func__, cache_3_pg_cnt,
        cache_size, v210_caches);
    if (v210_caches) mt_rte_free(v210_caches);
    if (ctx) mt_cvt_dma_ctx_uinit(ctx);
    return st20_v210_to_rfc4175_422be10_avx512_vbmi(pg_v210, pg_be, w, h);
  }
  rte_iova_t v210_caches_iova = rte_malloc_virt2iova(v210_caches);

  /* first with caches batch step */
  int cache_batch = pg_cnt / (cache_3_pg_cnt * 3);
  dbg("%s, pg_cnt %d cache_3_pg_cnt %d caches_num %d cache_batch %d\n", __func__, pg_cnt,
      cache_3_pg_cnt, caches_num, cache_batch);
  for (int i = 0; i < cache_batch; i++) {
    uint8_t* v210_cache = v210_caches + (i % caches_num) * (cache_3_pg_cnt * 16);
    dbg("%s, cache batch idx %d\n", __func__, i);

    int max_tran = i + caches_num;
    max_tran = RTE_MIN(max_tran, cache_batch);
    int cur_tran = mt_cvt_dma_ctx_get_tran(ctx, 0);
    /* push max be dma */
    while (cur_tran < max_tran) {
      rte_iova_t v210_cache_iova =
          v210_caches_iova + (cur_tran % caches_num) * cache_size;
      mt_dma_copy_busy(dma, v210_cache_iova, pg_v210_iova, cache_size);
      pg_v210 += (cache_3_pg_cnt * 16);
      pg_v210_iova += cache_size;
      mt_cvt_dma_ctx_push(ctx, 0);
      cur_tran = mt_cvt_dma_ctx_get_tran(ctx, 0);
    }
    mt_dma_submit_busy(dma);

    /* wait until current be dma copy done */
    while (mt_cvt_dma_ctx_get_done(ctx, 0) < (i + 1)) {
      uint16_t nb_dq = mt_dma_completed(dma, 1, NULL, NULL);
      if (nb_dq) mt_cvt_dma_ctx_pop(ctx);
    }

    uint8_t* v210 = v210_cache;
    int batch = cache_3_pg_cnt / 4;
    for (int j = 0; j < batch; j++) {
      __m512i input = _mm512_loadu_si512((__m512i*)v210);
      __m512i multishift0_result = _mm512_multishift_epi64_epi8(multishift0_mask, input);
      __m512i shuffle_result = _mm512_shuffle_epi8(input, shuffle_mask);
      __m512i multishift1_result =
          _mm512_multishift_epi64_epi8(multishift1_mask, shuffle_result);
      __m512i and0_result = _mm512_and_si512(multishift0_result, and_mask);
      __m512i and1_result = _mm512_andnot_si512(and_mask, multishift1_result);
      __m512i result = _mm512_or_si512(and0_result, and1_result);

      _mm512_mask_compressstoreu_epi8((__m512i*)pg_be, k_store, result);

      pg_be += 12;
      v210 += 64;
    }
  }

  pg_cnt = pg_cnt % (cache_3_pg_cnt * 3);
  mt_rte_free(v210_caches);
  mt_cvt_dma_ctx_uinit(ctx);

  /* remaining simd batch */
  int batch = pg_cnt / 12;
  for (int i = 0; i < batch; i++) {
    __m512i input = _mm512_loadu_si512((__m512i*)pg_v210);
    __m512i multishift0_result = _mm512_multishift_epi64_epi8(multishift0_mask, input);
    __m512i shuffle_result = _mm512_shuffle_epi8(input, shuffle_mask);
    __m512i multishift1_result =
        _mm512_multishift_epi64_epi8(multishift1_mask, shuffle_result);
    __m512i and0_result = _mm512_and_si512(multishift0_result, and_mask);
    __m512i and1_result = _mm512_andnot_si512(and_mask, multishift1_result);
    __m512i result = _mm512_or_si512(and0_result, and1_result);

    _mm512_mask_compressstoreu_epi8((__m512i*)pg_be, k_store, result);

    pg_be += 12;
    pg_v210 += 64;
  }

  return 0;
}
/* end st20_v210_to_rfc4175_422be10_avx512_vbmi */

int st20_downsample_rfc4175_422be10_wh_half_avx512_vbmi(uint8_t* pg_old, uint8_t* pg_new,
                                                        uint32_t w, uint32_t h,
                                                        uint32_t linesize_old,
                                                        uint32_t linesize_new) {
  int new_pg_in_zmm = 6;
  __mmask64 k = 0b1111100000111110000011111000001111100000111110000011111;
  /* calculate batch size */
  int new_pg_per_line = w / 2;
  int batches = new_pg_per_line / new_pg_in_zmm;
  for (int line = 0; line < h; line++) {
    /* calculate offset */
    uint8_t* src = pg_old + linesize_old * line * 2;
    uint8_t* dst = pg_new + linesize_new * line;
    /* for each batch */
    for (int i = 0; i < batches; i++) {
      __m512i input = _mm512_loadu_si512(src);
      _mm512_mask_compressstoreu_epi8(dst, k, input);
      src += new_pg_in_zmm * 2 * 5;
      dst += new_pg_in_zmm * 5;
    }
    /* handle left pgs */
    int left = new_pg_per_line % new_pg_in_zmm;
    while (left) {
      mtl_memcpy(dst, src, 5);
      src += 2 * 5;
      dst += 5;
      left--;
    }
  }
  return 0;
}
MT_TARGET_CODE_STOP
#endif
