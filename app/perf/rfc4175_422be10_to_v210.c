/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "../sample/sample_util.h"

static int perf_cvt_422_10_pg2_be_to_v210(mtl_handle st, int w, int h, int frames,
                                          int fb_cnt) {
  size_t fb_pg2_size = w * h * 5 / 2;
  size_t fb_pg2_size_v210 = w * h * 8 / 3;
  mtl_udma_handle dma = st_udma_create(st, 128, MTL_PORT_P);
  struct st20_rfc4175_422_10_pg2_be* pg_be =
      (struct st20_rfc4175_422_10_pg2_be*)mtl_hp_malloc(st, fb_pg2_size * fb_cnt,
                                                        MTL_PORT_P);
  struct st20_rfc4175_422_10_pg2_le* pg_le =
      (struct st20_rfc4175_422_10_pg2_le*)malloc(fb_pg2_size * fb_cnt);
  uint8_t* pg_v210 = (uint8_t*)malloc(fb_pg2_size_v210 * fb_cnt);
  mtl_iova_t pg_be_iova = mtl_hp_virt2iova(st, pg_be);
  mtl_iova_t pg_be_in_iova;
  size_t planar_size = fb_pg2_size;
  float planar_size_m = (float)planar_size / 1024 / 1024;
  enum mtl_simd_level cpu_level = mtl_get_simd_level();

  size_t be_1line_size = st_frame_size(ST_FRAME_FMT_YUV422RFC4175PG2BE10, w, 1);
  size_t v210_1line_size = st_frame_size(ST_FRAME_FMT_V210, w, 1);
  info("v210_1line_size %ld\n", v210_1line_size);

  struct st20_rfc4175_422_10_pg2_be* pg_be_in;
  struct st20_rfc4175_422_10_pg2_le* pg_le_out;
  uint8_t* pg_v210_out;

  for (int i = 0; i < fb_cnt; i++) {
    pg_be_in = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
    fill_rfc4175_422_10_pg2_data(pg_be_in, w, h);
  }

  clock_t start, end;
  float duration;
  bool test_2step = false;

  info("1 step conversion (be->v210)\n");
  start = clock();
  for (int i = 0; i < frames; i++) {
    pg_be_in = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
    pg_v210_out = pg_v210 + (i % fb_cnt) * (fb_pg2_size_v210 / sizeof(*pg_v210));
    st20_rfc4175_422be10_to_v210_simd(pg_be_in, pg_v210_out, w, h, MTL_SIMD_LEVEL_NONE);
  }
  end = clock();
  duration = (float)(end - start) / CLOCKS_PER_SEC;
  info("scalar, time: %f secs with %d frames(%dx%d,%fm@%d buffers)\n", duration, frames,
       w, h, planar_size_m, fb_cnt);

  if (cpu_level >= MTL_SIMD_LEVEL_AVX512) {
    start = clock();
    for (int i = 0; i < frames; i++) {
      pg_be_in = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
      pg_v210_out = pg_v210 + (i % fb_cnt) * (fb_pg2_size_v210 / sizeof(*pg_v210));
      st20_rfc4175_422be10_to_v210_simd(pg_be_in, pg_v210_out, w, h,
                                        MTL_SIMD_LEVEL_AVX512);
    }
    end = clock();
    float duration_simd = (float)(end - start) / CLOCKS_PER_SEC;
    info("avx512, time: %f secs with %d frames(%dx%d@%d buffers)\n", duration_simd,
         frames, w, h, fb_cnt);
    info("avx512, %fx performance to scalar\n", duration / duration_simd);

    if (v210_1line_size) {
      /* 1line conversion */
      start = clock();
      for (int i = 0; i < frames; i++) {
        pg_be_in = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
        pg_v210_out = pg_v210 + (i % fb_cnt) * (fb_pg2_size_v210 / sizeof(*pg_v210));

        for (int j = 0; j < h; j++) {
          st20_rfc4175_422be10_to_v210_simd(pg_be_in, pg_v210_out, w, 1,
                                            MTL_SIMD_LEVEL_AVX512);
          pg_be_in += be_1line_size / sizeof(*pg_be_in);
          pg_v210_out += v210_1line_size / sizeof(*pg_v210_out);
        }
      }
      end = clock();
      duration_simd = (float)(end - start) / CLOCKS_PER_SEC;
      info("avx512_1line, time: %f secs with %d frames(%dx%d@%d buffers)\n",
           duration_simd, frames, w, h, fb_cnt);
      info("avx512_1line, %fx performance to scalar\n", duration / duration_simd);
    }

    if (dma) {
      start = clock();
      for (int i = 0; i < frames; i++) {
        pg_be_in = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
        pg_be_in_iova = pg_be_iova + (i % fb_cnt) * (fb_pg2_size);
        pg_v210_out = pg_v210 + (i % fb_cnt) * (fb_pg2_size_v210 / sizeof(*pg_v210));
        st20_rfc4175_422be10_to_v210_simd_dma(dma, pg_be_in, pg_be_in_iova, pg_v210_out,
                                              w, h, MTL_SIMD_LEVEL_AVX512);
      }
      end = clock();
      float duration_simd = (float)(end - start) / CLOCKS_PER_SEC;
      info("dma+avx512, time: %f secs with %d frames(%dx%d@%d buffers)\n", duration_simd,
           frames, w, h, fb_cnt);
      info("dma+avx512, %fx performance to scalar\n", duration / duration_simd);
    }
  }
  if (cpu_level >= MTL_SIMD_LEVEL_AVX512_VBMI2) {
    start = clock();
    for (int i = 0; i < frames; i++) {
      pg_be_in = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
      pg_v210_out = pg_v210 + (i % fb_cnt) * (fb_pg2_size_v210 / sizeof(*pg_v210));
      st20_rfc4175_422be10_to_v210_simd(pg_be_in, pg_v210_out, w, h,
                                        MTL_SIMD_LEVEL_AVX512_VBMI2);
    }
    end = clock();
    float duration_vbmi = (float)(end - start) / CLOCKS_PER_SEC;
    info("avx512_vbmi, time: %f secs with %d frames(%dx%d@%d buffers)\n", duration_vbmi,
         frames, w, h, fb_cnt);
    info("avx512_vbmi, %fx performance to scalar\n", duration / duration_vbmi);

    if (dma) {
      start = clock();
      for (int i = 0; i < frames; i++) {
        pg_be_in = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
        pg_be_in_iova = pg_be_iova + (i % fb_cnt) * (fb_pg2_size);
        pg_v210_out = pg_v210 + (i % fb_cnt) * (fb_pg2_size_v210 / sizeof(*pg_v210));
        st20_rfc4175_422be10_to_v210_simd_dma(dma, pg_be_in, pg_be_in_iova, pg_v210_out,
                                              w, h, MTL_SIMD_LEVEL_AVX512_VBMI2);
      }
      end = clock();
      float duration_vbmi = (float)(end - start) / CLOCKS_PER_SEC;
      info("dma+avx512_vbmi, time: %f secs with %d frames(%dx%d@%d buffers)\n",
           duration_vbmi, frames, w, h, fb_cnt);
      info("dma+avx512_vbmi, %fx performance to scalar\n", duration / duration_vbmi);
    }
  }

  if (!test_2step) goto exit;

  info("2 steps conversion (be->le->v210)\n");
  start = clock();
  for (int i = 0; i < frames; i++) {
    pg_be_in = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
    pg_le_out = pg_le + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_le));
    pg_v210_out = pg_v210 + (i % fb_cnt) * (fb_pg2_size_v210 / sizeof(*pg_v210));
    st20_rfc4175_422be10_to_422le10_simd(pg_be_in, pg_le_out, w, h, MTL_SIMD_LEVEL_NONE);
    st20_rfc4175_422le10_to_v210_simd((uint8_t*)pg_le_out, pg_v210_out, w, h,
                                      MTL_SIMD_LEVEL_NONE);
  }
  end = clock();
  duration = (float)(end - start) / CLOCKS_PER_SEC;
  info("scalar, time: %f secs with %d frames(%dx%d,%fm@%d buffers)\n", duration, frames,
       w, h, planar_size_m, fb_cnt);

  if (cpu_level >= MTL_SIMD_LEVEL_AVX512) {
    start = clock();
    for (int i = 0; i < frames; i++) {
      pg_be_in = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
      pg_le_out = pg_le + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_le));
      pg_v210_out = pg_v210 + (i % fb_cnt) * (fb_pg2_size_v210 / sizeof(*pg_v210));
      st20_rfc4175_422be10_to_422le10_simd(pg_be_in, pg_le_out, w, h,
                                           MTL_SIMD_LEVEL_AVX512);
      st20_rfc4175_422le10_to_v210_simd((uint8_t*)pg_le_out, pg_v210_out, w, h,
                                        MTL_SIMD_LEVEL_AVX512);
    }
    end = clock();
    float duration_simd = (float)(end - start) / CLOCKS_PER_SEC;
    info("avx512, time: %f secs with %d frames(%dx%d@%d buffers)\n", duration_simd,
         frames, w, h, fb_cnt);
    info("avx512, %fx performance to scalar\n", duration / duration_simd);
  }

  if (cpu_level >= MTL_SIMD_LEVEL_AVX512_VBMI2) {
    start = clock();
    for (int i = 0; i < frames; i++) {
      pg_be_in = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
      pg_le_out = pg_le + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_le));
      pg_v210_out = pg_v210 + (i % fb_cnt) * (fb_pg2_size_v210 / sizeof(*pg_v210));
      st20_rfc4175_422be10_to_422le10_simd(pg_be_in, pg_le_out, w, h,
                                           MTL_SIMD_LEVEL_AVX512_VBMI2);
      st20_rfc4175_422le10_to_v210_simd((uint8_t*)pg_le_out, pg_v210_out, w, h,
                                        MTL_SIMD_LEVEL_AVX512_VBMI2);
    }
    end = clock();
    float duration_vbmi = (float)(end - start) / CLOCKS_PER_SEC;
    info("avx512_vbmi, time: %f secs with %d frames(%dx%d@%d buffers)\n", duration_vbmi,
         frames, w, h, fb_cnt);
    info("avx512_vbmi, %fx performance to scalar\n", duration / duration_vbmi);
  }

exit:
  mtl_hp_free(st, pg_be);
  free(pg_le);
  free(pg_v210);
  if (dma) st_udma_free(dma);

  return 0;
}

static void* perf_thread(void* arg) {
  mtl_handle dev_handle = arg;
  int frames = 60;
  int fb_cnt = 3;

  unsigned int lcore = 0;
  int ret = mtl_get_lcore(dev_handle, &lcore);
  if (ret < 0) {
    return NULL;
  }
  mtl_bind_to_lcore(dev_handle, pthread_self(), lcore);
  info("%s, run in lcore %u\n", __func__, lcore);

  perf_cvt_422_10_pg2_be_to_v210(dev_handle, 640, 480, frames, fb_cnt);
  perf_cvt_422_10_pg2_be_to_v210(dev_handle, 1280, 720, frames, fb_cnt);
  perf_cvt_422_10_pg2_be_to_v210(dev_handle, 1920, 1080, frames, fb_cnt);
  perf_cvt_422_10_pg2_be_to_v210(dev_handle, 1920 * 2, 1080 * 2, frames, fb_cnt);
  perf_cvt_422_10_pg2_be_to_v210(dev_handle, 1920 * 4, 1080 * 4, frames, fb_cnt);

  mtl_put_lcore(dev_handle, lcore);

  return NULL;
}

int main(int argc, char** argv) {
  struct st_sample_context ctx;
  int ret;

  ret = st_sample_tx_init(&ctx, argc, argv);
  if (ret < 0) return ret;

  pthread_t thread;
  pthread_create(&thread, NULL, perf_thread, ctx.st);
  pthread_join(thread, NULL);

  /* release sample(st) dev */
  st_sample_uinit(&ctx);
  return ret;
}