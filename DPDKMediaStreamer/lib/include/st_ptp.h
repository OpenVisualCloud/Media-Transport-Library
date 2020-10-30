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

#ifndef _ST_PTP_H
#define _ST_PTP_H

#include "st_api.h"
#include <rte_ethdev.h>
#include <rte_atomic.h>
#include <pthread.h>

typedef enum
{
	SYNC = 0x0,
	DELAY_REQ = 0x1,
	PDELAY_REQ = 0x2,
	PDELAY_RESP = 0x3,
	FOLLOW_UP = 0x8,
	DELAY_RESP = 0x9,
	PDELAY_RESP_FOLLOW_UP = 0xA,
	ANNOUNCE = 0xB,
	SIGNALING = 0xC,
	MANAGEMENT = 0xD
} st_ptp_messages;



typedef struct clock_id
{
	uint8_t id[8];
} clock_id_t;

struct port_id
{
    clock_id_t clockIdentity;
    uint16_t portNumber;
} __attribute__((packed));
typedef struct port_id port_id_t;


enum
{
	ARP_PROTOCOL = 0x0806,
	PTP_PROTOCOL = 0x88F7
};


struct ptp_header
{
    struct
    {
        uint8_t messageType:4;
        uint8_t transportSpecific:4;
    };
    struct
    {
        uint8_t versionPTP:4;
        uint8_t reserved0:4;
    };
    uint16_t messageLength;
    uint8_t domainNumber;
	uint8_t reserved1;
    uint16_t flagField;
    int64_t correctionField;
	uint32_t reserved2;
    port_id_t sourcePortIdentity;
    uint16_t sequenceId;
    uint8_t controlField;
    int8_t logMessageInterval;
} __attribute__((packed));
typedef struct ptp_header ptp_header_t;

struct ptp_tmstamp
{
	uint16_t sec_msb;
	uint32_t sec_lsb;
	uint32_t ns;
} __attribute__((packed));
typedef struct ptp_tmstamp ptp_tmstamp_t;

struct clock_quality
{
	uint8_t clockClass;
	uint8_t clockAccuracy;
	uint16_t offsetScaledLogVariance;
} __attribute__((packed));
typedef struct clock_quality clock_quality_t;

struct ptp_announce_msg
{
	ptp_header_t hdr;
	ptp_tmstamp_t originTimestamp;
	int16_t currentUtcOffset;
	uint8_t reserved;
	uint8_t grandmasterPriority1;
	clock_quality_t grandmasterClockQuality;
	uint8_t grandmasterPriority2;
	clock_id_t grandmasterIdentity;
	uint16_t stepsRemoved;
	uint8_t timeSource;
	uint8_t suffix[0];
} __attribute__((packed));
typedef struct ptp_announce_msg ptp_announce_msg_t;

struct ptp_sync_msg
{
	ptp_header_t hdr;
    ptp_tmstamp_t originTimestamp;
} __attribute__((packed));
typedef struct ptp_sync_msg ptp_sync_msg_t;
typedef struct ptp_sync_msg ptp_delay_req_msg_t;

struct ptp_follow_up_msg
{
	ptp_header_t hdr;
    ptp_tmstamp_t preciseOriginTimestamp;
	uint8_t suffix[0];
} __attribute__((packed));
typedef struct ptp_follow_up_msg ptp_follow_up_msg_t;

struct ptp_delay_resp_msg
{
	ptp_header_t hdr;
    ptp_tmstamp_t receiveTimestamp;
    port_id_t requestingPortIdentity;
	uint8_t suffix[0];
} __attribute__((packed));
typedef struct ptp_delay_resp_msg ptp_delay_resp_msg_t;

typedef enum
{
	PTP_NOT_INITIALIZED = 0x00,
	PTP_INITIALIZED     = 0x01,
} ptp_state_t;

typedef enum ptp_master_choose_mode
{
    ST_PTP_BEST_KNOWN_MASTER = 0,
    ST_PTP_SET_MASTER = 1,
    ST_PTP_FIRST_KNOWN_MASTER = 2,
} ptp_master_choose_mode_t;


typedef enum
{
    ST_PTP_CLOCK_SRC_AUTO,
    ST_PTP_CLOCK_SRC_ETH,
    ST_PTP_CLOCK_SRC_RTE,
    ST_PTP_CLOCK_SRC_RTC, //not supported
} st_ptp_clocksource_t;


typedef struct st_ptp_state
{
	ptp_state_t state;
	port_id_t masterPortIdentity;
	ptp_master_choose_mode_t masterChooseMode;
	port_id_t ourPortIdentity;
	uint16_t pauseToSendDelayReq;
    rte_atomic32_t isStop;
    uint16_t portId;
	uint16_t txRingId;
	struct rte_ring *txRing;
    struct rte_mempool *mbuf;
    volatile struct rte_mbuf *delReqPkt;
    pthread_mutex_t isDo;
    pthread_t ptpDelayReqThread;
    uint64_t t1;
	uint64_t t2;
    uint64_t t3;
	uint64_t t4;
    int ist2Soft;
	int ist3Soft;
	uint64_t t2HPet;
    uint64_t t3HPet;
    uint64_t t1HPetFreqStart;
    uint64_t t1HPetFreqClk;
    uint64_t t1HPetFreqClkNext;
	uint16_t syncSeqId;
	uint16_t delayReqId;
	int howSyncInAnnouce;
	int howDelayReqSent;
	int howDelayResInAnnouce;
	int howDelayResOurInAnnouce;
	int howHigherPortIdentity;
	int howDifDelayReqDelayRes;
	st_ptp_clocksource_t clkSrc;
} st_ptp_t;

extern st_status_t StParseEthernet(uint16_t portId, struct rte_mbuf *m);
extern st_status_t StPtpInit(uint16_t portId, struct rte_mempool *mbuf,  uint16_t txRingId, struct rte_ring *txRing);
extern st_status_t StPtpDeInit(uint16_t portId);
extern st_status_t StPtpIsSync(uint16_t portId);
extern st_status_t StSetClockSource(st_ptp_clocksource_t clkSrc);


#endif // _ST_PTP_H
