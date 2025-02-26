/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "../sample/sample_util.h"

static inline void rand_data(uint8_t* p, size_t sz, uint8_t base) {
  for (size_t i = 0; i < sz; i++) {
    p[i] = rand() + base;
  }
}

static int dma_copy_perf(mtl_handle st, int w, int h, int frames, int pkt_size) {
  mtl_udma_handle dma;
  int ret;
  uint16_t nb_desc = 1024;
  size_t fb_size = w * h * 5 / 2;            /* rfc4175_422be10 */
  fb_size = (fb_size / pkt_size) * pkt_size; /* align to pkt_size */
  float fb_size_m = (float)fb_size / 1024 / 1024;
  int fb_dst_iova_off = 0, fb_src_iova_off = 0;

  /* create user dma dev */
  dma = mtl_udma_create(st, nb_desc, MTL_PORT_P);
  if (!dma) {
    info("dma create fail\n");
    return -EIO;
  }

  void *fb_dst = NULL, *fb_src = NULL;
  mtl_iova_t fb_dst_iova, fb_src_iova;

  /* allocate fb dst and src(with random data) */
  fb_dst = mtl_hp_malloc(st, fb_size, MTL_PORT_P);
  if (!fb_dst) {
    info("fb dst create fail\n");
    mtl_udma_free(dma);
    return -ENOMEM;
  }
  fb_dst_iova = mtl_hp_virt2iova(st, fb_dst);
  fb_src = mtl_hp_malloc(st, fb_size, MTL_PORT_P);
  if (!fb_dst) {
    info("fb src create fail\n");
    mtl_hp_free(st, fb_dst);
    mtl_udma_free(dma);
    return -ENOMEM;
  }
  fb_src_iova = mtl_hp_virt2iova(st, fb_src);
  rand_data((uint8_t*)fb_src, fb_size, 0);

  clock_t start, end;
  float duration_cpu, duration_simd, duration_dma;

  start = clock();
  for (int idx = 0; idx < frames; idx++) {
    size_t copied_size = 0;
    while (copied_size < fb_size) {
      memcpy(fb_src + pkt_size, fb_dst + pkt_size, pkt_size);
      copied_size += pkt_size;
    }
  }
  end = clock();
  duration_cpu = (float)(end - start) / CLOCKS_PER_SEC;
  info("cpu, time: %f secs with %d frames(%dx%d,%fm), pkt_size %d\n", duration_cpu,
       frames, w, h, fb_size_m, pkt_size);

  start = clock();
  for (int idx = 0; idx < frames; idx++) {
    size_t copied_size = 0;
    while (copied_size < fb_size) {
      mtl_memcpy(fb_src + pkt_size, fb_dst + pkt_size, pkt_size);
      copied_size += pkt_size;
    }
  }
  end = clock();
  duration_simd = (float)(end - start) / CLOCKS_PER_SEC;
  info("simd, time: %f secs with %d frames(%dx%d,%fm), pkt_size %d\n", duration_simd,
       frames, w, h, fb_size_m, pkt_size);
  info("simd, %fx performance to cpu\n", duration_cpu / duration_simd);

  start = clock();
  for (int idx = 0; idx < frames; idx++) {
    while (fb_dst_iova_off < fb_size) {
      /* try to copy */
      while (fb_src_iova_off < fb_size) {
        ret = mtl_udma_copy(dma, fb_dst_iova + fb_src_iova_off,
                            fb_src_iova + fb_src_iova_off, pkt_size);
        if (ret < 0) break;
        fb_src_iova_off += pkt_size;
      }
      /* submit */
      mtl_udma_submit(dma);

      /* check complete */
      uint16_t nb_dq = mtl_udma_completed(dma, 32);
      fb_dst_iova_off += pkt_size * nb_dq;
    }
  }
  end = clock();
  duration_dma = (float)(end - start) / CLOCKS_PER_SEC;
  info("dma, time: %f secs with %d frames(%dx%d,%fm), pkt_size %d\n", duration_dma,
       frames, w, h, fb_size_m, pkt_size);
  info("dma, %fx performance to cpu\n", duration_cpu / duration_dma);
  info("\n");

  mtl_hp_free(st, fb_dst);
  mtl_hp_free(st, fb_src);

  ret = mtl_udma_free(dma);
  return 0;
}

int main(int argc, char** argv) {
  struct st_sample_context ctx;
  int ret;

  memset(&ctx, 0, sizeof(ctx));
  ret = dma_sample_parse_args(&ctx, argc, argv);
  if (ret < 0) return ret;

  ctx.st = mtl_init(&ctx.param);
  if (!ctx.st) {
    err("%s: mtl_init fail\n", __func__);
    return -EIO;
  }

  int frames = ctx.perf_frames;

  dma_copy_perf(ctx.st, 1920, 1080, frames, 128);
  dma_copy_perf(ctx.st, 1920 * 2, 1080 * 2, frames, 128);
  dma_copy_perf(ctx.st, 1920 * 4, 1080 * 4, frames, 128);
  info("\n");

  dma_copy_perf(ctx.st, 1920, 1080, frames, 1200);
  dma_copy_perf(ctx.st, 1920 * 2, 1080 * 2, frames, 1200);
  dma_copy_perf(ctx.st, 1920 * 4, 1080 * 4, frames, 1200);
  info("\n");

  dma_copy_perf(ctx.st, 1920, 1080, frames, 4096);
  dma_copy_perf(ctx.st, 1920 * 2, 1080 * 2, frames, 4096);
  dma_copy_perf(ctx.st, 1920 * 4, 1080 * 4, frames, 4096);
  info("\n");

  /* release sample(st) dev */
  if (ctx.st) {
    mtl_uninit(ctx.st);
    ctx.st = NULL;
  }
  return ret;
}
