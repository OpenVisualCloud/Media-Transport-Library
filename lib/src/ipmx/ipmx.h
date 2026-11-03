/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

#ifndef _IPMX_H_
#define _IPMX_H_

#include <errno.h>

#include "../mt_log.h"
#include "st20_api.h"
#include "st_pipeline_api.h"

/* TR-10-9 section 16: DSCP AF42 (36), ECN 0 */
#define IPMX_VIDEO_IPV4_TOS (36 << 2)

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

static inline int ipmx_udp_ports_check(const char* name, const uint16_t* udp_port,
                                       int num_port) {
  MTL_MAY_UNUSED(name);
  MTL_MAY_UNUSED(udp_port);
  MTL_MAY_UNUSED(num_port);
  return -ENOTSUP;
}
#endif

#endif
