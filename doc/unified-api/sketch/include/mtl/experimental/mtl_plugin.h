/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* Hand-formatted: clang-format stays off so the milestone tags keep their place
   (check.sh lint 5). */
/* clang-format off */
/*
 * mtl_plugin.h - codec and converter plugin ABI, version 2 (revision 0.2).
 *
 * A plugin is a shared object exporting one symbol, MTL_PLUGIN_ENTRY_SYMBOL. It never
 * links libmtl: the host passes it a function table. Plugin threads wait for work in
 * host->get_work(); no plugin code runs on an MTL tasklet. Every struct versions with
 * struct_size. Formats are explicit pairs, not bitmasks.
 */

#ifndef MTL_EXPERIMENTAL_MTL_PLUGIN_H
#define MTL_EXPERIMENTAL_MTL_PLUGIN_H

#include "mtl.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define MTL_PLUGIN_ABI_VERSION 2u

enum mtl_plugin_kind {
  MTL_PLUGIN_ENCODER = 1, /* raw frames to a codestream (ST 2110-22 TX) */
  MTL_PLUGIN_DECODER = 2, /* codestream to raw frames (ST 2110-22 RX) */
  MTL_PLUGIN_CONVERTER = 3, /* raw to raw (formats MTL does not convert itself) */
};
/* Plugin formats carry their number space: a transport format of mtl.h (below 0x10000),
   MTL_PLUGIN_APP(an mtl_format.h app format) or MTL_PLUGIN_CODESTREAM(MTL_CODEC_*); never a
   bare app value. */
#define MTL_PLUGIN_CODESTREAM(codec) (0x10000u | (uint32_t)(codec))
#define MTL_PLUGIN_APP(fmt) (0x20000u | (uint32_t)(fmt))

struct mtl_plugin_plane {
  MTL_ADDR(void) addr;
  uint64_t iova; /* for DMA-capable plugins */
  uint64_t bytes;
  uint32_t stride;
  uint32_t reserved;
};
/* Lent by the host between get_work and put_work. */
struct mtl_plugin_frame {
  uint32_t struct_size;
  uint32_t format; /* a plugin format (above) */
  uint32_t width;
  uint32_t height;
  uint32_t plane_count;
  uint32_t second_field;
  uint64_t data_bytes;  /* codestream: valid bytes in (decoder) or produced out (encoder) */
  int64_t media_tai_ns; /* carried through unchanged */
  struct mtl_plugin_plane plane[MTL_MAX_PLANES];
  uint64_t reserved[4];
};
struct mtl_plugin_session_req {
  uint32_t struct_size;
  uint32_t kind; /* enum mtl_plugin_kind */
  uint32_t in_format;
  uint32_t out_format;
  uint32_t width;
  uint32_t height;
  struct mtl_rational fps;
  uint32_t scan;
  uint32_t frame_count;
  uint32_t threads; /* MTL_OPT_CVIDEO_THREADS; 0 = plugin default */
  uint32_t quality;
  uint32_t numa; /* MTL_INDEX(node) */
  uint32_t reserved0;
  uint64_t codestream_bytes; /* encoder: the CBR target per unit */
  uint64_t reserved[4];
};
/* Valid from create_session until free_session returns. */
struct mtl_plugin_host {
  uint32_t struct_size;
  uint32_t abi_version;
  /* Blocks up to timeout for the next job: 0 with in and out set. */
  int (*get_work)(void* host_session, struct mtl_plugin_frame** in,
                  struct mtl_plugin_frame** out, int64_t timeout_ns);
  /* result: 0 or a negative MTL_E*. */
  int (*put_work)(void* host_session, struct mtl_plugin_frame* in,
                  struct mtl_plugin_frame* out, int result);
  void (*log)(void* host_session, uint32_t level, const char* line);
  uint64_t reserved[8];
};
struct mtl_plugin_format_pair {
  uint32_t in_format;
  uint32_t out_format;
};
struct mtl_plugin_device {
  uint32_t struct_size;
  uint32_t kind;          /* enum mtl_plugin_kind */
  const char* name;       /* "jpegxs-svt" */
  uint32_t device;        /* MTL_CODEC_DEVICE_* of mtl_options.h */
  /* the device struct and what it points to stay valid until mtl_plugin_unload returns */
  uint32_t pair_count;
  const struct mtl_plugin_format_pair* pairs;
  void* dev_priv;
  int (*create_session)(void* dev_priv, const struct mtl_plugin_session_req* req,
                        const struct mtl_plugin_host* host, void* host_session,
                        void** session_priv);
  int (*free_session)(void* dev_priv, void* session_priv);
  uint64_t reserved[4];
};

/* The only symbol a plugin exports; < 0 if it cannot serve host_abi. */
#define MTL_PLUGIN_ENTRY_SYMBOL "mtl_plugin_entry_v2"
typedef int (*mtl_plugin_entry_fn)(uint32_t host_abi, uint32_t* plugin_abi,
                                   const struct mtl_plugin_device* const** devs,
                                   uint32_t* count);

/* Host side (applications, not plugins). */
typedef struct mtl_plugin_h {
  uint64_t id;
} mtl_plugin_h;
/* Exactly one of path and dev (else -MTL_EINVAL): path loads a plugin .so into the
   instance; dev registers an in-process device (tests, apps that embed a codec). CP.
   (MS4) */
MTL_API_CP(4) int mtl_plugin_open(mtl_instance_h mt, const char* MTL_NULLABLE path,
                                  const struct mtl_plugin_device* MTL_NULLABLE dev,
                                  mtl_plugin_h* out);
/* -MTL_EBUSY while a session uses it (p is then not consumed). CP. */
static inline int mtl_plugin_unload(mtl_plugin_h p) {
  return mtl_close(mtl_obj(MTL_OBJ_PLUGIN, 0, p.id), 0);
}

MTL_SIZE_CHECK(mtl_plugin_plane, 32);
MTL_SIZE_CHECK(mtl_plugin_frame, 200);
MTL_SIZE_CHECK(mtl_plugin_session_req, 104);
MTL_SIZE_CHECK(mtl_plugin_host, 96);
MTL_SIZE_CHECK(mtl_plugin_format_pair, 8);
MTL_SIZE_CHECK(mtl_plugin_device, 88);
MTL_SIZE_CHECK(mtl_plugin_h, 8);

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_PLUGIN_H */
