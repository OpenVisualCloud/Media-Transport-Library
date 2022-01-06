/*
 * Copyright (C) 2020-2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

/*
 *
 *	Intel(R) ST Media Streaming Library
 *
 */

#ifndef _PACK_H
#define _PACK_H
#ifndef WINDOWSENV
#include <arpa/inet.h>
#endif
#include <stdint.h>

#ifndef __LITTLE_ENDIAN_BITFIELDS
#define __LITTLE_ENDIAN_BITFIELDS 1
#endif

/**
 * \struct rgba_8b
 *
 * \brief structure conatining info about colors in RGBA pixel format
 */
struct rgba_8b {
  uint8_t r; /**< 8-bit red color representation */
  uint8_t g; /**< 8-bit green color representation */
  uint8_t b; /**< 8-bit blue color representation */
  uint8_t a; /**< 8-bit alpha channel representation */
} __attribute__((__packed__));

typedef struct rgba_8b rgba_8b_t; /**< Type of the structure \ref rgba_8b. */

/**
 * \struct st_rfc4175_422_10_pg2
 *
 * \brief Structure describing two image pixels in YUV 4:2:2 10-bit format
 *
 \verbatim
 0               1               2               3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 | CB00 (10 bits)    | Y00 (10 bits)     | CR00 (10 bits)    | Y01
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 | Y01 (contd) |
 +-+-+-+-+-+-+-+-+
 \endverbatim
 *
 */
struct rfc4175_422_10_pg2 {
  uint8_t Cb00; /**< Blue-difference chrominance (8-bits) */
#if __LITTLE_ENDIAN_BITFIELDS
  uint8_t Y00 : 6;   /**< First Luminance (6-bits) */
  uint8_t Cb00_ : 2; /**< Blue-difference chrominance (2-bits)
                                             as complement to the 10-bits */

  uint8_t Cr00 : 4; /**< Red-difference chrominance */
  uint8_t Y00_ : 4; /**< First Luminance (4-bits) as complement
                                            to the 10-bits */

  uint8_t Y01 : 2;   /**< Second Luminance (2-bits) */
  uint8_t Cr00_ : 6; /**< Red-difference chrominance (6-bits)
                                             as complement to the 10-bits */
#else
  uint8_t Cb00_ : 2;
  uint8_t Y00 : 6;

  uint8_t Y00_ : 4;
  uint8_t Cr00 : 4;

  uint8_t Cr00_ : 6;
  uint8_t Y01 : 2;
#endif
  uint8_t Y01_; /**< Secoond Luminance (8-bits)
                                        as complement to the 10-bits*/
} __attribute__((__packed__));

struct rfc4175_422_10_pg2_le {
  uint8_t Cb00; /**< Blue-difference chrominance (8-bits) */
#if __LITTLE_ENDIAN_BITFIELDS
  uint8_t Cb00_ : 2;
  uint8_t Y00 : 6;

  uint8_t Y00_ : 4;
  uint8_t Cr00 : 4;

  uint8_t Cr00_ : 6;
  uint8_t Y01 : 2;
#else
  uint8_t Y00 : 6;   /**< First Luminance (6-bits) */
  uint8_t Cb00_ : 2; /**< Blue-difference chrominance (2-bits)
                                             as complement to the 10-bits */

  uint8_t Cr00 : 4; /**< Red-difference chrominance */
  uint8_t Y00_ : 4; /**< First Luminance (4-bits) as complement
                                            to the 10-bits */

  uint8_t Y01 : 2;   /**< Second Luminance (2-bits) */
  uint8_t Cr00_ : 6; /**< Red-difference chrominance (6-bits)
                                             as complement to the 10-bits */
#endif
  uint8_t Y01_; /**< Secoond Luminance (8-bits)
                                        as complement to the 10-bits*/
} __attribute__((__packed__));

typedef struct rfc4175_422_10_pg2
    rfc4175_422_10_pg2_t; /**< Type of the structure \ref rfc4175_422_10_pg2. */

typedef struct rfc4175_422_10_pg2_le
    rfc4175_422_10_pg2_le_t; /**< Type of the structure \ref rfc4175_422_10_pg2. */

/**
 * Function converting (packing) string of image pixels into the pixel group
 * Unpacked 422 10 bit fields in packets are in BE, PG2 will be in BE
 \verbatim
 0               1               2               3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 | CB00 (10 bits) | Y00 (10 bits) | CR00 (10 bits) |Y01
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 | Y01 (contd) |
 +-+-+-+-+-+-+-+-+
 \endverbatim
 *
 * @param pg 	- OUT buffer with packed pixels
 * @param cb00 	- IN parameter of blue-difference chrominance component (Cb)
 * @param y00 	- IN parameter of first luminance (Y)
 * @param cr00 	- IN parameter of red-difference chrominance component (Cr)
 * @param y01 	- IN parameter of second luminance (Y)
 *
 * @return no return
 */
static inline void Pack_422be10_PG2be(rfc4175_422_10_pg2_t* pg, uint16_t cb00,
                                      uint16_t y00, uint16_t cr00, uint16_t y01) {
  uint16_t cb0 = ntohs(cb00);
  uint16_t y0 = ntohs(y00);
  uint16_t cr0 = ntohs(cr00);
  uint16_t y1 = ntohs(y01);

  pg->Cb00 = (cb0 >> 2) & 0xff;
  pg->Cb00_ = cb0 & 0x3;
  pg->Y00 = (y0 >> 4) & 0x3f;
  pg->Y00_ = y0 & 0x0f;
  pg->Cr00 = (cr0 >> 6) & 0x0f;
  pg->Cr00_ = cr0 & 0x3f;
  pg->Y01 = (y1 >> 8) & 0x03;
  pg->Y01_ = y1 & 0xff;
}

/**
 * Function converting (packing) string of pixels of the image into the pixel group
 * Unpacked 422 10 bit fields in packets are in LE, PG2 will be in BE
 \verbatim
 0               1               2               3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 | CB00 (10 bits) | Y00 (10 bits) | CR00 (10 bits) |Y01
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 | Y01 (contd) |
 +-+-+-+-+-+-+-+-+
 \endverbatim
 *
 * @param pg 	- OUT buffer with packed pixels
 * @param cb0 	- IN parameter of blue-difference chrominance component (Cb)
 * @param y0 	- IN parameter of first luminance (Y)
 * @param cr0 	- IN parameter of red-difference chrominance component (Cr)
 * @param y1 	- IN parameter of second luminance (Y)
 *
 * @return no return
 */
static inline void Pack_422le10_PG2be(rfc4175_422_10_pg2_t* pg, uint16_t cb0, uint16_t y0,
                                      uint16_t cr0, uint16_t y1) {
  pg->Cb00 = (cb0 >> 2) & 0xff;
  pg->Cb00_ = cb0 & 0x3;
  pg->Y00 = (y0 >> 4) & 0x3f;
  pg->Y00_ = y0 & 0x0f;
  pg->Cr00 = (cr0 >> 6) & 0x0f;
  pg->Cr00_ = cr0 & 0x3f;
  pg->Y01 = (y1 >> 8) & 0x03;
  pg->Y01_ = y1 & 0xff;
}

/**
 * Function converting (packing) string of image pixels into the pixel group
 * Unpacked 422 10 bit fields in packets are in LE, PG2 will be in LE
 \verbatim
 0				1				2
 3 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 | CB00 (10 bits) | Y00 (10 bits) | CR00 (10 bits) |Y01
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 | Y01 (contd) |
 +-+-+-+-+-+-+-+-+
 \endverbatim
 *
 * @param pg 	- OUT buffer with packed pixels
 * @param cb00 	- IN parameter of blue-difference chrominance component (Cb)
 * @param y00 	- IN parameter of first luminance (Y)
 * @param cr00 	- IN parameter of red-difference chrominance component (Cr)
 * @param y01 	- IN parameter of second luminance (Y)
 *
 * @return no return
 */
static inline void Pack_422le10_PG2le(rfc4175_422_10_pg2_le_t* pg, uint16_t cb00,
                                      uint16_t y00, uint16_t cr00, uint16_t y01) {
  pg->Cb00 = cb00 & 0xff;
  pg->Cb00_ = (cb00 & 0x300) >> 8;
  pg->Y00 = y00 & 0x3f;
  pg->Y00_ = (y00 & 0x3c0) >> 6;
  pg->Cr00 = cr00 & 0x0f;
  pg->Cr00_ = (cr00 & 0x3f0) >> 4;
  pg->Y01 = y01 & 0x03;
  pg->Y01_ = (y01 & 0x3fc) >> 2;
}

/**
 * Function converting (packing) string of image pixels into the pixel group
 * Unpacked 422 10 bit fields in packets are in BE, PG2 will be in LE
 \verbatim
 0				1				2
 3 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 | CB00 (10 bits) | Y00 (10 bits) | CR00 (10 bits) |Y01
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 | Y01 (contd) |
 +-+-+-+-+-+-+-+-+
 \endverbatim
 *
 * @param pg 	- OUT buffer with packed pixels
 * @param cb00 	- IN parameter of blue-difference chrominance component (Cb)
 * @param y00 	- IN parameter of first luminance (Y)
 * @param cr00 	- IN parameter of red-difference chrominance component (Cr)
 * @param y01 	- IN parameter of second luminance (Y)
 *
 * @return no return
 */
static inline void Pack_422be10_PG2le(rfc4175_422_10_pg2_le_t* pg, uint16_t cb00,
                                      uint16_t y00, uint16_t cr00, uint16_t y01) {
  uint16_t cb0 = ntohs(cb00);
  uint16_t y0 = ntohs(y00);
  uint16_t cr0 = ntohs(cr00);
  uint16_t y1 = ntohs(y01);

  pg->Cb00 = cb0 & 0xff;
  pg->Cb00_ = (cb0 & 0x300) >> 8;
  pg->Y00 = y0 & 0x3f;
  pg->Y00_ = (y0 & 0x3c0) >> 6;
  pg->Cr00 = cr0 & 0x0f;
  pg->Cr00_ = (cr0 & 0x3f0) >> 4;
  pg->Y01 = y1 & 0x03;
  pg->Y01_ = (y1 & 0x3fc) >> 2;
}

/**
 * Function converting (unpacking) pixel group into the string of image pixels
 * PG2 fields in packets are in BE, result will be in LE
 \verbatim
 0               1               2               3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 | CB00 (10 bits) | Y00 (10 bits) | CR00 (10 bits) |Y01
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 | Y01 (contd) |
 +-+-+-+-+-+-+-+-+
 \endverbatim
 *
 * @param pg 	- IN buffer with packed pixels
 * @param cb00 	- OUT parameter of blue-difference chrominance component (Cb)
 * @param y00 	- OUT parameter of first luminance (Y)
 * @param cr00 	- OUT parameter of red-difference chrominance component (Cr)
 * @param y01 	- OUT parameter of second luminance (Y)
 *
 * @return no return
 */
static inline void Unpack_PG2be_422le10(rfc4175_422_10_pg2_t const* pg, uint16_t* cb00,
                                        uint16_t* y00, uint16_t* cr00, uint16_t* y01) {
  uint16_t cb, y0, cr, y1;
  cb = (pg->Cb00 << 2) + pg->Cb00_;
  y0 = (pg->Y00 << 4) + pg->Y00_;
  cr = (pg->Cr00 << 6) + pg->Cr00_;
  y1 = (pg->Y01 << 8) + pg->Y01_;

  *cb00 = cb;
  *y00 = y0;
  *cr00 = cr;
  *y01 = y1;
}

/**
 * Function converting (unpacking) pixel group into the string of image pixels
 * PG2 fields in packets are in BE, result will be in BE
 \verbatim
 0               1               2               3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 | CB00 (10 bits) | Y00 (10 bits) | CR00 (10 bits) |Y01
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 | Y01 (contd) |
 +-+-+-+-+-+-+-+-+
 \endverbatim
 *
 * @param pg 	- IN buffer with packed pixels
 * @param cb00 	- OUT parameter of blue-difference chrominance component (Cb)
 * @param y00 	- OUT parameter of first luminance (Y)
 * @param cr00 	- OUT parameter of red-difference chrominance component (Cr)
 * @param y01 	- OUT parameter of second luminance (Y)
 *
 * @return no return
 */
static inline void Unpack_PG2be_422be10(rfc4175_422_10_pg2_t const* pg, uint16_t* cb00,
                                        uint16_t* y00, uint16_t* cr00, uint16_t* y01) {
  uint16_t cb, y0, cr, y1;
  cb = (pg->Cb00 << 2) + pg->Cb00_;
  y0 = (pg->Y00 << 4) + pg->Y00_;
  cr = (pg->Cr00 << 6) + pg->Cr00_;
  y1 = (pg->Y01 << 8) + pg->Y01_;

  *cb00 = htons(cb);
  *y00 = htons(y0);
  *cr00 = htons(cr);
  *y01 = htons(y1);
}

/**
 * Function converting (unpacking) pixel group into the string of image pixels
 * PG2 fields in packets are in LE, result will be in BE
 \verbatim
 0               1               2               3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 | CB00 (10 bits) | Y00 (10 bits) | CR00 (10 bits) |Y01
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 | Y01 (contd) |
 +-+-+-+-+-+-+-+-+
 \endverbatim
 *
 * @param pg 	- IN buffer with packed pixels
 * @param cb00 	- OUT parameter of blue-difference chrominance component (Cb)
 * @param y00 	- OUT parameter of first luminance (Y)
 * @param cr00 	- OUT parameter of red-difference chrominance component (Cr)
 * @param y01 	- OUT parameter of second luminance (Y)
 *
 * @return no return
 */
static inline void Unpack_PG2le_422be10(rfc4175_422_10_pg2_le_t const* pg, uint16_t* cb00,
                                        uint16_t* y00, uint16_t* cr00, uint16_t* y01) {
  uint16_t cb, y0, cr, y1;
  cb = pg->Cb00 + (pg->Cb00_ << 8);
  y0 = pg->Y00 + (pg->Y00_ << 6);
  cr = pg->Cr00 + (pg->Cr00_ << 4);
  y1 = pg->Y01 + (pg->Y01_ << 2);

  *cb00 = htons(cb);
  *y00 = htons(y0);
  *cr00 = htons(cr);
  *y01 = htons(y1);
}

/**
 * Function converting (unpacking) pixel group into the string of image pixels
 * PG2 fields in packets are in LE, result will be in LE
 \verbatim
 0               1               2               3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 | CB00 (10 bits) | Y00 (10 bits) | CR00 (10 bits) |Y01
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 | Y01 (contd) |
 +-+-+-+-+-+-+-+-+
 \endverbatim
 *
 * @param pg 	- IN buffer with packed pixels
 * @param cb00 	- OUT parameter of blue-difference chrominance component (Cb)
 * @param y00 	- OUT parameter of first luminance (Y)
 * @param cr00 	- OUT parameter of red-difference chrominance component (Cr)
 * @param y01 	- OUT parameter of second luminance (Y)
 *
 * @return no return
 */
static inline void Unpack_PG2le_422le10(rfc4175_422_10_pg2_le_t const* pg, uint16_t* cb00,
                                        uint16_t* y00, uint16_t* cr00, uint16_t* y01) {
  uint16_t cb, y0, cr, y1;
  cb = pg->Cb00 + (pg->Cb00_ << 8);
  y0 = pg->Y00 + (pg->Y00_ << 6);
  cr = pg->Cr00 + (pg->Cr00_ << 4);
  y1 = pg->Y01 + (pg->Y01_ << 2);

  *cb00 = cb;
  *y00 = y0;
  *cr00 = cr;
  *y01 = y1;
}
#endif
