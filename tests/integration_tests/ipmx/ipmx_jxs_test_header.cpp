/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifdef MTL_HAS_IPMX

#include "ipmx_jxs_test_header.h"

#include <string.h>

/* ISO/IEC 21122-2 Table A.8, the TR-08 section 8.1.1 levels 1k-1, 2k-1, 4k-2, 8k-2 */
static const struct {
  uint8_t plev;
  double w_max, h_max, l_max, rs_max;
} ipmx_test_jxs_levels[] = {
    {0x04, 1280, 5120, 2621440, 83558400},
    {0x10, 2048, 8192, 4194304, 133693440},
    {0x24, 4096, 16384, 16777216, 534773760},
    {0x34, 8192, 32768, 67108864, 2139095040},
};

static uint16_t ipmx_test_jxs_plev(const struct st22_encoder_create_req* req) {
  double samples = (double)req->width * req->height;
  double bpp = req->max_codestream_size * 8.0 / samples;
  uint8_t sublev = bpp <= 3.0 ? 0x04 : 0x06; /* Sublev3bpp, Sublev4bpp */

  for (const auto& l : ipmx_test_jxs_levels) {
    if (req->width <= l.w_max && req->height <= l.h_max && samples <= l.l_max &&
        samples * st_frame_rate(req->fps) <= l.rs_max)
      return (l.plev << 8) | sublev;
  }
  return 0;
}

static uint8_t* ipmx_test_put16(uint8_t* p, uint16_t v) {
  p[0] = v >> 8;
  p[1] = v & 0xff;
  return p + 2;
}

size_t ipmx_test_jxs_header(uint8_t* dst, size_t dst_size,
                            const struct st22_encoder_create_req* req) {
  if (dst_size < IPMX_TEST_JXS_HDR_LEN) return 0;
  uint8_t* p = dst;
  uint32_t lcod = req->max_codestream_size;

  p = ipmx_test_put16(p, 0xff10); /* SOC */
  p = ipmx_test_put16(p, 0xff50); /* CAP, no capability bits */
  p = ipmx_test_put16(p, 4);
  p = ipmx_test_put16(p, 0);
  p = ipmx_test_put16(p, 0xff12); /* PIH, ISO/IEC 21122-1 Table A.7 */
  p = ipmx_test_put16(p, 26);
  p = ipmx_test_put16(p, lcod >> 16);
  p = ipmx_test_put16(p, lcod & 0xffff);
  p = ipmx_test_put16(p, 0x4a40); /* High444.12 */
  p = ipmx_test_put16(p, ipmx_test_jxs_plev(req));
  p = ipmx_test_put16(p, req->width);
  p = ipmx_test_put16(p, req->height);
  p = ipmx_test_put16(p, 0);      /* Cw */
  p = ipmx_test_put16(p, 4);      /* Hsl */
  *p++ = 3;                       /* Nc */
  *p++ = 4;                       /* Ng */
  *p++ = 8;                       /* Ss */
  *p++ = 20;                      /* Bw */
  *p++ = 0x84;                    /* Fq 8, Br 4 */
  *p++ = 0x01;                    /* Fslc 0, Ppoc 0, Cpih 1 */
  *p++ = 0x52;                    /* NL,x 5, NL,y 2 */
  *p++ = 0x10;                    /* Qpih 1 */
  p = ipmx_test_put16(p, 0xff13); /* CDT, B 8 and sx = sy = 1 for R, G, B */
  p = ipmx_test_put16(p, 8);
  for (int c = 0; c < 3; c++) {
    *p++ = 8;
    *p++ = 0x11;
  }
  return p - dst;
}

#endif
