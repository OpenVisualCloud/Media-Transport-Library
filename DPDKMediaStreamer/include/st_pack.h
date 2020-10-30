/*
* Copyright 2020 Intel Corporation.
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
*	Intel® ST 2110 Media Streaming Library
*
*/

#ifndef _ST_PACK_H
#define _ST_PACK_H

#include <arpa/inet.h>
#include <stdint.h>

#ifndef __LITTLE_ENDIAN_BITFIELDS
#define __LITTLE_ENDIAN_BITFIELDS 1
#endif

#define KILO 1000ul
#define MEGA 1000000ul
#define GIGA 1000000000ul

typedef union st_p210_yuv
{
	struct
	{
		uint16_t y0;
		uint16_t y1;
	} __attribute__((__packed__));
	struct
	{
		uint16_t cr;
		uint16_t cb;
	} __attribute__((__packed__));
} st_p210_yuv_t;

struct st_rgba_8b
{
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t a;
} __attribute__((__packed__));

typedef struct st_rgba_8b st_rgba_8b_t;

/*
0               1               2               3
0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| C’B00 (10 bits) | Y’00 (10 bits) | C’R00 (10 bits) |Y’01
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| Y’01 (cont’d) |
+-+-+-+-+-+-+-+-+

*/
struct st_rfc4175_422_10_pg2
{
	uint8_t Cb00;
#ifdef __LITTLE_ENDIAN_BITFIELDS
	uint8_t Y00 : 6;
	uint8_t Cb00_ : 2;

	uint8_t Cr00 : 4;
	uint8_t Y00_ : 4;

	uint8_t Y01 : 2;
	uint8_t Cr00_ : 6;
#else
	uint8_t Cb00_ : 2;
	uint8_t Y00 : 6;

	uint8_t Y00_ : 4;
	uint8_t Cr00 : 4;

	uint8_t Cr00_ : 6;
	uint8_t Y01 : 2;
#endif
	uint8_t Y01_;
} __attribute__((__packed__));

typedef struct st_rfc4175_422_10_pg2 st_rfc4175_422_10_pg2_t;

/*
0               1               2               3
0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| C’B00 (10 bits) | Y’00 (10 bits) | C’R00 (10 bits) |Y’01
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| Y’01 (cont’d) |
+-+-+-+-+-+-+-+-+
* Unpacked 422 10 bit fields in packets are in BE, PG2 shall be in BE
*/
static inline void
Pack_422be10_PG2be(st_rfc4175_422_10_pg2_t *pg, uint16_t cb00, uint16_t y00, uint16_t cr00,
				   uint16_t y01)
{
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

/*
0               1               2               3
0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| C’B00 (10 bits) | Y’00 (10 bits) | C’R00 (10 bits) |Y’01
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| Y’01 (cont’d) |
+-+-+-+-+-+-+-+-+
* Unpacked 422 10 bit fields in packets are in LE, PG2 shall be in BE
*/
static inline void
Pack_422le10_PG2be(st_rfc4175_422_10_pg2_t *pg, uint16_t cb0, uint16_t y0, uint16_t cr0,
				   uint16_t y1)
{
	pg->Cb00 = (cb0 >> 2) & 0xff;
	pg->Cb00_ = cb0 & 0x3;
	pg->Y00 = (y0 >> 4) & 0x3f;
	pg->Y00_ = y0 & 0x0f;
	pg->Cr00 = (cr0 >> 6) & 0x0f;
	pg->Cr00_ = cr0 & 0x3f;
	pg->Y01 = (y1 >> 8) & 0x03;
	pg->Y01_ = y1 & 0xff;
}

/*
0				1				2				3
0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| C’B00 (10 bits) | Y’00 (10 bits) | C’R00 (10 bits) |Y’01
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| Y’01 (cont’d) |
+-+-+-+-+-+-+-+-+
* Unpacked 422 10 bit fields in packets are in LE, PG2 shall be in BE
*/
static inline void
Pack_422le10_PG2le(st_rfc4175_422_10_pg2_t *pg, uint16_t cb00, uint16_t y00, uint16_t cr00,
				   uint16_t y01)
{
	pg->Cb00 = cb00 & 0xff;
	pg->Cb00_ = (cb00 & 0x300) >> 8;
	pg->Y00 = y00 & 0x3f;
	pg->Y00_ = (y00 & 0x3c0) >> 6;
	pg->Cr00 = cr00 & 0x0f;
	pg->Cr00_ = (cr00 & 0x3f0) >> 4;
	pg->Y01 = y01 & 0x03;
	pg->Y01_ = (y01 & 0x3fc) >> 2;
}

/*
0				1				2				3
0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| C’B00 (10 bits) | Y’00 (10 bits) | C’R00 (10 bits) |Y’01
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| Y’01 (cont’d) |
+-+-+-+-+-+-+-+-+
* Unpacked 422 10 bit fields in packets are in LE, PG2 shall be in BE
*/
static inline void
Pack_422be10_PG2le(st_rfc4175_422_10_pg2_t *pg, uint16_t cb00, uint16_t y00, uint16_t cr00,
				   uint16_t y01)
{
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

/*
0               1               2               3
0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| C’B00 (10 bits) | Y’00 (10 bits) | C’R00 (10 bits) |Y’01
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| Y’01 (cont’d) |
+-+-+-+-+-+-+-+-+
* PG2 fields in packets are in BE, result shall be in LE
*/
static inline void
Unpack_PG2be_422le10(st_rfc4175_422_10_pg2_t const *pg, uint16_t *cb00, uint16_t *y00,
					 uint16_t *cr00, uint16_t *y01)
{
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

/*
0               1               2               3
0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| C’B00 (10 bits) | Y’00 (10 bits) | C’R00 (10 bits) |Y’01
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| Y’01 (cont’d) |
+-+-+-+-+-+-+-+-+
* PG2 fields in packets are in BE, result shall be in BE
*/
static inline void
Unpack_PG2be_422be10(st_rfc4175_422_10_pg2_t const *pg, uint16_t *cb00, uint16_t *y00,
					 uint16_t *cr00, uint16_t *y01)
{
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

/*
0               1               2               3
0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| C’B00 (10 bits) | Y’00 (10 bits) | C’R00 (10 bits) |Y’01
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| Y’01 (cont’d) |
+-+-+-+-+-+-+-+-+
* PG2 fields in packets are in LE, result shall be in BE
*/
static inline void
Unpack_PG2le_422be10(st_rfc4175_422_10_pg2_t const *pg, uint16_t *cb00, uint16_t *y00,
					 uint16_t *cr00, uint16_t *y01)
{
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

/*
0               1               2               3
0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| C’B00 (10 bits) | Y’00 (10 bits) | C’R00 (10 bits) |Y’01
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| Y’01 (cont’d) |
+-+-+-+-+-+-+-+-+
* PG2 fields in packets are in LE, result shall be in LE
*/
static inline void
Unpack_PG2le_422le10(st_rfc4175_422_10_pg2_t const *pg, uint16_t *cb00, uint16_t *y00,
					 uint16_t *cr00, uint16_t *y01)
{
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
