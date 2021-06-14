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

#ifndef _ST_FMT_H_
#define _ST_FMT_H_
#include "st_api.h"

#include <stdint.h>

#define ST21_FMT_MAX 6
#define ST21_FMT_TX_MAX 3

enum ST21_FMT_P_25
{
	ST21_FMT_P_INTEL_720_25 = 0,
	ST21_FMT_P_INTEL_1080_25 = 1,
	ST21_FMT_P_INTEL_2160_25 = 2,
	ST21_FMT_P_MAX_TX_25 = ST21_FMT_TX_MAX,
	ST21_FMT_P_AYA_720_25 = 3,
	ST21_FMT_P_AYA_1080_25 = 4,
	ST21_FMT_P_AYA_2160_25 = 5,
	ST21_FMT_P_MAX_25 = ST21_FMT_MAX,
};

enum ST21_FMT_P_29
{
	ST21_FMT_P_INTEL_720_29 = 0,
	ST21_FMT_P_INTEL_1080_29 = 1,
	ST21_FMT_P_INTEL_2160_29 = 2,
	ST21_FMT_P_MAX_TX_29 = ST21_FMT_TX_MAX,
	ST21_FMT_P_AYA_720_29 = 3,
	ST21_FMT_P_AYA_1080_29 = 4,
	ST21_FMT_P_AYA_2160_29 = 5,
	ST21_FMT_P_MAX_29 = ST21_FMT_MAX,
};

enum ST21_FMT_P_50
{
	ST21_FMT_P_INTEL_720_50 = 0,
	ST21_FMT_P_INTEL_1080_50 = 1,
	ST21_FMT_P_INTEL_2160_50 = 2,
	ST21_FMT_P_MAX_TX_50 = ST21_FMT_TX_MAX,
	ST21_FMT_P_AYA_720_50 = 3,
	ST21_FMT_P_AYA_1080_50 = 4,
	ST21_FMT_P_AYA_2160_50 = 5,
	ST21_FMT_P_MAX_50 = ST21_FMT_MAX,
};

enum ST21_FMT_P_59
{
	ST21_FMT_P_INTEL_720_59 = 0,
	ST21_FMT_P_INTEL_1080_59 = 1,
	ST21_FMT_P_INTEL_2160_59 = 2,
	ST21_FMT_P_MAX_TX_59 = ST21_FMT_TX_MAX,
	ST21_FMT_P_AYA_720_59 = 3,
	ST21_FMT_P_AYA_1080_59 = 4,
	ST21_FMT_P_AYA_2160_59 = 5,
	ST21_FMT_P_MAX_59 = ST21_FMT_MAX,
};

enum ST21_FMT_I_25
{
	ST21_FMT_I_INTEL_720_25 = 0,
	ST21_FMT_I_INTEL_1080_25 = 1,
	ST21_FMT_I_INTEL_2160_25 = 2,
	ST21_FMT_I_MAX_TX_25 = ST21_FMT_TX_MAX,
	ST21_FMT_I_AYA_720_25 = 3,
	ST21_FMT_I_AYA_1080_25 = 4,
	ST21_FMT_I_AYA_2160_25 = 5,
	ST21_FMT_I_MAX_25 = ST21_FMT_MAX,
};

enum ST21_FMT_I_29
{
	ST21_FMT_I_INTEL_720_29 = 0,
	ST21_FMT_I_INTEL_1080_29 = 1,
	ST21_FMT_I_INTEL_2160_29 = 2,
	ST21_FMT_I_MAX_TX_29 = ST21_FMT_TX_MAX,
	ST21_FMT_I_AYA_720_29 = 3,
	ST21_FMT_I_AYA_1080_29 = 4,
	ST21_FMT_I_AYA_2160_29 = 5,
	ST21_FMT_I_MAX_29 = ST21_FMT_MAX,
};

enum ST21_FMT_I_50
{
	ST21_FMT_I_INTEL_720_50 = 0,
	ST21_FMT_I_INTEL_1080_50 = 1,
	ST21_FMT_I_INTEL_2160_50 = 2,
	ST21_FMT_I_MAX_TX_50 = ST21_FMT_TX_MAX,
	ST21_FMT_I_AYA_720_50 = 3,
	ST21_FMT_I_AYA_1080_50 = 4,
	ST21_FMT_I_AYA_2160_50 = 5,
	ST21_FMT_I_MAX_50 = ST21_FMT_MAX,
};

enum ST21_FMT_I_59
{
	ST21_FMT_I_INTEL_720_59 = 0,
	ST21_FMT_I_INTEL_1080_59 = 1,
	ST21_FMT_I_INTEL_2160_59 = 2,
	ST21_FMT_I_MAX_TX_59 = ST21_FMT_TX_MAX,
	ST21_FMT_I_AYA_720_59 = 3,
	ST21_FMT_I_AYA_1080_59 = 4,
	ST21_FMT_I_AYA_2160_59 = 5,
	ST21_FMT_I_MAX_59 = ST21_FMT_MAX,
};

extern st21_format_t sln422be10Hd720p59Fmt;
extern st21_format_t sln422be10Hd1080p59Fmt;
extern st21_format_t sln422be10Uhd2160p59Fmt;
extern st21_format_t all422be10Hd720p59Fmt;
extern st21_format_t all422be10Hd1080p59Fmt;
extern st21_format_t all422be10Uhd2160p59Fmt;

extern st21_format_t sln422be10Hd720p29Fmt;
extern st21_format_t sln422be10Hd1080p29Fmt;
extern st21_format_t sln422be10Uhd2160p29Fmt;
extern st21_format_t all422be10Hd720p29Fmt;
extern st21_format_t all422be10Hd1080p29Fmt;
extern st21_format_t all422be10Uhd2160p29Fmt;

extern st21_format_t sln422be10Hd720p50Fmt;
extern st21_format_t sln422be10Hd1080p50Fmt;
extern st21_format_t sln422be10Uhd2160p50Fmt;
extern st21_format_t all422be10Hd720p50Fmt;
extern st21_format_t all422be10Hd1080p50Fmt;
extern st21_format_t all422be10Uhd2160p50Fmt;

extern st21_format_t sln422be10Hd720p25Fmt;
extern st21_format_t sln422be10Hd1080p25Fmt;
extern st21_format_t sln422be10Uhd2160p25Fmt;
extern st21_format_t all422be10Hd720p25Fmt;
extern st21_format_t all422be10Hd1080p25Fmt;
extern st21_format_t all422be10Uhd2160p25Fmt;

extern st30_format_t doulbySurround71Pcm24bFmt;
extern st30_format_t stereoPcm24bFmt;

extern st40_format_t ancillaryDataFmt;

extern st21_format_t *fmtP25Table[ST21_FMT_P_MAX_25];
extern st21_format_t *fmtP29Table[ST21_FMT_P_MAX_29];
extern st21_format_t *fmtP50Table[ST21_FMT_P_MAX_50];
extern st21_format_t *fmtP59Table[ST21_FMT_P_MAX_59];
extern st21_format_t *fmtI25Table[ST21_FMT_I_MAX_25];
extern st21_format_t *fmtI29Table[ST21_FMT_I_MAX_29];
extern st21_format_t *fmtI50Table[ST21_FMT_I_MAX_50];
extern st21_format_t *fmtI59Table[ST21_FMT_I_MAX_59];

#endif /* _ST_FMT_H_ */
