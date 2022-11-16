/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../sample/sample_util.h"

static void fill_rand_y210(uint16_t* p, size_t sz) {
  for (size_t i = 0; i < sz / 2; i++) {
    p[i] = rand() & 0x3FF;
  }
}

static int perf_cvt_y210_to_be(mtl_handle st, int w, int h, int frames, int fb_cnt) {
  size_t fb_pg2_size = w * h * 5 / 2;
  size_t fb_size_y210 = w * h * 4;
  mtl_udma_handle dma = st_udma_create(st, 128, MTL_PORT_P);
  struct st20_rfc4175_422_10_pg2_be* pg_be =
      (struct st20_rfc4175_422_10_pg2_be*)malloc(fb_pg2_size * fb_cnt);
  uint16_t* pg_y210 = (uint16_t*)mtl_hp_malloc(st, fb_size_y210 * fb_cnt, MTL_PORT_P);
  mtl_iova_t pg_y210_iova = mtl_hp_virt2iova(st, pg_y210);
  mtl_iova_t pg_y210_in_iova;
  float fb_size_y210_m = (float)fb_size_y210 / 1024 / 1024;
  enum mtl_simd_level cpu_level = mtl_get_simd_level();

  uint16_t* pg_y210_in;
  struct st20_rfc4175_422_10_pg2_be* pg_be_out;

  for (int i = 0; i < fb_cnt; i++) {
    pg_y210_in = pg_y210 + (i % fb_cnt) * (fb_size_y210 / sizeof(*pg_y210_in));
    fill_rand_y210(pg_y210_in, fb_size_y210);
  }

  clock_t start, end;
  float duration;

  start = clock();
  for (int i = 0; i < frames; i++) {
    pg_y210_in = pg_y210 + (i % fb_cnt) * (fb_size_y210 / sizeof(*pg_y210_in));
    pg_be_out = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
    st20_y210_to_rfc4175_422be10_simd(pg_y210_in, pg_be_out, w, h, MTL_SIMD_LEVEL_NONE);
  }
  end = clock();
  duration = (float)(end - start) / CLOCKS_PER_SEC;
  info("scalar, time: %f secs with %d frames(%dx%d,%fm@%d buffers)\n", duration, frames,
       w, h, fb_size_y210_m, fb_cnt);

  if (cpu_level >= MTL_SIMD_LEVEL_AVX512) {
    start = clock();
    for (int i = 0; i < frames; i++) {
      pg_y210_in = pg_y210 + (i % fb_cnt) * (fb_size_y210 / sizeof(*pg_y210_in));
      pg_be_out = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
      st20_y210_to_rfc4175_422be10_simd(pg_y210_in, pg_be_out, w, h,
                                        MTL_SIMD_LEVEL_AVX512);
    }
    end = clock();
    float duration_simd = (float)(end - start) / CLOCKS_PER_SEC;
    info("avx512, time: %f secs with %d frames(%dx%d@%d buffers)\n", duration_simd,
         frames, w, h, fb_cnt);
    info("avx512, %fx performance to scalar\n", duration / duration_simd);
    if (dma) {
      start = clock();
      for (int i = 0; i < frames; i++) {
        pg_y210_in = pg_y210 + (i % fb_cnt) * (fb_size_y210 / sizeof(*pg_y210_in));
        pg_y210_in_iova = pg_y210_iova + (i % fb_cnt) * (fb_size_y210);
        pg_be_out = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
        st20_y210_to_rfc4175_422be10_simd_dma(dma, pg_y210_in, pg_y210_in_iova, pg_be_out,
                                              w, h, MTL_SIMD_LEVEL_AVX512);
      }
      end = clock();
      float duration_simd = (float)(end - start) / CLOCKS_PER_SEC;
      info("dma+avx512, time: %f secs with %d frames(%dx%d@%d buffers)\n", duration_simd,
           frames, w, h, fb_cnt);
      info("dma+avx512, %fx performance to scalar\n", duration / duration_simd);
    }
  }

  mtl_hp_free(st, pg_y210);
  free(pg_be);
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

  perf_cvt_y210_to_be(dev_handle, 640, 480, frames, fb_cnt);
  perf_cvt_y210_to_be(dev_handle, 1280, 720, frames, fb_cnt);
  perf_cvt_y210_to_be(dev_handle, 1920, 1080, frames, fb_cnt);
  perf_cvt_y210_to_be(dev_handle, 1920 * 2, 1080 * 2, frames, fb_cnt);
  perf_cvt_y210_to_be(dev_handle, 1920 * 4, 1080 * 4, frames, fb_cnt);

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
