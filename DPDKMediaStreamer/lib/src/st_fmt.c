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
 * st_fmt.c
 *
 *
 */

#include "st_fmt.h"

#include "st_pkt.h"

/*************************************************/
// Experimental formats, use them if a reason to be
// not compatible with other vendors purposely

st21_format_t dln422be10Hd720p59Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_720P,
	720,
	1280,
	750,
	15,								 /* 15 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	60000,
	1001,
	ST_INTEL_DLN_RFC4175_PKT,
	256,
	5,
	ST_HD_DLN_422_10_256_PIXELS, /* pkt size */
	16683333,					 /* ns */
	1800						 /* pkts in frame */
};

st21_format_t dln422be10Hd1080p59Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_1080P,
	1080,
	1920,
	1125,
	43,
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	60000,
	1001,
	ST_INTEL_DLN_RFC4175_PKT,
	240,
	8,
	ST_HD_DLN_422_10_240_PIXELS, /* pkt size */
	16683333,					 /* ns */
	4320						 /* pkts in frame */
};

/*************************************************/
// Standard formats
// rate 29 formats

st21_format_t sln422be10Hd720p29Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_720P,
	720,
	1280,
	750,
	15,								 /* 15 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	30000,
	1001,
	ST_INTEL_SLN_RFC4175_PKT,
	480,
	3,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	33366667,					 /* ns */
	2160						 /* pkts in frame */
};

st21_format_t sln422be10Hd1080p29Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_1080P,
	1080,
	1920,
	1125,
	22,								 /* 22 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	30000,
	1001,
	ST_INTEL_SLN_RFC4175_PKT,
	480,
	4,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	33366667,					 /* ns */
	4320						 /* pkts in frame */
};

st21_format_t sln422be10Uhd2160p29Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_2160P,
	2160,
	3840,
	2250,
	45,								 /* 45 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	30000,
	1001,
	ST_INTEL_SLN_RFC4175_PKT,
	480,
	8,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	33366667,					 /* ns */
	17280						 /* pkts in frame */
};

st21_format_t all422be10Hd720p29Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_720P,
	720,
	1280,
	750,
	15,								 /* 15 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	30000,
	1001,
	ST_OTHER_SLN_RFC4175_PKT,
	548,
	3,
	1432,	  /* pkt size */
	33366667, /* ns */
	2160	  /* pkts in frame */
};

st21_format_t all422be10Hd1080p29Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_1080P,
	1080,
	1920,
	1125,
	22,								 /* 22 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	30000,
	1001,
	ST_OTHER_SLN_RFC4175_PKT,
	548,
	4,
	1432,	  /* pkt size */
	33366667, /* ns */
	4320	  /* pkts in frame */
};

st21_format_t all422be10Uhd2160p29Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_2160P,
	2160,
	3840,
	2250,
	45,								 /* 45 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	30000,
	1001,
	ST_OTHER_SLN_RFC4175_PKT,
	480,
	8,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	33366667,					 /* ns */
	17280						 /* pkts in frame */
};

// rate 59 formats

st21_format_t sln422be10Hd720p59Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_720P,
	720,
	1280,
	750,
	15,								 /* 15 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	60000,
	1001,
	ST_INTEL_SLN_RFC4175_PKT,
	480,
	3,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	16683333,					 /* ns */
	2160						 /* pkts in frame */
};

st21_format_t sln422be10Hd1080p59Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_1080P,
	1080,
	1920,
	1125,
	43,
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	60000,
	1001,
	ST_INTEL_SLN_RFC4175_PKT,
	480,
	4,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	16683333,					 /* ns */
	4320						 /* pkts in frame */
};

st21_format_t sln422be10Uhd2160p59Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_2160P,
	2160,
	3840,
	2250,
	45,								 /* 45 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	60000,
	1001,
	ST_INTEL_SLN_RFC4175_PKT,
	480,
	8,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	16683333,					 /* ns */
	17280						 /* pkts in frame */
};

st21_format_t all422be10Hd720p59Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_720P,
	720,
	1280,
	750,
	15,								 /* 15 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	60000,
	1001,
	ST_OTHER_SLN_RFC4175_PKT,
	548,
	3,
	1432,	  /* pkt size */
	16683333, /* ns */
	2160	  /* pkts in frame */
};

st21_format_t all422be10Hd1080p59Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_1080P,
	1080,
	1920,
	1125,
	43,
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	60000,
	1001,
	ST_OTHER_SLN_RFC4175_PKT,
	548,
	4,
	1432,	  /* pkt size */
	16683333, /* ns */
	4320	  /* pkts in frame */
};

st21_format_t all422be10Uhd2160p59Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_2160P,
	2160,
	3840,
	2250,
	45,								 /* 45 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	60000,
	1001,
	ST_OTHER_SLN_RFC4175_PKT,
	548,
	4,
	1432,	  /* pkt size */
	16683333, /* ns */
	17280	  /* pkts in frame */
};

// rate 50 formats
st21_format_t sln422be10Hd720p50Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_720P,
	720,
	1280,
	750,
	15,								 /* 15 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	50,
	1,
	ST_INTEL_SLN_RFC4175_PKT,
	480,
	3,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	20000000,					 /* ns */
	2160						 /* pkts in frame */
};

st21_format_t sln422be10Hd1080p50Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_1080P,
	1080,
	1920,
	1125,
	22,								 /* 22 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	50,
	1,
	ST_INTEL_SLN_RFC4175_PKT,
	480,
	4,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	20000000,					 /* ns */
	4320						 /* pkts in frame */
};

st21_format_t sln422be10Uhd2160p50Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_2160P,
	2160,
	3840,
	2250,
	45,								 /* 45 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	50,
	1,
	ST_INTEL_SLN_RFC4175_PKT,
	480,
	8,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	20000000,					 /* ns */
	17280						 /* pkts in frame */
};

st21_format_t all422be10Hd720p50Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_720P,
	720,
	1280,
	750,
	15,								 /* 15 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	50,
	1,
	ST_OTHER_SLN_RFC4175_PKT,
	548,
	3,
	1432,	  /* pkt size */
	20000000, /* ns */
	2160	  /* pkts in frame */
};

st21_format_t all422be10Hd1080p50Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_1080P,
	1080,
	1920,
	1125,
	22,								 /* 22 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	25000,
	1000,
	ST_OTHER_SLN_RFC4175_PKT,
	548,
	4,
	1432,	  /* pkt size */
	20000000, /* ns */
	4320	  /* pkts in frame */
};

st21_format_t all422be10Uhd2160p50Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_2160P,
	2160,
	3840,
	2250,
	45,								 /* 45 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	50,
	1,
	ST_OTHER_SLN_RFC4175_PKT,
	480,
	8,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	20000000,					 /* ns */
	17280						 /* pkts in frame */
};

// rate 25 formats
st21_format_t sln422be10Hd720p25Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_720P,
	720,
	1280,
	750,
	15,								 /* 15 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	25,
	1,
	ST_INTEL_SLN_RFC4175_PKT,
	480,
	3,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	40000000,					 /* ns */
	2160						 /* pkts in frame */
};

st21_format_t sln422be10Hd1080p25Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_1080P,
	1080,
	1920,
	1125,
	22,								 /* 22 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	25,
	1,
	ST_INTEL_SLN_RFC4175_PKT,
	480,
	4,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	40000000,					 /* ns */
	4320						 /* pkts in frame */
};

st21_format_t sln422be10Uhd2160p25Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_2160P,
	2160,
	3840,
	2250,
	45,								 /* 45 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	25,
	1,
	ST_INTEL_SLN_RFC4175_PKT,
	480,
	8,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	40000000,					 /* ns */
	17280						 /* pkts in frame */
};

st21_format_t all422be10Hd720p25Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_720P,
	720,
	1280,
	750,
	15,								 /* 15 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	25,
	1,
	ST_OTHER_SLN_RFC4175_PKT,
	548,
	3,
	1432,	  /* pkt size */
	40000000, /* ns */
	2160	  /* pkts in frame */
};

st21_format_t all422be10Hd1080p25Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_1080P,
	1080,
	1920,
	1125,
	22,								 /* 22 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	25,
	1,
	ST_OTHER_SLN_RFC4175_PKT,
	548,
	4,
	1432,	  /* pkt size */
	40000000, /* ns */
	4320	  /* pkts in frame */
};

st21_format_t all422be10Uhd2160p25Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_2160P,
	2160,
	3840,
	2250,
	45,								 /* 45 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	25,
	1,
	ST_OTHER_SLN_RFC4175_PKT,
	480,
	8,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	40000000,					 /* ns */
	17280						 /* pkts in frame */
};

st21_format_t sln422be10Hd720i25Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_720I,
	720,
	1280,
	750,
	15,								 /* 15 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	25,
	1,
	ST_INTEL_SLN_RFC4175_PKT,
	480,
	3,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	40000000,					 /* ns */
	1080						 /* pkts in frame */
};

// interlaced formats i25

st21_format_t sln422be10Hd1080i25Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_1080I,
	1080,
	1920,
	1125,
	22,								 /* 22 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	25,
	1,
	ST_INTEL_SLN_RFC4175_PKT,
	480,
	4,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	40000000,					 /* ns */
	2160						 /* pkts in frame */
};

st21_format_t sln422be10Uhd2160i25Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_2160I,
	2160,
	3840,
	2250,
	45,								 /* 45 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	25,
	1,
	ST_INTEL_SLN_RFC4175_PKT,
	480,
	8,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	40000000,					 /* ns */
	8640						 /* pkts in frame */
};

st21_format_t all422be10Hd720i25Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_720I,
	720,
	1280,
	750,
	15,								 /* 15 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	25,
	1,
	ST_OTHER_SLN_RFC4175_PKT,
	548,
	3,
	1432,	  /* pkt size */
	40000000, /* ns */
	1080	  /* pkts in frame */
};

st21_format_t all422be10Uhd2160i25Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_2160I,
	2160,
	3840,
	2250,
	45,								 /* 45 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	25,
	1,
	ST_OTHER_SLN_RFC4175_PKT,
	480,
	8,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	40000000,					 /* ns */
	8640						 /* pkts in frame */
};

st21_format_t all422be10Hd1080i25Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_1080I,
	1080,
	1920,
	1125,
	22,								 /* 22 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	25,
	1,
	ST_OTHER_SLN_RFC4175_PKT,
	548,
	4,
	1432,	  /* pkt size */
	40000000, /* ns */
	2160	  /* pkts in frame */
};

// rate 29i formats
// rate 29 formats

st21_format_t sln422be10Hd720i29Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_720I,
	720,
	1280,
	750,
	15,								 /* 15 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	30000,
	1001,
	ST_INTEL_SLN_RFC4175_PKT,
	480,
	3,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	33366667,					 /* ns */
	1080						 /* pkts in frame */
};

st21_format_t sln422be10Hd1080i29Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_1080I,
	1080,
	1920,
	1125,
	22,								 /* 22 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	30000,
	1001,
	ST_INTEL_SLN_RFC4175_PKT,
	480,
	4,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	33366667,					 /* ns */
	2160						 /* pkts in frame */
};

st21_format_t sln422be10Uhd2160i29Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_2160I,
	2160,
	3840,
	2250,
	45,								 /* 45 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	30000,
	1001,
	ST_INTEL_SLN_RFC4175_PKT,
	480,
	4,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	33366667,					 /* ns */
	8640						 /* pkts in frame */
};

st21_format_t all422be10Hd720i29Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_720I,
	720,
	1280,
	750,
	15,								 /* 15 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	30000,
	1001,
	ST_OTHER_SLN_RFC4175_PKT,
	548,
	3,
	1432,	  /* pkt size */
	33366667, /* ns */
	1080	  /* pkts in frame */
};

st21_format_t all422be10Hd1080i29Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_1080I,
	1080,
	1920,
	1125,
	22,								 /* 22 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	30000,
	1001,
	ST_OTHER_SLN_RFC4175_PKT,
	548,
	4,
	1432,	  /* pkt size */
	33366667, /* ns */
	2160	  /* pkts in frame */
};

st21_format_t all422be10Uhd2160i29Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_2160I,
	2160,
	3840,
	2250,
	45,								 /* 45 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	30000,
	1001,
	ST_OTHER_SLN_RFC4175_PKT,
	480,
	8,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	33366667,					 /* ns */
	8640						 /* pkts in frame */
};

// rate 50i formats
st21_format_t sln422be10Hd720i50Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_720I,
	720,
	1280,
	750,
	15,								 /* 15 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	50,
	1,
	ST_INTEL_SLN_RFC4175_PKT,
	480,
	3,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	20000000,					 /* ns */
	1080						 /* pkts in frame */
};

st21_format_t sln422be10Hd1080i50Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_1080I,
	1080,
	1920,
	1125,
	22,								 /* 22 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	50,
	1,
	ST_INTEL_SLN_RFC4175_PKT,
	480,
	4,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	20000000,					 /* ns */
	2160						 /* pkts in frame */
};

st21_format_t sln422be10Uhd2160i50Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_2160I,
	2160,
	3840,
	2250,
	45,								 /* 45 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	50,
	1,
	ST_INTEL_SLN_RFC4175_PKT,
	480,
	8,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	20000000,					 /* ns */
	8640						 /* pkts in frame */
};

st21_format_t all422be10Hd720i50Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_720I,
	720,
	1280,
	750,
	15,								 /* 15 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	50,
	1,
	ST_OTHER_SLN_RFC4175_PKT,
	548,
	3,
	1432,	  /* pkt size */
	20000000, /* ns */
	1080	  /* pkts in frame */
};

st21_format_t all422be10Hd1080i50Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_1080I,
	1080,
	1920,
	1125,
	22,								 /* 22 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	25000,
	1000,
	ST_OTHER_SLN_RFC4175_PKT,
	548,
	4,
	1432,	  /* pkt size */
	20000000, /* ns */
	2160	  /* pkts in frame */
};

st21_format_t all422be10Uhd2160i50Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_2160I,
	2160,
	3840,
	2250,
	45,								 /* 45 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	50,
	1,
	ST_OTHER_SLN_RFC4175_PKT,
	480,
	8,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	20000000,					 /* ns */
	8640						 /* pkts in frame */
};

// rate 59 formats

st21_format_t sln422be10Hd720i59Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_720I,
	720,
	1280,
	750,
	15,								 /* 15 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	60000,
	1001,
	ST_INTEL_SLN_RFC4175_PKT,
	480,
	3,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	16683333,					 /* ns */
	1080						 /* pkts in frame */
};

st21_format_t sln422be10Hd1080i59Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_1080I,
	1080,
	1920,
	1125,
	22,								 /* 22 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	30000,
	1001,
	ST_INTEL_SLN_RFC4175_PKT,
	480,
	4,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	16683333,					 /* ns */
	2160						 /* pkts in frame */
};

st21_format_t sln422be10Uhd2160i59Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_2160I,
	2160,
	3840,
	2250,
	45,								 /* 45 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	60000,
	1001,
	ST_INTEL_SLN_RFC4175_PKT,
	480,
	8,
	ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
	16683333,					 /* ns */
	8640						 /* pkts in frame */
};

st21_format_t all422be10Hd720i59Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_720I,
	720,
	1280,
	750,
	15,								 /* 15 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	60000,
	1001,
	ST_OTHER_SLN_RFC4175_PKT,
	548,
	3,
	1432,	  /* pkt size */
	16683333, /* ns */
	1080	  /* pkts in frame */
};

st21_format_t all422be10Hd1080i59Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_1080I,
	1080,
	1920,
	1125,
	22,								 /* 22 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	60000,
	1001,
	ST_OTHER_SLN_RFC4175_PKT,
	548,
	4,
	1432,	  /* pkt size */
	16683333, /* ns */
	2160	  /* pkts in frame */
};

st21_format_t all422be10Uhd2160i59Fmt = {
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,
	ST21_2160I,
	2160,
	3840,
	2250,
	45,								 /* 45 lines of tr default offset */
	sizeof(st_rfc4175_422_10_pg2_t), /* in 5 bytes 2 pixels */
	2,
	90000, /* freq in Hz*/
	60000,
	1001,
	ST_OTHER_SLN_RFC4175_PKT,
	548,
	4,
	1432,	  /* pkt size */
	16683333, /* ns */
	8640	  /* pkts in frame */
};

st21_format_t *fmtP25Table[ST21_FMT_P_MAX_25] = {
	[ST21_FMT_P_INTEL_720_25] = &sln422be10Hd720p25Fmt,
	[ST21_FMT_P_INTEL_1080_25] = &sln422be10Hd1080p25Fmt,
	[ST21_FMT_P_INTEL_2160_25] = &sln422be10Uhd2160p25Fmt,
	[ST21_FMT_P_AYA_720_25] = &all422be10Hd720p25Fmt,
	[ST21_FMT_P_AYA_1080_25] = &all422be10Hd1080p25Fmt,
	[ST21_FMT_P_AYA_2160_25] = &all422be10Uhd2160p25Fmt,
};

st21_format_t *fmtP29Table[ST21_FMT_P_MAX_29] = {
	[ST21_FMT_P_INTEL_720_29] = &sln422be10Hd720p29Fmt,
	[ST21_FMT_P_INTEL_1080_29] = &sln422be10Hd1080p29Fmt,
	[ST21_FMT_P_INTEL_2160_29] = &sln422be10Uhd2160p29Fmt,
	[ST21_FMT_P_AYA_720_29] = &all422be10Hd720p29Fmt,
	[ST21_FMT_P_AYA_1080_29] = &all422be10Hd1080p29Fmt,
	[ST21_FMT_P_AYA_2160_29] = &all422be10Uhd2160p29Fmt,
};

st21_format_t *fmtP50Table[ST21_FMT_P_MAX_50] = {
	[ST21_FMT_P_INTEL_720_50] = &sln422be10Hd720p50Fmt,
	[ST21_FMT_P_INTEL_1080_50] = &sln422be10Hd1080p50Fmt,
	[ST21_FMT_P_INTEL_2160_50] = &sln422be10Uhd2160p50Fmt,
	[ST21_FMT_P_AYA_720_50] = &all422be10Hd720p50Fmt,
	[ST21_FMT_P_AYA_1080_50] = &all422be10Hd1080p50Fmt,
	[ST21_FMT_P_AYA_2160_50] = &all422be10Uhd2160p50Fmt,
};

st21_format_t *fmtP59Table[ST21_FMT_P_MAX_59] = {
	[ST21_FMT_P_INTEL_720_59] = &sln422be10Hd720p59Fmt,
	[ST21_FMT_P_INTEL_1080_59] = &sln422be10Hd1080p59Fmt,
	[ST21_FMT_P_INTEL_2160_59] = &sln422be10Uhd2160p59Fmt,
	[ST21_FMT_P_AYA_720_59] = &all422be10Hd720p59Fmt,
	[ST21_FMT_P_AYA_1080_59] = &all422be10Hd1080p59Fmt,
	[ST21_FMT_P_AYA_2160_59] = &all422be10Uhd2160p59Fmt,
};

st21_format_t *fmtI25Table[ST21_FMT_I_MAX_25] = {
	[ST21_FMT_I_INTEL_720_25] = &sln422be10Hd720i25Fmt,
	[ST21_FMT_I_INTEL_1080_25] = &sln422be10Hd1080i25Fmt,
	[ST21_FMT_I_INTEL_2160_25] = &sln422be10Uhd2160i25Fmt,
	[ST21_FMT_I_AYA_720_25] = &all422be10Hd720i25Fmt,
	[ST21_FMT_I_AYA_1080_25] = &all422be10Hd1080i25Fmt,
	[ST21_FMT_I_AYA_2160_25] = &all422be10Uhd2160i25Fmt,
};

st21_format_t *fmtI29Table[ST21_FMT_I_MAX_29] = {
	[ST21_FMT_I_INTEL_720_29] = &sln422be10Hd720i29Fmt,
	[ST21_FMT_I_INTEL_1080_29] = &sln422be10Hd1080i29Fmt,
	[ST21_FMT_I_INTEL_2160_29] = &sln422be10Uhd2160i29Fmt,
	[ST21_FMT_I_AYA_720_29] = &all422be10Hd720i29Fmt,
	[ST21_FMT_I_AYA_1080_29] = &all422be10Hd1080i29Fmt,
	[ST21_FMT_I_AYA_2160_29] = &all422be10Uhd2160i29Fmt,
};

st21_format_t *fmtI50Table[ST21_FMT_I_MAX_50] = {
	[ST21_FMT_I_INTEL_720_50] = &sln422be10Hd720i50Fmt,
	[ST21_FMT_I_INTEL_1080_50] = &sln422be10Hd1080i50Fmt,
	[ST21_FMT_I_INTEL_2160_50] = &sln422be10Uhd2160i50Fmt,
	[ST21_FMT_I_AYA_720_50] = &all422be10Hd720i50Fmt,
	[ST21_FMT_I_AYA_1080_50] = &all422be10Hd1080i50Fmt,
	[ST21_FMT_I_AYA_2160_50] = &all422be10Uhd2160i50Fmt,
};

st21_format_t *fmtI59Table[ST21_FMT_I_MAX_59] = {
	[ST21_FMT_I_INTEL_720_59] = &sln422be10Hd720i59Fmt,
	[ST21_FMT_I_INTEL_1080_59] = &sln422be10Hd1080i59Fmt,
	[ST21_FMT_I_INTEL_2160_59] = &sln422be10Uhd2160i59Fmt,
	[ST21_FMT_I_AYA_720_59] = &all422be10Hd720i59Fmt,
	[ST21_FMT_I_AYA_1080_59] = &all422be10Hd1080i59Fmt,
	[ST21_FMT_I_AYA_2160_59] = &all422be10Uhd2160i59Fmt,
};

/*
 * Audio formats
 */
//struct st30_format
//{
//	st30_sample_fmt_t sampleFmt;
//	uint32_t chanCount;	 //usually 1-8, default 2, but exceptionnaly max is 24 only for ST30_SURROUND_222
//	st30_chan_order_t chanOrder[8]; // for example [ST_SURROUND_51, ST30_STD_STEREO, 0, 0, ...]
//									// specifies 6 channels of 5.1 + 2 stereo, leaving remainig
//									// other positions empty
//	st30_sample_clk_t sampleClkRate;//48k or 96k of sampling clock rate
//	uint32_t sampleGrpSize;	 // number of bytes in the sample group,
//	uint32_t sampleGrpCount; // 48/96 sample groups per 1ms, 6/12 sample groups per 125us
//	uint32_t epochTime;	     // in nanoseconds, 1M for 1ms, 125k for 125us
//	uint32_t pktSize;		 //pkt size w/o VLAN header
//};

st30_format_t stereoPcm24bFmt = {
	ST30_PCM16_SAMPLING,
	2,
	{ ST30_STD_STEREO, 0, 0, 0, 0, 0, 0, 0 },
	48000, /* freq in Hz*/
	4,
	48,
	1000000,				  /* 1ms */
	ST_AUDIO_PKT_STEREO_PCM16 /* default pkt size */
};

/*
* Ancillary data format
*/
st40_format_t ancillaryDataFmt = { 90000, /* freq in Hz*/
								   //1000,//ST_HD_SLN_422_10_480_PIXELS, /* pkt size */
								   16683333, /* ns */
								   //1080,						 /* pkts in frame */
								   1000000, /* 1ms */
								   ST_ANC_RTP_PKT_MAX_LEN };
