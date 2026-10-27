/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

#include "ipmx.h"

/* TR-10-2 section 7: the port shall be even and above 1024, should be above 5000 */
#define IPMX_UDP_PORT_MIN (1025)
#define IPMX_UDP_PORT_PREFERRED_MIN (5001)

int ipmx_udp_ports_check(const char* name, const uint16_t* udp_port, int num_port) {
  for (int i = 0; i < num_port; i++) {
    uint16_t port = udp_port[i];

    if (!port) continue; /* the library default 10000 + 2 * idx is compliant */
    if (port & 1) {
      err("%s(%s,%d), udp port %u is odd, TR-10-2 section 7\n", __func__, name, i, port);
      return -EINVAL;
    }
    if (port < IPMX_UDP_PORT_MIN) {
      err("%s(%s,%d), udp port %u <= 1024, TR-10-2 section 7\n", __func__, name, i, port);
      return -EINVAL;
    }
    if (port < IPMX_UDP_PORT_PREFERRED_MIN)
      warn("%s(%s,%d), udp port %u <= 5000, TR-10-2 section 7 recommends above 5000\n",
           __func__, name, i, port);
  }
  return 0;
}

static int ipmx_video_check(const char* name, enum st20_type type, bool interlaced) {
  if (type == ST20_TYPE_RTP_LEVEL) {
    err("%s(%s), ST20_TYPE_RTP_LEVEL not supported, the TR-10-1 section 8.8.2 sender "
        "report schedule needs library built packets\n",
        __func__, name);
    return -ENOTSUP;
  }
  if (interlaced) {
    err("%s(%s), interlaced not supported by the IPMX MVP\n", __func__, name);
    return -ENOTSUP;
  }
  return 0;
}

int ipmx_tx_ops_check(const struct st20_tx_ops* ops) {
  const char* name = ops->name ? ops->name : "";
  int ret;

  ret = ipmx_udp_ports_check(name, ops->udp_port, ops->num_port);
  if (ret < 0) return ret;

  if (ops->pacing == ST21_PACING_LINEAR) {
    err("%s(%s), ST21_PACING_LINEAR not allowed, TR-10-1 section 8.1\n", __func__, name);
    return -EINVAL;
  }

  return ipmx_video_check(name, ops->type, ops->interlaced);
}

int ipmx_rx_ops_check(const struct st20_rx_ops* ops) {
  const char* name = ops->name ? ops->name : "";
  int ret;

  ret = ipmx_udp_ports_check(name, ops->udp_port, ops->num_port);
  if (ret < 0) return ret;

  return ipmx_video_check(name, ops->type, ops->interlaced);
}
