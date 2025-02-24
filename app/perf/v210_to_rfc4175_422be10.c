/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "../sample/sample_util.h"

static void fill_rand_v210(uint8_t *p, size_t sz) {
  for (size_t i = 0; i < sz; i++) {
    p[i] = rand();
    if ((i % 4) == 3)
      p[i] &= 0x3F;
  }
}

static int perf_cvt_v210_to_be(mtl_handle st, int w, int h, int frames,
                               int fb_cnt) {
  size_t fb_pg2_size = w * h * 5 / 2;
  size_t fb_size_v210 = w * h * 8 / 3;
  mtl_udma_handle dma = mtl_udma_create(st, 128, MTL_PORT_P);
  struct st20_rfc4175_422_10_pg2_be *pg_be =
      (struct st20_rfc4175_422_10_pg2_be *)malloc(fb_pg2_size * fb_cnt);
  uint8_t *pg_v210 =
      (uint8_t *)mtl_hp_malloc(st, fb_size_v210 * fb_cnt, MTL_PORT_P);
  mtl_iova_t pg_v210_iova = mtl_hp_virt2iova(st, pg_v210);
  mtl_iova_t pg_v210_in_iova;
  float fb_size_v210_m = (float)fb_size_v210 / 1024 / 1024;
  enum mtl_simd_level cpu_level = mtl_get_simd_level();

  uint8_t *pg_v210_in;
  struct st20_rfc4175_422_10_pg2_be *pg_be_out;

  for (int i = 0; i < fb_cnt; i++) {
    pg_v210_in = pg_v210 + (i % fb_cnt) * (fb_size_v210 / sizeof(*pg_v210_in));
    fill_rand_v210(pg_v210_in, fb_size_v210);
  }

  clock_t start, end;
  float duration;

  start = clock();
  for (int i = 0; i < frames; i++) {
    pg_v210_in = pg_v210 + (i % fb_cnt) * (fb_size_v210 / sizeof(*pg_v210_in));
    pg_be_out = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
    st20_v210_to_rfc4175_422be10_simd(pg_v210_in, pg_be_out, w, h,
                                      MTL_SIMD_LEVEL_NONE);
  }
  end = clock();
  duration = (float)(end - start) / CLOCKS_PER_SEC;
  info("scalar, time: %f secs with %d frames(%dx%d,%fm@%d buffers)\n", duration,
       frames, w, h, fb_size_v210_m, fb_cnt);

  if (cpu_level >= MTL_SIMD_LEVEL_AVX512) {
    start = clock();
    for (int i = 0; i < frames; i++) {
      pg_v210_in =
          pg_v210 + (i % fb_cnt) * (fb_size_v210 / sizeof(*pg_v210_in));
      pg_be_out = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
      st20_v210_to_rfc4175_422be10_simd(pg_v210_in, pg_be_out, w, h,
                                        MTL_SIMD_LEVEL_AVX512);
    }
    end = clock();
    float duration_simd = (float)(end - start) / CLOCKS_PER_SEC;
    info("avx512, time: %f secs with %d frames(%dx%d@%d buffers)\n",
         duration_simd, frames, w, h, fb_cnt);
    info("avx512, %fx performance to scalar\n", duration / duration_simd);
    if (dma) {
      start = clock();
      for (int i = 0; i < frames; i++) {
        pg_v210_in =
            pg_v210 + (i % fb_cnt) * (fb_size_v210 / sizeof(*pg_v210_in));
        pg_v210_in_iova = pg_v210_iova + (i % fb_cnt) * (fb_size_v210);
        pg_be_out = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
        st20_v210_to_rfc4175_422be10_simd_dma(dma, pg_v210_in, pg_v210_in_iova,
                                              pg_be_out, w, h,
                                              MTL_SIMD_LEVEL_AVX512);
      }
      end = clock();
      float duration_simd = (float)(end - start) / CLOCKS_PER_SEC;
      info("dma+avx512, time: %f secs with %d frames(%dx%d@%d buffers)\n",
           duration_simd, frames, w, h, fb_cnt);
      info("dma+avx512, %fx performance to scalar\n", duration / duration_simd);
    }
  }
  if (cpu_level >= MTL_SIMD_LEVEL_AVX512_VBMI2) {
    start = clock();
    for (int i = 0; i < frames; i++) {
      pg_v210_in =
          pg_v210 + (i % fb_cnt) * (fb_size_v210 / sizeof(*pg_v210_in));
      pg_be_out = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
      st20_v210_to_rfc4175_422be10_simd(pg_v210_in, pg_be_out, w, h,
                                        MTL_SIMD_LEVEL_AVX512_VBMI2);
    }
    end = clock();
    float duration_vbmi = (float)(end - start) / CLOCKS_PER_SEC;
    info("avx512_vbmi, time: %f secs with %d frames(%dx%d@%d buffers)\n",
         duration_vbmi, frames, w, h, fb_cnt);
    info("avx512_vbmi, %fx performance to scalar\n", duration / duration_vbmi);
    if (dma) {
      start = clock();
      for (int i = 0; i < frames; i++) {
        pg_v210_in =
            pg_v210 + (i % fb_cnt) * (fb_size_v210 / sizeof(*pg_v210_in));
        pg_v210_in_iova = pg_v210_iova + (i % fb_cnt) * (fb_size_v210);
        pg_be_out = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
        st20_v210_to_rfc4175_422be10_simd_dma(dma, pg_v210_in, pg_v210_in_iova,
                                              pg_be_out, w, h,
                                              MTL_SIMD_LEVEL_AVX512_VBMI2);
      }
      end = clock();
      float duration_simd = (float)(end - start) / CLOCKS_PER_SEC;
      info("dma+avx512_vbmi, time: %f secs with %d frames(%dx%d@%d buffers)\n",
           duration_simd, frames, w, h, fb_cnt);
      info("dma+avx512_vbmi, %fx performance to scalar\n",
           duration / duration_simd);
    }
  }

  mtl_hp_free(st, pg_v210);
  free(pg_be);
  if (dma)
    mtl_udma_free(dma);

  return 0;
}

static void *perf_thread(void *arg) {
  struct st_sample_context *ctx = arg;
  mtl_handle dev_handle = ctx->st;
  int frames = ctx->perf_frames;
  int fb_cnt = ctx->perf_fb_cnt;

  unsigned int lcore = 0;
  int ret = mtl_get_lcore(dev_handle, &lcore);
  if (ret < 0) {
    return NULL;
  }
  mtl_bind_to_lcore(dev_handle, pthread_self(), lcore);
  info("%s, run in lcore %u\n", __func__, lcore);

  perf_cvt_v210_to_be(dev_handle, 640, 480, frames, fb_cnt);
  perf_cvt_v210_to_be(dev_handle, 1280, 720, frames, fb_cnt);
  perf_cvt_v210_to_be(dev_handle, 1920, 1080, frames, fb_cnt);
  perf_cvt_v210_to_be(dev_handle, 1920 * 2, 1080 * 2, frames, fb_cnt);
  perf_cvt_v210_to_be(dev_handle, 1920 * 4, 1080 * 4, frames, fb_cnt);

  mtl_put_lcore(dev_handle, lcore);

  return NULL;
}

int main(int argc, char **argv) {
  struct st_sample_context ctx;
  int ret;

  memset(&ctx, 0, sizeof(ctx));
  ret = tx_sample_parse_args(&ctx, argc, argv);
  if (ret < 0)
    return ret;

  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s: mtl_init fail\n", __func__);
    return -EIO;
  }

  pthread_t thread;
  ret = pthread_create(&thread, NULL, perf_thread, &ctx);
  if (ret)
    goto exit;
  pthread_join(thread, NULL);

exit:
  /* release sample(st) dev */
  if (ctx.st) {
    mtl_uninit(ctx.st);
    ctx.st = NULL;
  }
  return ret;
}
