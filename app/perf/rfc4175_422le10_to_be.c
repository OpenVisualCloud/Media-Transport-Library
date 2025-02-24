/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "../sample/sample_util.h"

static void fill_422_10_pg2_le_data(struct st20_rfc4175_422_10_pg2_le *data,
                                    int w, int h) {
  int pg_size = w * h / 2;
  uint16_t cb, y0, cr, y1; /* 10 bit */

  y0 = 0x111;

  cb = 0x222;
  cr = 0x333;
  y1 = y0 + 1;

  for (int pg = 0; pg < pg_size; pg++) {
    data->Cb00 = cb >> 2;
    data->Cb00_ = cb;
    data->Y00 = y0 >> 4;
    data->Y00_ = y0;
    data->Cr00 = cr >> 6;
    data->Cr00_ = cr;
    data->Y01 = y1 >> 8;
    data->Y01_ = y1;
    data++;

    cb++;
    y0 += 2;
    cr++;
    y1 += 2;
  }
}

static int perf_cvt_422_10_pg2_le_to_be(mtl_handle st, int w, int h, int frames,
                                        int fb_cnt) {
  size_t fb_pg2_size = w * h * 5 / 2;
  mtl_udma_handle dma = mtl_udma_create(st, 128, MTL_PORT_P);
  struct st20_rfc4175_422_10_pg2_le *pg_le =
      (struct st20_rfc4175_422_10_pg2_le *)mtl_hp_malloc(
          st, fb_pg2_size * fb_cnt, MTL_PORT_P);
  mtl_iova_t pg_le_iova = mtl_hp_virt2iova(st, pg_le);
  mtl_iova_t pg_le_in_iova;
  struct st20_rfc4175_422_10_pg2_be *pg_be =
      (struct st20_rfc4175_422_10_pg2_be *)malloc(fb_pg2_size * fb_cnt);
  size_t planar_size = fb_pg2_size;
  float planar_size_m = (float)planar_size / 1024 / 1024;
  enum mtl_simd_level cpu_level = mtl_get_simd_level();

  struct st20_rfc4175_422_10_pg2_le *pg_le_in;
  struct st20_rfc4175_422_10_pg2_be *pg_be_out;

  for (int i = 0; i < fb_cnt; i++) {
    pg_le_in = pg_le + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
    fill_422_10_pg2_le_data(pg_le_in, w, h);
  }

  clock_t start, end;
  float duration;

  start = clock();
  for (int i = 0; i < frames * 1; i++) {
    pg_le_in = pg_le + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
    pg_be_out = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_le));
    st20_rfc4175_422le10_to_422be10_simd(pg_le_in, pg_be_out, w, h,
                                         MTL_SIMD_LEVEL_NONE);
  }
  end = clock();
  duration = (float)(end - start) / CLOCKS_PER_SEC;
  info("scalar, time: %f secs with %d frames(%dx%d,%fm@%d buffers)\n", duration,
       frames, w, h, planar_size_m, fb_cnt);

  if (cpu_level >= MTL_SIMD_LEVEL_AVX2) {
    start = clock();
    for (int i = 0; i < frames * 1; i++) {
      pg_le_in = pg_le + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
      pg_be_out = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_le));
      st20_rfc4175_422le10_to_422be10_simd(pg_le_in, pg_be_out, w, h,
                                           MTL_SIMD_LEVEL_AVX2);
    }
    end = clock();
    float duration_simd = (float)(end - start) / CLOCKS_PER_SEC;
    info("avx2, time: %f secs with %d frames(%dx%d@%d buffers)\n",
         duration_simd, frames, w, h, fb_cnt);
    info("avx2, %fx performance to scalar\n", duration / duration_simd);
  }

  if (cpu_level >= MTL_SIMD_LEVEL_AVX512) {
    start = clock();
    for (int i = 0; i < frames * 1; i++) {
      pg_le_in = pg_le + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
      pg_be_out = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_le));
      st20_rfc4175_422le10_to_422be10_simd(pg_le_in, pg_be_out, w, h,
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
        pg_le_in_iova = pg_le_iova + (i % fb_cnt) * (fb_pg2_size);
        pg_le_in = pg_le + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
        pg_be_out = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_le));
        st20_rfc4175_422le10_to_422be10_simd_dma(dma, pg_le_in, pg_le_in_iova,
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
      pg_le_in = pg_le + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
      pg_be_out = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_le));
      st20_rfc4175_422le10_to_422be10_simd(pg_le_in, pg_be_out, w, h,
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
        pg_le_in_iova = pg_le_iova + (i % fb_cnt) * (fb_pg2_size);
        pg_le_in = pg_le + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_be));
        pg_be_out = pg_be + (i % fb_cnt) * (fb_pg2_size / sizeof(*pg_le));
        st20_rfc4175_422le10_to_422be10_simd_dma(dma, pg_le_in, pg_le_in_iova,
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

  mtl_hp_free(st, pg_le);
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

  perf_cvt_422_10_pg2_le_to_be(dev_handle, 640, 480, frames, fb_cnt);
  perf_cvt_422_10_pg2_le_to_be(dev_handle, 1280, 720, frames, fb_cnt);
  perf_cvt_422_10_pg2_le_to_be(dev_handle, 1920, 1080, frames, fb_cnt);
  perf_cvt_422_10_pg2_le_to_be(dev_handle, 1920 * 2, 1080 * 2, frames, fb_cnt);
  perf_cvt_422_10_pg2_le_to_be(dev_handle, 1920 * 4, 1080 * 4, frames, fb_cnt);

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
