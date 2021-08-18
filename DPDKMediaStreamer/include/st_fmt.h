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

#define ST21_FMT_MAX 6	/// Maximum number of formats
#define ST21_FMT_TX_MAX 3	/// Maximum number of transmssion formats



/**
 * \enum ST21_FMT_P_25
 * 
 * \brief Enumeration of video format and resolutions for progressive scan and frame rate: 25 FPS.
 */
enum ST21_FMT_P_25
{
	ST21_FMT_P_INTEL_720_25 = 0,  /**< Intel platform dedicated format with progressive scan,
												25 FPS (frames per second) and 1280x720 resolution */
	ST21_FMT_P_INTEL_1080_25 = 1, /**< Intel platform dedicated format with progressive scan,
												25 FPS (frames per second) and 1920x1080 resolution */
	ST21_FMT_P_INTEL_2160_25 = 2, /**< Intel platform dedicated format with progressive scan
												25 FPS (frames per second) and 3840x2160 resolution */
	ST21_FMT_P_MAX_TX_25 = ST21_FMT_TX_MAX, /**< Maximum number of progressive scan formats
												25 FPS (frames per second) */
	ST21_FMT_P_AYA_720_25 = 3,				/**< Aya platform dedicated format with progressive scan
												25 FPS (frames per second) and 1280x720 resolution */
	ST21_FMT_P_AYA_1080_25 = 4,				/**< Aya platform dedicated format with progressive scan
												25 FPS (frames per second) and 1920x1080 resolution */
	ST21_FMT_P_AYA_2160_25 = 5,				/**< Aya platform dedicated format with progressive scan
												25 FPS (frames per second) and 3840x2160 resolution */
	ST21_FMT_P_MAX_25 = ST21_FMT_MAX,		/**< Maximum number of progressive scan transmission formats
												25 FPS (frames per second) */
};

/**
 * \enum ST21_FMT_P_29
 * 
 * \brief Enumeration of video format and resolutions for progressive scan and frame rate: 29.97 FPS.
 */
enum ST21_FMT_P_29
{
	ST21_FMT_P_INTEL_720_29 = 0,  /**< Intel platform dedicated format with progressive scan
												29.97 FPS (frames per second) and 1280x720 resolution */
	ST21_FMT_P_INTEL_1080_29 = 1, /**< Intel platform dedicated format with progressive scan
												29.97 FPS (frames per second) and 1920x1080 resolution */
	ST21_FMT_P_INTEL_2160_29 = 2, /**< Intel platform dedicated format with progressive scan
												29.97 FPS (frames per second) and 3840x2160 resolution */
	ST21_FMT_P_MAX_TX_29 = ST21_FMT_TX_MAX,	/**< Maximum number of progressive scan formats
												29.97 FPS (frames per second) */
	ST21_FMT_P_AYA_720_29 = 3,				/**< Aya platform dedicated format with progressive scan
												29.97 FPS (frames per second) and 1280x720 resolution */
	ST21_FMT_P_AYA_1080_29 = 4,				/**< Aya platform dedicated format with progressive scan
												29.97 FPS (frames per second) and 1920x1080 resolution */
	ST21_FMT_P_AYA_2160_29 = 5,				/**< Aya platform dedicated format with progressive scan
												29.97 FPS (frames per second) and 3840x2160 resolution */
	ST21_FMT_P_MAX_29 = ST21_FMT_MAX,		/**< Maximum number of progressive scan transmission formats
												29.97 FPS (frames per second) */
};

/**
 * \enum ST21_FMT_P_50
 * 
 * \brief Enumeration of video format and resolutions for progressive scan and frame rate: 50 FPS.
 */
enum ST21_FMT_P_50
{
	ST21_FMT_P_INTEL_720_50 = 0,  /**< Intel platform dedicated format with progressive scan
												50 FPS (frames per second) and 1280x720 resolution */
	ST21_FMT_P_INTEL_1080_50 = 1, /**< Intel platform dedicated format with progressive scan
												50 FPS (frames per second) and 1920x1080 resolution */
	ST21_FMT_P_INTEL_2160_50 = 2, /**< Intel platform dedicated format with progressive scan
												50 FPS (frames per second) and 3840x2160 resolution */
	ST21_FMT_P_MAX_TX_50 = ST21_FMT_TX_MAX,	/**< Maximum numbers of progressive scan formats
												50 FPS (frames per second) */
	ST21_FMT_P_AYA_720_50 = 3,				/**< Aya platform dedicated format with progressive scan
												50 FPS (frames per second) and 1280x720 resolution */
	ST21_FMT_P_AYA_1080_50 = 4,				/**< Aya platform dedicated format with progressive scan
												50 FPS (frames per second) and 1920x1080 resolution */
	ST21_FMT_P_AYA_2160_50 = 5,				/**< Aya platform dedicated format with progressive scan
												50 FPS (frames per second) and 3840x2160 resolution */
	ST21_FMT_P_MAX_50 = ST21_FMT_MAX,		/**< Maximum number of progressive scan formats
												50 FPS (frames per second) */
};

/**
 * \enum ST21_FMT_P_59
 * 
 * \brief Enumeration of video format and resolutions for progressive scan and frame rate: 59.94 FPS.
 */
enum ST21_FMT_P_59
{
	ST21_FMT_P_INTEL_720_59 = 0,  /**< Intel platform dedicated format with progressive scan
												59.94 FPS (frames per second) and 1280x720 resolution */
	ST21_FMT_P_INTEL_1080_59 = 1, /**< Intel platform dedicated format with progressive scan
												59.94 FPS (frames per second) and 1920x1080 resolution */
	ST21_FMT_P_INTEL_2160_59 = 2, /**< Intel platform dedicated format with progressive scan
												59.94 FPS (frames per second) and 3840x2160 resolution */
	ST21_FMT_P_MAX_TX_59 = ST21_FMT_TX_MAX,	/**< Maximum number of progressive scan formats
												59.94 FPS (frames per second) */
	ST21_FMT_P_AYA_720_59 = 3,				/**< Aya platform dedicated format with progressive scan
												59.94 FPS (frames per second) and 1280x720 resolution */
	ST21_FMT_P_AYA_1080_59 = 4,				/**< Aya platform dedicated format with progressive scan
												59.94 FPS (frames per second) and 1920x1080 resolution */
	ST21_FMT_P_AYA_2160_59 = 5,				/**< Aya platform dedicated format with progressive scan
												59.94 FPS (frames per second) and 3840x2160 resolution */
	ST21_FMT_P_MAX_59 = ST21_FMT_MAX,		/**< Maximum number of progressive scan formats
												59.94 FPS (frames per second) */
};

/**
 * \enum ST21_FMT_I_25
 * 
 * \brief Enumeration of video format and resolutions for interlaced scan and frame rate: 25 FPS.
 */
enum ST21_FMT_I_25
{
	ST21_FMT_I_INTEL_720_25 = 0,  /**< Intel platform dedicated format with interlaced scan
												25 FPS (frames per second) and 1280x720 resolution */
	ST21_FMT_I_INTEL_1080_25 = 1, /**< Intel platform dedicated format with interlaced scan
												25 FPS (frames per second) and 1920x1080 resolution */
	ST21_FMT_I_INTEL_2160_25 = 2, /**< Intel platform dedicated format with interlaced scan
												25 FPS (frames per second) and 3840x2160 resolution */
	ST21_FMT_I_MAX_TX_25 = ST21_FMT_TX_MAX,	/**< Maximum number of progressive scan formats
												25 FPS (frames per second) */
	ST21_FMT_I_AYA_720_25 = 3,				/**< Aya platform dedicated format with interlaced scan
												25 FPS (frames per second) and 1280x720 resolution */
	ST21_FMT_I_AYA_1080_25 = 4,				/**< Aya platform dedicated format with interlaced scan
												25 FPS (frames per second) and 1920x1080 resolution */
	ST21_FMT_I_AYA_2160_25 = 5,				/**< Aya platform dedicated format with interlaced scan
												25 FPS (frames per second) and 3840x2160 resolution */
	ST21_FMT_I_MAX_25 = ST21_FMT_MAX,		/**< Maximum number of progressive scan formats
												25 FPS (frames per second) */
};

/**
 * \enum ST21_FMT_I_29
 * 
 * \brief Enumeration of video format and resolutions for interlaced scan and frame rate: 29.97 FPS.
 */
enum ST21_FMT_I_29
{
	ST21_FMT_I_INTEL_720_29 = 0,  /**< Intel platform dedicated format with interlaced scan
												29.97 FPS (frames per second) and 1280x720 resolution */
	ST21_FMT_I_INTEL_1080_29 = 1, /**< Intel platform dedicated format with interlaced scan
												29.97 FPS (frames per second) and 1920x1080 resolution */
	ST21_FMT_I_INTEL_2160_29 = 2, /**< Intel platform dedicated format with interlaced scan
												29.97 FPS (frames per second) and 3840x2160 resolution */
	ST21_FMT_I_MAX_TX_29 = ST21_FMT_TX_MAX,	/**< Maximum number of progressive scan formats
												29.97 FPS (frames per second) */
	ST21_FMT_I_AYA_720_29 = 3,				/**< Aya platform dedicated format with interlaced scan
												29.97 FPS (frames per second) and 1280x720 resolution */
	ST21_FMT_I_AYA_1080_29 = 4,				/**< Aya platform dedicated format with interlaced scan
												29.97 FPS (frames per second) and 1920x1080 resolution */
	ST21_FMT_I_AYA_2160_29 = 5,				/**< Aya platform dedicated format with interlaced scan
												29.97 FPS (frames per second) and 3840x2160 resolution */
	ST21_FMT_I_MAX_29 = ST21_FMT_MAX,		/**< Maximum number of progressive scan formats
												29.97 FPS (frames per second) */
};

/**
 * \enum ST21_FMT_I_50
 * 
 * \brief Enumeration of video format and resolutions for interlaced scan and frame rate: 50 FPS.
 */
enum ST21_FMT_I_50
{
	ST21_FMT_I_INTEL_720_50 = 0,  /**< Intel platform dedicated format with interlaced scan
												50 FPS (frames per second) and 1280x720 resolution */
	ST21_FMT_I_INTEL_1080_50 = 1, /**< Intel platform dedicated format with interlaced scan
												50 FPS (frames per second) and 1920x1080 resolution */
	ST21_FMT_I_INTEL_2160_50 = 2, /**< Intel platform dedicated format with interlaced scan
												50 FPS (frames per second) and 3840x2160 resolution */
	ST21_FMT_I_MAX_TX_50 = ST21_FMT_TX_MAX,	/**< Maximum number of progressive scan formats
												50 FPS (frames per second) */
	ST21_FMT_I_AYA_720_50 = 3,				/**< Aya platform dedicated format with interlaced scan
												50 FPS (frames per second) and 1280x720 resolution */
	ST21_FMT_I_AYA_1080_50 = 4,				/**< Aya platform dedicated format with interlaced scan
												50 FPS (frames per second) and 1920x1080 resolution */
	ST21_FMT_I_AYA_2160_50 = 5,				/**< Aya platform dedicated format with interlaced scan
												50 FPS (frames per second) and 3840x2160 resolution */
	ST21_FMT_I_MAX_50 = ST21_FMT_MAX,		/**< Maximum number of progressive scan formats
												50 FPS (frames per second) */
};

/**
 * \enum ST21_FMT_I_59
 * 
 * \brief Enumeration of video format and resolutions for interlaced scan and frame rate: 59.94 FPS.
 */
enum ST21_FMT_I_59
{
	ST21_FMT_I_INTEL_720_59 = 0,  /**< Intel platform dedicated format with interlaced scan
												59.94 FPS (frames per second) and 1280x720 resolution */
	ST21_FMT_I_INTEL_1080_59 = 1, /**< Intel platform dedicated format with interlaced scan
												59.94 FPS (frames per second) and 1920x1080 resolution */
	ST21_FMT_I_INTEL_2160_59 = 2, /**< Intel platform dedicated format with interlaced scan
												59.94 FPS (frames per second) and 3840x2160 resolution */
	ST21_FMT_I_MAX_TX_59 = ST21_FMT_TX_MAX,	/**< Maximum number of formats of progressive scan
												59.94 FPS (frames per second) */
	ST21_FMT_I_AYA_720_59 = 3,				/**< Aya platform dedicated format with interlaced scan
												59.94 FPS (frames per second) and 1280x720 resolution */
	ST21_FMT_I_AYA_1080_59 = 4,				/**< Aya platform dedicated format with interlaced scan
												59.94 FPS (frames per second) and 1920x1080 resolution */
	ST21_FMT_I_AYA_2160_59 = 5,				/**< Aya platform dedicated format with interlaced scan
												59.94 FPS (frames per second) and 3840x2160 resolution */
	ST21_FMT_I_MAX_59 = ST21_FMT_MAX,		/**< Maximum number of progressive scan formats
												59.94 FPS (frames per second) */
};

extern st21_format_t sln422be10Hd720p59Fmt; /**< Structure containing predefined information about
												* 	single line transmission video session packet format
												* 	on Intel platforms with YUV 4:2:2 10-bit big endian
												*	pixel order, progressive scan,
												*	59.94 FPS (frames per second) and 1280x720 resolution */
extern st21_format_t sln422be10Hd1080p59Fmt;	/**< Structure containing predefined information about
												* 	single line transmission video session packet format 
												* 	on Intel platforms with YUV 4:2:2 10-bit big endian
												*	pixel order, progressive scan,
												*	59.94 FPS (frames per second) and 1920x1080 resolution */
extern st21_format_t sln422be10Uhd2160p59Fmt;	/**< Structure containing predefined information about
												* 	single line transmission video session packet format
												* 	on Intel platforms with YUV 4:2:2 10-bit big endian
												*	pixel order, progressive scan,
												*	59.94 FPS (frames per second) and 3840x2160 resolution */
extern st21_format_t all422be10Hd720p59Fmt;	/**< Structure containing predefined information about
												* 	single line transmission video session packet format
												* 	on all platforms with YUV 4:2:2 10-bit big endian
												*	pixel order, progressive scan,
												*	59.94 FPS (frames per second) and 1280x720 resolution */
extern st21_format_t all422be10Hd1080p59Fmt;	/**< Structure containing predefined information about
												* 	single line transmission video session packet format
												* 	on all platforms with YUV 4:2:2 10-bit big endian
												*	pixel order, progressive scan,
												*	59.94 FPS (frames per second) and 1920x1080 resolution */
extern st21_format_t all422be10Uhd2160p59Fmt;	/**< Structure containing predefined information about
												* 	single line transmission video session packet format
												* 	on all platforms with YUV 4:2:2 10-bit big endian
												*	pixel order, progressive scan,
												*	59.94 FPS (frames per second) and 3840x2160 resolution */

extern st21_format_t sln422be10Hd720p29Fmt;	/**< Structure containing predefined information about
												* 	single line transmission video session packet format
												* 	on Intel platforms with YUV 4:2:2 10-bit big endian
												*	pixel order, progressive scan,
												*	29.97 FPS (frames per second) and 1280x720 resolution */
extern st21_format_t sln422be10Hd1080p29Fmt;	/**< Structure containing predefined information about
												* 	single line transmission video session packet format
												* 	on Intel platforms with YUV 4:2:2 10-bit big endian
												*	pixel order, progressive scan,
												*	29.97 FPS (frames per second) and 1920x1080 resolution */
extern st21_format_t sln422be10Uhd2160p29Fmt;	/**< Structure containing predefined information about
												* 	single line transmissionvideo session packet format
												* 	on Intel platforms with YUV 4:2:2 10-bit big endian
												*	pixel order, progressive scan,
												*	29.97 FPS (frame per second) and 3840x2160 resolution */
extern st21_format_t all422be10Hd720p29Fmt;	/**< Structure containing predefined information about
												* 	single line transmission video session packet format
												* 	on Intel platforms with YUV 4:2:2 10-bit big endian
												*	pixel order, progressive scan,
												*	29.97 FPS (frames per second) and 1280x720 resolution */
extern st21_format_t all422be10Hd1080p29Fmt;	/**< Structure containing predefined information about
												* 	single line transmission video session packet format
												* 	on Intel platforms with YUV 4:2:2 10-bit big endian
												*	pixel order, progressive scan,
												*	29.97 FPS (frames per second) and 1920x1080 resolution */
extern st21_format_t all422be10Uhd2160p29Fmt;	/**< Structure containing predefined information about
												* 	single line transmission video session packet format
												* 	on Intel platforms with YUV 4:2:2 10-bit big endian
												*	pixel order, progressive scan,
												*	29.97 FPS (frames per second) and 3840x2160 resolution */

extern st21_format_t sln422be10Hd720p50Fmt;	/**< Structure containing predefined information about
												* 	single line transmission video session packet format 
												* 	on Intel platforms with YUV 4:2:2 10-bit big endian
												*	pixel order, progressive scan,
												*	50 FPS (frames per second) and 1280x720 resolution */
extern st21_format_t sln422be10Hd1080p50Fmt;	/**< Structure containing predefined information about
												* 	single line transmission video session packet format
												* 	on Intel platforms with YUV 4:2:2 10-bit big endian
												*	pixel order, progressive scan,
												*	50 FPS (frame per second) and 1920x1080 resolution */
extern st21_format_t sln422be10Uhd2160p50Fmt;	/**< Structure containing predefined information about
												* 	single line transmission video session packet format
												* 	on Intel platforms with YUV 4:2:2 10-bit big endian
												*	pixel order, progressive scan,
												*	50 FPS (frames per second) and 3840x2160 resolution */
extern st21_format_t all422be10Hd720p50Fmt;	/**< Structure containing predefined information about
												* 	single line transmission video session packet format
												* 	on all platforms with YUV 4:2:2 10-bit big endian
												*	pixel order, progressive scan,
												*	50 FPS (frames per second) and 1280x720 resolution */
extern st21_format_t all422be10Hd1080p50Fmt;	/**< Structure containing predefined information about
												* 	single line transmission video session packet format
												* 	on all platforms with YUV 4:2:2 10-bit big endian
												*	pixel order, progressive scan,
												*	50 FPS (frames per second) and 1920x1080 resolution */
extern st21_format_t all422be10Uhd2160p50Fmt;	/**< Structure containing predefined information about
												* 	single line transmission video session packet format 
												* 	on all platforms with YUV 4:2:2 10-bit big endian
												*	pixel order, progressive scan,
												*	50 FPS (frames per second) and 3840x2160 resolution */

extern st21_format_t sln422be10Hd720p25Fmt;	/**< Structure containing predefined information about
												* 	single line transmission video session packet format
												* 	on Intel platforms with YUV 4:2:2 10-bit big endian
												*	pixel order, progressive scan,
												*	25 FPS (frames per second) and 1280x720 resolution */
extern st21_format_t sln422be10Hd1080p25Fmt;	/**< Structure containing predefined information about
												* 	single line transmission video session packet format 
												* 	on Intel platforms with YUV 4:2:2 10-bit big endian
												*	pixel order, progressive scan,
												*	25 FPS (frames per second) and 1920x1080 resolution */
extern st21_format_t sln422be10Uhd2160p25Fmt;	/**< Structure containing predefined information about
												* 	single line transmission video session packet format
												* 	on Intel platforms with YUV 4:2:2 10-bit big endian
												*	pixel order, progressive scan,
												*	25 FPS (frames per second) and 3840x2160 resolution */
extern st21_format_t all422be10Hd720p25Fmt;	/**< Structure containing predefined information about
												* 	single line transmission video session packet format
												* 	on all platforms with YUV 4:2:2 10-bit big endian
												*	pixel order, progressive scan,
												*	25 FPS (frames per second) and 1280x720 resolution */
extern st21_format_t all422be10Hd1080p25Fmt;	/**< Structure containing predefined information about
												* 	single line transmission video session packet format
												* 	on all platforms with YUV 4:2:2 10-bit big endian
												*	pixel order, progressive scan,
												*	25 FPS (frames per second) and 1920x1080 resolution */
extern st21_format_t all422be10Uhd2160p25Fmt;	/**< Structure containing predefined information about
												* 	single line transmission video session packet format
												* 	on all platforms with YUV 4:2:2 10-bit big endian
												*	pixel order, progressive scan,
												*	25 FPS (frames per second) and 3840x2160 resolution */

extern st30_format_t dolbySurround71Pcm24bFmt;	/**< Structure containing predefined information about
												* 	Dolby Surround 7.1 channels audio session packet format
												*	with 24-bits, PCM Signal*/

extern st30_format_t stereoPcm24bFmt;	/**< Structure containing predefined information about
												* 	stereo audio session packet format with 24-bits, PCM signal*/

extern st40_format_t ancillaryDataFmt;	/**< Structure containing predefined information about
												* 	acillary session packet format */

extern st21_format_t *fmtP25Table[ST21_FMT_P_MAX_25];	/**< Table containing format structures 25 FPS (frames per second) progressive scan */
extern st21_format_t *fmtP29Table[ST21_FMT_P_MAX_29];	/**< Table containing format structures 29.97 FPS (frames per second) progressive scan */
extern st21_format_t *fmtP50Table[ST21_FMT_P_MAX_50];	/**< Table containing format structures 50 FPS (frames per second) progressive scan */
extern st21_format_t *fmtP59Table[ST21_FMT_P_MAX_59];	/**< Table containing format structures 59.94 FPS (frames per second) progressive scan */
extern st21_format_t *fmtI25Table[ST21_FMT_I_MAX_25];	/**< Table containing format structures 25 FPS (frames per second) interlaced scan */
extern st21_format_t *fmtI29Table[ST21_FMT_I_MAX_29];	/**< Table containing format structures 29.27 FPS (frames per second) interlaced scan */
extern st21_format_t *fmtI50Table[ST21_FMT_I_MAX_50];	/**< Table containing format structures 50 FPS (frames per second) interlaced scan */
extern st21_format_t *fmtI59Table[ST21_FMT_I_MAX_59];	/**< Table containing format structures 59.94 FPS (frames per second) interlaced scan */

#endif /* _ST_FMT_H_ */
