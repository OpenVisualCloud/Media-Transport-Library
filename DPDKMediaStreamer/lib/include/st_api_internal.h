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
#define FRAME_MAX 2

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#define ST_ASSERT rte_exit(127, "ASSERT error file %s function %s line %u\n", __FILE__, __FUNCTION__, __LINE__)

#define ETH_ADDR_LEN 6

/**
* Structure for connection/flow IPv4/v6 addresses and UDP ports
*/
struct st_flow 
{
	union 
	{
		struct sockaddr_in  addr4;
		struct sockaddr_in6 addr6;
	} src;
	union 
	{
		struct sockaddr_in  addr4;
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
			uint8_t ecn  : 2;
			uint8_t dscp : 6;
		};
		uint8_t tos;
	};
	uint8_t		dstMac[ETH_ADDR_LEN];
	uint8_t		srcMac[ETH_ADDR_LEN];
};
typedef struct st_flow st_flow_t;


typedef struct rvrtp_bufs 
{
	uint32_t  tmstamp;
	uint32_t  pkts;
	uint8_t	 *buf;
} rvrtp_bufs_t;

typedef enum 
{
	ST_SN_STATE_OFF = 0,
	ST_SN_STATE_ON = 1,   //created but stopped, waiting for frame start
	ST_SN_STATE_RUN = 2,  //actively sending a frame
	ST_SN_STATE_NO_NEXT_FRAME = 3, // hold waiting for the next frame
	ST_SN_STATE_NO_NEXT_SLICE = 4, // hold waiting for the next slice
	ST_SN_STATE_STOP_PENDING = 5,  // stop is pending, shall be then restarted or destroyed
	ST_SN_STATE_TIMEDOUT = 6, // stop after the too long hold
} st_sn_state_t;

#define ST_FRAG_HISTOGRAM_720P_DLN_SZ  (RTE_CACHE_LINE_ROUNDUP(720  * sizeof(uint8_t)))// aligned to 64
#define ST_FRAG_HISTOGRAM_720P_SLN_SZ  (RTE_CACHE_LINE_ROUNDUP(360  * sizeof(uint8_t)))// aligned to 64
#define ST_FRAG_HISTOGRAM_1080P_DLN_SZ (RTE_CACHE_LINE_ROUNDUP(540 * sizeof(uint8_t))) // aligned to 64
#define ST_FRAG_HISTOGRAM_1080P_SLN_SZ (RTE_CACHE_LINE_ROUNDUP(540 * sizeof(uint8_t))) // aligned to 64
#define ST_FRAG_HISTOGRAM_2160P_SLN_SZ (RTE_CACHE_LINE_ROUNDUP(2160 * sizeof(uint8_t)))// aligned to 64
#define ST_FRAG_HISTOGRAM_720I_SLN_SZ  (RTE_CACHE_LINE_ROUNDUP(180 * sizeof(uint8_t))) // aligned to 64
#define ST_FRAG_HISTOGRAM_1080I_SLN_SZ (RTE_CACHE_LINE_ROUNDUP(270 * sizeof(uint8_t))) // aligned to 64
#define ST_FRAG_HISTOGRAM_2160I_SLN_SZ (RTE_CACHE_LINE_ROUNDUP(1080 * sizeof(uint8_t)))// aligned to 64



typedef enum 
{
	ST_OFLD_HW_IP_CKSUM  = 0x1,
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
	uint16_t fieldId; //interlaced field 0 or 1 (odd or even)

	uint32_t line1Size;
	uint32_t line2Size;

	//receiver specific 
	uint8_t* data;//current buffer pointer for receiver

	uint32_t* lineHistogram;
	uint8_t*  fragHistogram;

}__rte_cache_aligned;

typedef struct rvrtp_pkt_ctx rvrtp_pkt_ctx_t;

typedef struct rvrtp_session rvrtp_session_t;

struct rvrtp_ebu_stat
{
	uint64_t cinTmstamp;
	uint64_t cinCnt;
	uint64_t cinSum;
	uint64_t cinMax;
	uint64_t cinMin;
	double   cinAvg;

	uint64_t vrxCnt;
	uint64_t vrxSum;
	uint64_t vrxMax;
	uint64_t vrxMin;
	double   vrxAvg;

	uint64_t latCnt;
	uint64_t latSum;
	uint64_t latMax;
	uint64_t latMin;
	double   latAvg;

	int64_t tmdCnt;
	int64_t tmdSum;
	int64_t tmdMax;
	int64_t tmdMin;
	double  tmdAvg;

	uint32_t prevPktTmstamp;
	uint32_t prevRtpTmstamp;
	uint64_t prevEpochTime;
	uint64_t prevTime;

	uint32_t tmiCnt;
	uint32_t tmiSum;
	uint32_t tmiMax;
	uint32_t tmiMin;
	double   tmiAvg;

	uint64_t fptCnt;
	uint64_t fptSum;
	uint64_t fptMax;
	uint64_t fptMin;
	double   fptAvg;
} __rte_cache_aligned;

typedef struct rvrtp_ebu_stat rvrtp_ebu_stat_t;

/**
* Function to build packet as it is dependent on a format
*/
typedef void *(*RvRtpUpdatePacket_f)(rvrtp_session_t *s, void *hdr, struct rte_mbuf *);

/**
* Function to receive packet as it is dependent on a format
*/
typedef st_status_t(*RvRtpRecvPacket_f)(rvrtp_session_t *s, struct rte_mbuf *rxbuf);

/**
* Structure for session packet format definition
*/
struct rvrtp_session
{
	st21_session_t  sn;
	st21_format_t	fmt;
	char		   *sdp;

	st_flow_t       fl[2];

	uint16_t		etherVlan;//tag to put if VLAN encap is enabled
	uint16_t		etherSize;//14 or 18 if with VLAN

	uint32_t		pktTime;
	uint32_t		tmstampTime; //in nanoseconds
	uint32_t		lastTmstamp;
	uint32_t		nicTxTime;

	st21_producer_t prod;
	uint8_t		   *prodBuf;

	st21_consumer_t cons;
	rvrtp_bufs_t    consBufs[FRAME_MAX];
	uint32_t        consState;

	uint32_t		tmstampToDrop[2];
	uint64_t		pktsDrop;
	uint64_t		frmsDrop;
	uint64_t		frmsFixed;

	st_ofld_hw_t    ofldFlags;

	uint32_t		ptpDropTime;

	struct rvrtp_device *dev;
	uint32_t		tid;

	volatile int           lock;
	volatile uint32_t      sliceOffset;
	volatile st_sn_state_t state;

	//functions set per format
	RvRtpUpdatePacket_f		UpdateRtpPkt;
	RvRtpRecvPacket_f		RecvRtpPkt;

	uint64_t           fragPattern;

	rvrtp_pkt_ctx_t    ctx;
	rvrtp_ebu_stat_t   ebu;
	union st_pkt_hdr   hdrPrint __rte_cache_aligned;
	uint64_t		   padding[8] __rte_cache_aligned;//usefull to capture memory corrupts
}__rte_cache_aligned;

void RvRtpInitPacketCtx(rvrtp_session_t *s, uint32_t ring);


struct rvrtp_device
{
	st_device_t		 dev;

	rvrtp_session_t* *snTable;
	uint32_t		  snCount;
	
	uint32_t		 quot;   //in bytes for a batch of packets
	uint32_t		 remaind;//remaind of the byte budget
	uint32_t		 timeQuot; //in nanoseconds
	uint32_t		 *timeTable;

	uint32_t		 rxOnly;
	uint32_t		 txOnly;

	uint32_t 		 maxRings;
	uint32_t 		 outOfBoundRing;

	struct rte_mempool *mbufPool;
	uint32_t           *txPktSizeL1;
	struct rte_ring*   *txRing;

	uint32_t		fmtIndex;

	//receive device Flow Table
	struct rte_flow *flTable[ST_MAX_FLOWS];

	uint32_t		lastAllocSn;//session ID that was allocated the prev time

	uint8_t			srcMacAddr[2][ETH_ADDR_LEN];

	uint64_t 		*packetsTx;
	uint64_t 		*pausesTx;

	volatile int     lock;
}__rte_cache_aligned;
typedef struct rvrtp_device rvrtp_device_t;


extern rvrtp_device_t stRecvDevice;
extern rvrtp_device_t stSendDevice;

struct tprs_scheduler
{
	uint32_t timeCursor;
	uint32_t timeRemaind;

	uint32_t quot;
	uint32_t remaind;

	uint64_t currentBytes;

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

	uint32_t slot; //pause table heap position 
	uint32_t top;  //packet vector heap position
	uint32_t burstSize;

} __rte_cache_aligned;
typedef struct tprs_scheduler tprs_scheduler_t;

st_status_t RvRtpValidateSession(st21_session_t *sn);
st_status_t RvRtpValidateDevice(st_device_t *dev);

/*
 * Internal functions for sessions creation
 */
st_status_t RvRtpCreateRxSession(rvrtp_device_t *dev, st21_session_t *sin, st21_format_t *fmt,
								 rvrtp_session_t **sout);

st_status_t RvRtpCreateTxSession(rvrtp_device_t *dev, st21_session_t *sin, st21_format_t *fmt, rvrtp_session_t **sout);

st_status_t RvRtpSendDeviceAdjustBudget(rvrtp_device_t *dev);


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
void *RvRtpUpdateDualLinePacket(rvrtp_session_t *s, void *hdr, struct rte_mbuf * m);


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
void *RvRtpUpdateSingleLinePacket(rvrtp_session_t *s, void *hdr, struct rte_mbuf *m);


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
void *RvRtpUpdateInterlacedPacket(rvrtp_session_t *s, void *hdr, struct rte_mbuf *m);

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
st_status_t RvRtpReceivePacketCallback(rvrtp_session_t *s, struct rte_mbuf *m);

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
st_status_t RvRtpReceiveFirstPacketsSln720p(rvrtp_session_t *s, struct rte_mbuf *mbuf);

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
st_status_t RvRtpReceiveFirstPacketsSln1080p(rvrtp_session_t *s, struct rte_mbuf *mbuf);

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
st_status_t RvRtpReceiveFirstPacketsSln2160p(rvrtp_session_t *s, struct rte_mbuf *mbuf);


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
st_status_t RvRtpReceiveFirstPackets720p(rvrtp_session_t *s, struct rte_mbuf *mbuf);

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
st_status_t RvRtpReceiveFirstPackets1080p(rvrtp_session_t *s, struct rte_mbuf *mbuf);

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
st_status_t RvRtpReceiveFirstPackets2160p(rvrtp_session_t *s, struct rte_mbuf *mbuf);

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
st_status_t RvRtpReceiveFirstPackets720i(rvrtp_session_t *s, struct rte_mbuf *mbuf);

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
st_status_t RvRtpReceiveFirstPackets1080i(rvrtp_session_t *s, struct rte_mbuf *mbuf);

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
st_status_t RvRtpReceiveFirstPackets2160i(rvrtp_session_t *s, struct rte_mbuf *mbuf);


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
st_status_t RvRtpReceiveFirstPacketsDln720p(rvrtp_session_t *s, struct rte_mbuf *mbuf);

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
st_status_t RvRtpReceiveFirstPacketsDln1080p(rvrtp_session_t *s, struct rte_mbuf *mbuf);

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
st_status_t RvRtpReceiveNextPackets720p(rvrtp_session_t *s, struct rte_mbuf *mbuf);

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
st_status_t RvRtpReceiveNextPackets1080p(rvrtp_session_t *s, struct rte_mbuf *mbuf);

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
st_status_t RvRtpReceiveNextPackets2160p(rvrtp_session_t *s, struct rte_mbuf *mbuf);

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
st_status_t RvRtpReceiveNextPackets720i(rvrtp_session_t *s, struct rte_mbuf *mbuf);

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
st_status_t RvRtpReceiveNextPackets1080i(rvrtp_session_t *s, struct rte_mbuf *mbuf);

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
st_status_t RvRtpReceiveNextPackets2160i(rvrtp_session_t *s, struct rte_mbuf *mbuf);

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
st_status_t RvRtpReceiveNextPacketsDln720p(rvrtp_session_t *s, struct rte_mbuf *mbuf);

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
st_status_t RvRtpReceiveNextPacketsDln1080p(rvrtp_session_t *s, struct rte_mbuf *mbuf);



/*****************************************************************************************
 *
 * RvRtpSessionCheckRunState
 *
 * RETURNS: 1 if session is ready to send packets, 0 otherwise
 *
 * SEE ALSO:
 */
int RvRtpSessionCheckRunState(rvrtp_session_t *s);

static inline void 
RvRtpSessionLock(rvrtp_session_t *s)
{
	int lock;
	do {
		lock = __sync_lock_test_and_set(&s->lock, 1);
	} while (lock != 0);
}

static inline void 
RvRtpSessionUnlock(rvrtp_session_t *s)
{
	//unlock session and sliceOffset
	__sync_lock_release(&s->lock, 0);
}

#define RVRTP_SEMAPHORE_WAIT(semaphore, value) \
	do				   						   \
	{										   \
		__sync_synchronize();				   \
	} while (semaphore != value);								   

#define RVRTP_SEMAPHORE_GIVE(semaphore, value) \
	__sync_fetch_and_add(&semaphore, value);

#define RVRTP_BARRIER_SYNC(barrier, threadId, maxThrds) \
	__sync_add_and_fetch(&barrier, 1);					\
	if (threadId == 0)									\
		while (barrier < maxThrds)						\
		{												\
			__sync_synchronize();						\
		}												\
	else												\
		while (barrier)									\
		{												\
			__sync_synchronize();						\
		}												\
	if (threadId == 0)	__sync_fetch_and_and(&barrier, 0);



void *RvRtpDummyBuildPacket(rvrtp_session_t *s, void *hdr, struct rte_mbuf *extMbuf);
/* internal api end */

/**
* CANDIDATES FOR REMOVAL from extranl API
* Not sure about the below functions yet (like passive producer/consumer solution)


st_status_t St21Send2Lines(st21_session_t *sn, uint8_t *frameBuf);
st_status_t St21SendUdp(st21_session_t *sn, const uint8_t *udp_pkt, const uint16_t size);

st_status_t St21Recv2LinesFrag(st21_session_t *sn, struct rte_mbuf *pkt[]);
st_status_t St21GetPTP(st21_session_t *sn, struct rte_mbuf *pkt[], uint32_t *tmstamp);

st_status_t St21RecvFrame(st21_session_t *sn, uint8_t *frameBufPrev, uint8_t *frameBufCurr);
st_status_t St21RecvUdp(st21_session_t *sn, uint8_t *udp_pkt, const uint16_t size);

*/

#endif // _ST_API_INTERNAL_H

