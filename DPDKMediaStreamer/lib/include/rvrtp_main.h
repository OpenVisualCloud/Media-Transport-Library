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

#ifndef _RVRTP_MAIN_H
#define _RVRTP_MAIN_H

/* General includes */
#include <inttypes.h>
#include <linux/if_ether.h>
#include <netinet/udp.h>
#include <stdbool.h>
#include <stdint.h>

/* DPDK includes */
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_ring.h>
#include <rte_time.h>

/* Self includes */
#include "st_api_internal.h"
#include "st_pkt.h"

#define LIB_VERSION_MAJOR 1
#define LIB_VERSION_MINOR 0
#define LIB_VERSION_LAST 11

#define MBUF_CACHE_SIZE 128
#define RX_RING_SIZE 16384
#define TX_RING_SIZE 4096
#define MAX_PAUSE_FRAMES (ST_MAX_SESSIONS_MAX * 2)
#define MIN_PKT_SIZE 64
#define IP_ADDR_LEN 4
#define MAC_ADDR_LEN 6
#define MAX_STR_LEN 80

typedef struct st_hw_caps
{
	uint32_t nicHwTmstamp : 1;
	uint32_t nicHwTimesync : 1;
	uint32_t reserved : 30;
} st_hw_caps_t;

typedef struct st_main_params
{
	volatile uint64_t schedStart;
	//used for sync between two schedule
	volatile uint64_t interSchedStart[MAX_RXTX_PORTS];
	volatile uint64_t ringStart;
	volatile uint64_t ringBarrier0;
	volatile uint64_t ringBarrier1;
	volatile uint64_t ringBarrier2;
	volatile uint64_t audioEnqStart;
	st_hw_caps_t hwCaps;
	uint32_t maxEnqThrds;
	st_thrd_params_t enqThrds[ST_MAX_ENQ_THREADS_MAX];
	uint32_t maxRcvThrds;
	uint32_t maxAudioRcvThrds;
	uint32_t maxAncRcvThrds;
	st_thrd_params_t rcvThrds[ST_MAX_RCV_THREADS_MAX];
	st_thrd_params_t audioRcvThrds[ST_AUDIO_MAX_RCV_THREADS_MAX];
	st_thrd_params_t ancRcvThrds[ST_ANC_MAX_RCV_THREADS_MAX];
	uint32_t maxSchThrds;
	uint16_t txPortId[MAX_RXTX_PORTS];
	uint16_t rxPortId[MAX_RXTX_PORTS];
	struct rte_mempool *mbufPool;

	// Input Parameters
	uint8_t ipAddr[MAX_RXTX_PORTS][MAX_RXTX_TYPES][IP_ADDR_LEN];  /**< destination IP for tx */
	uint8_t sipAddr[MAX_RXTX_PORTS][IP_ADDR_LEN]; /**< source IP */
	bool isEbuCheck;
	uint32_t pRx;
	uint32_t pTx;
	uint32_t rRx;
	uint32_t rTx;
	uint32_t rate;
	uint32_t interlaced;
	uint32_t fmtIndex;
	uint32_t audioFmtIndex;
	uint16_t audioFrameSize;
	uint32_t txBulkNum; /* The number of objects for each tx dequeue */
	uint32_t snCount;
	uint32_t sn30Count;
	uint32_t sn40Count;
	uint16_t udpPort;
	uint16_t numPorts;
	st21_buf_fmt_t bufFormat;
	st_pacing_type_t pacing; /* Pacing control way */
	uint64_t tscHz; /* The frequency of the RDTSC timer resolution */
	uint32_t userTmstamp; /* (Optional) Enqueue use user provided timestamp for packet's RTP timestamp field */
	char lib_cid[512];

	char outPortName[MAX_RXTX_PORTS][MAX_STR_LEN];
	char inPortName[MAX_RXTX_PORTS][MAX_STR_LEN];
	char dpdkParams[MAX_STR_LEN]; /**< parameters after empty --  */
} st_main_params_t;

typedef struct
{
	uint64_t pktsPriAllocFail;
	uint64_t pktsExtAllocFail;
	uint64_t pktsRedAllocFail;
	uint64_t pktsBuild;
	uint64_t pktsQueued;

	uint32_t pktsQueuePriFail;
	uint32_t pktsQueueRedFail;
	uint32_t sessionLkpFail;
	uint32_t sessionStateFail;
	uint32_t pktsChainPriFail;
	uint32_t pktsChainRedFail;

	/* place holder to keep it aligned to RTE_CACHE_LINE_SIZE */
	//uint64_t rsvrd[(current element size) - RTE_CACHE_LINE_SIZE%8];
} __rte_cache_aligned st_enqueue_stats_t;

typedef struct
{
	uint64_t badIpUdp;
	uint64_t badIpUdpR;
	uint64_t badRtp;
	uint64_t badRtpR;
	uint64_t tmpstampDone;
	uint64_t tmpstampDoneR;
	uint64_t outOfOrder;
	uint64_t outOfOrderR;
	uint64_t rtpTmstampOverflow;
	uint64_t rtpTmstampOverflowR;
	uint64_t rtpTmstampLess;
	uint64_t rtpTmstampLessR;

	uint64_t restartAsNewFrame;
	uint64_t restartAsNewFrameR;

	uint64_t firstPacketGood;
	uint64_t firstPacketGoodR;
	uint64_t nonFirstPacketGood;
	uint64_t nonFirstPacketGoodR;
	uint64_t lastPacketGood;
	uint64_t lastPacketGoodR;

	uint64_t nonFirstPacketPendGood;
	uint64_t nonFirstPacketPendGoodR;
	uint64_t lastPacketPendGood;
	uint64_t lastPacketPendGoodR;

	uint64_t fastCopyFail;
	uint64_t fastCopyFailR;
	uint64_t fastCopyFailErr;
	uint64_t fastCopyFailErrR;

	uint64_t userNotifyLine;
	uint64_t userNotifyPendLine;
	uint64_t userNotifyFrame;
	uint64_t userNotifyPendFrame;

	uint64_t completeFrames;
	uint64_t completePendFrames;
	uint64_t incompleteFrameDone;
	uint64_t incompletePendFrameDone;

	uint64_t forcePendBuffOut;
	uint64_t forcePendBuffOutR;
	uint64_t forceCurrBuffOut;
	uint64_t forceCurrBuffOutR;
} __rte_cache_aligned st_rcv_stats_t;


/*
 * Utility functions for device creation
 */
void StDevInitTxThreads(st_main_params_t *mp, st_device_impl_t *dev);
void StDevInitRxThreads(st_main_params_t *mp, st_device_impl_t *dev);
st_status_t StDevCalculateBudgets(st_device_impl_t *d, int num_port);

/* Defines */
//#define DEBUG

/* Globals */
extern struct tprs_scheduler sch;
extern st_main_params_t stMainParams;
extern const st_nic_rate_params_t *stDevParams;

extern rte_atomic32_t isTxDevToDestroy;
extern rte_atomic32_t isRxDevToDestroy;
extern rte_atomic32_t isStopMainThreadTasks;

static inline st_pacing_type_t StGetPacing()
{
	return stMainParams.pacing;
}

static inline bool StIsTscPacing()
{
	return (ST_PACING_TSC == StGetPacing()) ? true : false;
}

static inline void StSetPacing(st_pacing_type_t pacing)
{
	stMainParams.pacing = pacing;
}

static inline uint64_t StGetTscTimeHz()
{
	return stMainParams.tscHz;
}

static inline void StSetTscTimeHz(uint64_t hz)
{
	stMainParams.tscHz = hz;
}

/* Return relative TSC time in nanoseconds */
static inline uint64_t StGetTscTimeNano()
{
	double tsc = rte_get_tsc_cycles();
	double tsc_hz = StGetTscTimeHz();
	double time_nano = tsc / (tsc_hz / ((double)NSEC_PER_SEC));
	return time_nano;
}

extern void ShowWelcomeBanner();

void PrintVersion();
void PrintHelp();

// forward declarations
int LcoreMainPktRingEnqueue(void *args);
int LcoreMainAudioRingEnqueue(void *args);
int LcoreMainAncillaryRingEnqueue(void *args);
int LcoreMainReceiver(void *args);
int LcoreMainAudioReceiver(void *args);

typedef struct lcore_transmitter_args
{
	uint32_t threadId; /* Thread id, 0 to max threads */
	uint32_t bulkNum;  /* The number of objects for each dequeue, 1, 2 or 4 */
} lcore_transmitter_args_t;

int LcoreMainTransmitter(void *args);

st_status_t RvRtpValidateFormat(st21_format_t *fmt);
int St40CheckParityBits(uint16_t val);
st_status_t St40GetUDW(int idx, uint16_t *udw, uint8_t *data);
uint16_t St40CalcChecksum(int howData, uint8_t *data);

#endif
