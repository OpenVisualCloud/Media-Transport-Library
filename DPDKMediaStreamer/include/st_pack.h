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
*	Intel(R) ST 2110 Media Streaming Library
*
*/

#ifndef _ST_PACK_H
#define _ST_PACK_H

#include <arpa/inet.h>
#include <stdint.h>

#ifndef __LITTLE_ENDIAN_BITFIELDS
#define __LITTLE_ENDIAN_BITFIELDS 1
#endif

#define KILO 1000ul			/**< Multiplier 10^3*/
#define MEGA 1000000ul		/**< Multiplier 10^6*/
#define GIGA 1000000000ul	/**< Multiplier 10^9*/

/**
 * \struct st_rgba_8b
 *
 * \brief structure conatining info about colors in RGBA pixel format
 */
struct st_rgba_8b
{
	uint8_t r;		/**< 8-bit red color representation */
	uint8_t g;		/**< 8-bit green color representation */
	uint8_t b;		/**< 8-bit blue color representation */
	uint8_t a;		/**< 8-bit alpha channel representation */
} __attribute__((__packed__));

typedef struct st_rgba_8b st_rgba_8b_t; /**< Type of the structure \ref st_rgba_8b. */

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
struct st_rfc4175_422_10_pg2
{
	uint8_t Cb00;				/**< Blue-difference chrominance (8-bits) */
#ifdef __LITTLE_ENDIAN_BITFIELDS
	uint8_t Y00 : 6;			/**< First Luminance (6-bits) */
	uint8_t Cb00_ : 2;			/**< Blue-difference chrominance (2-bits) 
									as complement to the 10-bits */

	uint8_t Cr00 : 4;			/**< Red-difference chrominance */
	uint8_t Y00_ : 4;			/**< First Luminance (4-bits) as complement
									to the 10-bits */

	uint8_t Y01 : 2;			/**< Second Luminance (2-bits) */
	uint8_t Cr00_ : 6;			/**< Red-difference chrominance (6-bits)
									as complement to the 10-bits */
#else
	uint8_t Cb00_ : 2;
	uint8_t Y00 : 6;

	uint8_t Y00_ : 4;
	uint8_t Cr00 : 4;

	uint8_t Cr00_ : 6;
	uint8_t Y01 : 2;
#endif
	uint8_t Y01_;				/**< Secoond Luminance (8-bits) 
									as complement to the 10-bits*/
} __attribute__((__packed__));

typedef struct st_rfc4175_422_10_pg2 st_rfc4175_422_10_pg2_t; /**< Type of the structure \ref st_rfc4175_422_10_pg2. */

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

/**
 * Function converting (packing) string of image pixels into the pixel group
 * Unpacked 422 10 bit fields in packets are in LE, PG2 will be in LE
 \verbatim
 0				1				2				3
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

/**
 * Function converting (packing) string of image pixels into the pixel group
 * Unpacked 422 10 bit fields in packets are in BE, PG2 will be in LE
 \verbatim
 0				1				2				3
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

/**
 * \struct st_anc_pkt_payload_hdr
 * 
 * \brief Structure describe ancillary data payload header (RFC 8331)
 * 
 */
struct st_anc_pkt_payload_hdr
{
#ifdef __LITTLE_ENDIAN_BITFIELDS
	union
	{
		struct
		{
			uint32_t streamNum : 7;			/**< StreamNum field carrying identification of the source data
												stream number of the ANC data packet. Depends
												on S (Data Stream Flag) field (RFC 8331)*/
			uint32_t s : 1;					/**< Indicates whether the data stream number of a multi-stream
												data mapping used to transport the ANC data packet. 
												If the S bit is '0', then the StreamNum is not analyzed.
												If the S bit is '1', then the StreamNum field contains info
												about stream number in the ANC data packet. (RFC 8331) */
			uint32_t horizontalOffset : 12;	/**< Defines the location of the ANC data packet in the SDI raster 
												(RFC 8331) */
			uint32_t lineNumber : 11;		/**< Containing information about line number corresponds to the 
												location (vertical) of the ANC data packet in the SDI raster.
												(RFC 8331) */
			uint32_t c : 1;					/**< If the C bit is '0', the ANC data uses luma (Y)
												data channel. If the C bit is '0', the ANC data 
												uses color-difference data channel. (RFC 8331)*/
		} first_hdr_chunk;
		uint32_t swaped_first_hdr_chunk;	/**< Handle to make operating on first_hdr_chunk buffer easier. */
	};
	union
	{
		struct
		{
			uint32_t rsvdForUdw : 2;		/**< Starting point of the UDW (user data words). (RFC 8331)*/
			uint32_t dataCount : 10;		/**< The lower 8 bits of Data_Count, corresponding to bits b7 (MSB)
												through b0 (LSB) of the 10-bit Data_Count word, contain the actual
												count of 10-bit words in User_Data_Words. Bit b8 is the even parity
												for bits b7 through b0, and bit b9 is the inverse (logical NOT) 
												of bit b8. (RFC 8331) */
			uint32_t sdid : 10;				/**< Secondary Data Identification Word. Used only for a "Type 2" ANC
												data packet. Note that in a "Type 1" ANC data packet, this word
												will actually carry the Data Block Number (DBN). (RFC 8331) */
			uint32_t did : 10;				/**< Data Identification Word (RFC 8331) */
		} second_hdr_chunk;
		uint32_t swaped_second_hdr_chunk;	/**< Handle to make operating on second_hdr_chunk buffer easier. */
	};
#else
	uint32_t c : 1;
	uint32_t lineNumber : 11;
	uint32_t horizontalOffset : 12;
	uint32_t s : 1;
	uint32_t streamNum : 7;
	uint32_t did : 10;
	uint32_t sdid : 10;
	uint32_t dataCount : 10;
#endif
} __attribute__((__packed__));

typedef struct st_anc_pkt_payload_hdr st_anc_pkt_payload_hdr_t;	/**< Type of the structure \ref st_anc_pkt_payload_hdr. */

#endif
