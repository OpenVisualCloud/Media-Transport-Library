/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

#ifndef _IPMX_H_
#define _IPMX_H_

#include <errno.h>

#include "../mt_log.h"
#include "st20_api.h"

#ifdef MTL_HAS_IPMX
/** Validate st20 TX ops against the IPMX rules, 0 if compliant else negative errno. */
int ipmx_tx_ops_check(const struct st20_tx_ops* ops);
/** Validate st20 RX ops against the IPMX rules, 0 if compliant else negative errno. */
int ipmx_rx_ops_check(const struct st20_rx_ops* ops);
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

static inline int ipmx_udp_ports_check(const char* name, const uint16_t* udp_port,
                                       int num_port) {
  MTL_MAY_UNUSED(name);
  MTL_MAY_UNUSED(udp_port);
  MTL_MAY_UNUSED(num_port);
  return -ENOTSUP;
}
#endif

#endif
