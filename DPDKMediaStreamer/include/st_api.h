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

/**
*	@file 
*
*	Main API
*
*	Intel(R) ST 2110 Media Streaming Library
*
*/

#ifndef _ST_API_H
#define _ST_API_H

#include <rte_mbuf.h>

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C"
{
#endif	// __cplusplus

#define ST_VERSION_MAJOR 1	/**<   Major API version value  */

#define ST_VERSION_MINOR 0 /**<   Minor API version value  */

#define ST_VERSION_LAST 24 /**<   Patch API version value  */

#define ST_MAX_ESSENCE 3	/**< apps format: video, audio and ancillary data */

#define ST_MAX_EXT_BUFS 10	/**< Maximum size of external buffer (used for zero-copy memory) */
#define MAX_META 20 /**< Maximum number of allowed ancillary data parameters used as input/output to/from library*/

#define ST_PREFIX_APPNAME "kahawai"

#define ST_IS_IPV4_MCAST(ip) ((ip) >= 0xe0 && (ip) <= 0xef)

	//! Structure for API version information.
	/*! Used for API version verification. */
	struct st_version
	{
		uint16_t major; /*!< Major API version value. */
		uint16_t minor; /*!< Minor API version value. */
		uint16_t last;	/*!< Patch API version value */
	};
	typedef struct st_version st_version_t; /**< Type of the structure \ref st_version. */

/**
 * \enum st_status
 * 
 * \brief Enumeration for API return codes, errors are less than 0
 */
	typedef enum st_status
	{
		ST_OK = 0,					 /**< Returned when operation finished with success. */
		ST_GENERAL_ERR = -1,		 /**< NULL ptr or similar significant condition. */
		ST_NO_MEMORY = -2,			 /**< Not enough memory available. */
		ST_NOT_SUPPORTED = -3,		 /**< Library does not support requested format */
		ST_INVALID_PARAM = -4,		 /**< Unexpected argument to a function */
		ST_NOT_IMPLEMENTED = -5,	 /**< Feature is not implemented */
		ST_INVALID_API_VERSION = -6, /**< API version does not match the required one */
		ST_REMOTE_LAUNCH_FAIL = -7,	 /**< rte_eal_remote_launch failed */

		ST_BAD_PARAM_ID = -10,	/**< Wrong parameter selected. Supported parameters: \ref st_param  */
		ST_BAD_PARAM_VAL = -11, /**< Wrong parameter value provided */

		ST_BAD_NIC_PORT_ID = -19, /**<   NIC port ID is incorrect  */
		ST_BAD_UDP_DPORT = -20,	  /**<   Incorrect UDP destination port  */
		ST_BAD_UDP_SPORT = -21,	  /**<   Incorrect UDP source port  */
		ST_BAD_SRC_IPADDR = -22,  /**<   Incorrect src IP address  */
		ST_BAD_DST_IPADDR = -23,  /**<   Incorrect dst IP address  */

		ST_SN_ERR_NOT_COMPATIBLE = -30, /**<   Session not compatibile with provided data  */
		ST_SN_ERR_RATE_NO_FIT = -31,	/**<   Requested bitrate for the session is not compliant with available bitrate  */
		ST_SN_ERR_NO_TIMESLOT = -32,	/**<   No available timeslot for current session  */
		ST_SN_ERR_NOT_READY = -33,		/**<   Session not ready  */
		ST_SN_ERR_DISCONNECTED = -34,	/**<   Session disconnected */
		ST_SN_ERR_IN_USE = -35,			/**<   Session is busy  */

		ST_BAD_PRODUCER = -50,			  /**<   Incorrect producer  */
		ST_BAD_CONSUMER = -51,			  /**<   Incorrect consumer  */
		ST_TOO_SMALL_BUFFER = -52,		  /**<   Buffer size is too small  */
		ST_TOO_BIG_BUFFER = -53,		  /**<   Buffer size is too large  */
		ST_BUFFER_NOT_READY = -54,		  /**<   Buffer is not ready  */
		ST_PROD_ALREADY_REGISTERED = -55, /**<   Producer is already registered  */
		ST_CONS_ALREADY_REGISTERED = -56, /**<   Consumer is already registered  */

		ST_FMT_ERR_BAD_PIX_FMT = -100,			/**<   Incorrect pixel format  */
		ST_FMT_ERR_BAD_HEIGHT = -101,			/**<   Incorrect video height  */
		ST_FMT_ERR_BAD_WIDTH = -102,			/**<   Incorrect video width  */
		ST_FMT_ERR_BAD_VSCAN = -103,			/**<   Incorrect video vscan  */
		ST_FMT_ERR_BAD_TROFF = -104,			/**<   Incorrect TR offset  */
		ST_FMT_ERR_BAD_PG_SZ = -105,			/**<   Incorrect pixel group size  */
		ST_FMT_ERR_BAD_BLANKING = -106,			/**<   Incorrect blanking  */
		ST_FMT_ERR_BAD_CLK_RATE = -107,			/**<   Incorrect clock rate  */
		ST_FMT_ERR_BAD_PIXELS_IN_PKT = -108,	/**<   Incorrect pixels in the packet  */
		ST_FMT_ERR_BAD_PKT_SZ = -110,			/**<   Incorrect size of the packet  */
		ST_FMT_ERR_BAD_FRAME_TIME = -111,		/**<   Incorrect frame time  */
		ST_FMT_ERR_NOT_SUPPORTED_ON_TX = -113,	/**<   Not supported by the transmitter  */
		ST_FMT_ERR_BAD_PCM_SAMPLING = -120,		/**<   Incorrect PCM sampling  */
		ST_FMT_ERR_BAD_CHANNEL_ORDER = -121,	/**<   Incorrect order of the audio channels */
		ST_FMT_ERR_BAD_CHANNEL_COUNT = -122,	/**<   Incorrect number of the audio channels  */
		ST_FMT_ERR_BAD_SAMPLE_CLK_RATE = -123,	/**<   Incorrect clock rate of the audio signal  */
		ST_FMT_ERR_BAD_SAMPLE_GRP_SIZE = -124,	/**<   Incorrect size of the sample group  */
		ST_FMT_ERR_BAD_SAMPLE_GRP_COUNT = -125, /**<   Incorrect number of the sample group  */
		ST_FMT_ERR_BAD_AUDIO_EPOCH_TIME = -126, /**<   Incorrect epoch time in the audio format  */

		ST_PKT_DROP_BAD_PKT_LEN = -199,		/**< on device so session unknown */
		ST_PKT_DROP_BAD_IP_LEN = -200,		/**<   Incorrect IP packet length */
		ST_PKT_DROP_BAD_UDP_LEN = -201,		/**<   Incorrect UPD datagram length  */
		ST_PKT_DROP_BAD_RTP_HDR = -202,		/**<   Incorrect RTP header  */
		ST_PKT_DROP_BAD_RTP_TMSTAMP = -203, /**<   Incorrect RTP timestamp  */
		ST_PKT_DROP_NO_FRAME_BUF = -204,	/**<   Frame buffer not available  */
		ST_PKT_DROP_INCOMPL_FRAME = -205,	/**<   Received incomplete frame  */
		ST_PKT_DROP_BAD_RTP_LN_LEN = -206,	/**<   Incorrect RTP line length  */
		ST_PKT_DROP_BAD_RTP_LN_NUM = -207,	/**<   Incorrect RTP line number  */
		ST_PKT_DROP_BAD_RTP_OFFSET = -208,	/**<   Incorrect RTP offset  */
		ST_PKT_DROP_BAD_RTP_LN_CONT = -209, /**<   Incorrect RTP line count  */
		ST_PKT_DROP_REDUNDANT_PATH = -210,	/**<   Packet dropped on redundant path  */

		ST_PKT_LOST_TIMEDOUT = -300, /**<   Packet lost due to timeout  */

		ST_DEV_GENERAL_ERR = -500,			/**<   General device related error, not specified by explicit error code  */
		ST_DEV_BAD_PORT_NAME = -501,		/**<   Invorrect port name  */
		ST_DEV_BAD_PACING = -502,			/**<   Incorrect packet pacing  */
		ST_DEV_BAD_NIC_RATE = -503,			/**<   Incorrect NIC speed rate  */
		ST_DEV_BAD_EXACT_RATE = -504,		/**<   Incorrect frame rate  */
		ST_DEV_BAD_PORT_TYPE = -505,		/**<   Incorrect port type  */
		ST_DEV_PORT_MAX_TYPE_PREP = -506,	/**<   Maximum producer or consumer number achieved  */
		ST_DEV_CANNOT_PREP_CONSUMER = -507, /**<   Cannot prepare consumer  */
		ST_DEV_CANNOT_PREP_PRODUCER = -508, /**<   Cannot prepare producer  */
		ST_DEV_ERR_NOT_READY = -509,		/**<   Device not ready  */
		ST_DEV_NOT_ENOUGH_CORES = -510,		/**<   Not enough cores available to run the library  */
		ST_DEV_UNPLUGED_CABLE_ERR = -511,	/**<   No carrier connected to the interface  */

		ST_DEV_NOT_FIND_SPEED_CONF = -515, /**<   Cannot find the file with NIC speed configuration  */

		ST_DEV_NO_NUMA = -520,	   /**<   No available NUMA nodes  */
		ST_DEV_NO_1GB_PAGE = -521, /**<   Hugepage is less than 1 GB, minimum size of page is 1 GB  */
		ST_DEV_NO_MIN_NUMA = -522, /**<   Number of the available numa nodes is not large enough */

		ST_DEV_CANNOT_PREPARE_MBUF = -525, /**<   Preparation of mbuf (DPDK rte_mbuf) failed  */

		ST_DEV_FAIL_FLASH_RTE_FLOW = -530, /**<   Set flows (DPDK rte_flow) on NIC failed  */

		ST_DEV_CANNOT_LOAD_MOD = -543,			  /**<   Cannot load module  */
		ST_DEV_CANNOT_UNLOAD_MOD = -544,		  /**<   Cannot unload module  */
		ST_DEV_MOD_NOT_LOADED = -545,			  /**<   Module not loaded  */
		ST_DEV_CANNOT_BIND_MOD = -546,			  /**<   Cannot bind device with the requested driver  */
		ST_DEV_CANNOT_UNBIND_MOD = -547,		  /**<   Cannot unbind device from current driver  */
		ST_DEV_MOD_NOT_BINDED = -548,			  /**<   Device not binded to the requested driver  */
		ST_DEV_CANNOT_READ_CPUS = -549,			  /**<   Cannot get information about CPU's on host  */

		ST_KNI_GENERAL_ERR = -550,				  /**<   General KNI related error, not specified by explicit error code  */
		ST_KNI_CANNOT_PREPARE = -551,			  /**<   Initialization of the KNI failed  */
		ST_KNI_ALREADY_PREPARED = -552,			  /**<   KNI ais lready initialized  */
		ST_KNI_INTER_NOT_FOUND = -599,			  /**<   Interface for the KNI is not available  */

		ST_PTP_GENERAL_ERR = -600,                /**<   General PTP related error, not specified by explicit error code  */
		ST_PTP_NOT_VALID_CLK_SRC = -601,          /**<   Invalid PTP source clock */

		ST_ARP_GENERAL_ERR = -625,                /**<   General ARP related error, not specified by explicit error code  */
		ST_ARP_EXITED_WITH_NO_ARP_RESPONSE = -626, /**<  ARP was not able to get response from the network. Timeout was reached.  */

		ST_IGMP_GENERAL_ERR = -650,				  /**<   General IGMP related error, not specified by explicit error code */
		ST_IGMP_NOT_READY = -651,                 /**<	 IGMP is not initialized yet and not ready to be used. */
		ST_IGMP_QUERIER_NOT_READY = -652,         /**<   IGMP Querier was not initialized */
		ST_IGMP_WRONG_IP_ADDRESS = -653,          /**<   IP address does not fall within the multicast addresses range (224.0.0.0 - 239.255.255.255)  */
		ST_IGMP_SEND_QUERY_FAILED = -654,  /**<   Send IGMP query failed  */
		ST_IGMP_SEND_REPORT_FAILED = -655, /**<   Send IGMP report failed  */
		ST_IGMP_WRONG_CHECKSUM = -656,	   /**<   Checksum is not properly calculated  */

		ST_GUI_ERR_NO_SDL = -700,	  /**<   SDL (Simple DirectMedia layer) library is not available */
		ST_GUI_ERR_NO_WINDOW = -701,  /**<   Create window failed. SDL library related issue (see SDL_CreateWindow description).  */
		ST_GUI_ERR_NO_RENDER = -702,  /**<   Create renderer failed. SDL library related issue (see SDL_CreateRenderer description).  */
		ST_GUI_ERR_NO_TEXTURE = -703, /**<   Create renderer failed. SDL library related issue (see SDL_CreateTexture description).  */
		ST_RL_ERR = -704,	/**<   Rate limit profile init failed  */
	} st_status_t; /**< Type of the enum \ref st_status. */

#define ST_PKT_DROP(reason) (-(reason + 199))	/**<   Macro for packets drop calculation */
#define ST_PKT_DROP_MAX 12						/**<   Maximum number of packets that can be dropped */

#define ST_FRM_DROP(reason) (-(reason + 204))	/**<   Macro for frame drop calculation  */
#define ST_FRM_DROP_MAX 2						/**<   Maximum number of frame drops */

#define ST_PKT_LOST(reason) (-(reason + 300))	/**<   Macro for packet lost calculation  */
#define ST_PKT_LOST_MAX 1						/**<   Maximum number of packets lost  */

#define MAX_RXTX_TYPES 2  /**< Maximum types of transmission */
#define MAX_RXTX_PORTS 2  /**< Maximum number of physical ports that can be used by library */

/**
 * \enum st_dev_type
 * 
 * \brief Enumeration for type of the devices
 */
	typedef enum st_dev_type
	{
		ST_DEV_TYPE_PRODUCER = 0, /**<  Device prepared for producing and transmitting content.  */
		ST_DEV_TYPE_CONSUMER	  /**<  Device prepared for receiving and consuming content. */
	} st_dev_type_t; /**< Type of the enum \ref st_dev_type. */

/**
 * \enum st_essence_type
 * 
 * \brief Enumeration for content type (Video / Audio / Ancillary)
 */
	typedef enum st_essence_type
	{
		ST_ESSENCE_VIDEO = 0, /**< Video content type */
		ST_ESSENCE_AUDIO,	  /**< Audio content type */
		ST_ESSENCE_ANC,		  /**< Ancillary data content type */
		ST_ESSENCE_MAX		  /**< Video content type */
	} st_essence_type_t; /**< Type of the enum \ref st_essence_type. */

/**
 * \enum st_exact_rate
 * 
 * \brief Enumeration for frame rate
 */
	typedef enum st_exact_rate
	{
		ST_DEV_RATE_P_29_97 = 29,  /**< Scan: progressive, Rate: 29.97 FPS */
		ST_DEV_RATE_P_59_94 = 59,  /**< Scan: progressive, Rate: 59.94 FPS */
		ST_DEV_RATE_P_25_00 = 25,  /**< Scan: progressive, Rate: 25 FPS */
		ST_DEV_RATE_P_50_00 = 50,  /**< Scan: progressive, Rate: 50 FPS */
		ST_DEV_RATE_I_29_97 = 129, /**< Scan: interlaced, Rate: 29.97 FPS */
		ST_DEV_RATE_I_59_94 = 159, /**< Scan: interlaced, Rate: 59.94 FPS */
		ST_DEV_RATE_I_25_00 = 125, /**< Scan: interlaced, Rate: 25 FPS */
		ST_DEV_RATE_I_50_00 = 150, /**< Scan: interlaced, Rate: 50 FPS */
	} st_exact_rate_t; /**< Type of the enum \ref st_exact_rate. */

/**
 * \enum st_pacer_type
 * 
 * \brief Enumeration for ST2110-21 sender pacer types
 */
	typedef enum st_pacer_type
	{
		ST_2110_21_TPW = 1,	 /**< wide sender */
		ST_2110_21_TPNL = 2, /**< narrow linear sender */
		ST_2110_21_TPN = 3,	 /**< narrow gapped sender */
	} st_pacer_type_t; /**< Type of the enum \ref st_pacer_type. */

/**
 * \enum st_port_type
 * 
 * \brief Enumeration for ports
 */
	typedef enum st_port_type
	{
		ST_PPORT = 0, /**< Primary port for Rx/Tx */
		ST_RPORT = 1, /**< Redundant port */
	} st_port_type_t; /**< Type of the enum \ref st_port_type. */

/**
 * \enum st_txrx_type
 * 
 * \brief Enumeration for ports function
 */
	typedef enum st_txrx_type
	{
		ST_TX = 0,  // port is used for Tx
		ST_RX = 1,  // port is used for Rx
	} st_txrx_type_t; /**< Type of the enum \ref st_txrx_type. */

/**
 * \struct st_device
 * 
 * \brief Structure for ST device 
 */
	struct st_device
	{
		st_version_t ver;
		uint32_t snCount;	/**< Number of the video sessions */
		uint32_t sn30Count; /**< Number of the audio sessions */
		uint32_t sn40Count; /**< Number of the ancillary data sessions */
		st_dev_type_t type; /**< Producer or consumer */
		st_exact_rate_t exactRate; /**< Requested frame rate (25 FPS / 29.97 FPS / 50 FPS / 59.94 FPS) */
		st_pacer_type_t pacerType; /**< Type of pacer (Wide / Narrow Linear / Narrow Gapped) */
		uint32_t rateGbps; /**< rate in Gbps, 10, 25, 40, 100 are expected values */
		uint16_t port[MAX_RXTX_PORTS]; /**< Port used for communication */
		uint16_t mtu; /**< if > 1500 requested MTU (maximum transmission unit), updated with the maximum value which is possible on the links */
		uint16_t maxSt21Sessions; /**< expected maximum number of ST21 video sessions/channels to be supported at 1080p29 30000/1001 FPS */
		uint16_t maxSt30Sessions; /**< expected maximum number of ST30 or ST31 audio sessions/channels to be supported with 8 channels of audio */
		uint16_t maxSt40Sessions; /**< expected maximum number of ST40 ancillary sessions/channels to be supported with overlays and watermarks */
	};

	typedef struct st_device st_device_t; /**< Type of the structure \ref st_device. */

/**
 * \enum st21_param_val
 * 
 * \brief Enumeration for ST parameters values allowed for St21SetParam and St21GetParam
 */
	typedef enum st21_param_val
	{
		ST21_FRM_NO_FIX = 100,			  /**< allowed for ST_2110_21_FRM_FIX_MODE */
		ST21_FRM_FIX_PREV = 101,		  /**< allowed for ST_2110_21_FRM_FIX_MODE	*/
		ST21_FRM_FIX_2022_7 = 102,		  /**< allowed for ST_2110_21_FRM_FIX_MODE */
		ST21_FRM_FIX_PREV_N_2022_7 = 103, /**< allowed for ST_2110_21_FRM_FIX_MODE */
		ST21_FRM_2022_7_MODE_ON = 200,	  /**< allowed for ST_2110_21_FRM_2022_7_MODE */
		ST21_FRM_2022_7_MODE_OFF = 201,	  /**< allowed for ST_2110_21_FRM_2022_7_MODE */
	} st21_param_val_t; /**< Type of the enum  \ref st21_param_val. */

/**
 * \enum st_param
 * 
 * \brief Enumeration for ST parameters - functions such :
 * StPtpSetParam and StPtpGetParam
 * St21SetParam and St21GetParam
 * but also other additional parameters of other sessions
 */
	typedef enum st_param
	{
		ST21_FRM_FIX_MODE
		= 10, /**< configurable, by default enabled for ST21_FRM_FIX_PREV_N_2022_7 */
		ST21_FRM_2022_7_MODE
		= 11, /**< configurable, by default enabled if 2nd port is configured      */

		ST21_TPRS = 20,		 /**< value should be specified in nanoseconds, read-only */
		ST21_TR_OFFSET = 21, /**< value should be specified in nanoseconds, read-only */
		ST21_FRM_TIME = 22,	 /**< value should be specified in nanoseconds, read-only */
		ST21_PKT_TIME = 23,	 /**< value should be specified in nanoseconds, read-only */

		ST21_PIX_GRP_SZ = 30, /**< read-only */

		ST_BUILD_ID = 40,	 /**< read-only, Build version ID number */
		ST_LIB_VERSION = 41, /**< read-only Library version ID number*/

		ST_PTP_DROP_TIME
		= 100, /**<   Drop time after which the backup PTP clock is used instead of the primary PTP, also once the
 								* primary is back up, it needs to be stable for as least as long as that interval to switch back  */
		ST_PTP_CLOCK_ID = 101,			/**<   PTP clock ID  */
		ST_PTP_ADDR_MODE = 102,			/**<   Address mode of PTP clock  */
		ST_PTP_STEP_MODE = 103,			/**<   Step mode of PTP clock  */
		ST_PTP_CHOOSE_CLOCK_MODE = 104, /**<   Flag to choose clock mode  */

		ST_TX_FROM_P = 145,			/**<   Set transmission on primary port  */
		ST_RX_FROM_P = 146,			/**<   Set reception on primary port  */
		ST_TX_FROM_R = 147,			/**<   Set transmission on redundant port  */
		ST_RX_FROM_R = 148,			/**<   Set reception on redundant port  */

		ST_SOURCE_IP = 150,		    /**<   Source IP address  */
		ST_DESTINATION_IP_TX = 151, /**<   Destination IP address for transmission */
		ST_EBU_TEST = 152,		    /**  Enable EBU test flag \warning DEPRECATED, unused, subject to removal */
		ST_SN_COUNT = 153,		    /**<   Number of active (created) video sessions  */
		ST_DESTINATION_IP_RX = 154, /**<   Destination IP address for reception  */
		
		ST_P_PORT = 157,		   /**<   Primary port  */
		ST_R_PORT = 158,		   /**<   Redundant port  */
		ST_FMT_INDEX = 159,		   /**<   Video Format index  */
		ST_DPDK_PARAMS = 160,	   /**<   Parameters needed by DPDK to launch  */
		ST_RSOURCE_IP = 161,	   /**<   Redundant source IP address   */
		ST_RDESTINATION_IP_TX = 162,  /**<   Redundant destination IP address for transmission  */
		ST_RDESTINATION_IP_RX = 163,		/**<   Redundant destination IP address for reception  */
		ST_AUDIOFMT_INDEX = 164,   /**<   Audio Format index  */
		ST_BULK_NUM = 165,		   /**<   Size of bulk (to be placed on transmission ring)  */
		ST_SN30_COUNT = 166,	   /**<   Number of active (created) audio sessions  */
		ST_SN40_COUNT = 167,	   /**<   Number of active (created) ancillary data sessions  */
		ST_NUM_PORT = 168,		   /**<   Number of used ports (1 or 2)  */
		ST_AUDIO_FRAME_SIZE = 169, /**<   Size of audio frame  */
		ST_PACING_TYPE = 170,	   /**<   Type of packet pacing (ST_PACING_PAUSE or ST_PACING_TSC)  */
		ST_TSC_HZ = 171,		   /**<   Time of TSC (in Hz)  */
		ST_ENQUEU_THREADS = 172,   /**<   Number of enqueue threads  */
		ST_USER_TMSTAMP = 173,	   /**<   Use user timestamp  */
		ST_RL_BPS = 174,	   /**<   Manual set the rate(byte per second) for ratelimit queue */
		ST_LIB_SCOREID = 180,	   /**<   Choose core with given ID or get info about core ID  */
	} st_param_t; /**< Type of the enum \ref st_param. */

/**
 * \enum st21_prod_type
 * 
 * \brief Enumeration for viceo producer capabilities
 */
	typedef enum st21_prod_type
	{
		ST21_PROD_INVALID = 0x00,		  /**< Invalid video producer */
		ST21_PROD_P_FRAME = 0x10,		  /**< Producer of complete frames in progressive mode */
		ST21_PROD_P_FRAME_TMSTAMP = 0x11, /**< Producer of complete frames and have SDI timestamp already determined */
		ST21_PROD_I_FIELD = 0x12,		  /**< Producer of interlaced fields */
		ST21_PROD_I_FIELD_TMSTAMP = 0x13, /**< Producer of interlaced fields and have SDI timestamp already determined */
		ST21_PROD_P_FRAME_SLICE = 0x20,	  /**< Producer of slices of progressive frames */
		ST21_PROD_P_SLICE_TMSTAMP = 0x21, /**< Producer of sliced frames and have SDI timestamp already determined at the frame level */
		ST21_PROD_I_FIELD_SLICE = 0x22,	  /**< Producer of slices of interlaced fields */
		ST21_PROD_I_SLICE_TMSTAMP = 0x23, /**< Producer of slices of interlaced fields and have SDI timestamp already determined at the field level */
		ST21_PROD_RAW_RTP = 0x30,		  /**< Producer that assembles by their own RTP packets, it only uses the callback of St21BuildRtpPkt_f */
		ST21_PROD_RAW_L2_PKT = 0x31,      /**< producer that assembles by their own L2 packets (header points to L2 layer), it only uses callback of St21BuildRtpPkt_f */
		ST21_PROD_LAST = ST21_PROD_RAW_L2_PKT, /**<  Value refers to last defined video producer enumeration  */
	} st21_prod_type_t; /**< Type of the enum \ref st21_prod_type. */

/**
 * \enum st21_cons_type
 * 
 * \brief Enumeration for viceo consumer capabilities
 */
	typedef enum st21_cons_type
	{
		ST21_CONS_INVALID = 0x00,		  /**< Invalid video consumer */
		ST21_CONS_P_FRAME = 0x10,		  /**< consumer of complete progressive frames */
		ST21_CONS_P_FRAME_TMSTAMP = 0x11, /**< consumer of complete progressive frames and requests SDI timestamp notification */
		ST21_CONS_I_FIELD = 0x12,		  /**< consumer of interlaced fields */
		ST21_CONS_I_FIELD_TMSTAMP = 0x13, /**< consumer of interlaced fields and have SDI timestamp already determined */
		ST21_CONS_P_FRAME_SLICE = 0x20,	  /**< consumer of slices of progressive frames */
		ST21_CONS_P_SLICE_TMSTAMP = 0x21, /**< consumer of slices of progressive frames and requests SDI timestamp notification */
		ST21_CONS_I_FIELD_SLICE = 0x22,	  /**< consumer of slices of interlaced fields */
		ST21_CONS_I_SLICE_TMSTAMP = 0x23, /**< consumer of slices of interlaced fields and has SDI timestamp already determined at the field level */
		ST21_CONS_RAW_RTP = 0x30,		  /**< consumer of not parsed RTP packets, it only uses callback of St21RecvRtpPkt_f */
		ST21_CONS_RAW_L2_PKT = 0x31,      /**< consumer of not parsed raw L2 packets (header points to L2 layer), it only uses callback of St21RecvRtpPkt_f */
		ST21_CONS_LAST = ST21_CONS_RAW_L2_PKT, /**<  Value refers to last defined video consumer enumeration  */
	} st21_cons_type_t; /**< Type of the enum \ref st21_cons_type. */

/**
 * \enum st30_prod_type
 * 
 * \brief Enumeration for audio producer capabilities
 */
	typedef enum st30_prod_type
	{
		ST30_PROD_INVALID = 0x00,		  /**<  Invalid audio producer */
		ST30_PROD_INTERNAL_TMSTMAP = 0x1, /**< Producer of audio that emits it's own timestamp */
		ST30_PROD_EXTERNAL_TMSTAMP = 0x2, /**< Producer of complete frames and has timestamp already determined */
		ST30_PROD_RAW_RTP = 0x30, /**< Producer that assembles by their own RTP packets, it only uses callback of St30BuildRtpPkt_f */
		ST30_PROD_RAW_L2_PKT = 0x31, /**< Producer that assembles their own L2 packets (header points to L2 layer), it only uses callback of St30BuildRtpPkt_f */
		ST30_PROD_LAST = ST30_PROD_RAW_L2_PKT, /**<  Value refers to last defined audio producer enumeration  */
	} st30_prod_type_t; /**< Type of the enum \ref st30_prod_type. */

	/**
 * \enum st30_cons_type
 * 
 * \brief Enumeration for audio consumer capabilities
 */
	typedef enum st30_cons_type
	{
		ST30_CONS_INVALID = 0x00, /**< Invalid audio consumer */
		ST30_CONS_REGULAR = 0x1,  /**< Standard consumer of audio */
		ST30_CONS_RAW_RTP = 0x30, /**< Consumer of not parsed RTP packets, it only uses callback of St30RecvRtpPkt_f */
		ST30_CONS_RAW_L2_PKT = 0x31, /**< Consumer of not parsed raw L2 packets (header points to L2 layer), it only uses callback of St30RecvRtpPkt_f */
		ST30_CONS_LAST = ST30_CONS_RAW_L2_PKT,/**<  Value refers to last defined audio consumer enumeration  */
	} st30_cons_type_t; /**< Type of the enum \ref st30_cons_type. */

/**
 * \enum st40_prod_type
 * 
 * \brief Enumeration for ancillary data producer capabilities
 */
	typedef enum st40_prod_type
	{
		ST40_PROD_INVALID = 0x00, /**< Invalid ancillary data producer */
		ST40_PROD_REGULAR = 0x1,  /**< standard producer of anc data */
		ST40_PROD_EXTERNAL_TMSTAMP = 0x2, /**< producer of complete frames and has timestamp already determined */
		ST40_PROD_LAST = ST40_PROD_EXTERNAL_TMSTAMP, /**<  Value refers to last defined ancillary data producer enumeration  */
	} st40_prod_type_t; /**< Type of the enum \ref st40_prod_type. */

/**
 * \enum st40_cons_type
 * 
 * \brief Enumeration for ancillary data consumer capabilities
 */
	typedef enum st40_cons_type
	{
		ST40_CONS_INVALID = 0x00,			/**< Invalid ancillary data consumer */
		ST40_CONS_REGULAR = 0x1,			/**< Standard consumer of ancillary data */
		ST40_CONS_LAST = ST40_CONS_REGULAR,	/**<  Value refers to last defined ancillary data consumer enumeration  */
	} st40_cons_type_t; /**< Type of the enum \ref st40_cons_type. */

/**
 * \enum st_sn_flags
 * 
 * \brief Enumeration for session capabilities
 */
	typedef enum st_sn_flags
	{
		ST_SN_INVALID = 0x0000,		/**<  Invalid session */
		ST_SN_SINGLE_PATH = 0x0001, /**< raw video single path */
		ST_SN_DUAL_PATH = 0x0002,	/**< raw video dual path */

		// The enums below will be removed as not practical
		ST_SN_UNICAST = 0x0004,		/**< raw video unicast */
		ST_SN_MULTICAST = 0x0008,	/**< raw video multicasts */
		ST_SN_CONNECTLESS = 0x0010, /**< raw video connection without producer */
		ST_SN_CONNECT = 0x0020,		/**< raw video connected session */
	} st_sn_flags_t; /**< Type of the enum \ref st_sn_flags. */

/**
 * \enum st21_pix_fmt
 * 
 * \brief Enumeration for ST2110-21 supported pixel formats
 * of packet payload on a wire
 */
	typedef enum st21_pix_fmt
	{
		ST21_PIX_FMT_RGB_8BIT = 10,		  /**<  Pixel format: 8-bit RGB  */
		ST21_PIX_FMT_RGB_10BIT_BE,		  /**<  Pixel format: 10-bit RGB, Big endian */
		ST21_PIX_FMT_RGB_10BIT_LE,		  /**<  Pixel format: 10-bit RGB, Little endian  */
		ST21_PIX_FMT_RGB_12BIT_BE,		  /**<  Pixel format: 12-bit RGB, Big endian  */
		ST21_PIX_FMT_RGB_12BIT_LE,		  /**<  Pixel format: 12-bit RGB, Little endian  */

		ST21_PIX_FMT_BGR_8BIT = 20,		  /**<  Pixel format: 8-bit BGR  */
		ST21_PIX_FMT_BGR_10BIT_BE,		  /**<  Pixel format: 10-bit BGR, Big endian  */
		ST21_PIX_FMT_BGR_10BIT_LE,		  /**<  Pixel format: 10-bit BGR, Little endian  */
		ST21_PIX_FMT_BGR_12BIT_BE,		  /**<  Pixel format: 12-bit BGR, Big endian  */
		ST21_PIX_FMT_BGR_12BIT_LE,		  /**<  Pixel format: 12-bit BGR, Little endian  */

		ST21_PIX_FMT_YCBCR_420_8BIT = 30, /**<  Pixel format: 8-bit YCBCR 4:2:0  */
		ST21_PIX_FMT_YCBCR_420_10BIT_BE,  /**<  Pixel format: 10-bit YCBCR 4:2:0, Big endian  */
		ST21_PIX_FMT_YCBCR_420_10BIT_LE,  /**<  Pixel format: 10-bit YCBCR 4:2:0, Little endian  */
		ST21_PIX_FMT_YCBCR_420_12BIT_BE,  /**<  Pixel format: 12-bit YCBCR 4:2:0, Big endian  */
		ST21_PIX_FMT_YCBCR_420_12BIT_LE,  /**<  Pixel format: 12-bit YCBCR 4:2:0, Little endian  */

		ST21_PIX_FMT_YCBCR_422_8BIT = 40, /**<  Pixel format: 8-bit YCBCR 4:2:2  */
		ST21_PIX_FMT_YCBCR_422_10BIT_BE,  /**< Pixel format: 10-bit YCBCR 4:2:2, Big endian, 
												Most popular format in the the industry */
		ST21_PIX_FMT_YCBCR_422_10BIT_LE,  /**<  Pixel format: 10-bit YCBCR 4:2:2, Little endian  */
		ST21_PIX_FMT_YCBCR_422_12BIT_BE,  /**<  Pixel format: 12-bit YCBCR 4:2:2, Big endian  */
		ST21_PIX_FMT_YCBCR_422_12BIT_LE,  /**<  Pixel format: 12-bit YCBCR 4:2:2, Little endian  */
	} st21_pix_fmt_t; /**< Type of the enum \ref st21_pix_fmt. */

/**
 * \enum st21_vscan
 * 
 * \brief Enumeration for ST2110-21 scan types
 * i.e. Permutations of height and interlaced/progressive scanning
 */
	typedef enum st21_vscan
	{
		ST21_720I = 1,	/**<   Resolution: 1280x720, Scan: interlaced  */
		ST21_720P = 2,	/**<   Resolution: 1280x720, Scan: progressive  */
		ST21_1080I = 3, /**<   Resolution: 1920x1080, Scan: interlaced  */
		ST21_1080P = 4, /**<   Resolution: 1920x1080, Scan: progressive  */
		ST21_2160I = 5, /**<   Resolution: 3840x2160, Scan: interlaced  */
		ST21_2160P = 6, /**<   Resolution: 3840x2160, Scan: progressive  */
	} st21_vscan_t; /**< Type of the enum \ref st21_vscan. */

/**
 * \enum st21_pkt_fmt
 * 
 * \brief Enumeration for RTP Payload Format for video based on RFC 4175
 */
	typedef enum st21_pkt_fmt
	{
		ST_INTEL_SLN_RFC4175_PKT = 0, /**< INTEL standard single line packet */
		ST_INTEL_DLN_RFC4175_PKT = 1, /**< INTEL standard dual line packet   */
		ST_OTHER_SLN_RFC4175_PKT = 2, /**< Other vendors single line packets */
									  /**< with variable lengths             */
	} st21_pkt_fmt_t; /**< Type of the enum \ref st21_pkt_fmt. */

/**
 * \struct st21_format
 * 
 * \brief Structure for video session packet format definition
 */
	struct st21_format
	{
		st21_pix_fmt_t pixelFmt; /**< Pixel format */
		st21_vscan_t vscan;		 /**< vscan */
		uint32_t height;		 /**< Height */
		uint32_t width;			 /**< Width */
		uint32_t totalLines;	 /**< 1125 for HD, 2250 for UHD */
		uint32_t trOffsetLines;	 /**< 22 for HD, 45 for UHD */
		uint32_t pixelGrpSize;	 /**< 3 for RGB, 5 for 422 10 bit; 
							 - should match the format - for sanity checking */
		uint32_t pixelsInGrp;	 /**< number of pixels in each pixel group,
							  e.g. 1 for RGB, 2 for 422-10 and 420-8 */

		uint32_t clockRate;	 /**< 90k of sampling clock rate */
		uint32_t frmRateMul; /**< 60000 or 30000 */
		uint32_t frmRateDen; /**< 1001 */

		st21_pkt_fmt_t pktFmt; /**< if single, dual, or more lines of RFC4175 or other format */
		uint32_t pixelsInPkt;  /**< number of pixels in each packet */
		uint32_t pktsInLine;   /**< number of packets per each line */
		uint32_t pktSize;	   /**< pkt size w/o VLAN header */

		long double frameTime;	  /**< time of the frame in nanoseconds */
		uint32_t pktsInFrame; /**< packets in frame */
	};
	typedef struct st21_format st21_format_t; /**< Type of the structure \ref st21_format. */


/**
 * \enum st30_sample_fmt
 * 
 * \brief Enumeration of Audio sampling options
 */
	typedef enum st30_sample_fmt
	{
		ST30_PCM8_SAMPLING = 1,	 /**< 8bits 1B per channel  */
		ST30_PCM16_SAMPLING = 2, /**< 16bits 2B per channel */
		ST30_PCM24_SAMPLING = 3, /**< 24bits 3B per channel */
	} st30_sample_fmt_t; /**< Type of the enum \ref st30_sample_fmt. */

/**
 * \enum st30_chan_order
 * 
 * \brief Enumeration of Audio channels
 */
	typedef enum st30_chan_order
	{
		ST30_UNUSED = 0,		/**< unused channel order                                  */
		ST30_STD_MONO = 1,		/**< 1 channel of standard mono                            */
		ST30_DUAL_MONO = 2,		/**< 2 channels of dual mono                               */
		ST30_STD_STEREO = 3,	/**< 2 channels of standard stereo: Left, Right            */
		ST30_MAX_STEREO = 4,	/**< 2 channels of matrix stereo - Left Total, Right Total */
		ST30_SURROUND_51 = 5,	/**< 6 channles of Dolby 5.1 surround                      */
		ST30_SURROUND_71 = 7,	/**< 8 channels of Dolby 7.1 surround                      */
		ST30_SURROUND_222 = 22, /**< 24 channels of Dolby 22.2 surround                    */
		ST30_SGRP_SDI = 20,		/**< 4 channels of SDI audio group                         */
		ST30_UNDEFINED = 30,	/**< 1 channel of undefined audio                          */
	} st30_chan_order_t; /**< Type of the enum \ref st30_chan_order. */

/**
 * \enum st30_sample_clk
 * 
 * \brief Enumeration of Audio clock rate
 */
	typedef enum st30_sample_clk
	{
		ST30_SAMPLE_CLK_RATE_48KHZ = 48000, /**< ST 2110-30, clock rate 48 kHz */
		ST30_SAMPLE_CLK_RATE_96KHZ = 96000, /**< ST 2110-30, clock rate 96 kHz */
	} st30_sample_clk_t; /**< Type of the enum \ref st30_sample_clk. */

/**
 * \struct st30_format
 * 
 * \brief Structure for audio session packet format definition
 */
	typedef struct st30_format
	{
		st30_sample_fmt_t sampleFmt;
		uint32_t
			chanCount; /**< usually 1-8, default 2, an exception is 24 for ST30_SURROUND_222 */
		st30_chan_order_t
			chanOrder[8]; /**< for example [ST_SURROUND_51, ST30_STD_STEREO, 0, 0, ...] */
						  /**< specifies 6 channels of 5.1 + 2 stereo, leave remaining positions empty */
		st30_sample_clk_t sampleClkRate; /**< 48k or 96k of sampling clock rate */
		uint32_t sampleGrpSize;			 /**<  number of bytes in the sample group, */
		uint32_t sampleGrpCount; /**<  48/96 sample groups per 1ms, 6/12 sample groups per 125us */
		uint32_t epochTime;		 /**<  in nanoseconds, 1M for 1ms, 125k for 125us */
		uint32_t pktSize;		 /**< pkt size w/o VLAN header */
	} st30_format_t; /**< Type of the structure \ref st30_format. */

/**
 * \struct st40_format
 * 
 * \brief Structure for ancillary data session packet format definition
 */
	struct st40_format
	{
		uint32_t clockRate; /**< 90k of sampling clock rate */
		long double frameTime; /**< time of the frame in nanoseconds */
		uint32_t epochTime; /**<  */
		uint32_t pktSize;	/**< pkt size w/o VLAN header */
	};
	typedef struct st40_format st40_format_t; /**< Type of the structure \ref st40_format. */

/**
 * \struct st_format
 * 
 * \brief Structure for distinguishing the type of data supported in current session
 */
	typedef struct st_format
	{
		st_essence_type_t mtype; /**<  Video / Audio / Ancillary Data */
		union
		{
			st21_format_t v;   /**<  Format data for video  */
			st30_format_t a;   /**<  Format data for audio  */
			st40_format_t anc; /**<  Format data for ancillary data  */
		};
	} st_format_t; /**< Type of the structure \ref st_format. */

/**
 * \enum st21_buf_fmt
 * 
 * \brief Enumeration for input output formats of video buffers
 */
	typedef enum st21_buf_fmt
	{
		ST21_BUF_FMT_RGB_8BIT = 10, /**<  Video buffer format: 8-bit RGB */
		ST21_BUF_FMT_RGB_10BIT_BE,	/**<  Video buffer format: 10-bit RGB, Big endian */
		ST21_BUF_FMT_RGB_10BIT_LE,	/**<  Video buffer format: 10-bit RGB, Little endian  */
		ST21_BUF_FMT_RGB_12BIT_BE,	/**<  Video buffer format: 12-bit RGB, Big endian  */
		ST21_BUF_FMT_RGB_12BIT_LE,	/**<  Video buffer format: 12-bit RGB, Little endian  */

		ST21_BUF_FMT_RGBA_8BIT = 15, /**<  Video buffer format: 8-bit RGBA  */

		ST21_BUF_FMT_BGR_8BIT = 20, /**<  Video buffer format: 8-bit BGR  */
		ST21_BUF_FMT_BGR_10BIT_BE,	/**<  Video buffer format: 10-bit BGR, Big endian  */
		ST21_BUF_FMT_BGR_10BIT_LE,	/**<  Video buffer format: 10-bit BGR, Little endian  */
		ST21_BUF_FMT_BGR_12BIT_BE,	/**<  Video buffer format: 12-bit BGR, Big endian  */
		ST21_BUF_FMT_BGR_12BIT_LE,	/**<  Video buffer format: 12-bit BGR, Little endian  */

		ST21_BUF_FMT_BGRA_8BIT = 25, /**<  Video buffer format: 8-bit BGRA  */

		ST21_BUF_FMT_YUV_420_8BIT = 30, /**<  Video buffer format: 8-bit YUV 4:2:0  */
		ST21_BUF_FMT_YUV_420_10BIT_BE,	/**<  Video buffer format: 10-bit YUV 4:2:0, Big endian  */
		ST21_BUF_FMT_YUV_420_10BIT_LE,	/**<  Video buffer format: 10-bit YUV 4:2:0, Little endian  */
		ST21_BUF_FMT_YUV_420_12BIT_BE,	/**<  Video buffer format: 12-bit YUV 4:2:0, Big endian  */
		ST21_BUF_FMT_YUV_420_12BIT_LE,	/**<  Video buffer format: 12-bit YUV 4:2:0, Little endian  */

		ST21_BUF_FMT_YUV_422_8BIT = 40, /**<  Video buffer format: 8-bit YUV 4:2:2  */
		ST21_BUF_FMT_YUV_422_10BIT_BE,	/**<  Video buffer format: 10-bit YUV 4:2:2, Big endian  */
		ST21_BUF_FMT_YUV_422_10BIT_LE,	/**<  Video buffer format: 10-bit YUV 4:2:2, Little endian  */
		ST21_BUF_FMT_YUV_422_12BIT_BE,	/**<  Video buffer format: 12-bit YUV 4:2:2, Big endian  */
		ST21_BUF_FMT_YUV_422_12BIT_LE,	/**<  Video buffer format: 12-bit YUV 4:2:2, Little endian  */
	} st21_buf_fmt_t; /**< Type of the enum \ref st21_buf_fmt. */

/**
 * \enum st30_buf_fmt
 * 
 * \brief Enumeration of audio supported buffer formats
 */
	typedef enum st30_buf_fmt
	{
		ST30_BUF_FMT_WAV,	/**< Audio buffer format: Wave (wav)*/
	} st30_buf_fmt_t; /**< Type of the enum \ref st30_buf_fmt. */

/**
 * \enum st40_buf_fmt
 * 
 * \brief Enumeration for input output formats of ancillary data buffers
 * 		type supported by ST 2110-40 (SMPTE ST 291-1 Ancillary Data)
 */
	typedef enum st40_buf_fmt
	{
		ST40_BUF_FMT_CLOSED_CAPTIONS = 100,  /**<  Ancillary data buffer type: Closed captions   */
	} st40_buf_fmt_t; /**< Type of the enum \ref st40_buf_fmt. */
	
/**
 * \struct st_session_ext_mem
 * 
 * \brief Buffer used for zero-copy memory
 */
	struct st_session_ext_mem
	{
		struct rte_mbuf_ext_shared_info *shInfo[ST_MAX_EXT_BUFS]; /**<  TODO  */
		uint8_t *addr[ST_MAX_EXT_BUFS];							  /**<  TODO  */
		uint8_t *endAddr[ST_MAX_EXT_BUFS];						  /**<  TODO  */
		rte_iova_t bufIova[ST_MAX_EXT_BUFS];					  /**<  TODO  */
		int numExtBuf;											  /**<  TODO  */
	};
	typedef struct st_session_ext_mem st_ext_mem_t; /**< Type of the structure \ref st_session_ext_mem. */

/**
 * \struct st_session
 * 
 * \brief General and common for video, audio and ancillary data structure describing session
 */
	typedef struct st_session
	{
		st_essence_type_t type; /**<  Type of session (Video / Audio / Ancillary data)  */
		st_sn_flags_t caps;		/**<  Additional session flags  */
		st_format_t *fmt;		/**<  Format depending of session type (st_essence_type_t)  */
		uint8_t rtpProfile;		/**< Dynamic profile ID of the RTP session */
		uint32_t ssid;			/**<  Session ID  */
		uint16_t nicPort[2];	/**< NIC ports, second port valid only if multiple paths are supported */
		uint32_t timeslot;		/**< assigned timeslot ID [0- 1st timeslot, N] */
		uint32_t trOffset;      /**< offset of the timeslot since even EPOCH (calculated from timeslot) */
		uint32_t tprs;						/**< time of 2 consecutive packets of the same session (in nanoseconds) */
		uint32_t pktTime;					/**< time of the packet (in nanoseconds) */
		uint32_t frameSize;					/**<  Size of the content of the transmitted frame */
		uint64_t pktsDrop[ST_PKT_DROP_MAX]; /**<  Packets drop counter  */
		uint64_t frmsDrop[ST_FRM_DROP_MAX]; /**<  Frames drop counter  */
		uint64_t pktsLost[ST_PKT_LOST_MAX]; /**<  Packets lost counter  */
		uint64_t pktsSend;					/**<  Packets send counter  */
		uint64_t frmsSend;					/**<  Frames send counter  */
		uint64_t pktsRecv;					/**<  Packets received counter  */
		uint64_t frmsRecv;					/**<  Frames received counter  */
		st_ext_mem_t extMem;				/**<  External memory (for handling zero-copy memory) */
	} st_session_t; /**< Type of the structure \ref st_session. */

/**
 * \enum st_addr_opt
 * 
 * \brief Enumeration of flags used by network addresses structure for establishing connection
 */
	typedef enum st_addr_opt
	{
		ST_ADDR_UCAST_IPV4 = 0x1, /**<  IPv4 unicast  */
		ST_ADDR_MCAST_IPV4 = 0x2, /**<  IPv4 multicast  */
		ST_ADDR_UCAST_IPV6 = 0x4, /**<  IPv6 unicast  */
		ST_ADDR_MCAST_IPV6 = 0x8, /**<  IPv6 multicast  */

		ST_ADDR_VLAN_TAG = 0x10, /**<  VLAN TAG  */
		ST_ADDR_VLAN_DEI = 0x20, /**<  VLAN DEI  */
		ST_ADDR_VLAN_PCP = 0x40, /**<  VLAN PCP  */

		ST_ADDR_IP_ECN = 0x100,	 /**<  IP ECN  */
		ST_ADDR_IP_DSCP = 0x200, /**<  IP DSCP  */
	} st_addr_opt_t; /**< Type of the structure \ref st_addr_opt. */

/**
 * \enum st_pacing_type
 * 
 * \brief Enumeration for the type of pacing (PAUSE, TSC, NIC)
 */
	typedef enum st_pacing_type
	{
		ST_PACING_DEFAULT = 0, /**< determined by system */
		ST_PACING_PAUSE,	   /**< Use pause frame */
		ST_PACING_TSC,		   /**< By cpu TSC time stamp */
		ST_PACING_NIC_RL,	   /**< By hardware nic rate limit feature */
	} st_pacing_type_t; /**< Type of the enum \ref st_pacing_type. */

/**
 * \struct strtp_ancMeta
 * 
 * \brief Structure for ST2110-40 Input/Output
 */
	typedef struct strtp_ancMeta
	{
		uint16_t c;			 /**< If the C bit is '0', it means that the ANC data use luma (Y)
												data channel. If the C bit is '1', it means that the ANC data 
												use color-difference data channel. (RFC 8331)*/
		uint16_t lineNumber; /**< Contains information about line number corresponds to the 
												location (vertical) of the ANC data packet in the SDI raster.
												(RFC 8331) */
		uint16_t horiOffset; /**< Defines the location of the ANC data packet in an SDI raster 
												(RFC 8331) */
		uint16_t s;			 /**< Indicates whether the data stream number of a multi-stream
												data mapping used to transport the ANC data packet. 
												If the S bit is '0', then the StreamNum is not analyzed.
												If the S bit is '1', then the StreamNum field contains info
												about stream number in the ANC data packet. (RFC 8331) */
		uint16_t streamNum;	 /**< StreamNum field carries identification of the source data
												stream number of the ANC data packet. Depends
												on S (Data Stream Flag) field (RFC 8331)*/
		uint16_t did;		 /**< Data Identification Word (RFC 8331) */
		uint16_t sdid;		 /**< Secondary Data Identification Word. Used only for a "Type 2" ANC
												data packet. Note that in a "Type 1" ANC data packet, this word
												will actually carry the Data Block Number (DBN). (RFC 8331) */
		uint16_t udwSize;	 /**<  Size of the User Data Words  */
		uint16_t udwOffset;	 /**<  Offset of the User Data Words  */
	} strtp_ancMeta_t; /**< Type of the structure \ref strtp_ancMeta. */

/**
 * \struct strtp_ancFrame
 * 
 * \brief Structure containing info about meta data and content to build and send ancillary data frame (SMPTE ST 2110-40)
 */
	typedef struct strtp_ancFrame
	{
		uint32_t tmStamp;				/**<  Timestamp  */
		strtp_ancMeta_t meta[MAX_META]; /**<  Meta data  */
		uint8_t *data;					/**<  Handle to data buffer  */
		uint32_t dataSize;				/**<  Size of content data  */
		uint32_t metaSize;				/**<  Size of meta data  */
	} strtp_ancFrame_t; /**< Type of the structure \ref strtp_ancFrame. */

/**
 * \struct st_addr
 * 
 * \brief Structure for connection address of IPv4/v6 and UDP ports
 */
	struct st_addr
	{
		st_addr_opt_t options; /**<  Additional flags defined in \ref st_addr_opt_t */
		union
		{
			struct sockaddr_in addr4;  /**<  Source IPv4 address  */
			struct sockaddr_in6 addr6; /**<  Source IPv6 address  */
		} src;
		union
		{
			struct sockaddr_in addr4;  /**<  Destination IPv4 address  */
			struct sockaddr_in6 addr6; /**<  Destination IPv6 address  */
		} dst;
		union
		{
			struct
			{
				uint16_t tag : 12; /**<  VLAN TAG  */
				uint16_t dei : 1;  /**<  VLAN DEI  */
				uint16_t pcp : 3;  /**<  VLAN PCP  */
			};
			uint16_t vlan; /**<  VLAN  */
		};
		union
		{
			struct
			{
				uint8_t ecn : 2;  /**<  ECN flag  */
				uint8_t dscp : 6; /**<  DSCP flag  */
			};
			uint8_t tos; /**< Type of service */
		};
	};

	typedef struct st_addr st_addr_t; /**< Type of the structure \ref st_addr. */

/**
 * \union st_param_val_t
 * 
 * \brief Union used for parameters passing form/to application/library by \ref StSetParam an \ref StGetParam
 */
	typedef union
	{
		uint32_t valueU32; /**<  32bit data type for Setting or Getting parameters */
		uint64_t valueU64; /**<  64bit data type for Setting or Getting parameters  */
		char *strPtr;	   /**<  Pointer to a string for Setting or Getting parameters  */
		void *ptr;		   /**<  Pointer of void type for Setting or Getting parameters  */
		bool valueBool;	   /**<  Binary flag for turning on some functionalities */
	} st_param_val_t;

	/*!
  * Called by the application to set specific library attribute to specific value
  *
  * @param prm	- IN parameter describing library attribute that should be changed.
  * @param val 	- IN parameter value to be set by library.
  *
  * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
  */
	st_status_t StSetParam(st_param_t prm, st_param_val_t val);

	/*!
  * Called by the application to retrieve library information such as version or build number.
  *
  * @param prm	- IN parameter describing requested information.
  * @param val 	- OUT pointer to object where to copy information data.
  *
  * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
  */
	st_status_t StGetParam(st_param_t prm, st_param_val_t *val);

/*!
  * Called by the application to initialize ST2110 device on the specified NIC PCI devices
  *
  *
  * @param inDev 	- IN structure with device parameters.
  * @param port1Bdf 	- IN string describing Bus:Device.Function of the PCI device of the primary port (primary stream).
  * @param port2Bdf 	- IN string describing Bus:Device.Function of the PCI device of the secondary port (secondary stream).
  * @param outDev 	- OUT created device object w/ fields updated per link capabilities.
  *
  * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
  *
  */
	st_status_t StCreateDevice(
		st_device_t *inDev,		//IN structure with device parameters.
		const char *port1Bdf,	//IN BDF of primary port PCI device.
		const char *port2Bdf,	//IN BDF of secondary port PCI device.
		st_device_t **outDev);	//OUT created device object w/ fields updated per link capabilities.

	/*!
 * Called by the application to start ST2110 device for operation.
 *
 *
 * @param dev - IN created device object can be already populated with sessions.
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 *
 */
	st_status_t StStartDevice(
		st_device_t *dev);	//IN created device object can be already populated with sessions.

	/*!
 * Called by the application to deinitialize ST2110 device.
 *
 *
 * @param dev 	- IN pointer to the device object to deinitialize.
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 *
 */
	st_status_t StDestroyDevice(st_device_t *dev);	//IN ST device to destroy.

/**
 * \struct st_ptp_clock_id
 * 
 * \brief ID of the PTP clock 
 */
	struct st_ptp_clock_id
	{
		uint8_t id[8];
	};
	typedef struct st_ptp_clock_id st_ptp_clock_id_t;

/**
 * \enum st_ptp_addr_mode
 * 
 * \brief 
 */
	typedef enum st_ptp_addr_mode
	{
		ST_PTP_MULTICAST_ADDR = 0,
		ST_PTP_UNICAST_ADDR = 1,
	} st_ptp_addr_mode_t;

/**
 * \enum st_ptp_step_mode
 * 
 * \brief 
 */
	typedef enum st_ptp_step_mode
	{
		ST_PTP_TWO_STEP = 0,
		ST_PTP_ONE_STEP = 1,
	} st_ptp_step_mode_t;

/**
 * \enum st_ptp_master_choose_mode
 * 
 * \brief 
 */
	typedef enum st_ptp_master_choose_mode
	{
		ST_PTP_BEST_KNOWN_MASTER = 0,
		ST_PTP_SET_MASTER = 1,
		ST_PTP_FIRST_KNOWN_MASTER = 2,
	} st_ptp_master_choose_mode_t;
	/*!
 * Called by the application to assign PTP primary and backup clock IDs
 *
 *
 * @param priClock 	- IN pointer to the clock ID structure of the primary PTP Grandmaster.
 * @param bkpClock 	- IN pointer to the clock ID structure of the backup PTP Grandmaster.
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 *
 */
	st_status_t StPtpSetClockSource(st_ptp_clock_id_t const *priClock,
									st_ptp_clock_id_t const *bkpClock);

	/**
 * \warning DEPRECATED (TODO: MERGE THIS WITH StSetParam AND REMOVE)
 */
	st_status_t StPtpSetParam(st_param_t prm, st_param_val_t val);

	/**
 * \warning DEPRECATED (TODO: MERGE THIS WITH StGetParam AND REMOVE)
 */
	st_status_t StPtpGetParam(st_param_t prm, st_param_val_t *val, uint16_t portId);

	/*!
 * Called by the application to get active PTP clock IDs.
 *
 * @param currClock	- OUT pointer to the clock ID structure that is updated with the PTP Grandmaster.
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	st_status_t StPtpGetClockSource(st_ptp_clock_id_t *currClock);

	/*!
 * Return relative CPU time (in nanoseconds).
 *
 * @return time in nano seconds as double.
 */
	static inline uint64_t
	StGetCpuTimeNano()
	{
#define NANO_PER_SEC (1 * 1000 * 1000 * 1000)
		double tsc = rte_get_tsc_cycles();
		double tsc_hz = rte_get_tsc_hz();
		double time_nano = tsc / (tsc_hz / ((double)NANO_PER_SEC));
		return time_nano;
	}

	extern uint64_t (*StPtpGetTime)(void) __attribute__((nonnull));

	/*!
 * Called by the application to get active PTP clock IDs.
 *
 * @param dev - ST device.
 * @param count - count of active ST2110-21 sessions.
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	st_status_t StGetSessionCount(st_device_t *dev,	 //IN ST device.
								  uint32_t *count);	 //OUT count of active ST2110-21 sessions.

	/*!
 * Called by the application to create a new video session on NIC device.
 *
 * @param dev - IN device on which to create session.
 * @param inSn - IN session structure.
 * @param fmt - deprecated new proposed way below or use new function.
 * @param outSn - OUT created session object w/ fields updated respectively.
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	st_status_t StCreateSession(
		st_device_t *dev,		// IN device on which to create session.
		st_session_t *inSn,		// IN session structure.
		st_format_t *fmt,		// deprecated new proposed way below or use new function.
		st_session_t **outSn);	// OUT created session object w/ fields updated respectively.

	/*!
 * Called by the application to destroy session.
 *
 * @param sn - IN session to be destroyed.
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	st_status_t StDestroySession(st_session_t *sn);

	/*!
 * Called by the application to add static entry into the ARP table.
 *
 * @param sn - IN pointer to ST2110 session.
 * @param nicPort - IN Port on Network Interface card.
 * @param macAddr - IN MAC address of NIC port.
 * @param ipAddr - IN IP address to be set in ARP table.
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	st_status_t StSetStaticArpEntry(st_session_t *sn, uint16_t nicPort, uint8_t *macAddr,
									uint8_t *ipAddr);

	/*!
 * Called by the application to get current status of the ARP table.
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	st_status_t StGetArpTable();

/*!
 * Called by both Transmitter and Receiver to assign/bind IP addresses of the stream.
 * Upon correct scenario completes with ST_OK.
 * Should be called twice if redundant 2022-7 path mode is used to add both addresses
 * on the ports as required respectively (for both path addresses and VLANs).
 * @param sn - IN session object.
 * @param addr - IN address structure defined by \ref st_addr and filled with proper values.
 * @param nicPort - IN Physical port ID of NIC used by library.
 * 
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	st_status_t StBindIpAddr(st_session_t *sn, st_addr_t *addr, uint16_t nicPort);

	/*!
 * Called by the consumer application to join a producer session multicast group.
 * This procedure starts a background thread that periodically sends IGMP Membership
 * reports to switches so that they can setup their IGMP snooping.
 *
 * @param addr - IN address structure defined by \ref st_addr and filled with proper values.
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	st_status_t StJoinMulticastGroup(st_addr_t *addr);

/*!
 * Called by the applications to get format information of the session.
 * This is a complementary method to St21GetSdp() and St21GetParam() functions.
 *
 * @param sn - IN session.
 * @param fmt - OUT format defined by \ref st_format
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	st_status_t StGetFormat(st_session_t *sn, st_format_t *fmt);

	/*!
 * Called by the application to get SDP text in a newly allocated text buffer.
 * Reading SDP allows a complete understanding of session and format.
 * This is complementary method to St21GetFormat() and St21GetParam().
 *
 * @param sn - IN session.
 * @param sdpBuf - OUT SDP buffer.
 * @param sdpBufSize - IN size of requested SDP buffer.
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	st_status_t St21GetSdp(st_session_t *sn, char *sdpBuf, uint32_t sdpBufSize);

	/******************************************************************************************************
 *
 * Callbacks to producer or consumer application to manage live streaming
 *
 ******************************************************************************************************/

	/*!
 * Callback to consumer application to pass the RTP packet after UDP header to the application
 * (rtpHdr points to RTP header). Application is responsible for parsing and understanding the packet.
 * This is for applications implementing own packet parsing and own formats. Nanosecond PTP timestamp is also passed.
 * The passed buffers are freed after the call by the library so the application should consume them within the callback.
 * 
 * @param appHandle - IN/OUT Handle to the app.
 * @param pktHdr - IN Handle to the RTP header.
 * @param hdrSize - IN Size of the RTP header.
 * @param rtpPayload - IN Handle to the buffer with data.
 * @param payloadSize - IN Size of the payload (data to receive).
 * @param tmstamp - IN Timestamp.
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	typedef st_status_t (*St21RecvRtpPkt_f)(void *appHandle, uint8_t *pktHdr, uint16_t hdrSize,
											uint8_t *rtpPayload, uint16_t payloadSize,
											uint64_t tmstamp);

	/*!
 * Callback to producer or consumer application to get the next frame buffer necessary to continue streaming.
 * If application cannot return the next buffer then it should return NULL and then call St21ProducerUpdate()
 * or St21ConsumerUpdate() to restart streaming.
 * 
 * @param appHandle - IN/OUT Handle to the app.
 * @param prevFrameBuf - IN Pointer to buffer holding previous video frame.
 * @param bufSize - IN Size of buffer holding video frame.
 * @param tmstamp - user provided timestamp of the frame provided by this API to be set in RTP header of outcoming packets (optional)
 * @param fieldId - IN For interlaced video this field describes whether it's odd frame(0) or even (1)
 *
 * @return pointer to buffer containing new video frame (TX) or buffer that will be used to fill next frame (RX) (returned type uint8_t)
 */
	typedef uint8_t *(*St21GetNextFrameBuf_f)(void *appHandle, uint8_t *prevFrameBuf,
											  uint32_t bufSize, uint32_t *tmstamp, uint32_t fieldId);

	/*!
 * Callback to producer or consumer application to get the next slice buffer offset necessary to continue streaming.
 * If application cannot return the next buffer then it should return the same value as prevOffset and then call
 * St21ProducerUpdate or St21ConsumerUpdate to restart streaming.
 *
 * @param appHandle - IN/OUT Handle to the app.
 * @param frameBuf - IN Buffer containing current video slice.
 * @param prevOffset - IN Offset of the previous video slice.
 * @param fieldId - IN For interlaced video this field describes whether it's odd frame(0) or even (1)
 *
 * @return Offset of the next slice to be fetched (returned type uint32_t)
 */

	typedef uint32_t (*St21GetNextSliceOffset_f)(void *appHandle, uint8_t *frameBuf,
												 uint32_t prevOffset, uint32_t fieldId);

	/*!
 * Callback to producer application to get timestamp as transported in SDI frame.
 *
 * @param appHandle - IN/OUT Handle to the app.
 *
 * @return uint32_t
 */
	typedef uint32_t (*St21GetFrameTmstamp_f)(void *appHandle);

	/*!
 * Callback to consumer application with notification about the frame receive completion.
 * Upon packets loss, buffer release is signaled with St21NotifyFrameDone_f callback.
 *
 * @param appHandle - IN/OUT Handle to the app
 * @param frameBuf - IN Buffer containing video frame data
 * @param tmstamp - IN Timestamp of received Video frame retrieved from RTP header
 * @param fieldId - IN ID of field (in case of interlaced scan)
 *
 * @return void
 */
	typedef void (*St21NotifyFrameRecv_f)(void *appHandle, uint8_t *frameBuf, uint32_t tmstamp,
										  uint32_t fieldId);

	/*!
 * Callback to consumer application with notification about the slice receive completion.
 * Slice buffer can not be released yet or reused since it can be used in the next frame.
 * Upon packets loss, buffer release is singled with St21NotifyFrameDone_f callback.
 *
 * @param appHandle - IN/OUT Handle to the app
 * @param frameBuf - IN Buffer containing video frame data
 * @param sliceOffset - IN Offset of the previous sent slice of data
 * @param fieldId - IN ID of field (in case of interlaced scan)
 *
 * @return void
 */
	typedef void (*St21NotifySliceRecv_f)(void *appHandle, uint8_t *frameBuf, uint32_t sliceOffset,
										  uint32_t tmstamp, uint32_t fieldId);

	/*!
 * Callback to producer or consumer application with notification about the frame completion.
 * Frame buffer can be released or reused after it, but not sooner.
 *
 * @param appHandle - IN/OUT Handle to the app
 * @param frameBuf - IN Buffer containing video frame data
 * @param fieldId - IN ID of field (in case of interlaced scan)
 *
 * @return void
 */
	typedef void (*St21NotifyFrameDone_f)(void *appHandle, uint8_t *frameBuf, uint32_t fieldId);

	/*!
 * Callback to producer or consumer application with notification about the slice completion
 * Slice buffer can be released or reused after it, but not sooner.
 *
 * @param appHandle - IN/OUT Handle to the app
 * @param sliceBuf - IN buffer containing video slice data
 * @param fieldId - IN ID od field (in case of interlaced scan)
 *
 * @return void
 */
	typedef void (*St21NotifySliceDone_f)(void *appHandle, uint8_t *sliceBuf, uint32_t fieldId);

	/*!
 * Callback to producer or consumer application with notification about the stopping of the session.
 * It means that all buffer pointers can be released after it, but not sooner.
 *
 * @param appHandle - IN/OUT Handle to the app
 *
 * @return void
 */
	typedef void (*St21NotifyStopDone_f)(void *appHandle);

	/*!
 * Callback to producer or consumer application with notification about the unexpected session drop.
 * It means that all buffer pointers can be released after it.
 *
 * @param appHandle - IN/OUT Handle to the app
 *
 * @return void
 */
	typedef void (*St21NotifyStreamDrop_f)(void *appHandle);

	/*!
 * Callback to consumer application about putting the frame 90kHz receive timestamp in stream.
 *
 * @param appHandle - IN/OUT Handle to the app
 * @param tmstamp - IN Timestamp
 *
 * @return void
 */
	typedef void (*St21PutFrameTmstamp_f)(void *appHandle, uint32_t tmstamp);

	/**
 * \struct st21_producer
 * 
 * \brief Structure for video producer application
 */
	struct st21_producer
	{
		void *appHandle;	/**< App handle */
		st21_prod_type_t prodType;	/**< Slice mode or frame mode w/ or w/o timestamp */
		uint32_t frameSize;	/**< Size of the video frame */
		uint32_t frameOffset;	/**< Offset of the previous sent frames */
		uint32_t sliceSize;	/**< Size of the video slice */
		uint32_t sliceOffset;	/**< Offset of the previous sent slice of data */
		uint32_t sliceCount;  /**< Count of slices in Frame if slice mode */
		uint32_t dualPixelSize;
		uint32_t pixelGrpsInSlice;
		uint32_t linesInSlice;	/**< Size of the slice (in lines) */
		uint32_t firstTmstamp;	/**< First Timestamp time */
		uint8_t *frameBuf;	/**< Current frameBuffer */
		uint32_t frameCursor;
		volatile uint32_t frameCursorSending;
		St21GetNextFrameBuf_f St21GetNextFrameBuf;	/**< Callback type \ref St21GetNextFrameBuf_f */
		St21GetNextSliceOffset_f St21GetNextSliceOffset;	/**< Callback type \ref St21GetNextSliceOffset_f */
		St21GetFrameTmstamp_f St21GetFrameTmstamp;	/**< Callback type \ref St21GetFrameTmstamp_f */
		St21NotifyFrameDone_f St21NotifyFrameDone;	/**< Callback type \ref St21NotifyFrameDone_f */
		St21NotifySliceDone_f St21NotifySliceDone;	/**< Callback type \ref St21NotifySliceDone_f */
		St21NotifyStopDone_f St21NotifyStopDone;	/**< Callback type \ref St21NotifyStopDone_f */
	};
	typedef struct st21_producer st21_producer_t;

	/*!
 * Called by the producer to register live producer for video streaming.
 *
 * @param sn - IN Session pointer.
 * @param prod - IN Register callbacks to allow interaction with live producer.
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	st_status_t
	StRegisterProducer(st_session_t *sn, void *prod);

	/*!
 * Called by the producer asynchronously to start each frame of video streaming
 *
 * @param sn - IN Session pointer.
 * @param frameBuf - IN 1st frame buffer for the session.
 * @param linesOffset - IN offset in complete lines of the frameBuf after which the producer filled the buffer.
 * @param tmstamp - IN if not 0 then 90kHz timestamp of the frame.
 * @param ptpTime - IN if not 0 then start new frame at the given PTP timestamp + TROFFSET .
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	st_status_t St21ProducerStartFrame(st_session_t *sn, uint8_t *frameBuf, uint32_t linesOffset, 
										uint32_t tmstamp, uint64_t ptpTime);

	/*!
 * Called by the producer asynchronously to update video streaming.
 * In case producer has more data to send, it also restarts streaming
 * if the producer callback failed due to lack of buffer with video.
 *
 * @param sn - IN/OUT Session pointer
 * @param frameBuf - IN frame buffer for the session from which to restart.
 * @param linesOffset - IN offset in complete lines of the frameBuf after which producer filled the buffer.
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	st_status_t St21ProducerUpdate(st_session_t *sn, uint8_t *frameBuf, uint32_t linesOffset);

	/*!
 * Called by the producer asynchronously to stop video streaming,
 * the session will notify the producer about completion with callback.
 *
 * @param sn - IN/OUT Session pointer
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	st_status_t StProducerStop(st_session_t *sn);

	/**
 * \struct st21_consumer
 * 
 * \brief Structure for video consumer
 */
	struct st21_consumer
	{
		void *appHandle;	/**< Handle to the application */
		st21_cons_type_t consType; /**< Type of consumer defined by \ref st21_cons_type */
		uint32_t frameSize; /**< Size of the video frame */
		uint32_t sliceSize;	/**< Size of the video slice */
		uint32_t sliceCount;  /**< count of slices in Frame if slice mode */
		St21GetNextFrameBuf_f St21GetNextFrameBuf;	/**< Callback type \ref St21GetNextFrameBuf_f */
		St21GetNextSliceOffset_f St21GetNextSliceOffset;	/**< Callback type \ref St21GetNextSliceOffset_f */
		St21NotifyFrameRecv_f St21NotifyFrameRecv;	/**< Callback type \ref St21NotifyFrameRecv_f */
		St21NotifySliceRecv_f St21NotifySliceRecv;	/**< Callback type \ref St21NotifySliceRecv_f */
		St21PutFrameTmstamp_f St21PutFrameTmstamp;	/**< Callback type \ref St21PutFrameTmstamp_f */
		St21NotifyFrameDone_f St21NotifyFrameDone;	/**< Callback type \ref St21NotifyFrameDone_f */
		St21NotifySliceDone_f St21NotifySliceDone;	/**< Callback type \ref St21NotifySliceDone_f */
		St21NotifyStopDone_f St21NotifyStopDone;	/**< Callback type \ref St21NotifyStopDone_f */
		St21RecvRtpPkt_f St21RecvRtpPkt;	/**< Callback type \ref St21RecvRtpPkt_f */
	};
	typedef struct st21_consumer st21_consumer_t;

	/*!
 * Called by the consumer to register live consumer for video streaming.
 *
 * @param sn - IN Session pointer.
 * @param cons - IN consumer callbacks structure.
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	st_status_t StRegisterConsumer(st_session_t *sn,  //IN session pointer.
								   void *cons);		  //IN consumer callbacks structure.

	/*!
 * Called by the consumer asynchronously to start video streaming.
 *
 * @param sn - IN Session pointer.
 * @param frameBuf - IN 1st frame buffer for the session.
 * @param ptpTime - if not 0 then start receiving session since the given PTP time.
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	st_status_t St21ConsumerStartFrame(
		st_session_t *sn,	//IN session pointer.
		uint8_t *frameBuf,	//IN 1st frame buffer for the session.
		uint64_t ptpTime);	//IN if not 0 then start receiving session since the given ptp time.

	/*!
 * Called by the consumer asynchronously to update or restart video streaming.
 * if the consumer callback failed due to lack of available buffer.
 *
 * @param sn - IN Session pointer.
 * @param frameBuf - IN Frame buffer for the session from which to restart.
 * @param linesOffset - IN Offset in complete lines of the frameBuf to which consumer can fill the buffer.
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	st_status_t St21ConsumerUpdate(
		st_session_t *sn,	//IN Session pointer.
		uint8_t *frameBuf,	//IN Frame buffer for the session from which to restart.
		uint32_t
			linesOffset);  //IN Offset in complete lines of the frameBuf to which consumer can fill the buffer.

	/*!
 * Called by the consumer asynchronously to stop video streaming.
 * The session will notify the consumer about completion with callback.
 *
 * @param sn -  IN/OUT session pointer.
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	st_status_t ConsumerStop(st_session_t *sn);

	/*!
 * Allocate memory for transmitting frames
 *
 * @param sn - IN Session pointer.
 * @param frameSize - IN Size of memory.
 *
 * @return Handle to the buffer for essence content
 */
	uint8_t *StAllocFrame(st_session_t *sn,		//IN session pointer.
						  uint32_t frameSize);	//IN Size of memory.

	/*!
 * Free memory if ref counter is zero, otherwise reports error that memory is
 * still in use and free should be retried.
 *
 * @param sn - IN Session pointer.
 * @param frame - IN Address of memory to be freed.
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	st_status_t StFreeFrame(st_session_t *sn,  //IN Session pointer.
							uint8_t *frame);   //IN Address of memory to be freed.
	/****************************************************************************************************
 *
 *
 * ST30 API group
******************************************************************************************************/

/******************************************************************************************************
 *
 * Callbacks to Audio producer or consumer application to manage live streaming
 *
 ******************************************************************************************************/

	/*!
 * Callback to Audio consumer application to pass RTP packet after UDP header to the application
 * (rtpHdr points to RTP header). Application is responsible of parsing and understanding the packet.
 * This is for the application implementing their own packet parsing and formats. PTP timestamp (in nanoseconds) is also passed.
 * The passed buffers are freed after the call by the library, so the application should consume them within the callback.
 *
 * @param appHandle - IN/OUT Handle to the app.
 * @param pktHdr - IN Handle to the RTP header.
 * @param hdrSize - IN Size of the RTP header.
 * @param rtpPayload - IN Handle to the buffer with data.
 * @param payloadSize - IN Size of the payload (data to receive).
 * @param tmstamp - IN Timestamp.
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	typedef st_status_t (*St30RecvRtpPkt_f)(void *appHandle, uint8_t *pktHdr, uint16_t hdrSize,
											uint8_t *rtpPayload, uint16_t payloadSize,
											uint64_t tmstamp);

	/*!
 * Callback to producer or consumer application to get next frame buffer necessary to continue streaming
 * If application cannot return the next buffer returns NULL and then has to call St30ProducerUpdate
 * or St30ConsumerUpdate to restart streaming.
 *
 * @param appHandle - IN/OUT Handle to the app.
 * @param prevAudioBuf - Handle to the previous buffer with audio data.
 * @param bufSize - IN Size of the audio buffer.
 * @param tmstamp - user provided timestamp to be used in RTP tmstamp field when enqueueing packets for this frame (not supported yet)
 *
 * @return uint8_t
 */
	typedef uint8_t *(*St30GetNextAudioBuf_f)(void *appHandle, uint8_t *prevAudioBuf,
											  uint32_t bufSize, uint32_t *tmstamp);

	/*!
 * Callback to producer or consumer application to get next slice buffer offset necessary to continue streaming.
 * If application cannot return the next buffer then it should returns the same value as prevOffset and then call
 * St30ProducerUpdate or St30ConsumerUpdate to restart streaming.
 *
 * @param appHandle - IN/OUT Handle to the app.
 * @param audioBuf - IN Buffer containing audio data.
 * @param prevOffset - IN Offset of the previous sent data.
 * @param tmstamp - IN Timestamp.
 *
 * @return uint32_t
 */
	typedef uint32_t (*St30GetNextSampleOffset_f)(void *appHandle, uint8_t *audioBuf,
												  uint32_t prevOffset, uint32_t *tmstamp);

	/*!
 * Callback to consumer application with notification about the frame receive completion.
 * Frame buffer can not be yet released or reused since it can be used to the next frame
 * upon packets loss, buffer release is signaled with St30NotifyBufferDone_f callback.
 *
 * @param appHandle - IN/OUT Handle to the app.
 * @param audioBuf - IN Buffer containing audio data.
 * @param bufOffset - IN Offset of the previous sent audio frames
 * @param tmstamp - IN Timestamp.
 *
 * @return void
 */
	typedef void (*St30NotifySampleRecv_f)(void *appHandle, uint8_t *audioBuf, uint32_t bufOffset,
										   uint32_t tmstamp);

	/*!
 * Callback to producer or consumer application with notification about the frame completion
 * Audio buffer can be released or reused after it, but not sooner.
 *
 * @param appHandle - IN/OUT Handle to the app.
 * @param audioBuf - IN Bbuffer containing audio data.
 *
 * @return void
 */
	typedef void (*St30NotifyBufferDone_f)(void *appHandle, uint8_t *audioBuf);

	/*!
 * Callback to producer or consumer application with notification about completion of the session stop.
 * It means that all buffer pointers can be released after it, but not sooner.
 *
 * @param appHandle - IN/OUT Handle to the app
 *
 * @return void
 */
	typedef void (*St30NotifyStopDone_f)(void *appHandle);

	/*!
 * Callback to producer or consumer application with notification about the unexpected session drop.
 * It means that all buffer pointers can be released after it.
 *
 * @param appHandle - IN/OUT Handle to the app
 *
 * @return void
 */
	typedef void (*St30NotifyStreamDrop_f)(void *appHandle);

	/**
 * \struct st30_producer
 * 
 * \brief Structure for audio producer application
 */
	struct st30_producer
	{
		void *appHandle;	/**< Handle to the application */
		st30_prod_type_t prodType;	/**< slice mode or frame mode w/ or w/o timestamp */
		uint32_t bufSize;	/**< Size of the audio buffer */
		uint32_t bufOffset;	/**< Offset of the previous sent audio frames */
		uint8_t *frameBuf;	/**< Current audio frame buffer */
		uint32_t frameCursor;
		St30GetNextAudioBuf_f St30GetNextAudioBuf;	/**< Callback type \ref St30GetNextAudioBuf_f */
		St30GetNextSampleOffset_f St30GetNextSampleOffset;	/**< Callback type \ref St30GetNextSampleOffset_f */
		St30NotifyBufferDone_f St30NotifyBufferDone;	/**< Callback type \ref St30NotifyBufferDone_f */
		St30NotifyStopDone_f St30NotifyStopDone;	/**< Callback type \ref St30NotifyStopDone_f */
	};
	typedef struct st30_producer st30_producer_t;

	/*!
 * Called by the producer asynchronously to start each frame of video streaming
 *
 * @param sn - Session pointer.
 * @param audioBuf - IN 1st frame buffer for the session.
 * @param bufOffset - IN offset in the buffer.
 * @param tmstamp - IN if not 0 then 48kHz timestamp of the samples.
 * @param ptpTime - IN if not 0 start at the given PTP timestamp.
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	st_status_t
	St30ProducerStartFrame(st_session_t *sn, uint8_t *audioBuf, uint32_t bufOffset, uint32_t tmstamp, uint64_t ptpTime);

	/*!
 * Called by the producer asynchronously to update video streaming.
 * In case producer has more data to send, it also restarts streaming
 * if the producer callback failed due to lack of buffer with video.
 *
 * @param sn - IN/OUT Session pointer.
 * @param audioBuf - IN Audio buffer for the session from which to restart.
 * @param bufOffset - IN Offset in the buffer.
 * @param tmstamp - IN if not 0 then 48kHz timestamp of the samples.
 * @param ptpTime - IN if not 0 start at the given PTP timestamp.
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	st_status_t
	St30ProducerUpdate(st_session_t *sn, uint8_t *audioBuf, uint32_t bufOffset, uint32_t tmstamp, uint64_t ptpTime);

	/*!
 * Called by the producer asynchronously to stop audio streaming.
 * The session will notify the producer about completion with callback.
 *
 * @param sn IN/OUT session pointer
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	st_status_t St30ProducerStop(st_session_t *sn);

	/**
 * \struct st30_consumer
 * 
 * \brief Structure for audio consumer application
 */
	struct st30_consumer
	{
		void *appHandle;	/**< Handle to the application */
		st30_cons_type_t consType;	// slice mode or frame mode w/ or w/o timestamp
		uint32_t bufSize;	/**< Size of the audio buffer */
		St30GetNextAudioBuf_f St30GetNextAudioBuf;	/**< Callback type \ref St30GetNextAudioBuf_f */
		St30NotifySampleRecv_f St30NotifySampleRecv;	/**< Callback type \ref St30NotifySampleRecv_f */
		St30NotifyBufferDone_f St30NotifyBufferDone;	/**< Callback type \ref St30NotifyBufferDone_f */
		St30NotifyStopDone_f St30NotifyStopDone;	/**< Callback type \ref St30NotifyStopDone_f */
		St30RecvRtpPkt_f St30RecvRtpPkt;	/**< Callback type \ref St30RecvRtpPkt_f */
	};
	typedef struct st30_consumer st30_consumer_t;

	/*!
 * Called by the consumer asynchronously to start audio streaming
 *
 * @param sn - IN Session pointer.
 * @param frameBuf - IN 1st frame buffer for the session.
 * @param ptpTime -IN if not 0 start receiving session since the given PTP time.
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	st_status_t St30ConsumerStartFrame(st_session_t *sn, uint8_t *frameBuf, uint64_t ptpTime);

	/*!
 * Called by the consumer asynchronously to update video streaming.
 * In case consumer has more data to send, it also restarts streaming.
 * if the consumer callback failed due to lack of buffer with video.
 */
	st_status_t
	St30ConsumerUpdate(st_session_t *sn, uint8_t *audioBuf, uint32_t bufOffset, uint32_t tmstamp, uint64_t ptpTime);

	/*!
 * Called by the consumer asynchronously to stop audio streaming.
 * The session will notify the consumer about completion with callback.
 */
	st_status_t St30ConsumerStop(st_session_t *sn);

/******************************************************************************************************
 *
 * ST40 API group
 *
******************************************************************************************************/

/******************************************************************************************************
 *
 * Callbacks to Ancillary data producer or consumer application to manage live streaming
 *
 ******************************************************************************************************/

	/*!
 * Callback to consumer application to get next frame buffer necessary to continue streaming.
 * If application cannot return the next buffer it should return NULL.
 *
 * @param appHandle - IN/OUT Handle to the app
 *
 * @return void
 */
	typedef void *(*St40GetNextAncFrame_f)(void *appHandle);

	/*!
 * Callback to producer or consumer application with notification about the frame completion.
 * Ancillary data buffer can be released or reused after it, but not sooner.
 * 
 * @param appHandle - IN/OUT Handle to the app.
 * @param ancBuf - IN Buffer containing ancillary data.
 *
 * @return void
 */
	typedef void (*St40NotifyFrameDone_f)(void *appHandle, void *ancBuf);

	/*!
 * \struct st40_producer
 * 
 * \brief Structure for ancillary data producer application
 * 
 */
	struct st40_producer
	{
		void *appHandle; 	/**< Handle to the application */
		st40_prod_type_t prodType;	/**< slice mode or frame mode w/ or w/o timestamp */
		struct rte_mempool *mbufPool;
		uint32_t bufSize;	/**< Size of the ancillary data buffer */
		uint32_t bufOffset;	/**< Offset of the previous sent ancillary data */
		strtp_ancFrame_t *frameBuf; /**< Current ancillary data buffer */
		uint32_t frameCursor;
		St40GetNextAncFrame_f St40GetNextAncFrame;	/**< Callback type \ref St40GetNextAncFrame_f */
		St40NotifyFrameDone_f St40NotifyFrameDone;	/**< Callback type \ref St40NotifyFrameDone_f */
	};
	typedef struct st40_producer st40_producer_t;

	/*! 
 * Called by the producer asynchronously to start each frame of ancillary data streaming.
 *
 * @param sn - IN Session pointer.
 * @param ancBuf - IN 1st frame buffer for the session.
 * @param bufOffset - IN Offset in the buffer.
 * @param tmstamp - IN Timestamp.
 * @param ptpTime -IN if not 0 start receiving session since the given PTP time.
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
*/
	st_status_t
	St40ProducerStartFrame(st_session_t *sn, uint8_t *ancBuf, uint32_t bufOffset, uint32_t tmstamp, uint64_t ptpTime);

	/**
 * \struct st40_consumer
 * 
 * \brief Structure for ancillary data consumer application
 */
	struct st40_consumer
	{
		void *appHandle;	/**< Handle to the application */
		st40_cons_type_t consType;	// slice mode or frame mode w/ or w/o timestamp
		uint32_t bufSize;	/**< Size of the ancillary data buffer */
		St40GetNextAncFrame_f St40GetNextAncFrame;	/**< Callback type \ref St40GetNextAncFrame_f */
		St40NotifyFrameDone_f St40NotifyFrameDone;	/**< Callback type \ref St40NotifyFrameDone_f */
	};
	typedef struct st40_consumer st40_consumer_t;

	/**
 * Called by the consumer asynchronously to start each frame of ancillary data streaming
 * 
 * @param sn - IN Session pointer.
 *
 * @return ZERO if success or negative value in case of error, see error codes: \ref st_status_t
 */
	st_status_t St40ConsumerStartFrame(st_session_t *sn);

	/**
 * Display library statistics
 */
	void StDisplayExitStats(void);

	static inline uint64_t
	StTimespecToNs(const struct timespec *ts)
	{
		return ((uint64_t)ts->tv_sec * NS_PER_S) + ts->tv_nsec;
	}

	/* Monotonic time (in nanoseconds) since some unspecified starting point. */
	static inline uint64_t
	StGetMonotonicTimeNano()
	{
		struct timespec ts;

		clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
		return StTimespecToNs(&ts);
	}

#ifdef __cplusplus
}
#endif	// __cplusplus
#endif
