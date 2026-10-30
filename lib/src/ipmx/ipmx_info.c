/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

#include "ipmx_info.h"

#include "../mt_log.h"
#include "../mt_util.h"
#include "../st2110/st_tx_video_session.h"

struct ipmx_info_sampling {
  const char* sampling;
  uint8_t depth;
};

/* ST 2110-20 section 7.4.1; V210 and planar 10-bit LE have no RFC 4175 pgroup */
static const struct ipmx_info_sampling ipmx_info_samplings[] = {
    [ST20_FMT_YUV_422_10BIT] = {"YCbCr-4:2:2", 10},
    [ST20_FMT_YUV_422_8BIT] = {"YCbCr-4:2:2", 8},
    [ST20_FMT_YUV_422_12BIT] = {"YCbCr-4:2:2", 12},
    [ST20_FMT_YUV_422_16BIT] = {"YCbCr-4:2:2", 16},
    [ST20_FMT_YUV_420_8BIT] = {"YCbCr-4:2:0", 8},
    [ST20_FMT_YUV_420_10BIT] = {"YCbCr-4:2:0", 10},
    [ST20_FMT_YUV_420_12BIT] = {"YCbCr-4:2:0", 12},
    [ST20_FMT_YUV_420_16BIT] = {"YCbCr-4:2:0", 16},
    [ST20_FMT_RGB_8BIT] = {"RGB", 8},
    [ST20_FMT_RGB_10BIT] = {"RGB", 10},
    [ST20_FMT_RGB_12BIT] = {"RGB", 12},
    [ST20_FMT_RGB_16BIT] = {"RGB", 16},
    [ST20_FMT_YUV_444_8BIT] = {"YCbCr-4:4:4", 8},
    [ST20_FMT_YUV_444_10BIT] = {"YCbCr-4:4:4", 10},
    [ST20_FMT_YUV_444_12BIT] = {"YCbCr-4:4:4", 12},
    [ST20_FMT_YUV_444_16BIT] = {"YCbCr-4:4:4", 16},
};

static inline void ipmx_info_put_be16(uint8_t* p, uint16_t v) {
  v = htons(v);
  memcpy(p, &v, sizeof(v));
}

static inline void ipmx_info_put_be32(uint8_t* p, uint32_t v) {
  v = htonl(v);
  memcpy(p, &v, sizeof(v));
}

int ipmx_video_info_from_session(struct mtl_main_impl* impl,
                                 struct st_tx_video_session_impl* s,
                                 struct ipmx_video_info* info) {
  enum mtl_port port = mt_port_logic2phy(s->port_maps, MTL_SESSION_PORT_P);
  const struct st20_tx_ops* ops = &s->ops;
  const struct st_fps_timing* fps = &s->fps_tm;
  struct rte_ether_addr mac;
  const uint8_t* m = mac.addr_bytes;
  int ret;

  if (ops->fmt >= RTE_DIM(ipmx_info_samplings)) {
    err("%s(%d), fmt %d has no ST 2110-20 section 7.4.1 sampling\n", __func__, s->idx,
        ops->fmt);
    return -ENOTSUP;
  }
  ret = mt_macaddr_get(impl, port, &mac);
  if (ret < 0) {
    err("%s(%d), mac get fail %d\n", __func__, s->idx, ret);
    return ret;
  }

  memset(info, 0, sizeof(*info));
  /* TR-10-1 section 10.4 localmac form, the SR switches to PTP when it applies */
  snprintf(info->ts_refclk, sizeof(info->ts_refclk),
           "localmac=%02X-%02X-%02X-%02X-%02X-%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  /* TR-10-1 section 10.5: the RTP clock derives from the Internal Clock */
  snprintf(info->mediaclk, sizeof(info->mediaclk), "direct=0");
  snprintf(info->sampling, sizeof(info->sampling), "%s",
           ipmx_info_samplings[ops->fmt].sampling);
  info->depth = ipmx_info_samplings[ops->fmt].depth;
  info->gpm = ops->packing != ST20_PACKING_BPM;
  /* TR-10-2 section 10 and TR-10-9 section 20 defaults, no ops to signal others */
  info->par_w = 1;
  info->par_h = 1;
  snprintf(info->range, sizeof(info->range), "NARROW");
  snprintf(info->colorimetry, sizeof(info->colorimetry), "BT709");
  snprintf(info->tcs, sizeof(info->tcs), "SDR");
  info->width = ops->width;
  info->height = ops->height;
  info->rate_num = fps->mul;
  info->rate_den = fps->den;
  /* TR-10-9 section 10, a non-baseband sender */
  info->pixclk =
      ((uint64_t)ops->width * ops->height * fps->mul + fps->den / 2) / fps->den;
  info->htotal = ops->width;
  info->vtotal = ops->height;
  return 0;
}

uint16_t ipmx_info_block_build(const struct ipmx_video_info* info, uint8_t* buf) {
  uint8_t* mib = buf + IPMX_INFO_HDR_LEN;
  uint64_t pixclk = info->pixclk;

  RTE_BUILD_BUG_ON(IPMX_INFO_HDR_LEN + IPMX_MEDIA_INFO_VIDEO_LEN != IPMX_INFO_BLOCK_LEN);
  ipmx_info_put_be16(&buf[0], IPMX_INFO_TAG);
  ipmx_info_put_be16(&buf[2], IPMX_INFO_BLOCK_LEN / 4 - 1);
  buf[IPMX_INFO_BLOCK_VERSION_OFFSET] = info->version;
  memset(&buf[IPMX_INFO_BLOCK_VERSION_OFFSET + 1], 0, 3);
  memcpy(&buf[IPMX_TS_REFCLK_OFFSET], info->ts_refclk, sizeof(info->ts_refclk));
  memcpy(&buf[IPMX_MEDIACLK_OFFSET], info->mediaclk, sizeof(info->mediaclk));

  ipmx_info_put_be16(&mib[0], IPMX_MEDIA_INFO_VIDEO);
  ipmx_info_put_be16(&mib[2], IPMX_MEDIA_INFO_VIDEO_LEN / 4 - 1);
  memcpy(&mib[4], info->sampling, sizeof(info->sampling));
  mib[20] = (info->floating << 7) | (info->depth & 0x7f);
  mib[21] = (info->gpm << 7) | (info->interlaced << 6) | (info->segmented << 5);
  mib[22] = info->par_w;
  mib[23] = info->par_h;
  memcpy(&mib[24], info->range, sizeof(info->range));
  memcpy(&mib[36], info->colorimetry, sizeof(info->colorimetry));
  memcpy(&mib[56], info->tcs, sizeof(info->tcs));
  ipmx_info_put_be16(&mib[72], info->width);
  ipmx_info_put_be16(&mib[74], info->height);
  ipmx_info_put_be32(&mib[76],
                     (info->rate_num & 0x3fffff) << 10 | (info->rate_den & 0x3ff));
  ipmx_info_put_be32(&mib[80], (uint32_t)(pixclk >> 32));
  ipmx_info_put_be32(&mib[84], (uint32_t)pixclk);
  ipmx_info_put_be16(&mib[88], info->htotal);
  ipmx_info_put_be16(&mib[90], info->vtotal);
  return IPMX_INFO_BLOCK_LEN;
}

struct ipmx_refclk ipmx_refclk_from_ptp(const struct ipmx_ptp_snap* snap,
                                        uint64_t now_ns) {
  struct ipmx_refclk clk = {0};
  int log_interval;
  uint64_t timeout_ns;

  /* RFC 7273: an IEEE1588-2008 ptp-domain is 0 to 127 */
  if (!snap->builtin || !snap->connected || !snap->announce_ns || snap->domain > 127)
    return clk;
  /* the IEEE 1588 default and the ST 2059-2 profile ranges */
  log_interval = RTE_MAX(-3, RTE_MIN(4, snap->log_announce_interval));
  timeout_ns = log_interval >= 0 ? (3ull * NS_PER_S) << log_interval
                                 : (3ull * NS_PER_S) >> -log_interval;
  /* signed: an Announce stamped on the PTP thread after now_ns was read is fresh */
  if ((int64_t)(now_ns - snap->announce_ns) >= (int64_t)timeout_ns) return clk;

  clk.ptp = true;
  clk.domain = snap->domain;
  clk.gm_id = snap->gm_id;
  return clk;
}

void ipmx_refclk_print(const struct ipmx_refclk* clk, const char* localmac,
                       char* ts_refclk) {
  const uint64_t id = clk->gm_id;

  if (!clk->ptp) {
    memcpy(ts_refclk, localmac, IPMX_TS_REFCLK_LEN);
    return;
  }
  memset(ts_refclk, 0, IPMX_TS_REFCLK_LEN);
  /* ST 2110-10 section 8.2 */
  snprintf(ts_refclk, IPMX_TS_REFCLK_LEN,
           "ptp=IEEE1588-2008:%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X:%u",
           (unsigned int)(id >> 56) & 0xff, (unsigned int)(id >> 48) & 0xff,
           (unsigned int)(id >> 40) & 0xff, (unsigned int)(id >> 32) & 0xff,
           (unsigned int)(id >> 24) & 0xff, (unsigned int)(id >> 16) & 0xff,
           (unsigned int)(id >> 8) & 0xff, (unsigned int)id & 0xff, clk->domain);
}
