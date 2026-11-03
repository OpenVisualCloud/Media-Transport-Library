/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

#include "ipmx_jxs.h"

#include "../st2110/st_main.h"
#include "ipmx.h"

#define IPMX_JXS_SOC (0xff10)
#define IPMX_JXS_EOC (0xff11)
#define IPMX_JXS_PIH (0xff12)
#define IPMX_JXS_CDT (0xff13)
#define IPMX_JXS_SLH (0xff20)
#define IPMX_JXS_CAP (0xff50)
/* ISO/IEC 21122-1 Table A.7: Lpih 26 */
#define IPMX_JXS_PIH_LEN (26)
/* TR-08 section 8.1.1: NL,x 5, NL,y 2, Qpih 1 uniform */
#define IPMX_JXS_NLX (5)
#define IPMX_JXS_NLY (2)
#define IPMX_JXS_QPIH (1)
/* TR-08 section 8.1.1 Table 1 RGB: Cpih 1 */
#define IPMX_JXS_CPIH (1)
/* RGB 4:4:4 8-bit: Nc 3, B 8, sx = sy = 1 */
#define IPMX_JXS_NC (3)
#define IPMX_JXS_B (8)
#define IPMX_JXS_SXSY (0x11)
/* ISO/IEC 21122-3 Table A.9/A.10: valid, bit depth 8, RGB 4:4:4 */
#define IPMX_JXS_SCHAR (0x8072)

/* ISO/IEC 21122-2 Table A.8, only the levels TR-08 section 8.1.1 allows */
static const struct ipmx_jxs_level {
  uint8_t plev;
  uint32_t w_max;
  uint32_t h_max;
  uint64_t l_max;
  uint64_t rs_max; /* samples per second */
} ipmx_jxs_levels[] = {
    {0x04, 1280, 5120, 2621440, 83558400},        /* 1k-1 */
    {0x10, 2048, 8192, 4194304, 133693440},       /* 2k-1 */
    {0x24, 4096, 16384, 16777216, 534773760},     /* 4k-2 */
    {0x34, 8192, 32768, 67108864, 2139095040ULL}, /* 8k-2 */
};

/* TR-08 section 8.1.1 Sublev3bpp / Sublev4bpp; 0x06 is reserved in ISO/IEC
 * 21122-2:2022 Table A.16, the code is the VSF ipmx_jxsv_validate_pcap.py
 * SDP_SUBLEVEL_TO_PLEV_LO one */
#define IPMX_JXS_SUBLEV_3BPP (0x04)
#define IPMX_JXS_SUBLEV_4BPP (0x06)

int ipmx_jxs_plev(uint32_t width, uint32_t height, enum st_fps fps, size_t codestream_max,
                  uint16_t* plev) {
  struct st_fps_timing fps_tm;
  uint64_t samples = (uint64_t)width * height;
  uint64_t bits = (uint64_t)codestream_max * 8;
  uint8_t sublev;

  if (!samples || st_get_fps_timing(fps, &fps_tm) < 0) return -ENOTSUP;

  if (bits <= samples * 3)
    sublev = IPMX_JXS_SUBLEV_3BPP;
  else if (bits <= samples * 4)
    sublev = IPMX_JXS_SUBLEV_4BPP;
  else
    return -ENOTSUP;

  for (size_t i = 0; i < MTL_ARRAY_SIZE(ipmx_jxs_levels); i++) {
    const struct ipmx_jxs_level* l = &ipmx_jxs_levels[i];

    if (width > l->w_max || height > l->h_max || samples > l->l_max) continue;
    if (samples * fps_tm.mul > l->rs_max * fps_tm.den) continue;
    *plev = (l->plev << 8) | sublev;
    return 0;
  }
  return -ENOTSUP;
}

int ipmx_jxs_boxes_fill(struct st22_boxes* boxes, uint32_t width, uint32_t height,
                        enum st_fps fps, size_t codestream_max) {
  struct st_fps_timing fps_tm;
  uint16_t plev;
  int ret;

  ret = ipmx_jxs_plev(width, height, fps, codestream_max, &plev);
  if (ret < 0) return ret;
  st_get_fps_timing(fps, &fps_tm);

  /* ISO/IEC 21122-3 A.5.3.2 and TR-08 section 8.1.2.1: the maximum, in Mbit/s */
  uint64_t frame_bits = 8 * (sizeof(*boxes) + (uint64_t)codestream_max);
  uint64_t den = (uint64_t)fps_tm.den * 1000 * 1000;
  struct st22_jpvi* jpvi = &boxes->jpvs.jpvi;
  jpvi->brat = htonl((frame_bits * fps_tm.mul + den - 1) / den);
  jpvi->schar = htons(IPMX_JXS_SCHAR);

  boxes->jpvs.jxpl.ppih = htons(IPMX_JXS_PPIH);
  boxes->jpvs.jxpl.plev = htons(plev);

  /* ISO/IEC 21122-3 Table A.20/A.21: CP 1, TC 1 BT.709, MC 0 identity, V narrow */
  static const uint8_t methdat[7] = {0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00};
  memcpy(boxes->colr.methdat, methdat, sizeof(methdat));
  return 0;
}

static inline uint16_t ipmx_jxs_u16(const uint8_t* p) {
  return (p[0] << 8) | p[1];
}

static int ipmx_jxs_pih_body_check(const uint8_t* p, size_t size, uint32_t width,
                                   uint32_t height, uint16_t plev) {
  /* offsets after Lpih, ISO/IEC 21122-1 Table A.7 */
  uint16_t pih_plev = ipmx_jxs_u16(p + 6);
  uint8_t sublev = pih_plev & 0xff;

  if (ipmx_jxs_u16(p + 4) != IPMX_JXS_PPIH) return -IPMX_JXS_PIH_PPIH;
  if (!(pih_plev >> 8) || (pih_plev >> 8) != (plev >> 8)) return -IPMX_JXS_PIH_LEVEL;
  /* RFC 9134 section 3.3: the box Plev is a limit, the codestream may need less */
  if ((sublev != IPMX_JXS_SUBLEV_3BPP && sublev != IPMX_JXS_SUBLEV_4BPP) ||
      sublev > (plev & 0xff))
    return -IPMX_JXS_PIH_SUBLEVEL;
  /* TR-08 section 8.1.1: above 3 bpp, up to 4, is Sublev4bpp */
  if (sublev != IPMX_JXS_SUBLEV_4BPP && (uint64_t)size * 8 > (uint64_t)width * height * 3)
    return -IPMX_JXS_PIH_SUBLEVEL_RATE;
  if (ipmx_jxs_u16(p + 8) != width) return -IPMX_JXS_PIH_WF;
  if (ipmx_jxs_u16(p + 10) != height) return -IPMX_JXS_PIH_HF;
  if (p[16] != IPMX_JXS_NC) return -IPMX_JXS_PIH_NC;
  if ((p[21] & 0x0f) != IPMX_JXS_CPIH) return -IPMX_JXS_PIH_CPIH;
  if ((p[22] >> 4) != IPMX_JXS_NLX) return -IPMX_JXS_PIH_NLX;
  if ((p[22] & 0x0f) != IPMX_JXS_NLY) return -IPMX_JXS_PIH_NLY;
  if (((p[23] >> 4) & 0x3) != IPMX_JXS_QPIH) return -IPMX_JXS_PIH_QPIH;
  return 0;
}

static int ipmx_jxs_cdt_check(const uint8_t* p, uint16_t len) {
  if (len != 2 * IPMX_JXS_NC + 2) return -IPMX_JXS_PIH_LCDT;
  for (int c = 0; c < IPMX_JXS_NC; c++) {
    if (p[2 * c] != IPMX_JXS_B) return -IPMX_JXS_PIH_B;
    if (p[2 * c + 1] != IPMX_JXS_SXSY) return -IPMX_JXS_PIH_SAMPLING;
  }
  return 0;
}

/* the marker segment at off: its marker, and its length that counts itself */
static int ipmx_jxs_segment(const uint8_t* cs, size_t size, size_t off, uint16_t* marker,
                            uint16_t* len) {
  if (size - off < 4) return -IPMX_JXS_PIH_TRUNCATED;
  *marker = ipmx_jxs_u16(cs + off);
  *len = ipmx_jxs_u16(cs + off + 2);
  if ((*marker >> 8) != 0xff || *len < 2) return -IPMX_JXS_PIH_BAD_SEGMENT;
  if (size - off - 2 < *len) return -IPMX_JXS_PIH_TRUNCATED;
  return 0;
}

int ipmx_jxs_pih_check(const uint8_t* cs, size_t size, uint32_t width, uint32_t height,
                       uint16_t plev) {
  uint16_t marker, len;
  size_t off = 2;
  int ret;

  /* ISO/IEC 21122-1 A.4.1, A.4.3, A.4.4: SOC, CAP and PIH come first, in this order */
  if (size < 2) return -IPMX_JXS_PIH_TRUNCATED;
  if (ipmx_jxs_u16(cs) != IPMX_JXS_SOC) return -IPMX_JXS_PIH_NO_SOC;
  ret = ipmx_jxs_segment(cs, size, off, &marker, &len);
  if (ret < 0) return ret;
  if (marker != IPMX_JXS_CAP) return -IPMX_JXS_PIH_NO_CAP;
  off += 2 + len;

  ret = ipmx_jxs_segment(cs, size, off, &marker, &len);
  if (ret < 0) return ret;
  if (marker != IPMX_JXS_PIH) return -IPMX_JXS_PIH_NO_PIH;
  if (len != IPMX_JXS_PIH_LEN) return -IPMX_JXS_PIH_LPIH;
  ret = ipmx_jxs_pih_body_check(cs + off + 4, size, width, height, plev);
  if (ret < 0) return ret;
  off += 2 + len;

  /* A.4.5: the CDT precedes the first slice, other segments may come before it */
  while (true) {
    if (size - off >= 2) {
      marker = ipmx_jxs_u16(cs + off);
      if (marker == IPMX_JXS_SLH || marker == IPMX_JXS_EOC) return -IPMX_JXS_PIH_NO_CDT;
      if (marker == IPMX_JXS_SOC) return -IPMX_JXS_PIH_REPEATED;
    }
    ret = ipmx_jxs_segment(cs, size, off, &marker, &len);
    if (ret < 0) return ret;
    if (marker == IPMX_JXS_CAP || marker == IPMX_JXS_PIH) return -IPMX_JXS_PIH_REPEATED;
    if (marker == IPMX_JXS_CDT) return ipmx_jxs_cdt_check(cs + off + 4, len);
    off += 2 + len;
  }
}

static const char* const ipmx_jxs_pih_err_names[] = {
    [IPMX_JXS_PIH_TRUNCATED] = "truncated",
    [IPMX_JXS_PIH_NO_SOC] = "no SOC",
    [IPMX_JXS_PIH_NO_CAP] = "no CAP after SOC",
    [IPMX_JXS_PIH_NO_PIH] = "no PIH after CAP",
    [IPMX_JXS_PIH_LPIH] = "Lpih",
    [IPMX_JXS_PIH_REPEATED] = "second SOC, CAP or PIH",
    [IPMX_JXS_PIH_BAD_SEGMENT] = "bad segment",
    [IPMX_JXS_PIH_PPIH] = "Ppih",
    [IPMX_JXS_PIH_LEVEL] = "level",
    [IPMX_JXS_PIH_SUBLEVEL] = "sublevel",
    [IPMX_JXS_PIH_SUBLEVEL_RATE] = "sublevel below the frame bpp",
    [IPMX_JXS_PIH_WF] = "Wf",
    [IPMX_JXS_PIH_HF] = "Hf",
    [IPMX_JXS_PIH_NC] = "Nc",
    [IPMX_JXS_PIH_CPIH] = "Cpih",
    [IPMX_JXS_PIH_NLX] = "NL,x",
    [IPMX_JXS_PIH_NLY] = "NL,y",
    [IPMX_JXS_PIH_QPIH] = "Qpih",
    [IPMX_JXS_PIH_NO_CDT] = "no CDT",
    [IPMX_JXS_PIH_LCDT] = "Lcdt",
    [IPMX_JXS_PIH_B] = "component B",
    [IPMX_JXS_PIH_SAMPLING] = "sx/sy",
};

int ipmx_jxs_frame_check(const char* name, int idx, const struct st22_boxes* boxes,
                         const void* codestream, size_t size, uint32_t width,
                         uint32_t height, uint32_t* fail_cnt) {
  int ret =
      ipmx_jxs_pih_check(codestream, size, width, height, ntohs(boxes->jpvs.jxpl.plev));
  if (ret >= 0) return 0;

  if (!(*fail_cnt)++)
    err("%s(%s,%d), frame dropped, JPEG XS header check fail: %s, TR-08 section 8.1.1 "
        "needs High444.12 RGB 8-bit matching the session\n",
        __func__, name, idx, ipmx_jxs_pih_err_names[-ret]);
  else
    dbg("%s(%s,%d), frame dropped, %s\n", __func__, name, idx,
        ipmx_jxs_pih_err_names[-ret]);
  return ret;
}
