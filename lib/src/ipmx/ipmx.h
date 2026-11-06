/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef _IPMX_H_
#define _IPMX_H_

#include <errno.h>

#include "../mt_log.h"
#include "st20_api.h"
#include "st_pipeline_api.h"

/* TR-10-9 section 16: DSCP AF42 (36), ECN 0 */
#define IPMX_VIDEO_IPV4_TOS (36 << 2)

struct st22_boxes;

#ifdef MTL_HAS_IPMX
/** Validate st20 TX ops against the IPMX rules, 0 if compliant else negative errno. */
int ipmx_tx_ops_check(const struct st20_tx_ops* ops);
/** Validate st20 RX ops against the IPMX rules, 0 if compliant else negative errno. */
int ipmx_rx_ops_check(const struct st20_rx_ops* ops);
/** Validate st22 TX ops against the IPMX rules, 0 if compliant else negative errno. */
int ipmx_st22_tx_ops_check(const struct st22_tx_ops* ops);
/** Validate st22 RX ops against the IPMX rules, 0 if compliant else negative errno. */
int ipmx_st22_rx_ops_check(const struct st22_rx_ops* ops);
/** Check an st22p codec against the IPMX rules, 0 for JPEG XS else -ENOTSUP. */
int ipmx_st22p_codec_check(const char* name, enum st22_codec codec);
/** Check st22p TX ops against the IPMX rules: JPEG XS of GBRPLANAR8, 0 else -ENOTSUP. */
int ipmx_st22p_tx_ops_check(const struct st22p_tx_ops* ops);
/**
 * Write the TR-08 IPMX values of a session into its st22 boxes: brat, schar, jxpl and
 * colr. 0, or -ENOTSUP when ipmx_jxs_plev() finds no Plev.
 */
int ipmx_jxs_boxes_fill(struct st22_boxes* boxes, uint32_t width, uint32_t height,
                        enum st_fps fps, size_t codestream_max);
/**
 * ipmx_jxs_pih_check() of one TX frame against the session and its boxes Plev, data
 * plane safe. A failure increments *fail_cnt and logs err() only when *fail_cnt was 0.
 */
int ipmx_jxs_frame_check(const char* name, int idx, const struct st22_boxes* boxes,
                         const void* codestream, size_t size, uint32_t width,
                         uint32_t height, uint32_t* fail_cnt);
/**
 * Check udp_port[0..num_port) against the IPMX port rule, 0 or -EINVAL. A port pruned
 * at create because its link is down is never checked, nor ever used.
 */
int ipmx_udp_ports_check(const char* name, const uint16_t* udp_port, int num_port);
#else
static inline int ipmx_tx_ops_check(const struct st20_tx_ops* ops) {
  MTL_MAY_UNUSED(ops);
  err("%s, no IPMX support in this build, rebuild with -Denable_ipmx=true\n", __func__);
  return -ENOTSUP;
}

static inline int ipmx_rx_ops_check(const struct st20_rx_ops* ops) {
  MTL_MAY_UNUSED(ops);
  err("%s, no IPMX support in this build, rebuild with -Denable_ipmx=true\n", __func__);
  return -ENOTSUP;
}

static inline int ipmx_st22_tx_ops_check(const struct st22_tx_ops* ops) {
  MTL_MAY_UNUSED(ops);
  err("%s, no IPMX support in this build, rebuild with -Denable_ipmx=true\n", __func__);
  return -ENOTSUP;
}

static inline int ipmx_st22_rx_ops_check(const struct st22_rx_ops* ops) {
  MTL_MAY_UNUSED(ops);
  err("%s, no IPMX support in this build, rebuild with -Denable_ipmx=true\n", __func__);
  return -ENOTSUP;
}

static inline int ipmx_st22p_codec_check(const char* name, enum st22_codec codec) {
  MTL_MAY_UNUSED(name);
  MTL_MAY_UNUSED(codec);
  err("%s, no IPMX support in this build, rebuild with -Denable_ipmx=true\n", __func__);
  return -ENOTSUP;
}

static inline int ipmx_st22p_tx_ops_check(const struct st22p_tx_ops* ops) {
  MTL_MAY_UNUSED(ops);
  err("%s, no IPMX support in this build, rebuild with -Denable_ipmx=true\n", __func__);
  return -ENOTSUP;
}

static inline int ipmx_jxs_boxes_fill(struct st22_boxes* boxes, uint32_t width,
                                      uint32_t height, enum st_fps fps,
                                      size_t codestream_max) {
  MTL_MAY_UNUSED(boxes);
  MTL_MAY_UNUSED(width);
  MTL_MAY_UNUSED(height);
  MTL_MAY_UNUSED(fps);
  MTL_MAY_UNUSED(codestream_max);
  return -ENOTSUP;
}

static inline int ipmx_jxs_frame_check(const char* name, int idx,
                                       const struct st22_boxes* boxes,
                                       const void* codestream, size_t size,
                                       uint32_t width, uint32_t height,
                                       uint32_t* fail_cnt) {
  MTL_MAY_UNUSED(name);
  MTL_MAY_UNUSED(idx);
  MTL_MAY_UNUSED(boxes);
  MTL_MAY_UNUSED(codestream);
  MTL_MAY_UNUSED(size);
  MTL_MAY_UNUSED(width);
  MTL_MAY_UNUSED(height);
  MTL_MAY_UNUSED(fail_cnt);
  return -ENOTSUP;
}

static inline int ipmx_udp_ports_check(const char* name, const uint16_t* udp_port,
                                       int num_port) {
  MTL_MAY_UNUSED(name);
  MTL_MAY_UNUSED(udp_port);
  MTL_MAY_UNUSED(num_port);
  return -ENOTSUP;
}
#endif

#endif
