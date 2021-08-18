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

#include "st_ptp.h"

#include <rte_ethdev.h>

#include "rvrtp_main.h"
#include "st_arp.h"
#include "st_igmp.h"

#include <pthread.h>
#include <st_api.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#define _BSD_SOURCE
#include <rte_alarm.h>
#include <rte_atomic.h>

#include <endian.h>
#include <math.h>
#include <string.h>


#define RTE_LOGTYPE_ST_PTP (RTE_LOGTYPE_USER1)
#define RTE_LOGTYPE_ST_PTP1 (RTE_LOGTYPE_USER1)
#define RTE_LOGTYPE_ST_PTP2 (RTE_LOGTYPE_USER2)
#define RTE_LOGTYPE_ST_PTP3 (RTE_LOGTYPE_USER3)
#define RTE_LOGTYPE_ST_PTP4 (RTE_LOGTYPE_USER4)
#define RTE_LOGTYPE_ST_PTP5 (RTE_LOGTYPE_USER5)
#define RTE_LOGTYPE_ST_PTP6 (RTE_LOGTYPE_USER6)
#define RTE_LOGTYPE_ST_PTP7 (RTE_LOGTYPE_USER7)
#define RTE_LOGTYPE_ST_PTP8 (RTE_LOGTYPE_USER8)


#define PAUSE_TO_SEND_FIRST_DELAY_REQ (50)	//us
#define ORDER_WAIT_TIME (50)				//us
#define MIN_FREQ_MES_TIME (10000000000ll)	//ns

#define ST_PTP_CLOCK_IDENTITY_MAGIC (0xfeff)

//INFO_PTP enable print of all ptp debug information
#define INFO_PTP (0)

#if INFO_PTP == 0
#undef PTP_LOG
#define PTP_LOG(...)
#else
#undef PTP_LOG
#define PTP_LOG(...) RTE_LOG(__VA_ARGS__)
#endif

#define USE_LOCK (1)

static st_ptp_t ptpState[MAX_RXTX_PORTS] = {
	{
		.state = PTP_NOT_INITIALIZED,
		.masterChooseMode = ST_PTP_FIRST_KNOWN_MASTER,
	},
	{
		.state = PTP_NOT_INITIALIZED,
		.masterChooseMode = ST_PTP_FIRST_KNOWN_MASTER,
	},
};

#define PTP_SUM_LOG(...) RTE_LOG(__VA_ARGS__)
#define SUMMARY_INFO_PTP_PERIOD (10)	 //s
#define IGMP_JOIN_PTP_GROUP_PERIOD (20)	 //s

typedef struct
{
	int64_t lastAvgAbsVal;
	int64_t lastPartAvgAbsVal;
	int64_t lastMaxAbsOff;
	int64_t lastMinAbsOff;
	int cntToSum;
	rte_atomic32_t isWorks;
	rte_atomic32_t isClr;
} st_ptp_summary_statistic;
static st_ptp_summary_statistic ptpSumStat[MAX_RXTX_PORTS];

void
StPtpPrintStatus_callback(void *arg)
{
	long portId = (long)arg;
	if (rte_atomic32_read(&isStopMainThreadTasks) == 1)
	{
		StPtpDeInit(portId);
		return;
	}

	PTP_SUM_LOG(INFO, ST_PTP, "PTP report for port:                    %ld\n", portId);
	if (rte_atomic32_read(&ptpSumStat[portId].isWorks) != 0)
	{
		PTP_SUM_LOG(INFO, ST_PTP, "Curent L4/L2 mode:                      %s\n",
					ptpState[portId].ptpLMode == ST_PTP_L2_MODE ? "L2(802.3)" : "L4(IPV4/UDP)");
		PTP_SUM_LOG(INFO, ST_PTP, "Average absolute adjusment value:       %ld\n",
					ptpSumStat[portId].lastAvgAbsVal);
		PTP_SUM_LOG(INFO, ST_PTP, "Max absolute adjustment value:          %ld\n",
					ptpSumStat[portId].lastMaxAbsOff);
		PTP_SUM_LOG(INFO, ST_PTP, "Min absolute adjustment value:          %ld\n\n",
					ptpSumStat[portId].lastMinAbsOff);
	}
	else
	{
		PTP_SUM_LOG(INFO, ST_PTP, "PTP for portId: %ld not synchronized yet\n", portId);
	}
	rte_atomic32_clear(&ptpSumStat[portId].isWorks);
	rte_atomic32_set(&ptpSumStat[portId].isClr, 1);
	if (0
		!= rte_eal_alarm_set(SUMMARY_INFO_PTP_PERIOD * 1000000ll, StPtpPrintStatus_callback,
							 (void *)portId))
	{
		/* retry once more if nto exit */
		if (0
			!= rte_eal_alarm_set(SUMMARY_INFO_PTP_PERIOD * 1000000ll, StPtpPrintStatus_callback,
								 (void *)portId))
			rte_exit(EXIT_FAILURE, "failed to enable PTP stats handler for portId: %ld!\n", portId);
	}
}

static inline int
StPtpComparePortIdentities(const port_id_t *a, const port_id_t *b)
{
	return memcmp(a, b, sizeof(port_id_t));
}

int addrMode;
int stepMode;
clock_id_t setClockId;

#if USE_LOCK == 1
static inline void
StPtpLock(uint16_t portId)
{
	int lock;
	do
	{
		lock = __sync_lock_test_and_set(&ptpState[portId].lock, 1);
	} while (lock != 0);
}

static inline void
StPtpUnlock(uint16_t portId)
{
	__sync_lock_release(&ptpState[portId].lock, 0);
}
#else
#define StPtpLock(x)
#define StPtpUnlock(x)
#endif

st_status_t
StPtpGetParam(st_param_t prm, st_param_val_t *val, uint16_t portId)
{
	switch (prm)
	{
	case ST_PTP_CLOCK_ID:
		memcpy(val->ptr, &ptpState[portId].setClockId, sizeof(ptpState[portId].setClockId));
		break;
	case ST_PTP_ADDR_MODE:
		val->valueU32 = ptpState[portId].addrMode;
		break;
	case ST_PTP_STEP_MODE:
		val->valueU32 = ptpState[portId].stepMode;
		break;
	case ST_PTP_CHOOSE_CLOCK_MODE:
		val->valueU32 = ptpState[portId].masterChooseMode;
		break;
	default:
		PTP_LOG(INFO, USER1, "Parameter not supported by StPtpGetParam\n");
		break;
	}
	return ST_OK;
}

st_status_t
StPtpSetParam(st_param_t prm, st_param_val_t val)
{
	switch (prm)
	{
	case ST_PTP_CLOCK_ID:
		memcpy(&ptpState[ST_PPORT].setClockId, val.ptr, sizeof(ptpState[ST_PPORT].setClockId));
		memcpy(&ptpState[ST_RPORT].setClockId, val.ptr, sizeof(ptpState[ST_RPORT].setClockId));
		break;
	case ST_PTP_ADDR_MODE:
		ptpState[ST_PPORT].addrMode = val.valueU32;
		ptpState[ST_RPORT].addrMode = val.valueU32;
		break;
	case ST_PTP_STEP_MODE:
		ptpState[ST_PPORT].stepMode = val.valueU32;
		ptpState[ST_RPORT].stepMode = val.valueU32;
		break;
	case ST_PTP_CHOOSE_CLOCK_MODE:
		ptpState[ST_PPORT].masterChooseMode = val.valueU32;
		ptpState[ST_RPORT].masterChooseMode = val.valueU32;
		break;
	default:
		PTP_LOG(INFO, USER1, "Parameter not supported by StPtpGetParam\n");
		break;
	}
	return ST_OK;
}

#if INFO_PTP == 1
static void
StPtpPrintClockIdentity(char *fieldName, clock_id_t *clockIdentity)
{
	PTP_LOG(INFO, ST_PTP5, "%s.clockIdentity: %02x:%02x:%02x:%02x%02x:%02x:%02x:%02x\n", fieldName,
			clockIdentity->id[0], clockIdentity->id[1], clockIdentity->id[2], clockIdentity->id[3],
			clockIdentity->id[4], clockIdentity->id[5], clockIdentity->id[6], clockIdentity->id[7]);
}

static void
StPtpPrintPortIdentity(char *fieldName, port_id_t *portIdentity)
{
	StPtpPrintClockIdentity(fieldName, &portIdentity->clockIdentity);
	PTP_LOG(INFO, ST_PTP5, "%s.portNumber: %04x\n", fieldName, htons(portIdentity->portNumber));
}

static void
StPtpPrintHeader(ptp_header_t *ptpHdr)
{
	PTP_LOG(INFO, ST_PTP5, "\n");
	PTP_LOG(INFO, ST_PTP5, "### PTP HEADER ###\n");
	PTP_LOG(INFO, ST_PTP5, "messageType: 0x%01x\n", ptpHdr->messageType);
	PTP_LOG(INFO, ST_PTP5, "transportSpecific: 0x%01x\n", ptpHdr->transportSpecific);
	PTP_LOG(INFO, ST_PTP5, "versionPTP: 0x%01x\n", ptpHdr->versionPTP);
	PTP_LOG(INFO, ST_PTP5, "messageLength: %d\n", htons(ptpHdr->messageLength));
	PTP_LOG(INFO, ST_PTP5, "domainNumber: %d\n", ptpHdr->domainNumber);
	PTP_LOG(INFO, ST_PTP5, "flagField: 0x%04x\n", htons(ptpHdr->flagField));
	PTP_LOG(INFO, ST_PTP5, "correctionField: 0x%016lx\n", be64toh(ptpHdr->correctionField));
	StPtpPrintPortIdentity("sourcePortIdentity", (port_id_t *)&ptpHdr->sourcePortIdentity);
	PTP_LOG(INFO, ST_PTP5, "sequenceId: 0x%02x\n", htons(ptpHdr->sequenceId));
	PTP_LOG(INFO, ST_PTP5, "controlField: 0x%02x\n", ptpHdr->controlField);
	PTP_LOG(INFO, ST_PTP5, "logMessageInterval: 0x%02x\n", ptpHdr->logMessageInterval);
	PTP_LOG(INFO, ST_PTP5, "\n");
}

static void
StPtpPrintTimeStamp(char *fieldName, ptp_tmstamp_t *ts)
{
	uint64_t sec = (uint64_t)ntohl(ts->sec_lsb) + ((uint64_t)ntohs(ts->sec_msb) << 32);
	PTP_LOG(INFO, ST_PTP5, "%s: %ld:%d\n", fieldName, sec, ntohl(ts->ns));
}

static void
StPtpPrintClockQuality(char *fieldName, clock_quality_t *cs)
{
	PTP_LOG(INFO, ST_PTP5, "%s.clockClass: %d\n", fieldName, cs->clockClass);
	PTP_LOG(INFO, ST_PTP5, "%s.clockAccuracy: %d\n", fieldName, cs->clockAccuracy);
	PTP_LOG(INFO, ST_PTP5, "%s.offsetScaledLogVariance: %d\n", fieldName,
			ntohs(cs->offsetScaledLogVariance));
}

static void
StPtpPrintAnnounceMsg(ptp_announce_msg_t *ptpHdr)
{
	PTP_LOG(INFO, ST_PTP5, "\033[31;1m\n");
	PTP_LOG(INFO, ST_PTP5, "\n##### PTP ANNOUNCE MESSAGE #####\n");
	StPtpPrintHeader((ptp_header_t *)ptpHdr);
	PTP_LOG(INFO, ST_PTP5, "### PTP ANNOUNCE MESSAGE DATA ###\n");
	StPtpPrintTimeStamp("originTimestamp", &ptpHdr->originTimestamp);
	PTP_LOG(INFO, ST_PTP5, "currentUtcOffset: %d\n", ntohs(ptpHdr->currentUtcOffset));
	PTP_LOG(INFO, ST_PTP5, "grandmasterPriority1: %d\n", ptpHdr->grandmasterPriority1);
	StPtpPrintClockQuality("grandmasterClockQuality", &ptpHdr->grandmasterClockQuality);
	PTP_LOG(INFO, ST_PTP5, "grandmasterPriority2: %d\n", ptpHdr->grandmasterPriority2);
	StPtpPrintClockIdentity("grandmasterIdentity", &ptpHdr->grandmasterIdentity);
	PTP_LOG(INFO, ST_PTP5, "stepsRemoved: %d\n", ntohs(ptpHdr->stepsRemoved));
	PTP_LOG(INFO, ST_PTP5, "timeSource: %d\n", ptpHdr->timeSource);
	PTP_LOG(INFO, ST_PTP5, "\033[39;0m\n");
}

static void
StPtpPrintSyncDelayReqMsg(ptp_sync_msg_t *ptpHdr, char *msgName, uint32_t color)
{
	PTP_LOG(INFO, ST_PTP5, "\033[%x;1m\n", color);
	PTP_LOG(INFO, ST_PTP5, "\n##### PTP %s MESSAGE #####\n", msgName);
	StPtpPrintHeader((ptp_header_t *)ptpHdr);
	PTP_LOG(INFO, ST_PTP5, "### PTP %s MESSAGE DATA ###\n", msgName);
	StPtpPrintTimeStamp("originTimestamp", &ptpHdr->originTimestamp);
	PTP_LOG(INFO, ST_PTP5, "\n");
	PTP_LOG(INFO, ST_PTP5, "\033[39;0m\n");
}

static inline void
StPtpPrintSyncMsg(ptp_sync_msg_t *ptpHdr)
{
	StPtpPrintSyncDelayReqMsg(ptpHdr, "SYNC", 0x32);
}

static inline void
StPtpPrintDelayReqMsg(ptp_delay_req_msg_t *ptpHdr)
{
	StPtpPrintSyncDelayReqMsg(ptpHdr, "DELAY_REQ", 0x36);
}

static void
StPtpPrintFollowUpMsg(ptp_follow_up_msg_t *ptpHdr)
{
	PTP_LOG(INFO, ST_PTP5, "\033[33;1m\n");
	PTP_LOG(INFO, ST_PTP5, "\n##### PTP FOLLOW_UP MESSAGE #####\n");
	StPtpPrintHeader((ptp_header_t *)ptpHdr);
	PTP_LOG(INFO, ST_PTP5, "### PTP FOLLOW_UP MESSAGE DATA ###\n");
	StPtpPrintTimeStamp("preciseOriginTimestamp", &ptpHdr->preciseOriginTimestamp);
	PTP_LOG(INFO, ST_PTP5, "\n");
	PTP_LOG(INFO, ST_PTP5, "\033[39;0m\n");
}

static void
StPtpPrintDelayResMsg(ptp_delay_resp_msg_t *ptpHdr)
{
	PTP_LOG(INFO, ST_PTP5, "\033[34;1m\n");
	PTP_LOG(INFO, ST_PTP5, "\n##### PTP DELAY_RESP MESSAGE #####\n");
	StPtpPrintHeader((ptp_header_t *)ptpHdr);
	PTP_LOG(INFO, ST_PTP5, "### PTP DELAY_RESP MESSAGE DATA ###\n");
	StPtpPrintTimeStamp("receiveTimestamp", &ptpHdr->receiveTimestamp);
	StPtpPrintPortIdentity("requestingPortIdentity", (port_id_t *)&ptpHdr->requestingPortIdentity);
	PTP_LOG(INFO, ST_PTP5, "\n");
	PTP_LOG(INFO, ST_PTP5, "\033[39;0m\n");
}
#else
#define StPtpPrintClockIdentity(...)
#define StPtpPrintPortIdentity(...)
#define StPtpPrintHeader(...)
#define StPtpPrintTimeStamp(...)
#define StPtpPrintClockQuality(...)
#define StPtpPrintAnnounceMsg(...)
#define StPtpPrintSyncDelayReqMsg(...)
#define StPtpPrintSyncMsg(...)
#define StPtpPrintDelayReqMsg(...)
#define StPtpPrintFollowUpMsg(...)
#define StPtpPrintDelayResMsg(...)
#endif

static inline struct timespec
ns_to_timespec(const uint64_t t)
{
	struct timespec ts;
	ts.tv_nsec = t % NS_PER_S;
	ts.tv_sec = t / NS_PER_S;
	return ts;
}

static inline uint64_t
timespec64_to_ns(const struct timespec *ts)
{
	return ((uint64_t)ts->tv_sec * NS_PER_S) + ts->tv_nsec;
}

static inline uint64_t
net_tstamp_to_ns(const ptp_tmstamp_t *ts)
{
	uint64_t sec = (uint64_t)ntohl(ts->sec_lsb) + ((uint64_t)ntohs(ts->sec_msb) << 32);
	return (sec * NS_PER_S) + ntohl(ts->ns);
}

static uint64_t
StPtpTimeFromEth(void)
{
	struct timespec spec;
	uint16_t portId = ST_PPORT;

	StPtpLock(portId);
	rte_eth_timesync_read_time(ptpState[portId].portId, &spec);
	StPtpUnlock(portId);
	return spec.tv_sec * GIGA + spec.tv_nsec;
}

static void
StPtpPrepClockIdentityFromMac(clock_id_t *clockId, struct rte_ether_addr *mac)
{
	uint16_t ptpMagicClockId = ST_PTP_CLOCK_IDENTITY_MAGIC;
	memcpy(&clockId->id[0], &mac->addr_bytes[0], 3);
	memcpy(&clockId->id[3], &ptpMagicClockId, 2);
	memcpy(&clockId->id[5], &mac->addr_bytes[3], 3);
}

static void
StPtpPrepMacFromClockIdentity(struct rte_ether_addr *mac, clock_id_t *clockId)
{
	memcpy(&mac->addr_bytes[0], &clockId->id[0], 3);
	memcpy(&mac->addr_bytes[3], &clockId->id[5], 3);
}

static int
StPtpIsInitializedAndIsOurMaster(ptp_header_t *ptpHdr, uint16_t portId)
{
	if (ptpState[portId].state != PTP_INITIALIZED)
	{
		PTP_LOG(WARNING, ST_PTP, "PTP slave not initialized yet\n");
		return -1;
	}
	if (StPtpComparePortIdentities(&ptpHdr->sourcePortIdentity,
								   &ptpState[portId].masterPortIdentity)
		!= 0)
	{
		StPtpPrintPortIdentity("notKnownMaster", &ptpHdr->sourcePortIdentity);
		return -1;
	}
	return 0;
}

static void
StPtpParseSyncMsg(struct rte_mbuf *m, ptp_sync_msg_t *ptpHdr, uint16_t portId, uint16_t rxTmstampIdx)
{
	if (StPtpIsInitializedAndIsOurMaster(&ptpHdr->hdr, portId) != 0)
		return;

	StPtpPrintSyncMsg(ptpHdr);
	struct timespec timestamp;
	int ret;
	StPtpLock(portId);
	ret = rte_eth_timesync_read_rx_timestamp(ptpState[portId].portId, &timestamp, rxTmstampIdx);
	if (ret < 0)
		rte_eth_timesync_read_time(ptpState[portId].portId, &timestamp);
	StPtpUnlock(portId);
	ptpState[portId].t2 = timespec64_to_ns(&timestamp);
	ptpState[portId].syncSeqId = ptpHdr->hdr.sequenceId;
	ptpState[portId].howSyncInAnnouce++;
}

static const ptp_delay_req_msg_t delayReqMsgPat = {
	{ .messageType = DELAY_REQ,
	  .transportSpecific = 0,
	  .versionPTP = 2,
	  .reserved0 = 0,
	  //.messageLength = htobe16(sizeof(ptp_delay_req_msg_t)),
	  .reserved1 = 0,
	  .flagField = 0,
	  .correctionField = 0,
	  .reserved2 = 0,
	  .controlField = 0,
	  .logMessageInterval = 0 },
	.originTimestamp = { 0 },
};

static void
StPtpParseFollowUpMsg(ptp_follow_up_msg_t *ptpHdr, uint16_t portId)
{

	ptp_delay_req_msg_t *ptpMsg;
	ptp_ipv4_udp_t *ipv4Hdr = NULL;	 //must be becouse gcc report that it can be not initialized
	StPtpPrintFollowUpMsg(ptpHdr);

	if (StPtpIsInitializedAndIsOurMaster(&ptpHdr->hdr, portId) != 0)
	{
		PTP_LOG(INFO, ST_PTP5, "FOLLOW_UP - not our master\n");
		return;
	}
	if (ptpHdr->hdr.sequenceId != ptpState[portId].syncSeqId)
	{
		PTP_LOG(INFO, ST_PTP5, "FOLLOW_UP sequenceId different than SYNC\n");
		return;
	}
	struct rte_mbuf *reqPkt = rte_pktmbuf_alloc(ptpState[portId].mbuf);
	if (reqPkt == NULL)
	{
		PTP_LOG(WARNING, ST_PTP, "FOLLOW_UP rte_pktmbuf_alloc fail\n");
		return;
	}
	//copy t1 time
	ptpState[portId].t1 = net_tstamp_to_ns(&ptpHdr->preciseOriginTimestamp);

	struct rte_ether_hdr *ethHdr = rte_pktmbuf_mtod(reqPkt, struct rte_ether_hdr *);

	reqPkt->ol_flags = 0;

	if (ptpState[portId].vlanRx)
	{
		reqPkt->pkt_len = sizeof(struct rte_ether_hdr) + sizeof(ptp_delay_req_msg_t)
						  + sizeof(struct rte_vlan_hdr);
		ethHdr->ether_type = htons(RTE_ETHER_TYPE_VLAN);
		struct rte_vlan_hdr *vlan_header = (struct rte_vlan_hdr *)((char *)&ethHdr->ether_type + 2);
		vlan_header->vlan_tci = htons(ptpState[portId].vlanTci);
		if (ptpState[portId].ptpLMode == ST_PTP_L4_MODE)
		{
			vlan_header->eth_proto = htons(RTE_ETHER_TYPE_IPV4);
			ptpMsg
				= rte_pktmbuf_mtod_offset(reqPkt, ptp_delay_req_msg_t *,
										  sizeof(struct rte_ether_hdr) + sizeof(struct rte_vlan_hdr)
											  + sizeof(ptp_ipv4_udp_t));
			ipv4Hdr = rte_pktmbuf_mtod_offset(reqPkt, ptp_ipv4_udp_t *,
											  sizeof(struct rte_ether_hdr)
												  + sizeof(struct rte_vlan_hdr));
			reqPkt->pkt_len += sizeof(ptp_ipv4_udp_t);
		}
		else
		{
			vlan_header->eth_proto = htons(RTE_ETHER_TYPE_1588);
			ptpMsg = rte_pktmbuf_mtod_offset(reqPkt, ptp_delay_req_msg_t *,
											 sizeof(struct rte_ether_hdr)
												 + sizeof(struct rte_vlan_hdr));
		}
	}
	else
	{
		reqPkt->pkt_len = sizeof(struct rte_ether_hdr) + sizeof(ptp_delay_req_msg_t);

		reqPkt->ol_flags |= PKT_TX_IEEE1588_TMST;
		if (ptpState[portId].vlanTci != 0)
		{
			reqPkt->ol_flags |= PKT_TX_VLAN;
			reqPkt->vlan_tci = ptpState[portId].vlanTci;
		}
		if (ptpState[portId].ptpLMode == ST_PTP_L4_MODE)
		{
			ethHdr->ether_type = htons(RTE_ETHER_TYPE_IPV4);
			ptpMsg = rte_pktmbuf_mtod_offset(reqPkt, ptp_delay_req_msg_t *,
											 sizeof(struct rte_ether_hdr) + sizeof(ptp_ipv4_udp_t));
			ipv4Hdr
				= rte_pktmbuf_mtod_offset(reqPkt, ptp_ipv4_udp_t *, sizeof(struct rte_ether_hdr));
			reqPkt->pkt_len += sizeof(ptp_ipv4_udp_t);
		}
		else
		{
			ethHdr->ether_type = htons(RTE_ETHER_TYPE_1588);
			ptpMsg = rte_pktmbuf_mtod_offset(reqPkt, ptp_delay_req_msg_t *,
											 sizeof(struct rte_ether_hdr));
		}
	}
	rte_eth_macaddr_get(portId, &ethHdr->s_addr);
	if (ptpState[portId].addrMode == ST_PTP_MULTICAST_ADDR)
	{
		if (ptpState[portId].ptpLMode == ST_PTP_L4_MODE)
		{
			static struct rte_ether_addr const multicast
				= { { 0x01, 0x00, 0x5e, 0x00, 0x01, 0x81 } };
			rte_ether_addr_copy(&multicast, &ethHdr->d_addr);
		}
		else
		{
			static struct rte_ether_addr const multicast = { { 0x01, 0x1b, 0x19, 0x0, 0x0, 0x0 } };
			rte_ether_addr_copy(&multicast, &ethHdr->d_addr);
		}
	}
	else
	{
		static struct rte_ether_addr addr;
		StPtpPrepMacFromClockIdentity(&addr, &ptpState[portId].masterPortIdentity.clockIdentity);
		rte_ether_addr_copy(&addr, &ethHdr->d_addr);
	}

	if (ptpState[portId].ptpLMode == ST_PTP_L4_MODE)
	{
		*ipv4Hdr = ptpState[portId].dstIPv4;
		ipv4Hdr->udp.dst_port = ipv4Hdr->udp.src_port = htons(ST_PTP_UDP_EVENT_PORT);
		ipv4Hdr->ip.time_to_live = 255;
		reqPkt->ol_flags |= PKT_TX_IPV4 | PKT_TX_IP_CKSUM;
		reqPkt->l2_len = 14;
		reqPkt->l3_len = 20;
	}

	reqPkt->data_len = reqPkt->pkt_len;

	memcpy((void *)ptpMsg, (void *)&delayReqMsgPat, sizeof(ptp_delay_req_msg_t));
	ptpMsg->hdr.messageLength = htons(sizeof(ptp_delay_req_msg_t));
	ptpMsg->hdr.domainNumber = ptpHdr->hdr.domainNumber;
	ptpMsg->hdr.sourcePortIdentity = ptpState[portId].ourPortIdentity;
	ptpState[portId].delayReqId++;
	ptpMsg->hdr.sequenceId = ptpState[portId].delayReqId;

	StPtpPrintDelayReqMsg(ptpMsg);
	ptpState[portId].delReqPkt = reqPkt;
	pthread_mutex_unlock(&ptpState[portId].isDo);
}

static void
StPtpParseDelayResMsg(ptp_delay_resp_msg_t *ptpHdr, uint16_t portId)
{
	if (ptpState[portId].state != PTP_INITIALIZED)
	{
		PTP_LOG(WARNING, ST_PTP4, "PTP slave not initialized yet\n");
		return;
	}
	StPtpPrintDelayResMsg(ptpHdr);
	ptpState[portId].howDelayResInAnnouce++;
	int ret = StPtpComparePortIdentities(&ptpHdr->requestingPortIdentity,
										 &ptpState[portId].ourPortIdentity);
	PTP_LOG(WARNING, ST_PTP4, "StPtpParseDelayResMsg ret= %d\n", ret);
	if (ret != 0)
	{
		if (ret > 0)
			ptpState[portId].howHigherPortIdentity++;
		PTP_LOG(WARNING, ST_PTP6,
				"StPtpParseDelayResMsg ptpState[portId].howHigherPortIdentity %d\n",
				ptpState[portId].howHigherPortIdentity);
		PTP_LOG(WARNING, ST_PTP6, "\n\nDELAY_RESP not for us START\n");
		StPtpPrintPortIdentity("requestingPortIdentity", &ptpHdr->requestingPortIdentity);
		StPtpPrintPortIdentity("ourPortIdentity", &ptpState[portId].ourPortIdentity);
		PTP_LOG(WARNING, ST_PTP6, "\nDELAY_RESP not for us END\n\n");
		return;
	}
	if (ptpState[portId].delayReqId != ptpHdr->hdr.sequenceId)
	{
		PTP_LOG(WARNING, ST_PTP6, "DELAY_RESP not DELAY_REQ id\n");
		return;
	}
	ptpState[portId].howDelayResOurInAnnouce++;

	ptpState[portId].t4 = net_tstamp_to_ns(&ptpHdr->receiveTimestamp);
	StPtpLock(portId);
	int64_t delta = (((int64_t)ptpState[portId].t4 - (int64_t)ptpState[portId].t3)
					 - ((int64_t)ptpState[portId].t2 - (int64_t)ptpState[portId].t1))
					/ 2;

	PTP_LOG(WARNING, ST_PTP6, "t1=%ld t2=%ld t3=%ld t4=%ld\n", ptpState[portId].t1,
			ptpState[portId].t2, ptpState[portId].t3, ptpState[portId].t4);

	/* useful info for debug */
	PTP_LOG(WARNING, ST_PTP6, "%s: t2-t1=%ld t4-t3=%ld\n",
			ptpState[portId].vlanRx ? "VLAN" : "VLAN strip",
			ptpState[portId].t2 - ptpState[portId].t1, ptpState[portId].t4 - ptpState[portId].t3);
	PTP_LOG(WARNING, ST_PTP6, "t4-t1=%ld t3-t2=%ld\n", ptpState[portId].t4 - ptpState[portId].t1,
			ptpState[portId].t3 - ptpState[portId].t2);

	if (rte_eth_timesync_adjust_time(portId, delta) == 0)
	{
		PTP_LOG(WARNING, ST_PTP4, "delta: %ld\n", delta);
	}
	else
	{
		PTP_LOG(WARNING, ST_PTP, "rte_eth_timesync_adjust_time fail\n");
	}
	StPtpUnlock(portId);

	//here calculation only
	if (rte_atomic32_read(&ptpSumStat[portId].isClr) != 0)
	{
		ptpSumStat[portId].lastPartAvgAbsVal = 0;
		ptpSumStat[portId].cntToSum = 0;
		ptpSumStat[portId].lastMaxAbsOff = ptpSumStat[portId].lastMinAbsOff = abs(delta);
		rte_atomic32_clear(&ptpSumStat[portId].isClr);
	}
	int64_t absDelta = abs(delta);
	ptpSumStat[portId].lastPartAvgAbsVal += absDelta;
	ptpSumStat[portId].cntToSum++;
	ptpSumStat[portId].lastAvgAbsVal
		= (ptpSumStat[portId].lastPartAvgAbsVal + ptpSumStat[portId].cntToSum / 2 + 1)
		  / ptpSumStat[portId].cntToSum;
	ptpSumStat[portId].lastMaxAbsOff
		= absDelta > ptpSumStat[portId].lastMaxAbsOff ? absDelta : ptpSumStat[portId].lastMaxAbsOff;
	ptpSumStat[portId].lastMinAbsOff
		= absDelta < ptpSumStat[portId].lastMinAbsOff ? absDelta : ptpSumStat[portId].lastMinAbsOff;
	rte_atomic32_set(&ptpSumStat[portId].isWorks, 1);
}

static st_status_t
StPtpGetPortClockIdentity(uint16_t portId, clock_id_t *clockId)
{
	int ret;
	struct rte_ether_addr mac;
	ret = rte_eth_macaddr_get(portId, &mac);
	if (ret != 0)
		return ST_GENERAL_ERR;
	StPtpPrepClockIdentityFromMac(clockId, &mac);
	return ST_OK;
}

static void
StPtpParseAnnouceMsg(struct rte_mbuf *m, ptp_announce_msg_t *ptpAnMsg, uint16_t portId)
{
	//first prepare actual clock identity
	port_id_t ourPortIdentity;
	ourPortIdentity.portNumber = htons(1);	//now allways
	StPtpPrintAnnounceMsg(ptpAnMsg);
	if (StPtpGetPortClockIdentity(portId, &ourPortIdentity.clockIdentity) != 0)
	{
		ptpState[portId].state = PTP_NOT_INITIALIZED;
		return;
	}
	if (ptpState[portId].state == PTP_NOT_INITIALIZED)
	{
		//copy our clock id and maybe master id
		ptpState[portId].ourPortIdentity = ourPortIdentity;
		if (ptpState[portId].masterChooseMode == ST_PTP_FIRST_KNOWN_MASTER)
			ptpState[portId].masterPortIdentity = ptpAnMsg->hdr.sourcePortIdentity;
		else
			return;	 //difrent not supported
		ptpState[portId].state = PTP_INITIALIZED;
		StPtpPrintPortIdentity("masterPortIdentity", &ptpState[portId].masterPortIdentity);
		StPtpPrintPortIdentity("ourPortIdentity", &ptpState[portId].ourPortIdentity);
	}
	else if (ptpState[portId].state == PTP_INITIALIZED)
	{
		PTP_LOG(WARNING, ST_PTP4, "Before pauseToSendDelayReq %d\n",
				ptpState[portId].pauseToSendDelayReq);
		if (ptpState[portId].howSyncInAnnouce != 0)
		{
			ptpState[portId].howDifDelayReqDelayRes
				+= ptpState[portId].howSyncInAnnouce - ptpState[portId].howDelayResOurInAnnouce;
			int howDelPer;
			if (ptpState[portId].howDelayResInAnnouce == ptpState[portId].howDelayResOurInAnnouce)
				howDelPer = rand() % 10;
			else
				howDelPer
					= ptpState[portId].howHigherPortIdentity / ptpState[portId].howSyncInAnnouce;
			ptpState[portId].pauseToSendDelayReq
				= PAUSE_TO_SEND_FIRST_DELAY_REQ + howDelPer * ORDER_WAIT_TIME;
		}
		else
			ptpState[portId].pauseToSendDelayReq = PAUSE_TO_SEND_FIRST_DELAY_REQ;

		PTP_LOG(WARNING, ST_PTP4, "howSyncInAnnouce %d\n", ptpState[portId].howSyncInAnnouce);
		PTP_LOG(WARNING, ST_PTP4, "howHigherPortIdentity %d\n",
				ptpState[portId].howHigherPortIdentity);
		PTP_LOG(WARNING, ST_PTP4, "howDifDelayReqDelayRes %d\n",
				ptpState[portId].howDifDelayReqDelayRes);
		PTP_LOG(WARNING, ST_PTP4, "howDelayResInAnnouce %d\n",
				ptpState[portId].howDelayResInAnnouce);
		PTP_LOG(WARNING, ST_PTP4, "howDelayResOurInAnnouce %d\n",
				ptpState[portId].howDelayResOurInAnnouce);
		PTP_LOG(WARNING, ST_PTP4, "After pauseToSendDelayReq %d\n\n",
				ptpState[portId].pauseToSendDelayReq);
	}
	//all other now ignore
	ptpState[portId].howSyncInAnnouce = 0;
	ptpState[portId].howDelayResInAnnouce = 0;
	ptpState[portId].howDelayResOurInAnnouce = 0;
	ptpState[portId].howHigherPortIdentity = 0;

	if (ptpState[portId].ptpLMode == ST_PTP_L4_MODE)  //get/set our IP and configure dst
	{
		ptpState[portId].dstIPv4 = ptpState[portId].tdstIPv4;
		//ptpState[portId].dst.src_addr
		//TODO: Add validation of IP and how to prepare
		memcpy(&ptpState[portId].dstIPv4.ip.src_addr, stMainParams.sipAddr[portId], IP_ADDR_LEN);
		ptpState[portId].dstIPv4.ip.total_length
			= htons(sizeof(ptp_ipv4_udp_t) + sizeof(ptp_delay_req_msg_t));
		ptpState[portId].dstIPv4.ip.hdr_checksum = 0;
		ptpState[portId].dstIPv4.udp.dgram_len
			= htons(sizeof(struct rte_udp_hdr) + sizeof(ptp_delay_req_msg_t));
		//ptpState[portId].dstIPv4.ip.hdr_checksum = rte_ipv4_cksum((const struct rte_ipv4_hdr *)&ptpState[portId].dstIPv4.ip);
	}

	if (m->ol_flags & PKT_RX_VLAN)
	{
		ptpState[portId].vlanTci = m->vlan_tci;
	}
	else
	{
		ptpState[portId].vlanTci = 0;
	}
}

static st_status_t
StPtpParsePtp(struct rte_mbuf *m, uint16_t portId, uint16_t rxTmstampIdx, st_ptp_l_mode mode,
			  uint16_t vlan)
{
	ptp_header_t *ptpHdr;
	ptpState[portId].ptpLMode = ST_PTP_L2_MODE;
	//test is L4 or L2
	if (mode == ST_PTP_L4_MODE)
	{
		ptp_ipv4_udp_t *ipv4Hdr;
		if (vlan)
		{
			ptpHdr
				= rte_pktmbuf_mtod_offset(m, ptp_header_t *,
										  sizeof(struct rte_ether_hdr) + sizeof(struct rte_vlan_hdr)
											  + sizeof(ptp_ipv4_udp_t));
			ipv4Hdr = rte_pktmbuf_mtod_offset(
				m, ptp_ipv4_udp_t *, sizeof(struct rte_ether_hdr) + sizeof(struct rte_vlan_hdr));
		}
		else
		{
			ptpHdr = rte_pktmbuf_mtod_offset(m, ptp_header_t *,
											 sizeof(struct rte_ether_hdr) + sizeof(ptp_ipv4_udp_t));
			ipv4Hdr = rte_pktmbuf_mtod_offset(m, ptp_ipv4_udp_t *, sizeof(struct rte_ether_hdr));
		}
		//first test is UDP and port is 320 or 319
		if ((ntohs(ipv4Hdr->udp.src_port) == ST_PTP_UDP_EVENT_PORT)
			|| (ntohs(ipv4Hdr->udp.dst_port) == ST_PTP_UDP_EVENT_PORT)
			|| (ntohs(ipv4Hdr->udp.src_port) == ST_PTP_UDP_GEN_PORT)
			|| (ntohs(ipv4Hdr->udp.dst_port) == ST_PTP_UDP_GEN_PORT))
		{  //this is UDP ptp message, we need support it
			StPtpPrintHeader(ptpHdr);
			//prepare IPV4 header for send
			ptpState[portId].tdstIPv4 = *ipv4Hdr;  //to remove?
			ptpState[portId].ptpLMode = ST_PTP_L4_MODE;
		}
		else
			return ST_GENERAL_ERR;
	}
	else if (vlan)	//L2
	{
		ptpHdr = rte_pktmbuf_mtod_offset(
			m, ptp_header_t *, sizeof(struct rte_ether_hdr) + sizeof(struct rte_vlan_hdr));
	}
	else
	{
		ptpHdr = rte_pktmbuf_mtod_offset(m, ptp_header_t *, sizeof(struct rte_ether_hdr));
	}

	ptpState[portId].vlanRx = vlan;

	switch (ptpHdr->messageType)
	{
	case SYNC:
		StPtpParseSyncMsg(m, (ptp_sync_msg_t *)ptpHdr, portId, rxTmstampIdx);
		break;
	case FOLLOW_UP:
		StPtpParseFollowUpMsg((ptp_follow_up_msg_t *)ptpHdr, portId);
		break;
	case DELAY_RESP:
		StPtpParseDelayResMsg((ptp_delay_resp_msg_t *)ptpHdr, portId);
		break;
	case ANNOUNCE:
		StPtpParseAnnouceMsg(m, (ptp_announce_msg_t *)ptpHdr, portId);
		break;
	case DELAY_REQ:
		StPtpPrintDelayReqMsg((ptp_delay_req_msg_t *)ptpHdr);
		break;
	case PDELAY_REQ:
		break;
	default:
		PTP_LOG(INFO, ST_PTP4, "Unknown %02x PTP4L frame type\n", ptpHdr->messageType);
		break;
	}
	//bellow code must be, it clear PTP timestamp registers,
	//TODO - i t should be chnaged to read before all parse ptp messages
	struct timespec timestamp;
	rte_eth_timesync_read_rx_timestamp(portId, &timestamp, rxTmstampIdx);
	return ST_OK;
}

st_status_t
StParseEthernet(uint16_t portId, struct rte_mbuf *m)
{
	struct rte_ether_hdr *const ethHdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
	uint16_t ether_type;
	struct rte_vlan_hdr *vlan_header;
	int vlan;

	ether_type = ntohs(ethHdr->ether_type);
	vlan = 0;
	if (ether_type == RTE_ETHER_TYPE_VLAN)
	{
		/* TODO: will consider Vlan QinQ later */
		vlan_header = (struct rte_vlan_hdr *)((void *)&ethHdr->ether_type + 2);
		ptpState[portId].vlanTci = ntohs(vlan_header->vlan_tci);
		ether_type = ntohs(vlan_header->eth_proto);
		vlan = 1;
	}
	switch (ether_type)
	{
	case RTE_ETHER_TYPE_1588:  //L2
		ptpState[portId].vlanRx = vlan;
		StPtpParsePtp(m, portId, m->timesync, ST_PTP_L2_MODE, vlan);
		break;
	case RTE_ETHER_TYPE_ARP:
		ParseArp(rte_pktmbuf_mtod_offset(m, struct rte_arp_hdr *, sizeof(struct rte_ether_hdr)),
				 portId);
		break;
	case RTE_ETHER_TYPE_IPV4:
		if (StPtpParsePtp(m, portId, m->timesync, ST_PTP_L4_MODE, vlan) == ST_OK)
			return ST_OK;
		ParseIp(rte_pktmbuf_mtod_offset(m, ipv4Hdr_t *, sizeof(struct rte_ether_hdr)), m, portId);
		break;
	default:
		//PTP_LOG(INFO, ST_PTP4, "Unknow %04x frame type\n", ntohs(ethHdr->ether_type));
		return ST_NOT_SUPPORTED;
	}
	return ST_OK;
}

uint64_t (*StPtpGetTime)(void) = StPtpTimeFromEth;	//TimeFromRtc;


st_status_t
StPtpGetClockSource(st_ptp_clock_id_t *currClock)
{
	if (!currClock)
	{
		return ST_INVALID_PARAM;
	}
	//	memcpy(currClock, followUp.hdr.source_port_id.clock_id.id,
	//		   sizeof(followUp.hdr.source_port_id.clock_id.id));
	return ST_OK;
}

st_status_t
StPtpSetClockSource(st_ptp_clock_id_t const *priClock, st_ptp_clock_id_t const *bkpClock)
{
	if (!priClock)
	{
		return ST_INVALID_PARAM;
	}
	//	memcpy(&primaryClock.addr, priClock, sizeof(primaryClock));
	//	PtpGetSourceClock = PtpGetPrimaryPTP;
	//new version here
	return ST_OK;
}

static void *
StPtpDelayReqThread(void *arg)
{
	uint16_t portId = (uint64_t)arg;
	int wait_us = 0;
	while (rte_atomic32_read(&ptpState[portId].isStop) == 0)
	{
		struct timespec ts;
		pthread_mutex_lock(&ptpState[portId].isDo);
		if (rte_atomic32_read(&ptpState[portId].isStop) > 0)
		{
			if (ptpState[portId].delReqPkt != NULL)
			    rte_pktmbuf_free((struct rte_mbuf *)ptpState[portId].delReqPkt);
			break;
		}
		rte_delay_us_sleep(ptpState[portId].pauseToSendDelayReq);
		rte_eth_timesync_read_tx_timestamp(ptpState[portId].portId, &ts);
		StPtpLock(portId);
		if (rte_eth_tx_burst(ptpState[portId].portId, ptpState[portId].txRingId,
							 (struct rte_mbuf **)&ptpState[portId].delReqPkt, 1)
			== 0)
		//if (rte_ring_sp_enqueue(ptpState[portId].txRing, (void *)ptpState[portId].delReqPkt))
		// not remove under line - I'll be it test yet
		{
			rte_pktmbuf_free((struct rte_mbuf *)ptpState[portId].delReqPkt);
			PTP_LOG(WARNING, ST_PTP4, "delReqPkt didn't send\n");
			StPtpUnlock(portId);
			continue;
		}

		ptpState[portId].howDelayReqSent++;
		wait_us = 0;
		ts.tv_nsec = 0;
		ts.tv_sec = 0;

		/* Wait at least 50 us to read TX timestamp. */
		while ((rte_eth_timesync_read_tx_timestamp(ptpState[portId].portId, &ts) < 0) && (wait_us < 50))
		{
			rte_delay_us(1);
			wait_us++;
		}
		if (wait_us == 50)
		{
			rte_eth_timesync_read_time(ptpState[portId].portId, &ts);
		}
		ptpState[portId].t3 = timespec64_to_ns(&ts);  //temp soft
		StPtpUnlock(portId);

	}
	return NULL;
}

static st_addr_t addr;

void
StJoinPtpMulticastGroup_callback(void *arg)
{
	if (rte_atomic32_read(&isStopMainThreadTasks) == 1)
	{
		return;
	}

	StJoinMulticastGroup(&addr);

	if (0
		!= rte_eal_alarm_set(IGMP_JOIN_PTP_GROUP_PERIOD * 1000000ll,
							 StJoinPtpMulticastGroup_callback, NULL))
	{
		/* retry once more if nto exit */
		if (0
			!= rte_eal_alarm_set(IGMP_JOIN_PTP_GROUP_PERIOD * 1000000ll,
								 StJoinPtpMulticastGroup_callback, NULL))
			rte_exit(EXIT_FAILURE, "Failed to join PTP multicast group!\n");
	}
}

st_status_t
StPtpInit(uint16_t portId, struct rte_mempool *mbuf, uint16_t txRingId, struct rte_ring *txRing)
{
	int ret;
	ret = rte_eth_timesync_enable(portId);
	if (ret != 0)
	{
		PTP_LOG(ERR, ST_PTP, "Cannot clock on port %d\n", portId);
		return ST_PTP_GENERAL_ERR;
	}
	ptpState[portId].portId = portId;
	ptpState[portId].txRingId = txRingId;
	ptpState[portId].txRing = txRing;
	ptpState[portId].mbuf = mbuf;
	ptpState[portId].delReqPkt = NULL;
	rte_atomic32_init(&ptpState[portId].isStop);
	pthread_mutex_init(&ptpState[portId].isDo, NULL);
	pthread_mutex_lock(&ptpState[portId].isDo);
	srand(time(NULL));
	ptpState[portId].pauseToSendDelayReq
		= PAUSE_TO_SEND_FIRST_DELAY_REQ + (rand() % 10) * ORDER_WAIT_TIME;
	PTP_LOG(WARNING, ST_PTP, "ptpState[portId].pauseToSendDelayReq: %d\n",
			ptpState[portId].pauseToSendDelayReq);
	ptpState[portId].state = PTP_NOT_INITIALIZED;

	inet_pton(AF_INET, "224.0.1.129", &addr.dst.addr4.sin_addr);

	if (StPtpGetPortClockIdentity(ptpState[portId].portId,
								  &ptpState[portId].ourPortIdentity.clockIdentity)
		!= 0)
	{
		PTP_LOG(ERR, ST_PTP, "Cannot init portclockid PTP, not valid MAC?\n");
		return ST_PTP_GENERAL_ERR;
	}

	ret = pthread_create(&ptpState[portId].ptpDelayReqThread, NULL,
						 (void *(*)(void *))StPtpDelayReqThread, (void *)(uint64_t)portId);
	if (ret != 0)
	{
		PTP_LOG(ERR, ST_PTP, "Cannot init PTP - cannot create thread\n");
		return ST_PTP_GENERAL_ERR;
	}

	StJoinPtpMulticastGroup_callback(NULL);

	StPtpPrintStatus_callback((void *)((long)portId));
	return ST_OK;
}

st_status_t
StPtpDeInit(uint16_t portId)
{
	rte_atomic32_add(&ptpState[portId].isStop, 1);
	pthread_mutex_unlock(&ptpState[portId].isDo);
	pthread_join(ptpState[portId].ptpDelayReqThread, NULL);
	return ST_OK;
}

