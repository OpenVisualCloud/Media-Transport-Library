/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

#include "ipmx_info.h"

#include "../mt_log.h"
#include "../mt_util.h"
#include "../st2110/st_tx_video_session.h"

#define IPMX_INFO_TAG (0x5831)
#define IPMX_MEDIA_INFO_VIDEO (0x0001)
#define IPMX_INFO_HDR_LEN (84)
#define IPMX_MEDIA_INFO_VIDEO_LEN (92)

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
  /* TR-10-1 section 10.4 localmac form, the PTP form is not tracked yet */
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
  buf[4] = info->version;
  memset(&buf[5], 0, 3);
  memcpy(&buf[8], info->ts_refclk, sizeof(info->ts_refclk));
  memcpy(&buf[72], info->mediaclk, sizeof(info->mediaclk));

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
