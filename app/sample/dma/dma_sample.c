/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <openssl/sha.h>

#include "../sample_util.h"

static inline void rand_data(uint8_t* p, size_t sz, uint8_t base) {
  for (size_t i = 0; i < sz; i++) {
    p[i] = rand() + base;
  }
}

static int dma_copy_sample(mtl_handle st) {
  mtl_udma_handle dma;
  int ret;
  uint16_t nb_desc = 1024;
  int nb_elements = nb_desc * 8, element_size = 1260;
  size_t fb_size = element_size * nb_elements;
  int fb_dst_iova_off = 0, fb_src_iova_off = 0;

  /* create user dma dev */
  dma = mtl_udma_create(st, nb_desc, MTL_PORT_P);
  if (!dma) {
    err("dma create fail\n");
    return -EIO;
  }

  void *fb_dst = NULL, *fb_src = NULL;
  mtl_iova_t fb_dst_iova, fb_src_iova;
  unsigned char fb_dst_shas[SHA256_DIGEST_LENGTH];
  unsigned char fb_src_shas[SHA256_DIGEST_LENGTH];

  /* allocate fb dst and src(with random data) */
  fb_dst = mtl_hp_malloc(st, fb_size, MTL_PORT_P);
  if (!fb_dst) {
    err("fb dst create fail\n");
    mtl_udma_free(dma);
    return -ENOMEM;
  }
  fb_dst_iova = mtl_hp_virt2iova(st, fb_dst);
  fb_src = mtl_hp_malloc(st, fb_size, MTL_PORT_P);
  if (!fb_src) {
    err("fb src create fail\n");
    mtl_hp_free(st, fb_dst);
    mtl_udma_free(dma);
    return -ENOMEM;
  }
  fb_src_iova = mtl_hp_virt2iova(st, fb_src);
  rand_data((uint8_t*)fb_src, fb_size, 0);
  st_sha256((unsigned char*)fb_src, fb_size, fb_src_shas);

  uint64_t start_ns = mtl_ptp_read_time(st);
  while (fb_dst_iova_off < fb_size) {
    /* try to copy */
    while (fb_src_iova_off < fb_size) {
      ret = mtl_udma_copy(dma, fb_dst_iova + fb_src_iova_off,
                          fb_src_iova + fb_src_iova_off, element_size);
      if (ret < 0) break;
      fb_src_iova_off += element_size;
    }
    /* submit */
    mtl_udma_submit(dma);

    /* do any other job*/

    /* check complete */
    uint16_t nb_dq = mtl_udma_completed(dma, 32);
    fb_dst_iova_off += element_size * nb_dq;
  }
  uint64_t end_ns = mtl_ptp_read_time(st);

  /* all copy completed, check sha */
  st_sha256((unsigned char*)fb_dst, fb_size, fb_dst_shas);
  ret = memcmp(fb_dst_shas, fb_src_shas, SHA256_DIGEST_LENGTH);
  if (ret != 0) {
    err("sha check fail\n");
  } else {
    info("dma copy %" PRIu64 "k with time %dus\n", fb_size / 1024,
         (int)(end_ns - start_ns) / 1000);
  }

  mtl_hp_free(st, fb_dst);
  mtl_hp_free(st, fb_src);

  ret = mtl_udma_free(dma);
  return 0;
}

static int dma_map_copy_sample(mtl_handle st) {
  mtl_udma_handle dma = NULL;
  int ret = -EIO;
  uint16_t nb_desc = 1024;
  int nb_elements = nb_desc * 8, element_size = 1260;
  size_t fb_size = element_size * nb_elements;
  size_t pg_sz = mtl_page_size(st);
  /* 2 more pages to hold the head and tail */
  size_t fb_size_malloc = fb_size + 2 * pg_sz;
  int fb_dst_iova_off = 0, fb_src_iova_off = 0;

  /* create user dma dev */
  dma = mtl_udma_create(st, nb_desc, MTL_PORT_P);
  if (!dma) {
    err("%s: dma create fail\n", __func__);
    return -EIO;
  }

  void *fb_dst_malloc = NULL, *fb_src_malloc = NULL;
  void *fb_dst = NULL, *fb_src = NULL;
  mtl_iova_t fb_dst_iova = MTL_BAD_IOVA, fb_src_iova = MTL_BAD_IOVA;
  unsigned char fb_dst_shas[SHA256_DIGEST_LENGTH];
  unsigned char fb_src_shas[SHA256_DIGEST_LENGTH];

  /* allocate fb dst and src(with random data) */
  fb_dst_malloc = malloc(fb_size_malloc);
  if (!fb_dst_malloc) {
    err("%s: fb dst malloc fail\n", __func__);
    ret = -ENOMEM;
    goto out;
  }
  fb_dst = (void*)MTL_ALIGN((uint64_t)fb_dst_malloc, pg_sz);
  fb_dst_iova = mtl_dma_map(st, fb_dst, fb_size);
  if (fb_dst_iova == MTL_BAD_IOVA) {
    err("%s: fb dst mmap fail\n", __func__);
    ret = -EIO;
    goto out;
  }

  fb_src_malloc = malloc(fb_size_malloc);
  if (!fb_src_malloc) {
    err("%s: fb src malloc fail\n", __func__);
    ret = -ENOMEM;
    goto out;
  }
  fb_src = (void*)MTL_ALIGN((uint64_t)fb_src_malloc, pg_sz);
  fb_src_iova = mtl_dma_map(st, fb_src, fb_size);
  if (fb_src_iova == MTL_BAD_IOVA) {
    err("%s: fb src mmap fail\n", __func__);
    ret = -EIO;
    goto out;
  }

  rand_data((uint8_t*)fb_src, fb_size, 0);
  st_sha256((unsigned char*)fb_src, fb_size, fb_src_shas);

  uint64_t start_ns = mtl_ptp_read_time(st);
  while (fb_dst_iova_off < fb_size) {
    /* try to copy */
    while (fb_src_iova_off < fb_size) {
      ret = mtl_udma_copy(dma, fb_dst_iova + fb_src_iova_off,
                          fb_src_iova + fb_src_iova_off, element_size);
      if (ret < 0) break;
      fb_src_iova_off += element_size;
    }
    /* submit */
    mtl_udma_submit(dma);

    /* do any other job*/

    /* check complete */
    uint16_t nb_dq = mtl_udma_completed(dma, 32);
    fb_dst_iova_off += element_size * nb_dq;
  }
  uint64_t end_ns = mtl_ptp_read_time(st);

  /* all copy completed, check sha */
  st_sha256((unsigned char*)fb_dst, fb_size, fb_dst_shas);
  ret = memcmp(fb_dst_shas, fb_src_shas, SHA256_DIGEST_LENGTH);
  if (ret != 0) {
    err("%s: sha check fail\n", __func__);
  } else {
    info("%s: dma map copy %" PRIu64 "k with time %dus\n", __func__, fb_size / 1024,
         (int)(end_ns - start_ns) / 1000);
  }

out:
  if (fb_src_malloc) {
    if (fb_src_iova != MTL_BAD_IOVA) mtl_dma_unmap(st, fb_src, fb_src_iova, fb_size);
    free(fb_src_malloc);
  }
  if (fb_dst_malloc) {
    if (fb_dst_iova != MTL_BAD_IOVA) mtl_dma_unmap(st, fb_dst, fb_dst_iova, fb_size);
    free(fb_dst_malloc);
  }
  if (dma) mtl_udma_free(dma);
  return ret;
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

  /* dma copy with st_hp_*** memory */
  ret = dma_copy_sample(ctx.st);
  if (ret < 0) goto exit;
  /* dma copy with malloc/free memory, use map before passing to DMA */
  ret = dma_map_copy_sample(ctx.st);
  if (ret < 0) goto exit;

exit:
  /* release sample(st) dev */
  if (ctx.st) {
    mtl_uninit(ctx.st);
    ctx.st = NULL;
  }
  return ret;
}
