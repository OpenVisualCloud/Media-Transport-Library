/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* A hand-formatted design sketch: formatted when it moves to include/ (milestone M0). */
/* clang-format off */
/*
 * mtl_convert.h - standalone colour and sample conversion, revision 0.2.
 *
 * The same SIMD converters the sessions use, for applications that convert outside a
 * session (file tools, test pattern generators). One call replaces today's ~105 per-pair
 * functions; the pair, the CPU features and an optional DMA engine are chosen inside.
 * Pure functions on the caller's thread: no instance is needed unless DMA is wanted.
 */

#ifndef MTL_EXPERIMENTAL_MTL_CONVERT_H
#define MTL_EXPERIMENTAL_MTL_CONVERT_H

#include "mtl.h"

#if defined(__cplusplus)
extern "C" {
#endif

struct mtl_convert_image {
  uint32_t format; /* transport format (mtl.h) or app format (mtl_format.h) */
  uint32_t is_app; /* 1 = app format */
  uint32_t plane_count;
  uint32_t reserved;
  struct mtl_plane plane[MTL_MAX_PLANES];
};
#define MTL_CONVERT_NO_SIMD 0x1u     /* scalar reference path (tests) */
#define MTL_CONVERT_FIELD_SPLIT 0x2u /* a frame in, two fields out (dst planes 0-1, 2-3) */
#define MTL_CONVERT_FIELD_MERGE 0x4u /* two fields in, one woven frame out */
#define MTL_CONVERT_HALF_SCALE 0x8u  /* downsample by 2 in both directions */
struct mtl_convert_desc {
  uint32_t struct_size;
  uint32_t width;
  uint32_t height;
  uint32_t reserved0;
  struct mtl_convert_image src;
  struct mtl_convert_image dst;
  uint64_t flags;    /* MTL_CONVERT_* */
  mtl_instance_h mt; /* null = CPU only; else may use the instance's DMA engines */
  uint64_t reserved[4];
};
/* -MTL_ENOTSUP for a pair MTL does not convert. DPC. */
MTL_API_DPC int mtl_convert(const struct mtl_convert_desc* d);

/* ST 2110-31 AM824 subframe: byte 0 = B (block start) bit 5, F (frame start) bit 4,
   V, U, C, P bits 3-0; bytes 1-3 = the 24-bit sample, big endian. */
struct mtl_am824_subframe {
  uint8_t flags;
  uint8_t sample[3];
};
/* ST 2110-31: AM824 subframes <-> AES3 subframes. DPC. */
MTL_API_DPC int mtl_convert_am824_to_aes3(const void* am824, void* aes3, uint32_t subframes);
MTL_API_DPC int mtl_convert_aes3_to_am824(const void* aes3, void* am824, uint32_t subframes);

#if defined(MTL_LATER)
/* Channel selection and order in one copy (IS-08 maps, applied by the application at a
   media index): dst channel i = src channel map[i], or silence for MTL_AUDIO_SILENT.
   Interleaved samples of one format; bytes per sample from it. DPC. */
#define MTL_AUDIO_SILENT 0xffffu
MTL_API_DPC int mtl_audio_remap(void* dst, uint32_t dst_channels, const void* src,
                                uint32_t src_channels, const uint16_t* map, uint32_t samples,
                                uint32_t format);
#endif

MTL_SIZE_CHECK(mtl_convert_image, 112);
MTL_SIZE_CHECK(mtl_am824_subframe, 4);
MTL_SIZE_CHECK(mtl_convert_desc, 288);

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_CONVERT_H */
