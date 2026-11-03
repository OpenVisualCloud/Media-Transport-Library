/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

#include "ipmx.h"
#include "ipmx_jxs.h"

/* TR-10-2/TR-10-11 section 7: the port shall be even and above 1024, should be above 5000
 */
#define IPMX_UDP_PORT_MIN (1025)
#define IPMX_UDP_PORT_PREFERRED_MIN (5001)

int ipmx_udp_ports_check(const char* name, const uint16_t* udp_port, int num_port) {
  for (int i = 0; i < num_port; i++) {
    uint16_t port = udp_port[i];

    if (!port) continue; /* the library default 10000 + 2 * idx is compliant */
    if (port & 1) {
      err("%s(%s,%d), udp port %u is odd, TR-10-2/TR-10-11 section 7\n", __func__, name,
          i, port);
      return -EINVAL;
    }
    if (port < IPMX_UDP_PORT_MIN) {
      err("%s(%s,%d), udp port %u <= 1024, TR-10-2/TR-10-11 section 7\n", __func__, name,
          i, port);
      return -EINVAL;
    }
    if (port < IPMX_UDP_PORT_PREFERRED_MIN)
      warn(
          "%s(%s,%d), udp port %u <= 5000, TR-10-2/TR-10-11 section 7 recommends above "
          "5000\n",
          __func__, name, i, port);
  }
  return 0;
}

static int ipmx_video_check(const char* name, enum st20_type type, enum st20_fmt fmt,
                            bool interlaced) {
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
  if (fmt == ST20_FMT_V210 || fmt == ST20_FMT_YUV_422_PLANAR10LE) {
    err("%s(%s), fmt %d not allowed, no ST 2110-20 section 7.4.1 sampling\n", __func__,
        name, fmt);
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

  if (ops->flags & ST20_TX_FLAG_ENABLE_RTCP) {
    err("%s(%s), ST20_TX_FLAG_ENABLE_RTCP not supported, its NACK port is media port + "
        "1, which TR-10-1 section 8.7 reserves for the sender report\n",
        __func__, name);
    return -ENOTSUP;
  }

  return ipmx_video_check(name, ops->type, ops->fmt, ops->interlaced);
}

int ipmx_rx_ops_check(const struct st20_rx_ops* ops) {
  const char* name = ops->name ? ops->name : "";
  int ret;

  ret = ipmx_udp_ports_check(name, ops->udp_port, ops->num_port);
  if (ret < 0) return ret;

  if (ops->flags & ST20_RX_FLAG_ENABLE_RTCP) {
    err("%s(%s), ST20_RX_FLAG_ENABLE_RTCP not supported, its NACK port is media port + "
        "1, which TR-10-1 section 8.7 reserves for the sender report\n",
        __func__, name);
    return -ENOTSUP;
  }

  return ipmx_video_check(name, ops->type, ops->fmt, ops->interlaced);
}

int ipmx_st22_tx_ops_check(const struct st22_tx_ops* ops) {
  const char* name = ops->name ? ops->name : "";
  int ret;

  ret = ipmx_udp_ports_check(name, ops->udp_port, ops->num_port);
  if (ret < 0) return ret;

  if (ops->pacing == ST21_PACING_LINEAR) {
    err("%s(%s), ST21_PACING_LINEAR not allowed, TR-10-11 sections 7 and 11 shape "
        "traffic per the TR-10-1 section 8.1 network compatibility model, IPMX "
        "receivers need not support linear senders\n",
        __func__, name);
    return -EINVAL;
  }

  if (ops->flags & ST22_TX_FLAG_ENABLE_RTCP) {
    err("%s(%s), ST22_TX_FLAG_ENABLE_RTCP not supported, its NACK port is media port + "
        "1, which TR-10-1 section 8.7 reserves for the sender report\n",
        __func__, name);
    return -ENOTSUP;
  }

  if (ops->flags & ST22_TX_FLAG_DISABLE_BOXES) {
    err("%s(%s), ST22_TX_FLAG_DISABLE_BOXES not allowed, RFC 9134 sections 3.4 and 4.1 "
        "packetize the boxes with the codestream\n",
        __func__, name);
    return -ENOTSUP;
  }

  if (ops->type == ST22_TYPE_RTP_LEVEL) {
    err("%s(%s), ST22_TYPE_RTP_LEVEL not supported, the TR-10-1 section 8.8.2 sender "
        "report schedule needs library built packets\n",
        __func__, name);
    return -ENOTSUP;
  }

  if (ops->interlaced) {
    err("%s(%s), interlaced not supported by the IPMX MVP\n", __func__, name);
    return -ENOTSUP;
  }

  uint16_t plev;
  if (ipmx_jxs_plev(ops->width, ops->height, ops->fps, ops->framebuff_max_size, &plev) <
      0) {
    err("%s(%s), %ux%u fps %d framebuff_max_size %zu not supported, IPMX sends up to "
        "4 bpp at a TR-08 section 8.1.1 level\n",
        __func__, name, ops->width, ops->height, ops->fps, ops->framebuff_max_size);
    return -ENOTSUP;
  }

  return 0;
}

int ipmx_st22_rx_ops_check(const struct st22_rx_ops* ops) {
  const char* name = ops->name ? ops->name : "";
  int ret;

  ret = ipmx_udp_ports_check(name, ops->udp_port, ops->num_port);
  if (ret < 0) return ret;

  if (ops->flags & ST22_RX_FLAG_ENABLE_RTCP) {
    err("%s(%s), ST22_RX_FLAG_ENABLE_RTCP not supported, its NACK port is media port + "
        "1, which TR-10-1 section 8.7 reserves for the sender report\n",
        __func__, name);
    return -ENOTSUP;
  }

  if (ops->flags & ST22_RX_FLAG_DISABLE_BOXES) {
    err("%s(%s), ST22_RX_FLAG_DISABLE_BOXES not allowed, RFC 9134 sections 3.4 and 4.1 "
        "packetize the boxes with the codestream\n",
        __func__, name);
    return -ENOTSUP;
  }

  if (ops->type == ST22_TYPE_RTP_LEVEL) {
    err("%s(%s), ST22_TYPE_RTP_LEVEL not supported, kept out for the planned st22 sender "
        "report receive\n",
        __func__, name);
    return -ENOTSUP;
  }

  if (ops->interlaced) {
    err("%s(%s), interlaced not supported by the IPMX MVP\n", __func__, name);
    return -ENOTSUP;
  }

  return 0;
}

int ipmx_st22p_codec_check(const char* name, enum st22_codec codec) {
  if (codec != ST22_CODEC_JPEGXS) {
    err("%s(%s), codec %d not allowed, IPMX st22p supports JPEG XS only\n", __func__,
        name ? name : "", codec);
    return -ENOTSUP;
  }
  return 0;
}

int ipmx_st22p_tx_ops_check(const struct st22p_tx_ops* ops) {
  int ret = ipmx_st22p_codec_check(ops->name, ops->codec);
  if (ret < 0) return ret;

  if (ops->input_fmt != ST_FRAME_FMT_GBRPLANAR8) {
    err("%s(%s), input_fmt %s not allowed, the IPMX MVP st22p is GBRPLANAR8 only, "
        "RGB 4:4:4 8-bit\n",
        __func__, ops->name ? ops->name : "", st_frame_fmt_name(ops->input_fmt));
    return -ENOTSUP;
  }
  return 0;
}
