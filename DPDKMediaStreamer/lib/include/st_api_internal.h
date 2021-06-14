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

#ifndef _ST_API_INTERNAL_H
#define _ST_API_INTERNAL_H

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_ring.h>

#include "st_api.h"
#include "st_fmt.h"
#include "st_pkt.h"

#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
/* internal api start */

typedef unsigned int st_rx_queue_t;
typedef unsigned int st_port_t;

typedef struct st_thrd_params
{
	uint32_t thrdSnFirst;
	uint32_t thrdSnLast;
	uint32_t pktsCount;
} st_thrd_params_t;

/* Macros */
#define MIN(x, y) (x < y ? x : y)
#define MAX(x, y) (x > y ? x : y)

#define U64 unsigned long long

#define FRAME_PREV 0
#define FRAME_CURR 1
#define FRAME_PEND 2
#define FRAME_MAX 3

typedef enum st_histogram
{
	CURR_HIST = 0,
	PEND_HIST = 1,
	NUM_HISTOGRAMS = 2
} st_histogram_t;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#define ST_ASSERT                                                                                  \
	rte_exit(127, "ASSERT error file %s function %s line %u\n", __FILE__, __FUNCTION__, __LINE__)

#define ETH_ADDR_LEN 6

/**
* Structure for connection/flow IPv4/v6 addresses and UDP ports
*/
struct st_flow
{
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
	uint8_t dstMac[ETH_ADDR_LEN];
	uint8_t srcMac[ETH_ADDR_LEN];
};
typedef struct st_flow st_flow_t;

typedef struct rvrtp_bufs
{
	uint32_t tmstamp;
	uint32_t pkts;
	uint8_t *buf;
	uint8_t lastGoodPacketPort;
} rvrtp_bufs_t;

typedef enum
{
	ST_SN_STATE_OFF = 0,
	ST_SN_STATE_ON = 1,				 //created but stopped, waiting for frame start
	ST_SN_STATE_RUN = 2,			 //actively sending a frame
	ST_SN_STATE_NO_NEXT_FRAME = 3,	 // hold waiting for the next frame
	ST_SN_STATE_NO_NEXT_BUFFER = 3,	 // hold waiting for the next audio buffer
	ST_SN_STATE_NO_NEXT_SLICE = 4,	 // hold waiting for the next slice
	ST_SN_STATE_NO_NEXT_OFFSET = 4,	 // hold waiting for the next audio buffer offset
	ST_SN_STATE_STOP_PENDING = 5,	 // stop is pending, shall be then restarted or destroyed
	ST_SN_STATE_TIMEDOUT = 6,		 // stop after the too long hold
} st_sn_state_t;

#define ST_FRAG_HISTOGRAM_720P_DLN_SZ                                                              \
	(RTE_CACHE_LINE_ROUNDUP(720 * sizeof(uint8_t)))	 // aligned to 64
#define ST_FRAG_HISTOGRAM_720P_SLN_SZ                                                              \
	(RTE_CACHE_LINE_ROUNDUP(360 * sizeof(uint8_t)))	 // aligned to 64
#define ST_FRAG_HISTOGRAM_1080P_DLN_SZ                                                             \
	(RTE_CACHE_LINE_ROUNDUP(540 * sizeof(uint8_t)))	 // aligned to 64
#define ST_FRAG_HISTOGRAM_1080P_SLN_SZ                                                             \
	(RTE_CACHE_LINE_ROUNDUP(540 * sizeof(uint8_t)))	 // aligned to 64
#define ST_FRAG_HISTOGRAM_2160P_SLN_SZ                                                             \
	(RTE_CACHE_LINE_ROUNDUP(2160 * sizeof(uint8_t)))  // aligned to 64
#define ST_FRAG_HISTOGRAM_720I_SLN_SZ                                                              \
	(RTE_CACHE_LINE_ROUNDUP(180 * sizeof(uint8_t)))	 // aligned to 64
#define ST_FRAG_HISTOGRAM_1080I_SLN_SZ                                                             \
	(RTE_CACHE_LINE_ROUNDUP(270 * sizeof(uint8_t)))	 // aligned to 64
#define ST_FRAG_HISTOGRAM_2160I_SLN_SZ                                                             \
	(RTE_CACHE_LINE_ROUNDUP(1080 * sizeof(uint8_t)))  // aligned to 64

typedef enum
{
	ST_OFLD_HW_IP_CKSUM = 0x1,
	ST_OFLD_HW_UDP_CKSUM = 0x2,
} st_ofld_hw_t;

struct rvrtp_pkt_ctx
{
	union
	{
		struct
		{
#ifdef __LITTLE_ENDIAN_BITFIELDS
			uint32_t seqLo : 16;
			uint32_t seqHi : 16;
#else
			uint32_t seqHi : 16;
			uint32_t seqLo : 16;
#endif
		} lohi;
		uint32_t sequence;
	} seqNumber;

	uint32_t sliceOffset;

	uint32_t tmstamp;
	uint32_t tmstampOddInc;
	uint32_t tmstampEvenInc;
	uint32_t alignTmstamp;
	uint64_t epochs;

	uint16_t line1PixelGrpSize;
	uint16_t line2PixelGrpSize;

	uint16_t line1Offset;
	uint16_t line2Offset;

	uint16_t line1Number;
	uint16_t line2Number;

	uint16_t line1Length;
	uint16_t line2Length;

	uint16_t ipPacketId;
	uint16_t fieldId;  //interlaced field 0 or 1 (odd or even)

	uint32_t line1Size;
	uint32_t line2Size;

	//receiver specific
	uint8_t *data;	//current buffer pointer for receiver

	uint32_t *lineHistogram;
	uint8_t *fragHistogram[NUM_HISTOGRAMS];

} __rte_cache_aligned;

typedef struct rvrtp_pkt_ctx rvrtp_pkt_ctx_t;

typedef struct rartp_pkt_ctx
{
	uint16_t seqNumber;

	uint32_t tmstamp;
	uint64_t epochs;

	uint16_t ipPacketId;
	uint32_t payloadSize;

	//offset in the audio buffer
	uint32_t bufOffset;

	uint32_t histogramSize;
	uint16_t *histogram;

	//receiver specific
	uint8_t *data;	//current buffer pointer for receiver

} __rte_cache_aligned rartp_pkt_ctx_t;

struct ranc_pkt_ctx
{
	uint16_t seqNumber;
	uint16_t extSeqNumber;

	uint32_t tmstamp;
	uint64_t epochs;

	uint16_t ipPacketId;
	uint32_t payloadSize;  //size of anc header and payload of ancillary data

	//offset in the ancillary buffer
	uint32_t bufOffset;
	uint16_t pktSize;  //this var conatins size of rtp header, anc header
					   //and payload of ancillary data

	//receiver specific
	uint8_t *data;	//current buffer pointer for receiver
	rvrtp_pkt_ctx_t *vctx;

} __rte_cache_aligned;

typedef struct ranc_pkt_ctx ranc_pkt_ctx_t;

typedef struct st_session_impl st_session_impl_t;

struct rvrtp_ebu_stat
{
	uint64_t cinTmstamp;
	uint64_t cinCnt;
	uint64_t cinSum;
	uint64_t cinMax;
	uint64_t cinMin;
	double cinAvg;

	uint64_t vrxCnt;
	uint64_t vrxSum;
	uint64_t vrxMax;
	uint64_t vrxMin;
	double vrxAvg;

	uint64_t latCnt;
	uint64_t latSum;
	uint64_t latMax;
	uint64_t latMin;
	double latAvg;

	int64_t tmdCnt;
	int64_t tmdSum;
	int64_t tmdMax;
	int64_t tmdMin;
	double tmdAvg;

	uint32_t prevPktTmstamp;
	uint32_t prevRtpTmstamp;
	uint64_t prevEpochTime;
	uint64_t prevTime;

	uint32_t tmiCnt;
	uint32_t tmiSum;
	uint32_t tmiMax;
	uint32_t tmiMin;
	double tmiAvg;

	uint64_t fptCnt;
	uint64_t fptSum;
	uint64_t fptMax;
	uint64_t fptMin;
	double fptAvg;
} __rte_cache_aligned;

typedef struct rvrtp_ebu_stat rvrtp_ebu_stat_t;

/**
* Function to build packet as it is dependent on a format
*/
typedef void *(*RvRtpUpdatePacket_f)(st_session_impl_t *s, void *hdr, struct rte_mbuf *);

/**
* Function to receive packet as it is dependent on a format
*/
typedef st_status_t (*RvRtpRecvPacket_f)(st_session_impl_t *s, struct rte_mbuf *rxbuf);

/**
* Structure for session packet format definition
*/
struct st_session_impl
{
	st_session_t sn;
	st_format_t fmt;
	st_flow_t fl[MAX_RXTX_PORTS];
	char *sdp;

	uint16_t etherVlan;	 //tag to put if VLAN encap is enabled
	uint16_t etherSize;	 //14 or 18 if with VLAN

	uint32_t pktTime;
	uint32_t tmstampTime;  //in nanoseconds
	uint32_t lastTmstamp;
	uint32_t nicTxTime;

	/* TODO
 * I prefer to use "void *" here
 * but too many code change */
	union
	{
		st21_producer_t prod;
		st30_producer_t aprod;
		st40_producer_t ancprod;
		st21_consumer_t cons;
		st30_consumer_t acons;
		st40_consumer_t anccons;
	};

	union
	{
		uint8_t *prodBuf;
		union
		{
			uint8_t *consBuf;
			struct
			{
				rvrtp_bufs_t consBufs[FRAME_MAX];
				uint32_t consState;
			};
		};
	};

	uint16_t pendCnt;
	uint32_t tmstampToDrop[2];
	uint32_t tmstampDone;
	uint64_t pktsDrop;
	uint64_t frmsDrop;
	uint64_t frmsFixed;

	st_ofld_hw_t ofldFlags;

	uint32_t ptpDropTime;

	struct st_device_impl *dev;
	uint32_t tid;

	volatile int lock;

	union
	{
		volatile uint32_t sliceOffset;
		volatile uint32_t bufOffset;
	};
	volatile st_sn_state_t state;

	RvRtpUpdatePacket_f UpdateRtpPkt;
	RvRtpRecvPacket_f RecvRtpPkt;

	uint64_t fragPattern;

	rvrtp_pkt_ctx_t vctx;
	union
	{
		rartp_pkt_ctx_t actx;
		ranc_pkt_ctx_t ancctx;
	};
	rvrtp_ebu_stat_t ebu;
	union st_pkt_hdr hdrPrint[MAX_RXTX_PORTS] __rte_cache_aligned;
	uint64_t padding[8] __rte_cache_aligned;  //usefull to capture memory corrupts
} __rte_cache_aligned;

typedef union anc_udw_10_6e
{
	struct
	{
		uint16_t : 6;
		uint16_t udw : 10;
	};
	uint16_t val;
} __attribute__((__packed__)) anc_udw_10_6e_t;

typedef union anc_udw_2e_10_4e
{
	struct
	{
		uint16_t : 4;
		uint16_t udw : 10;
		uint16_t : 2;
	};
	uint16_t val;
} __attribute__((__packed__)) anc_udw_2e_10_4e_t;

typedef union anc_udw_4e_10_2e
{
	struct
	{
		uint16_t : 2;
		uint16_t udw : 10;
		uint16_t : 4;
	};
	uint16_t val;
} __attribute__((__packed__)) anc_udw_4e_10_2e_t;

typedef union anc_udw_6e_10
{
	struct
	{
		uint16_t udw : 10;
		uint16_t : 6;
	};
	uint16_t val;
} __attribute__((__packed__)) anc_udw_6e_10_t;

void RvRtpInitPacketCtx(st_session_impl_t *s, uint32_t ring);

typedef struct rartp_session rartp_session_t;

/**
 * @brief 
 * 
 */
typedef void *(*RaRtpUpdatePacket_f)(rartp_session_t *s, void *hdr, struct rte_mbuf *m);

/**
* Function to receive packet as it is dependent on a format
*/
typedef st_status_t (*RaRtpRecvPacket_f)(rartp_session_t *s, struct rte_mbuf *m);

st_status_t RaRtpReceivePacketsRegular(st_session_impl_t *s, struct rte_mbuf *m);
st_status_t RaRtpReceivePacketsCallback(st_session_impl_t *s, struct rte_mbuf *m);

st_status_t RancRtpReceivePacketsRegular(st_session_impl_t *s, struct rte_mbuf *m);
st_status_t RancRtpReceivePacketsCallback(st_session_impl_t *s, struct rte_mbuf *m);
/**
* Structure for audio St30 session 
*/
struct rartp_session
{
	st_session_t sn;
	st30_format_t fmt;

	st_flow_t fl[2];

	uint16_t etherVlan;	 //tag to put if VLAN encap is enabled
	uint16_t etherSize;	 //14 or 18 if with VLAN

	double tmstampTime;	 //in nanoseconds
	uint32_t lastTmstamp;
	uint32_t nicTxTime;

	st30_producer_t prod;
	uint8_t *prodBuf;

	st30_consumer_t cons;
	uint8_t *consBuf;

	uint64_t pktsDrop;
	uint64_t frmsDrop;
	uint64_t frmsFixed;

	st_ofld_hw_t ofldFlags;

	struct st_device_impl *dev;
	uint32_t tid;

	volatile int lock;
	volatile uint32_t bufOffset;
	volatile st_sn_state_t state;

	//functions set per format
	RaRtpUpdatePacket_f UpdateRtpPkt;
	RaRtpRecvPacket_f RecvRtpPkt;

	rartp_pkt_ctx_t ctx;
	union st_pkt_hdr hdrPrint __rte_cache_aligned;
	uint64_t padding[8] __rte_cache_aligned;  //usefull to capture memory corrupts
} __rte_cache_aligned;

struct st_device_impl
{
	st_device_t dev;

	st_session_impl_t **snTable;

	uint32_t snCount;

	st_session_impl_t **sn30Table;
	uint32_t sn30Count;

	st_session_impl_t **sn40Table;
	uint32_t sn40Count;

	uint32_t quot;		//in bytes for a batch of packets
	uint32_t remaind;	//remaind of the byte budget
	uint32_t timeQuot;	//in nanoseconds
	uint32_t *timeTable;

	uint32_t rxOnly;
	uint32_t txOnly;

	uint32_t maxRings;
	uint32_t outOfBoundRing;

	struct rte_mempool *mbufPool;
	uint32_t *txPktSizeL1;
	struct rte_ring **txRing[MAX_RXTX_PORTS];

	uint32_t fmtIndex;

	//receive device Flow Table
	struct rte_flow *flTable[ST_MAX_FLOWS];

	uint32_t lastAllocSn;	 //video St21 session ID that was allocated the prev time
	uint32_t lastAllocSn30;	 //audio St30 session ID that was allocated the prev time
	uint32_t lastAllocSn40;	 //ancillary data St40 session ID that was allocated the prev time

	uint32_t numPorts;
	uint8_t srcMacAddr[MAX_RXTX_PORTS][ETH_ADDR_LEN];

	uint64_t *packetsTx[MAX_RXTX_PORTS];
	uint64_t *pausesTx[MAX_RXTX_PORTS];
	int32_t adjust;

	volatile int lock;
} __rte_cache_aligned;
typedef struct st_device_impl st_device_impl_t;

extern st_device_impl_t stRecvDevice;
extern st_device_impl_t stSendDevice;

struct tprs_scheduler
{
	int	timeCursor;
	uint32_t timeRemaind;

	uint32_t quot;
	int32_t	adjust;
	uint32_t remaind;

	uint32_t *ringThreshHi;
	uint32_t *ringThreshLo;
	uint32_t *deqRingMap;

	uint32_t ring;
	uint32_t lastSnRing;
	uint32_t lastTxRing;
	uint32_t outOfBoundRing;

	uint32_t queueId;
	uint32_t thrdId;

	uint32_t minPktSize;
	uint32_t pktSize;

	uint32_t slot;	//pause table heap position
	uint32_t top;	//packet vector heap position
	uint32_t burstSize;

} __rte_cache_aligned;
typedef struct tprs_scheduler tprs_scheduler_t;

typedef struct st_session_method
{
	int init;
	st_status_t (*create_tx_session)(st_device_impl_t *d, st_session_t *in, st_format_t *fmt,
									 st_session_impl_t **out);
	st_status_t (*create_rx_session)(st_device_impl_t *d, st_session_t *in, st_format_t *fmt,
									 st_session_impl_t **out);
	st_status_t (*destroy_tx_session)(st_session_impl_t *sn);
	st_status_t (*destroy_rx_session)(st_session_impl_t *sn);

	void (*init_packet_ctx)(st_session_impl_t *s, uint32_t ring);

	void (*update_packet)(st_session_t *s, void *hdr, struct rte_mbuf *m);
	st_status_t (*recv_packet)(st_session_t *s, struct rte_mbuf *m);

	//st_status_t (*register_producer)(st_session_t *sn, st_producer_t *prod);

} st_session_method_t;

void st_init_session_method(st_session_method_t *method, st_essence_type_t type);
void rvrtp_method_init();
void rartp_method_init();
void ranc_method_init();

st_status_t StValidateSession(st_session_t *sn);
st_status_t StValidateDevice(st_device_t *dev);

int StSessionGetPktsize(st_session_impl_t *s);

/*
 * Internal functions for sessions creation
 */
st_status_t RvRtpCreateRxSession(st_device_impl_t *dev, st_session_t *sin, st_format_t *fmt,
								 st_session_impl_t **sout);
st_status_t RvRtpCreateTxSession(st_device_impl_t *dev, st_session_t *sin, st_format_t *fmt,
								 st_session_impl_t **sout);
st_status_t RaRtpCreateRxSession(st_device_impl_t *dev, st_session_t *sin, st_format_t *fmt,
								 st_session_impl_t **sout);
st_status_t RaRtpCreateTxSession(st_device_impl_t *dev, st_session_t *sin, st_format_t *fmt,
								 st_session_impl_t **sout);
st_status_t RancRtpCreateRxSession(st_device_impl_t *dev, st_session_t *sin, st_format_t *fmt,
								   st_session_impl_t **sout);
st_status_t RancRtpCreateTxSession(st_device_impl_t *dev, st_session_t *sin, st_format_t *fmt,
								   st_session_impl_t **sout);
st_status_t RvRtpDestroyTxSession(st_session_impl_t *s);
st_status_t RvRtpDestroyRxSession(st_session_impl_t *s);
st_status_t RaRtpDestroyTxSession(st_session_impl_t *s);
st_status_t RaRtpDestroyRxSession(st_session_impl_t *s);
st_status_t RancRtpDestroyTxSession(st_session_impl_t *s);
st_status_t RancRtpDestroyRxSession(st_session_impl_t *s);

st_status_t RvRtpSendDeviceAdjustBudget(st_device_impl_t *dev);

/*****************************************************************************************
 *
 * RvRtpUpdateDualLinePacket - UDP RAW Video RTP header constructor routine.
 *
 * DESCRIPTION
 * Constructs the UDP RFC 4175 packet with 2 lines of video
 * and headers in a packer buffer.
 *
 * RETURNS: IP header location
 */
void *RvRtpUpdateDualLinePacket(st_session_impl_t *s, void *hdr, struct rte_mbuf *m);

/*****************************************************************************************
 *
 * RvRtpUpdateSingleLinePacket - UDP RAW Video RTP header constructor routine.
 *
 * DESCRIPTION
 * Constructs the UDP RFC 4175 packet with 1 line of video
 * and headers in a packer buffer.
 *
 * RETURNS: IP header location
 */
void *RvRtpUpdateSingleLinePacket(st_session_impl_t *s, void *hdr, struct rte_mbuf *m);

/*****************************************************************************
 *
 * RvRtpUpdateInterlacedPacket - UDP RAW Video RTP header constructor routine.
 *
 * DESCRIPTION
 * Constructs the UDP RFC 4175 packet with single line of interlaced video
 * and headers in a packer buffer.
 *
 * RETURNS: IP header location
 *
 * SEE ALSO:
 */
void *RvRtpUpdateInterlacedPacket(st_session_impl_t *s, void *hdr, struct rte_mbuf *m);

/*****************************************************************************************
 *
 * RvRtpReceivePacketCallback
 *
 * DESCRIPTION
 * Main function to processes packet within session context when callback is called on 
 * receive
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t RvRtpReceivePacketCallback(st_session_impl_t *s, struct rte_mbuf *m);

/*****************************************************************************************
 *
 * RvRtpReceiveFirstPacketsSln720p
 *
 * DESCRIPTION
 * Main function to processes single line 720p first packets within session context
 * Intel standard packets only
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t RvRtpReceiveFirstPacketsSln720p(st_session_impl_t *s, struct rte_mbuf *mbuf);

/*****************************************************************************************
 *
 * RvRtpReceiveFirstPacketsSln1080p
 *
 * DESCRIPTION
 * Main function to processes single line 1080p first packets within session context
 * Intel standard packets only
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t RvRtpReceiveFirstPacketsSln1080p(st_session_impl_t *s, struct rte_mbuf *mbuf);

/*****************************************************************************************
 *
 * RvRtpReceiveFirstPacketsSln2160p
 *
 * DESCRIPTION
 * Main function to processes single line 1080p first packets within session context
 * Intel standard packets only
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t RvRtpReceiveFirstPacketsSln2160p(st_session_impl_t *s, struct rte_mbuf *mbuf);

/*****************************************************************************************
 *
 * RvRtpReceiveFirstPackets720p
 *
 * DESCRIPTION
 * Main function to processes single line 720p first packets within session context
 * Other vendors packets with not all equal lengths
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t RvRtpReceiveFirstPackets720p(st_session_impl_t *s, struct rte_mbuf *mbuf);

/*****************************************************************************************
 *
 * RvRtpReceiveFirstPackets1080p
 *
 * DESCRIPTION
 * Main function to processes single line 1080p first packets within session context
 * Other vendors packets with not all equal lengths
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t RvRtpReceiveFirstPackets1080p(st_session_impl_t *s, struct rte_mbuf *mbuf);

/*****************************************************************************************
 *
 * RvRtpReceiveFirstPackets2160p
 *
 * DESCRIPTION
 * Main function to processes single line 1080p first packets within session context
 * Other vendors packets with not all equal lengths
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t RvRtpReceiveFirstPackets2160p(st_session_impl_t *s, struct rte_mbuf *mbuf);

/*****************************************************************************************
 *
 * RvRtpReceiveFirstPackets720i
 *
 * DESCRIPTION
 * Main function to processes single line 720i first packets within session context
 * Other vendors packets with not all equal lengths
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t RvRtpReceiveFirstPackets720i(st_session_impl_t *s, struct rte_mbuf *mbuf);

/*****************************************************************************************
 *
 * RvRtpReceiveFirstPackets1080i
 *
 * DESCRIPTION
 * Main function to processes single line 1080i first packets within session context
 * Other vendors packets with not all equal lengths
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t RvRtpReceiveFirstPackets1080i(st_session_impl_t *s, struct rte_mbuf *mbuf);

/*****************************************************************************************
 *
 * RvRtpReceiveFirstPackets2160i
 *
 * DESCRIPTION
 * Main function to processes single line 2160i first packets within session context
 * Other vendors packets with not all equal lengths
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t RvRtpReceiveFirstPackets2160i(st_session_impl_t *s, struct rte_mbuf *mbuf);

/*****************************************************************************************
 *
 * RvRtpReceiveFirstPacketsDln720p
 *
 * DESCRIPTION
 * Main function to processes dual line 720p first packets within session context
 * Intel standard packets only
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t RvRtpReceiveFirstPacketsDln720p(st_session_impl_t *s, struct rte_mbuf *mbuf);

/*****************************************************************************************
 *
 * RvRtpReceiveFirstPacketsDln1080p
 *
 * DESCRIPTION
 * Main function to processes dual line 1080p first packets within session context
 * Intel standard packets only
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t RvRtpReceiveFirstPacketsDln1080p(st_session_impl_t *s, struct rte_mbuf *mbuf);

/*****************************************************************************************
 *
 * RvRtpReceiveNextPackets720p
 *
 * DESCRIPTION
 * Main function to processes single line 720p Next packets within session context
 * Other vendors packets with not all equal lengths
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t RvRtpReceiveNextPackets720p(st_session_impl_t *s, struct rte_mbuf *mbuf);

/*****************************************************************************************
 *
 * RvRtpReceiveNextPackets1080p
 *
 * DESCRIPTION
 * Main function to processes single line 1080p Next packets within session context
 * Other vendors packets with not all equal lengths
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t RvRtpReceiveNextPackets1080p(st_session_impl_t *s, struct rte_mbuf *mbuf);

/*****************************************************************************************
 *
 * RvRtpReceiveNextPackets2160p
 *
 * DESCRIPTION
 * Main function to processes single line 1080p Next packets within session context
 * Other vendors packets with not all equal lengths
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t RvRtpReceiveNextPackets2160p(st_session_impl_t *s, struct rte_mbuf *mbuf);

/*****************************************************************************************
 *
 * RvRtpReceiveNextPackets720i
 *
 * DESCRIPTION
 * Main function to processes single line 720p Next packets within session context
 * Other vendors packets with not all equal lengths
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t RvRtpReceiveNextPackets720i(st_session_impl_t *s, struct rte_mbuf *mbuf);

/*****************************************************************************************
 *
 * RvRtpReceiveNextPackets1080i
 *
 * DESCRIPTION
 * Main function to processes single line 1080p Next packets within session context
 * Other vendors packets with not all equal lengths
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t RvRtpReceiveNextPackets1080i(st_session_impl_t *s, struct rte_mbuf *mbuf);

/*****************************************************************************************
 *
 * RvRtpReceiveNextPackets2160i
 *
 * DESCRIPTION
 * Main function to processes single line 1080p Next packets within session context
 * Other vendors packets with not all equal lengths
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t RvRtpReceiveNextPackets2160i(st_session_impl_t *s, struct rte_mbuf *mbuf);

/*****************************************************************************************
 *
 * RvRtpReceiveNextPacketsDln720p
 *
 * DESCRIPTION
 * Main function to processes dual line 720p Next packets within session context
 * Intel standard packets only
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t RvRtpReceiveNextPacketsDln720p(st_session_impl_t *s, struct rte_mbuf *mbuf);

/*****************************************************************************************
 *
 * RvRtpReceiveNextPacketsDln1080p
 *
 * DESCRIPTION
 * Main function to processes dual line 1080p Next packets within session context
 * Intel standard packets only
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t RvRtpReceiveNextPacketsDln1080p(st_session_impl_t *s, struct rte_mbuf *mbuf);

/*****************************************************************************************
 *
 * RvRtpSessionCheckRunState
 *
 * RETURNS: 1 if session is ready to send packets, 0 otherwise
 *
 * SEE ALSO:
 */
int RvRtpSessionCheckRunState(st_session_impl_t *s);

void *RancRtpUpdateAncillaryPacket(st_session_impl_t *s, void *hdr, struct rte_mbuf *m);

static inline void
StSessionLock(st_session_impl_t *s)
{
	int lock;
	do
	{
		lock = __sync_lock_test_and_set(&s->lock, 1);
	} while (lock != 0);
}

static inline void
StSessionUnlock(st_session_impl_t *s)
{
	//unlock session and sliceOffset
	__sync_lock_release(&s->lock, 0);
}

static inline void
RaRtpSessionLock(rartp_session_t *s)
{
	int lock;
	do
	{
		lock = __sync_lock_test_and_set(&s->lock, 1);
	} while (lock != 0);
}

static inline void
RaRtpSessionUnlock(rartp_session_t *s)
{
	//unlock session and sliceOffset
	__sync_lock_release(&s->lock, 0);
}

#define RVRTP_SEMAPHORE_WAIT(semaphore, value)                                                     \
	do                                                                                             \
	{                                                                                              \
		__sync_synchronize();                                                                      \
	} while (semaphore != value);

#define RVRTP_SEMAPHORE_GIVE(semaphore, value) __sync_fetch_and_add(&semaphore, value);

#define RVRTP_BARRIER_SYNC(barrier, threadId, maxThrds)                                            \
	__sync_add_and_fetch(&barrier, 1);                                                             \
	if (threadId == 0)                                                                             \
		while (barrier < maxThrds)                                                                 \
		{                                                                                          \
			__sync_synchronize();                                                                  \
		}                                                                                          \
	else                                                                                           \
		while (barrier)                                                                            \
		{                                                                                          \
			__sync_synchronize();                                                                  \
		}                                                                                          \
	if (threadId == 0)                                                                             \
		__sync_fetch_and_and(&barrier, 0);

void *RvRtpDummyBuildPacket(st_session_impl_t *s, void *hdr, struct rte_mbuf *m);

int32_t RaRtpGetTimeslot(st_device_impl_t *dev);
st_status_t RaRtpGetTmstampTime(st30_format_t *fmt, double *tmstampTime);

int32_t RancRtpGetTimeslot(st_device_impl_t *dev);
void RancRtpSetTimeslot(st_device_impl_t *dev, int32_t timeslot, st_session_impl_t *s);
uint32_t RancRtpGetFrameTmstamp(st_session_impl_t *s, uint32_t firstWaits, U64 *roundTime,
								struct rte_mbuf *m);

/* internal api end */

#endif	// _ST_API_INTERNAL_H
