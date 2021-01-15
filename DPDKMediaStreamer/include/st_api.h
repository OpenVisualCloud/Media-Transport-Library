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

#ifndef _ST_API_H
#define _ST_API_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <rte_mbuf.h>

#define ST_VERSION_MAJOR 1
#define ST_VERSION_MAJOR_CURRENT ST_VERSION_MAJOR

#define ST_VERSION_MINOR 0
#define ST_VERSION_MINOR_CURRENT ST_VERSION_MINOR

#define ST_VERSION_LAST 17
#define ST_VERSION_LAST_CURRENT ST_VERSION_LAST

struct st_version
{
	uint16_t major;
	uint16_t minor;
	uint16_t last;
};
typedef struct st_version st_version_t;

/**
 * Enumeration for API return codes, errors are below 0
 */
typedef enum st_status
{
	ST_OK = 0,
	ST_GENERAL_ERR = -1,  // NULL ptr or similar heavy condition
	ST_NO_MEMORY = -2,
	ST_NOT_SUPPORTED = -3,
	ST_INVALID_PARAM = -4,	//unexpected argument to a function
	ST_NOT_IMPLEMENTED = -5,
	ST_INVALID_API_VERSION = -6,

	ST_BAD_PARAM_ID = -10,
	ST_BAD_PARAM_VAL = -11,

	ST_BAD_NIC_PORT_ID = -19,
	ST_BAD_UDP_DPORT = -20,
	ST_BAD_UDP_SPORT = -21,
	ST_BAD_SRC_IPADDR = -22,
	ST_BAD_DST_IPADDR = -23,

	ST_SN_ERR_NOT_COMPATIBLE = -30,
	ST_SN_ERR_RATE_NO_FIT = -31,
	ST_SN_ERR_NO_TIMESLOT = -32,
	ST_SN_ERR_NOT_READY = -33,
	ST_SN_ERR_DISCONNECTED = -34,
	ST_SN_ERR_IN_USE = -35,

	ST_BAD_PRODUCER = -50,
	ST_BAD_CONSUMER = -51,
	ST_TOO_SMALL_BUFFER = -52,
	ST_TOO_BIG_BUFFER = -53,
	ST_BUFFER_NOT_READY = -54,
	ST_PROD_ALREADY_REGISTERED = -55,
	ST_CONS_ALREADY_REGISTERED = -55,

	ST_FMT_ERR_BAD_PIX_FMT = -100,
	ST_FMT_ERR_BAD_HEIGHT = -101,
	ST_FMT_ERR_BAD_WIDTH = -102,
	ST_FMT_ERR_BAD_VSCAN = -103,
	ST_FMT_ERR_BAD_TROFF = -104,
	ST_FMT_ERR_BAD_PG_SZ = -105,
	ST_FMT_ERR_BAD_BLANKING = -106,
	ST_FMT_ERR_BAD_CLK_RATE = -107,
	ST_FMT_ERR_BAD_PIXELS_IN_PKT = -108,
	ST_FMT_ERR_BAD_PKTS_IN_LINE = -109,
	ST_FMT_ERR_BAD_PKT_SZ = -110,
	ST_FMT_ERR_BAD_FRAME_TIME = -111,
	ST_FMT_ERR_BAD_PKTS_IN_FRAME = -112,
	ST_FMT_ERR_NOT_SUPPORTED_ON_TX = -113,

	ST_PKT_DROP_BAD_PKT_LEN = -199,	 //on device so session unknown
	ST_PKT_DROP_BAD_IP_LEN = -200,	 //sessions statistics
	ST_PKT_DROP_BAD_UDP_LEN = -201,
	ST_PKT_DROP_BAD_RTP_HDR = -202,
	ST_PKT_DROP_BAD_RTP_TMSTAMP = -203,
	ST_PKT_DROP_NO_FRAME_BUF = -204,
	ST_PKT_DROP_INCOMPL_FRAME = -205,
	ST_PKT_DROP_BAD_RTP_LN_LEN = -206,
	ST_PKT_DROP_BAD_RTP_LN_NUM = -207,
	ST_PKT_DROP_BAD_RTP_OFFSET = -208,
	ST_PKT_DROP_BAD_RTP_LN_CONT = -209,

	ST_PKT_LOST_TIMEDOUT = -300,

	ST_APP_FILE_OPEN_ERR = -400,

	ST_DEV_GENERAL_ERR = -500,
	ST_DEV_BAD_PORT_NAME = -501,
	ST_DEV_BAD_PACING = -502,
	ST_DEV_BAD_NIC_RATE = -503,
	ST_DEV_BAD_EXACT_RATE = -504,
	ST_DEV_BAD_PORT_TYPE = -505,
	ST_DEV_PORT_MAX_TYPE_PREP = -506,
	ST_DEV_CANNOT_PREP_CONSUMER = -507,
	ST_DEV_CANNOT_PREP_PRODUCER = -508,
	ST_DEV_ERR_NOT_READY = -509,

	ST_DEV_NOT_FIND_SPEED_CONF = -515,

	ST_DEV_NO_NUMA = -520,
	ST_DEV_NO_1GB_PAGE = -521,
	ST_DEV_NO_MIN_NUMA = -522,

	ST_DEV_CANNOT_PREPARE_MBUF = -525,

	ST_DEV_CANNOT_LOAD_MOD = -543,
	ST_DEV_CANNOT_UNLOAD_MOD = -544,
	ST_DEV_MOD_NOT_LOADED = -545,
	ST_DEV_CANNOT_BIND_MOD = -546,
	ST_DEV_CANNOT_UNBIND_MOD = -547,
	ST_DEV_MOD_NOT_BINDED = -548,
	ST_DEV_CANNOT_READ_CPUS = -549,
	ST_DEV_MAX_ERR = ST_DEV_CANNOT_READ_CPUS,

	ST_KNI_GENERAL_ERR = -550,
	ST_KNI_CANNOT_PREPARE = -551,
	ST_KNI_ALREADY_PREPARED = -552,
	ST_KNI_INTER_NOT_FOUND = -599,
	ST_KNI_MAX_ERR = -ST_KNI_INTER_NOT_FOUND,

	ST_PTP_GENERAL_ERR = -600,
	ST_PTP_NOT_VALID_CLK_SRC = -601,

	ST_IGMP_GENERAL_ERR = -650,
	ST_IGMP_QUERIER_NOT_READY = -651,
	ST_IGMP_WRONG_IP_ADDRESS = -652,

} st_status_t;

#define ST_PKT_DROP(reason) (-(reason + 200))
#define ST_PKT_DROP_MAX 10

#define ST_FRM_DROP(reason) (-(reason + 204))
#define ST_FRM_DROP_MAX 2

#define ST_PKT_LOST(reason) (-(reason + 300))
#define ST_PKT_LOST_MAX 1

typedef enum st_dev_type
{
	ST_DEV_TYPE_PRODUCER,
	ST_DEV_TYPE_CONSUMER
} st_dev_type_t;

typedef enum st_exact_rate
{
	ST_DEV_RATE_P_29_97 = 29,
	ST_DEV_RATE_P_59_94 = 59,
	ST_DEV_RATE_P_25_00 = 25,
	ST_DEV_RATE_P_50_00 = 50,
	ST_DEV_RATE_I_29_97 = 129,
	ST_DEV_RATE_I_59_94 = 159,
	ST_DEV_RATE_I_25_00 = 125,
	ST_DEV_RATE_I_50_00 = 150,
} st_exact_rate_t;

/**
 * Enumeration for ST2110-21 sender pacer types
 */
typedef enum st_pacer_type
{
	ST_2110_21_TPW = 1,	  //wide sender
	ST_2110_21_TPNL = 2,  //narrow linear sender
	ST_2110_21_TPN = 3,	  //narrow gapped sender
} st_pacer_type_t;

/**
 * Structure for ST device 2110-21 video session
 */
struct st_device
{
	st_version_t ver;
	uint32_t snCount;
	st_dev_type_t type;	 //producer or consumer
	st_pacer_type_t pacerType;
	st_exact_rate_t exactRate;
	uint32_t rateGbps;	//rate in Gbps, 10, 25, 40, 100 are expected values
	uint16_t port[2];
	uint16_t mtu;			   //if > 1500 requested MTU, updated with value possible on the links
	uint16_t maxSt21Sessions;  // expected maximal number of ST21 video sessions/channels
							   // to be supported of 1080p29 30000/1001 FPS
	uint16_t maxSt30Sessions;  // expected maximal number of ST30 or ST31 audio sessions/channels
							   // to be supported of 8 channels audio
	uint16_t maxSt40Sessions;  // expected maximal number of ST40 ancillary sessions/channels
							   //to be supported of overlays and watermarks
};
typedef struct st_device st_device_t;

/**
 * Enumeration for ST parameters values allowed for St21SetParam and St21GetParam
 */
typedef enum st21_param_val
{
	ST21_FRM_NO_FIX = 100,			   // allowed for ST_2110_21_FRM_FIX_MODE
	ST21_FRM_FIX_PREV = 101,		   // allowed for ST_2110_21_FRM_FIX_MODE
	ST21_FRM_FIX_2022_7 = 102,		   // allowed for ST_2110_21_FRM_FIX_MODE
	ST21_FRM_FIX_PREV_N_2022_7 = 103,  // allowed for ST_2110_21_FRM_FIX_MODE
	ST21_FRM_2022_7_MODE_ON = 200,	   // allowed for ST_2110_21_FRM_2022_7_MODE
	ST21_FRM_2022_7_MODE_OFF = 201,	   // allowed for ST_2110_21_FRM_2022_7_MODE
} st21_param_val_t;

/**
 * Enumeration for ST parameters - functions such :
 * StPtpSetParam and StPtpGetParam
 * St21SetParam and St21GetParam
 * but also other futher parameters of other sessions
 */
typedef enum st_param
{
	ST21_FRM_FIX_MODE = 10,		//configurable, by default enabled for ST21_FRM_FIX_PREV_N_2022_7
	ST21_FRM_2022_7_MODE = 11,	//configurable, by default enabled if 2nd port is configured

	ST21_TPRS = 20,		  //value shall be in nanoseconds, read-only
	ST21_TR_OFFSET = 21,  //value shall be in nanoseconds, read-only
	ST21_FRM_TIME = 22,	  //value shall be in nanoseconds, read-only
	ST21_PKT_TIME = 23,	  //value shall be in nanoseconds, read-only

	ST21_PIX_GRP_SZ = 30,  // read-only

	ST_BUILD_ID = 40,		// read-only
	ST_LIB_VERSION = 41,	// read-only

	ST_PTP_DROP_TIME = 100,
	ST_PTP_CLOCK_ID = 101,
	ST_PTP_ADDR_MODE = 102,
	ST_PTP_STEP_MODE = 103,
	ST_PTP_CHOOSE_CLOCK_MODE = 104,

	ST_SOURCE_IP = 150,
	ST_DESTINATION_IP = 151,
	ST_EBU_TEST = 152,
	ST_SN_COUNT = 153,
	ST_TX_ONLY = 154,
	ST_RX_ONLY = 155,
	ST_MAC = 156,
	ST_OUT_PORT = 157,
	ST_IN_PORT = 158,
	ST_FMT_INDEX = 159,
	ST_DPDK_PARAMS = 160,
} st_param_t;

/**
 * Enumeration for viceo producer capabilities
 */
typedef enum st21_prod_type
{
	ST21_PROD_INVALID = 0x00,
	ST21_PROD_P_FRAME = 0x10,		   //producer of complete frames in progressive mode
	ST21_PROD_P_FRAME_TMSTAMP = 0x11,  //producer of complete frames and having SDI
									   //timestamp already determined
	ST21_PROD_I_FIELD = 0x12,		   //producer of interlaced fields
	ST21_PROD_I_FIELD_TMSTAMP = 0x13,  //producer of interlaced fields and having SDI
									   //timestamp already determined
	ST21_PROD_P_FRAME_SLICE = 0x20,	   //producer of slices of progressive frames
	ST21_PROD_P_SLICE_TMSTAMP = 0x21,  //producer of sliced frames and having SDI
									   //timestamp already determined at the frame level
	ST21_PROD_I_FIELD_SLICE = 0x22,	   //producer of slices of interlaced fields
	ST21_PROD_I_SLICE_TMSTAMP = 0x23,  //producer of slices of interlaced fields and
									   //having SDI timestamp already determined at the field level
	ST21_PROD_RAW_RTP = 0x30,		   //producer that assembles by their own RTP packets,
									   //it uses only callback of St21BuildRtpPkt_f
	ST21_PROD_RAW_L2_PKT = 0x31,	   //producer that assembles by their own L2 packets (header points to L2 layer),
									   //it uses only callback of St21BuildRtpPkt_f
	ST21_PROD_LAST = ST21_PROD_RAW_L2_PKT,
} st21_prod_type_t;

/**
 * Enumeration for viceo consumer capabilities
 */
typedef enum st21_cons_type
{
	ST21_CONS_INVALID = 0x00,
	ST21_CONS_P_FRAME = 0x10,		   //consumer of complete progressive frames
	ST21_CONS_P_FRAME_TMSTAMP = 0x11,  //consumer of complete progressive frames
									   //and requesting SDI timestamp notification
	ST21_CONS_I_FIELD = 0x12,		   //consumer of interlaced fields
	ST21_CONS_I_FIELD_TMSTAMP = 0x13,  //consumer of interlaced fields and having SDI
									   //timestamp already determined
	ST21_CONS_P_FRAME_SLICE = 0x20,	   //consumer of slices of progressive frames
	ST21_CONS_P_SLICE_TMSTAMP = 0x21,  //consumer of slices of progressive frames
									   //and requesting SDI timestamp notification
	ST21_CONS_I_FIELD_SLICE = 0x22,	   //consumer of slices of interlaced fields
	ST21_CONS_I_SLICE_TMSTAMP = 0x23,  //consumer of slices of interlaced fields and
									   //having SDI timestamp already determined at the field level
	ST21_CONS_RAW_RTP = 0x30,		   //consumer of not parsed RTP packets, it uses only
									   //callback of St21RecvRtpPkt_f
	ST21_CONS_RAW_L2_PKT = 0x31,	   //consumer of not parsed raw L2 packets (header points to L2 layer),
									   // it uses only callback of St21RecvRtpPkt_f

	ST21_CONS_LAST = ST21_CONS_RAW_L2_PKT,
} st21_cons_type_t;

/**
 * Enumeration for session capabilities
 */
typedef enum st21_sn_flags
{
	ST21_SN_INVALID = 0x0000,
	ST21_SN_SINGLE_PATH = 0x0001,	//raw video single path
	ST21_SN_DUAL_PATH = 0x0002,		//raw video dual path
	ST21_SN_UNICAST = 0x0004,		//raw video unicast
	ST21_SN_MULTICAST = 0x0008,		//raw video multicasts
	ST21_SN_CONNECTLESS = 0x0010,	//raw video connection less producer
	ST21_SN_CONNECT = 0x0020,		//raw video connected session
	ST21_SN_ANNOUNCE_SDP = 0x0040,	//valid for ST21_SN_CONNECT type and marks
									//additional SDP announcement
	ST21_SN_VLAN_PRESENT = 0x0100,	//VLAN tagged
	ST21_SN_VLAN_PCP = 0x0200,		//VLAN tagged with PCP traffic classification
	ST21_SN_DIFF_SERV = 0x0400,		//IP differentiated services enabled,
									//dscp field used in IP protocol
} st21_sn_flags_t;

/**
 * Enumeration for ST2110-21 supported pixel formats
 * of packet payload on a wire
 */
typedef enum st21_pix_fmt
{
	ST21_PIX_FMT_RGB_8BIT = 10,
	ST21_PIX_FMT_RGB_10BIT_BE,
	ST21_PIX_FMT_RGB_10BIT_LE,
	ST21_PIX_FMT_RGB_12BIT_BE,
	ST21_PIX_FMT_RGB_12BIT_LE,

	ST21_PIX_FMT_BGR_8BIT = 20,
	ST21_PIX_FMT_BGR_10BIT_BE,
	ST21_PIX_FMT_BGR_10BIT_LE,
	ST21_PIX_FMT_BGR_12BIT_BE,
	ST21_PIX_FMT_BGR_12BIT_LE,

	ST21_PIX_FMT_YCBCR_420_8BIT = 30,
	ST21_PIX_FMT_YCBCR_420_10BIT_BE,
	ST21_PIX_FMT_YCBCR_420_10BIT_LE,
	ST21_PIX_FMT_YCBCR_420_12BIT_BE,
	ST21_PIX_FMT_YCBCR_420_12BIT_LE,

	ST21_PIX_FMT_YCBCR_422_8BIT = 40,
	ST21_PIX_FMT_YCBCR_422_10BIT_BE,  // only supported format by the industry
	ST21_PIX_FMT_YCBCR_422_10BIT_LE,
	ST21_PIX_FMT_YCBCR_422_12BIT_BE,
	ST21_PIX_FMT_YCBCR_422_12BIT_LE,
} st21_pix_fmt_t;

/**
 * Enumeration for ST2110-21 scan types
 * i.e. Permutations of height and intercaled/progressive scanning
 */
typedef enum st21_vscan
{
	ST21_720I = 1,
	ST21_720P = 2,
	ST21_1080I = 3,
	ST21_1080P = 4,
	ST21_2160I = 5,
	ST21_2160P = 6,
} st21_vscan_t;

typedef enum st21_pkt_fmt
{
	ST_INTEL_SLN_RFC4175_PKT = 0,  //INTEL standard single line packet
	ST_INTEL_DLN_RFC4175_PKT = 1,  //INTEL standard dual line packet
	ST_OTHER_SLN_RFC4175_PKT = 2,  //Other vendors single line packets
								   //with variable lengths
} st21_pkt_fmt_t;

/**
 * Structure for session packet format definition
 */
struct st21_format
{
	st21_pix_fmt_t pixelFmt;
	st21_vscan_t vscan;
	uint32_t height;
	uint32_t width;
	uint32_t totalLines;	 // 1125 for HD, 2250 for UHD
	uint32_t trOffsetLines;	 // 22 for HD, 45 for UHD
	uint32_t pixelGrpSize;	 // 3 for RGB, 5 for 422 10 bit;
							 //- shall match the format - for sanity check
	uint32_t pixelsInGrp;	 // number of pixels in each pixel group,
							 //e.g. 1 for RGB, 2 for 422-10 and 420-8

	uint32_t clockRate;	  //90k of sampling clock rate
	uint32_t frmRateMul;  //60000 or 30000
	uint32_t frmRateDen;  //1001

	st21_pkt_fmt_t pktFmt;	// if single, dual or more lines of RFC4175 or other format
	uint32_t pixelsInPkt;	// number of pixels in each line
	uint32_t pktsInLine;	// number of packet per each line
	uint32_t pktSize;		//pkt size w/o VLAN header

	uint32_t frameTime;	   // time in nanoseconds of the frame
	uint32_t pktsInFrame;  //packets in frame
};
typedef struct st21_format st21_format_t;

/**
 * Enumeration for format names s of video buffers
 */
typedef enum st21_format_name
{
	ST21_FMT_INTEL_422_BE10_HD720_P_59 = 100,
	ST21_FMT_INTEL_422_BE10_HD1080_P_59,
	ST21_FMT_INTEL_422_BE10_UHD2160_P_59,
	ST21_FMT_OTHER_422_BE10_HD720_P_59,
	ST21_FMT_OTHER_422_BE10_HD1080_P_59,
	ST21_FMT_OTHER_422_BE10_UHD2160_P_59,

	ST21_FMT_INTEL_422_BE10_HD720_P_29 = 200,
	ST21_FMT_INTEL_422_BE10_HD1080_P_29,
	ST21_FMT_INTEL_422_BE10_UHD2160_P_29,
	ST21_FMT_OTHER_422_BE10_HD720_P_29,
	ST21_FMT_OTHER_422_BE10_HD1080_P_29,
	ST21_FMT_OTHER_422_BE10_UHD2160_P_29,

	ST21_FMT_INTEL_422_BE10_HD720_P_25 = 300,
	ST21_FMT_INTEL_422_BE10_HD1080_P_25,
	ST21_FMT_INTEL_422_BE10_UHD2160_P_25,
	ST21_FMT_OTHER_422_BE10_HD720_P_25,
	ST21_FMT_OTHER_422_BE10_HD1080_P_25,
	ST21_FMT_OTHER_422_BE10_UHD2160_P_25,

	ST21_FMT_INTEL_422_BE10_HD720_P_50 = 400,
	ST21_FMT_INTEL_422_BE10_HD1080_P_50,
	ST21_FMT_INTEL_422_BE10_UHD2160_P_50,
	ST21_FMT_OTHER_422_BE10_HD720_P_50,
	ST21_FMT_OTHER_422_BE10_HD1080_P_50,
	ST21_FMT_OTHER_422_BE10_UHD2160_P_50,

	ST21_FMT_INTEL_422_BE10_HD720_I_59 = 500,
	ST21_FMT_INTEL_422_BE10_HD1080_I_59,
	ST21_FMT_INTEL_422_BE10_UHD2160_I_59,
	ST21_FMT_OTHER_422_BE10_HD720_I_59,
	ST21_FMT_OTHER_422_BE10_HD1080_I_59,
	ST21_FMT_OTHER_422_BE10_UHD2160_I_59,

	ST21_FMT_INTEL_422_BE10_HD720_I_29 = 600,
	ST21_FMT_INTEL_422_BE10_HD1080_I_29,
	ST21_FMT_INTEL_422_BE10_UHD2160_I_29,
	ST21_FMT_OTHER_422_BE10_HD720_I_29,
	ST21_FMT_OTHER_422_BE10_HD1080_I_29,
	ST21_FMT_OTHER_422_BE10_UHD2160_I_29,

	ST21_FMT_INTEL_422_BE10_HD720_I_25 = 700,
	ST21_FMT_INTEL_422_BE10_HD1080_I_25,
	ST21_FMT_INTEL_422_BE10_UHD2160_I_25,
	ST21_FMT_OTHER_422_BE10_HD720_I_25,
	ST21_FMT_OTHER_422_BE10_HD1080_I_25,
	ST21_FMT_OTHER_422_BE10_UHD2160_I_25,

	ST21_FMT_INTEL_422_BE10_HD720_I_50 = 800,
	ST21_FMT_INTEL_422_BE10_HD1080_I_50,
	ST21_FMT_INTEL_422_BE10_UHD2160_I_50,
	ST21_FMT_OTHER_422_BE10_HD720_I_50,
	ST21_FMT_OTHER_422_BE10_HD1080_I_50,
	ST21_FMT_OTHER_422_BE10_UHD2160_I_50,

} st21_format_name_t;


/**
 * Enumeration for input output formats of video buffers
 */
typedef enum st21_buf_fmt
{
	ST21_BUF_FMT_RGB_8BIT = 10,
	ST21_BUF_FMT_RGB_10BIT_BE,
	ST21_BUF_FMT_RGB_10BIT_LE,
	ST21_BUF_FMT_RGB_12BIT_BE,
	ST21_BUF_FMT_RGB_12BIT_LE,

	ST21_BUF_FMT_RGBA_8BIT = 15,

	ST21_BUF_FMT_BGR_8BIT = 20,
	ST21_BUF_FMT_BGR_10BIT_BE,
	ST21_BUF_FMT_BGR_10BIT_LE,
	ST21_BUF_FMT_BGR_12BIT_BE,
	ST21_BUF_FMT_BGR_12BIT_LE,

	ST21_BUF_FMT_BGRA_8BIT = 25,

	ST21_BUF_FMT_YUV_420_8BIT = 30,
	ST21_BUF_FMT_YUV_420_10BIT_BE,
	ST21_BUF_FMT_YUV_420_10BIT_LE,
	ST21_BUF_FMT_YUV_420_12BIT_BE,
	ST21_BUF_FMT_YUV_420_12BIT_LE,

	ST21_BUF_FMT_YUV_422_8BIT = 40,
	ST21_BUF_FMT_YUV_422_10BIT_BE,
	ST21_BUF_FMT_YUV_422_10BIT_LE,
	ST21_BUF_FMT_YUV_422_12BIT_BE,
	ST21_BUF_FMT_YUV_422_12BIT_LE,
} st21_buf_fmt_t;

#define ST_MAX_EXT_BUFS 10
struct st21_session_ext_mem
{
	struct rte_mbuf_ext_shared_info *shInfo[ST_MAX_EXT_BUFS];
	uint8_t *addr[ST_MAX_EXT_BUFS];
	uint8_t *endAddr[ST_MAX_EXT_BUFS];
	rte_iova_t bufIova[ST_MAX_EXT_BUFS];
	int numExtBuf;
};
typedef struct st21_session_ext_mem st21_ext_mem_t;
/**
 * Structure for ST2110-21 video session
 */
struct st21_session
{
	st21_sn_flags_t caps;
	uint32_t ssid;
	uint16_t nicPort[2];  // NIC ports, second valid if multiple paths are supported
	uint32_t timeslot;	  // assigned timeslot ID [0- 1st timeslot, N]
	uint32_t trOffset;	  // offset of the timeslot since even EPOCH (calcualted from timeslot)
	uint32_t tprs;		  // time in nanoseconds of 2 consecutive packets begins of the same session
	uint32_t pktTime;	  // time in nanoseconds of the packet
	uint32_t frameSize;
	uint64_t pktsDrop[ST_PKT_DROP_MAX];
	uint64_t frmsDrop[ST_FRM_DROP_MAX];
	uint64_t pktsLost[ST_PKT_LOST_MAX];
	uint64_t pktsSend;
	uint64_t frmsSend;
	uint64_t pktsRecv;
	uint64_t frmsRecv;
	st21_ext_mem_t extMem;
};
typedef struct st21_session st21_session_t;

typedef enum st_addr_opt
{
	ST_ADDR_UCAST_IPV4 = 0x1,
	ST_ADDR_MCAST_IPV4 = 0x2,
	ST_ADDR_UCAST_IPV6 = 0x4,
	ST_ADDR_MCAST_IPV6 = 0x8,

	ST_ADDR_VLAN_TAG = 0x10,
	ST_ADDR_VLAN_DEI = 0x20,
	ST_ADDR_VLAN_PCP = 0x40,

	ST_ADDR_IP_ECN = 0x100,
	ST_ADDR_IP_DSCP = 0x200,
} st_addr_opt_t;

/**
 * Structure for connection address of IPv4/v6 and UDP ports
 */
struct st_addr
{
	st_addr_opt_t options;
	union
	{
		struct sockaddr_in addr4;
		struct sockaddr_in6 addr6;
	} src;
	union
	{
		struct sockaddr_in addr4;
		struct sockaddr_in6 addr6;
	} dst;
	union
	{
		struct
		{
			uint16_t tag : 12;
			uint16_t dei : 1;
			uint16_t pcp : 3;
		};
		uint16_t vlan;
	};
	union
	{
		struct
		{
			uint8_t ecn : 2;
			uint8_t dscp : 6;
		};
		uint8_t tos;
	};
};
typedef struct st_addr st_addr_t;

typedef union
{
	uint32_t valueU32;
	uint64_t valueU64;
	char 	*strPtr;
	void * ptr;
} st_param_val_t;

st_status_t StGetParam(st_param_t prm, st_param_val_t *val);
st_status_t StSetParam(st_param_t prm, st_param_val_t val);

/*!
  * Called by the application to initialize ST2110 device on the specified NIC PCI devices
  *
  *
  * @param inDev 	- IN structure with device parameters
  * @param port1Bdf 	- IN string decribing Bus:Device.Function of the PCI device of the primary port (primary stream)
  * @param port2Bdf 	- IN string decribing Bus:Device.Function of the PCI device of the secondary port (secondary stream)
  * @param outDev 	- OUT created device object w/ fields updated per link capabilities
  *
  * @return \ref st_status_t
  *
  */
st_status_t StCreateDevice(
	st_device_t *inDev,		//IN structure with device parameters
	const char *port1Bdf,	//IN BDF of primary port PCI device
	const char *port2Bdf,	//IN BDF of secondary port PCI device
	st_device_t **outDev);	//OUT created device object w/ fields updated per link capabilities

/*!
 * Called by the application to start ST2110 device for operation
 *
 *
 * @param dev - IN created device object can be already populated with sessions
 *
 * @return \ref st_status_t
 *
 */
st_status_t
StStartDevice(st_device_t *dev);  //IN created device object can be already populated with sessions

/*!
 * Called by the application to deinitialize ST2110 device
 *
 *
 * @param dev 	- IN pointer to the device object to deinitialize
 *
 * @return \ref st_status_t
 *
 */
st_status_t StDestroyDevice(st_device_t *dev);	//IN ST device to destroy

struct st_ptp_clock_id
{
	uint8_t id[8];
};
typedef struct st_ptp_clock_id st_ptp_clock_id_t;

typedef enum st_ptp_addr_mode
{
    ST_PTP_MULTICAST_ADDR = 0,
    ST_PTP_UNICAST_ADDR = 1,
} st_ptp_addr_mode_t;

typedef enum st_ptp_step_mode
{
    ST_PTP_TWO_STEP = 0,
    ST_PTP_ONE_STEP = 1,
} st_ptp_step_mode_t;

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
 * @param priClock 	- IN pointer to the clock ID structure of the primary PTP Grandmaster
 * @param bkpClock 	- IN pointer to the clock ID structure of the backup PTP Grandmaster
 *
 * @return \ref st_status_t
 *
 */
st_status_t StPtpSetClockSource(st_ptp_clock_id_t const *priClock,
								st_ptp_clock_id_t const *bkpClock);

/*!
 * Called by the application to assign in case of prm value of:
 * ST_PTP_DROP_TIME
 * Drop time after which the backup PtP clock is used instead of primary Ptp, also one the
 * primary is back then it needs to be stable as least as long that interval to switch back
 *
 * @param val - threshold in nanosecond, threshold above which the backup PTP Grandmaster
 *              is used as the active Grandmaster
 *
 *
 * ST_PTP_THRESHOLD
 * PTP adjustment threshold time above which
 * the Ptp timer gets PTP Grandmaster clock unconditionally as is (since the difference is
 * too huge). Otherwise, a half of the difference is used to adjust the local PTP clock.
 * if set to 0 then always PTP Grandmaster is get as is (default beavior). Then the value of
 *
 * @param val - threshold in nanosecond, threshold above which the recoved value of PTP Grandmaster
 *              is used as the local PTP clock
 *
 * @return \ref st_status_t
 *
 */
st_status_t StPtpSetParam(st_param_t prm,  st_param_val_t val);

st_status_t StPtpGetParam(st_param_t prm,  st_param_val_t *val);

/*!
 * Called by the application to get active PTP clock IDs
 *
 *
 * @param currClock	- OUT pointer to the clock ID structure that is updated with the PTP Grandmaster
 *
 * @return \ref st_status_t
 *
 */
st_status_t StPtpGetClockSource(st_ptp_clock_id_t *currClock);

extern uint64_t (*StPtpGetTime)(void) __attribute__((nonnull));

st_status_t St21GetSessionCount(st_device_t *dev,  //IN ST device
								uint32_t *count);  //OUT count of active ST2110-21 sessions

/**
 * Called by the application to create a new session on NIC device
 */
st_status_t St21CreateSession(
	st_device_t *dev,		  // IN device on which to create session
	st21_session_t *inSn,	  // IN structure
	st21_format_t *fmt,		  // IN session packet's format
	st21_session_t **outSn);  // OUT created session object w/ fields updated respectively

st_status_t St21DestroySession(st21_session_t *sn /* IN */);

/**
 * Connected session with IGMP signaling (multicast case)
 *
 * Producer -> Consumer flows example for case when SDP is requirement to be announced
 * Producer							Consumer
 * 1. St21CreateSession()			1. St21CreateSession()
 * 2. St21BindIpAddr()				2. St21BindIpAddr() -- Multicast IP case --> 2. St21JoinSession()
 * 3. St21RegisterProducer()			3. St21RegisterConsumer()                             -- || --
 * 4. St21ListenSession() -->IgmpQuery------------------------------------------> 2. St21JoinSession()
 *                      L2 IGMP snooping enabled net switch <-IgmpMemberReport--- 3. IgmpReplyQuery()
 *                              or
 * 4.
 *                      L2 IGMP snooping enabled net switch <-IgmpMemberReport--- 2. St21JoinSession()
 *		5. St21ProducerStartFrame() 4. St21ConsumerStartFrame()
 *
 *		7. --> ST2110-21 producer sends RTP RFC4175 packets to consumer
 *
 *                                   5. St21ConsumerStop()
 * 4. <----------------------------  6. St21DropSession()
 *		8. St21ProducerStop()
 */

/**
 * Connection less session IGMP (unicast only)
 *
 * Producer -> Consumer flows example for case SDP is not requirement
 * Producer							Consumer
 * 1. St21CreateSession()			1. St21CreateSession()
 * 2. St21BindIpAddr()				2. St21BindIpAddr()
 * 3. St21RegisterProducer()			3. St21RegisterConsumer()
 * 4. St21ProducerStartFrame()       4. St21ConsumerStartFrame()
 * 5. --> ST2110-21 producer sends RTP RFC4175 packets to consumer
 *                                   6. St21ConsumerStop()
 * 7. St21ProducerStop()
 */

/**
 * Called by the both sides to assign/bind IP addresses of the stream.
 * Upon correct scenario completes with ST_OK.
 * Shall be called twice if redundant 2022-7 path mode is used to add both addressed
 * on the ports as required respecitvely
 * path addresses and VLANs
 */
st_status_t St21BindIpAddr(st21_session_t *sn, st_addr_t *addr, uint16_t nicPort);


/**
 * Called by the consumer application to join producer session multicast group
 * This procedure starts a background thread that periodically sends IGMP Membership
 * reports to switches so that they can setup their IGMP snooping
 */
st_status_t St21JoinSession(st21_session_t *sn, st_addr_t *addr);

st_status_t St21SetParam(st21_session_t *sn, st_param_t prm, uint64_t val);
st_status_t St21GetParam(st21_session_t *sn, st_param_t prm, uint64_t *val);

/**
 * Called by the applications to get format information of the session.
 * This is complementary method to St21GetSdp and several St21GetParam
 */
st_status_t St21GetFormat(st21_session_t *sn, st21_format_t *fmt);

/**
 * Called by the application to get SDP text in newly allocated text buffer
 * Reading SDP allows to understand session and format
 * This is complementary method to St21GetFormat and several St21GetParam
 */
st_status_t St21GetSdp(st21_session_t *sn, char *sdpBuf, uint32_t sdpBufSize);

/******************************************************************************************************
 *
 * Callbacks to producer or consumer application to manage live streaming
 *
 ******************************************************************************************************/

/**
 * Callback to producer application to build RTP packet by the application
 * (rtpHdr points to RTP header that is after UDP). Application is responsible of parsing and understanidng the packet.
 * This is for application implementing own packet assembling and own formats.
 */
typedef st_status_t (*St21BuildRtpPkt_f)(void *appHandle, uint8_t *pktHdr, uint16_t *hdrSize,
										 uint8_t *rtpPayload, uint16_t *payloadSize);

/**
 * Callback to consumer application to pass RTP packet after UDP header to the application
 * (rtpHdr points to RTP header). Application is responsible of parsing and understanding the packet.
 * This is for application implementing own packet parsing and own formats. Nanosecond PTP timestamp is passed also.
 * The passed buffers are freed after the call by the library so the application shall consume them within the callback.
 */
typedef st_status_t (*St21RecvRtpPkt_f)(void *appHandle, uint8_t *pktHdr, uint16_t hdrSize,
										uint8_t *rtpPayload, uint16_t payloadSize, uint64_t tmstamp);

/**
 * Callback to producer or consumer application to get next frame buffer necessary to continue streaming
 * If application cannot return the next buffer returns NULL and then has to call St21ProducerUpdate
 * or St21ConsumerUpdate to restart streaming
 */
typedef uint8_t *(*St21GetNextFrameBuf_f)(void *appHandle, uint8_t *prevFrameBuf, uint32_t bufSize,
										  uint32_t fieldId);
/**
 * Callback to producer or consumer application to get next slice buffer offset necessary to continue streaming
 * If application cannot return the next buffer returns the same value as prevOffset and then has to call
 * St21ProducerUpdate or St21ConsumerUpdate to restart streaming
 */
typedef uint32_t (*St21GetNextSliceOffset_f)(void *appHandle, uint8_t *frameBuf,
											 uint32_t prevOffset, uint32_t fieldId);

/**
 * Callback to producer application to get timestamp as transported in SDI frame
 */
typedef uint32_t (*St21GetFrameTmstamp_f)(void *appHandle);

/**
 * Callback to consumer application with notification about the frame receive completion
 * Frame buffer can not be yet released or reused since it can be used to the next frame
 * upon packets loss, buffer release is singled with St21NotifyFrameDone_f callback
 */
typedef void (*St21NotifyFrameRecv_f)(void *appHandle, uint8_t *frameBuf, uint32_t tmstamp,
									  uint32_t fieldId);

/**
 * Callback to consumer application with notification about the slice receive completion
 * Slice buffer can not be yet released or reused since it can be used to the next frame
 * upon packets loss, buffer release is singled with St21NotifyFrameDone_f callback
 */
typedef void (*St21NotifySliceRecv_f)(void *appHandle, uint8_t *frameBuf, uint32_t sliceOffset,
									  uint32_t fieldId);

/**
 * Callback to producer or consumer application with notification about the frame completion
 * Frame buffer can be released or reused after it but not sooner
 */
typedef void (*St21NotifyFrameDone_f)(void *appHandle, uint8_t *frameBuf, uint32_t fieldId);
/**
 * Callback to producer or consumer application with notification about the slice completion
 * Slice buffer can be released or reused after it but not sooner
 */
typedef void (*St21NotifySliceDone_f)(void *appHandle, uint8_t *sliceBuf, uint32_t fieldId);
/**
 * Callback to producer or consumer application with notification about completion of the session stop
 * It means that all buffer pointers can be released after it but not sooner
 */
typedef void (*St21NotifyStopDone_f)(void *appHandle);

/**
 * Callback to producer or consumer application with notification about the unexpected session drop
 * It means that all buffer pointers can be released after it
 */
typedef void (*St21NotifyStreamDrop_f)(void *appHandle);

/**
 * Callback to consumer application about the frame 90kHz timestamp as received in stream
 */
typedef void (*St21PutFrameTmstamp_f)(void *appHandle, uint32_t tmstamp);

/**
 * Structure for video producer application
 */
struct st21_producer
{
	void *appHandle;
	st21_prod_type_t prodType;	// slice mode or frame mode w/ or w/o timestamp
	uint32_t frameSize;
	uint32_t sliceSize;
	uint32_t sliceCount;  // count of slices in Frame if slice mode
	St21GetNextFrameBuf_f St21GetNextFrameBuf;
	St21GetNextSliceOffset_f St21GetNextSliceOffset;
	St21GetFrameTmstamp_f St21GetFrameTmstamp;
	St21NotifyFrameDone_f St21NotifyFrameDone;
	St21NotifySliceDone_f St21NotifySliceDone;
	St21NotifyStopDone_f St21NotifyStopDone;
	St21BuildRtpPkt_f St21BuildRtpPkt;
};
typedef struct st21_producer st21_producer_t;

/**
 * Called by the producer to register live producer for video streaming
 */
st_status_t St21RegisterProducer(
	st21_session_t *sn,		 //IN session pointer
	st21_producer_t *prod);	 //IN register callbacks to allow interaction with live producer

/**
 * Called by the producer asynchronously to start each frame of video streaming
 */
st_status_t St21ProducerStartFrame(
	st21_session_t *sn,	 //IN session pointer
	uint8_t *frameBuf,	 //IN 1st frame buffer for the session
	uint32_t linesOffset,//IN offset in complete lines of the frameBuf to which producer filled the buffer
	uint32_t tmstamp,	//IN if not 0 then 90kHz timestamp of the frame
	uint64_t ptpTime);	//IN if not 0 start new frame at the given PTP timestamp + TROFFSET

/**
 * Called by the producer asynchronously to update video streaming
 * in case producer has more data to send, it also restart streaming
 * if the producer callback failed due to lack of buffer with video
 */
st_status_t St21ProducerUpdate(
	st21_session_t *sn,	 //IN session pointer
	uint8_t *frameBuf,	 //IN frame buffer for the session from which to restart
	uint32_t linesOffset);  //IN offset in complete lines of the frameBuf to which producer filled the buffer

/**
 * Called by the producer asynchronously to stop video streaming,
 * the session will notify the producer about completion with callback
 */
st_status_t St21ProducerStop(st21_session_t *sn);

/**
 * Structure for video consumer
 */
struct st21_consumer
{
	void *appHandle;
	st21_cons_type_t consType;
	uint32_t frameSize;
	uint32_t sliceSize;
	uint32_t sliceCount;  // count of slices in Frame if slice mode
	St21GetNextFrameBuf_f St21GetNextFrameBuf;
	St21GetNextSliceOffset_f St21GetNextSliceOffset;
	St21NotifyFrameRecv_f St21NotifyFrameRecv;
	St21NotifySliceRecv_f St21NotifySliceRecv;
	St21PutFrameTmstamp_f St21PutFrameTmstamp;
	St21NotifyFrameDone_f St21NotifyFrameDone;
	St21NotifySliceDone_f St21NotifySliceDone;
	St21NotifyStopDone_f St21NotifyStopDone;
	St21RecvRtpPkt_f St21RecvRtpPkt;
};
typedef struct st21_consumer st21_consumer_t;

/**
 * Called by the consumer to register live consumer for video streaming
 */
st_status_t St21RegisterConsumer(
    st21_session_t *sn,	  //IN session pointer
    st21_consumer_t *cons);  //IN consumer callbacks structure

/**
 * Called by the consumer asynchronously to start video streaming
 */
st_status_t St21ConsumerStartFrame(
	st21_session_t *sn,	 //IN session pointer
	uint8_t *frameBuf,	 //IN 1st frame buffer for the session
	uint64_t ptpTime);	 //IN if not 0 start receiving session since the given ptp time

/**
 * Called by the consumer asynchronously to update or restart video streaming
 * if the consumer callback failed due to lack of available buffer
 */
st_status_t St21ConsumerUpdate(
	st21_session_t *sn,	 //IN session pointer
	uint8_t *frameBuf,	 //IN frame buffer for the session from which to restart
	uint32_t linesOffset);//IN offset in complete lines of the frameBuf to which consumer can fill the buffer

/**
 * Called by the consumer asynchronously to stop video streaming,
 * the session will notify the consumer about completion with callback
 */
st_status_t St21ConsumerStop(st21_session_t *sn);

/**
 * Allocate memory for transmitting frames
 */
uint8_t * St21AllocFrame(
	st21_session_t *sn, 	//IN session pointer
	uint32_t frameSize);	//IN Size of memory

/**
 * Free memory if ref counter is zero, otherwise reports error that memory is
 * still in use and free should be retried
 */
st_status_t
St21FreeFrame(
	st21_session_t *sn,	//IN session pointer
       	uint8_t* frame);	//IN Addres of memory to be freed

#endif
