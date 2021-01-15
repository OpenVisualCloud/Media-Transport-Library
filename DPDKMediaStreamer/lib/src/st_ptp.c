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

#include "st_ptp.h"

#include <rte_ethdev.h>

#include "rvrtp_main.h"
#include "st_igmp.h"

#include <st_api.h>
#include <sys/time.h>
#include <time.h>

#include <pthread.h>
#include <stdlib.h>

#define _BSD_SOURCE
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


#define PAUSE_TO_SEND_FIRST_DELAY_REQ (50) //us
#define ORDER_WAIT_TIME (50) //us
#define MIN_FREQ_MES_TIME (10000000000ll) //ns

#define ST_PTP_CLOCK_IDENTITY_MAGIC (0xfeff)

//#define DEBUG_PTP

#ifndef DEBUG_PTP
#undef RTE_LOG
#define RTE_LOG(...)
#endif


static st_ptp_t ptpState =
{
	.state = PTP_NOT_INITIALIZED,
	.masterChooseMode = ST_PTP_FIRST_KNOWN_MASTER,
};

static inline int
StPtpComparePortIdentities(const port_id_t *a, const port_id_t *b)
{
    return memcmp(a,b, sizeof(port_id_t));
}

	int addrMode;
	int stepMode;
	clock_id_t setClockId;

static inline void
StPtpLock()
{
	int lock;
	do
	{
		lock = __sync_lock_test_and_set(&ptpState.lock, 1);
	} while (lock != 0);
}

static inline void
StPtpUnlock()
{
	__sync_lock_release(&ptpState.lock, 0);
}

st_status_t StPtpGetParam(st_param_t prm,  st_param_val_t *val)
{
    switch (prm)
    {
	case ST_PTP_CLOCK_ID:
        memcpy(val->ptr, &ptpState.setClockId, sizeof(ptpState.setClockId));
        break;
	case ST_PTP_ADDR_MODE:
        val->valueU32 = ptpState.addrMode;
        break;
	case ST_PTP_STEP_MODE:
        val->valueU32 = ptpState.stepMode;
        break;
	case ST_PTP_CHOOSE_CLOCK_MODE:
        val->valueU32 = ptpState.masterChooseMode;
        break;
	default:
		RTE_LOG(INFO, USER1, "Parameter not supported by StPtpGetParam\n");
		break;
    }
    return ST_OK;
}

st_status_t StPtpSetParam(st_param_t prm,  st_param_val_t val)
{
    switch (prm)
    {
	case ST_PTP_CLOCK_ID:
        memcpy(&ptpState.setClockId, val.ptr, sizeof(ptpState.setClockId));
        break;
	case ST_PTP_ADDR_MODE:
        ptpState.addrMode = val.valueU32;
        break;
	case ST_PTP_STEP_MODE:
        ptpState.stepMode = val.valueU32;
        break;
	case ST_PTP_CHOOSE_CLOCK_MODE:
        ptpState.masterChooseMode = val.valueU32;
        break;
	default:
		RTE_LOG(INFO, USER1, "Parameter not supported by StPtpGetParam\n");
		break;
    }
    return ST_OK;
}

#ifdef DEBUG_PTP
static void StPtpPrintClockIdentity(char *fieldName, clock_id_t *clockIdentity)
{
    RTE_LOG(INFO, ST_PTP5, "%s.clockIdentity: %02x:%02x:%02x:%02x%02x:%02x:%02x:%02x\n", fieldName,
        clockIdentity->id[0],
        clockIdentity->id[1],
        clockIdentity->id[2],
        clockIdentity->id[3],
        clockIdentity->id[4],
        clockIdentity->id[5],
        clockIdentity->id[6],
        clockIdentity->id[7]);
}

static void StPtpPrintPortIdentity(char *fieldName, port_id_t *portIdentity)
{
    StPtpPrintClockIdentity(fieldName, &portIdentity->clockIdentity);
    RTE_LOG(INFO, ST_PTP5, "%s.portNumber: %04x\n", fieldName, htons(portIdentity->portNumber));
}

static void StPtpPrintHeader(ptp_header_t* ptpHdr)
{
    RTE_LOG(DEBUG, ST_PTP5, "### PTP HEADER ###\n");
    RTE_LOG(DEBUG, ST_PTP5, "messageType: 0x%01x\n", ptpHdr->messageType);
    RTE_LOG(DEBUG, ST_PTP5, "transportSpecific: 0x%01x\n", ptpHdr->transportSpecific);
    RTE_LOG(DEBUG, ST_PTP5, "versionPTP: 0x%01x\n", ptpHdr->versionPTP);
    RTE_LOG(DEBUG, ST_PTP5, "messageLength: %d\n", htons(ptpHdr->messageLength));
    RTE_LOG(DEBUG, ST_PTP5, "domainNumber: %d\n", ptpHdr->domainNumber);
    RTE_LOG(DEBUG, ST_PTP5, "flagField: 0x%04x\n", htons(ptpHdr->flagField));
    RTE_LOG(DEBUG, ST_PTP5, "correctionField: 0x%016lx\n", be64toh(ptpHdr->correctionField));
    StPtpPrintPortIdentity("sourcePortIdentity", (port_id_t *)&ptpHdr->sourcePortIdentity);
    RTE_LOG(DEBUG, ST_PTP5, "sequenceId: 0x%02x\n", htons(ptpHdr->sequenceId));
    RTE_LOG(DEBUG, ST_PTP5, "controlField: 0x%02x\n", ptpHdr->controlField);
    RTE_LOG(DEBUG, ST_PTP5, "logMessageInterval: 0x%02x\n", ptpHdr->logMessageInterval);
}

static void StPtpPrintTimeStamp(char *fieldName, ptp_tmstamp_t *ts)
{
    uint64_t sec = (uint64_t)ntohl(ts->sec_lsb) + ((uint64_t)ntohs(ts->sec_msb) << 32);
    RTE_LOG(DEBUG, ST_PTP5, "%s: %ld:%d\n", fieldName, sec, ntohl(ts->ns));
}

static void StPtpPrintClockQuality(char *fieldName, clock_quality_t *cs)
{
    RTE_LOG(DEBUG, ST_PTP5, "%s.clockClass: %d\n", fieldName,cs->clockClass);
    RTE_LOG(DEBUG, ST_PTP5, "%s.clockAccuracy: %d\n", fieldName, cs->clockAccuracy);
    RTE_LOG(DEBUG, ST_PTP5, "%s.offsetScaledLogVariance: %d\n", fieldName, ntohs(cs->offsetScaledLogVariance));
}

static void StPtpPrintAnnounceMsg(ptp_announce_msg_t* ptpHdr)
{
    RTE_LOG(DEBUG, ST_PTP5, "\033[31;1m\n");
    RTE_LOG(DEBUG, ST_PTP5, "\n##### PTP ANNOUNCE MESSAGE #####\n");
    StPtpPrintHeader((ptp_header_t*)ptpHdr);
    RTE_LOG(DEBUG, ST_PTP5, "### PTP ANNOUNCE MESSAGE DATA ###\n");
    StPtpPrintTimeStamp("originTimestamp", &ptpHdr->originTimestamp);
    RTE_LOG(DEBUG, ST_PTP5, "currentUtcOffset: %d\n", ntohs(ptpHdr->currentUtcOffset));
    RTE_LOG(DEBUG, ST_PTP5, "grandmasterPriority1: %d\n", ptpHdr->grandmasterPriority1);
    StPtpPrintClockQuality("grandmasterClockQuality", &ptpHdr->grandmasterClockQuality);
    RTE_LOG(DEBUG, ST_PTP5, "grandmasterPriority2: %d\n", ptpHdr->grandmasterPriority2);
    StPtpPrintClockIdentity("grandmasterIdentity",&ptpHdr->grandmasterIdentity);
    RTE_LOG(DEBUG, ST_PTP5, "stepsRemoved: %d\n", ntohs(ptpHdr->stepsRemoved));
    RTE_LOG(DEBUG, ST_PTP5, "timeSource: %d\n", ptpHdr->timeSource);
    RTE_LOG(DEBUG, ST_PTP5, "\033[39;0m\n");
}

static void
StPtpPrintSyncDelayReqMsg(ptp_sync_msg_t* ptpHdr, char* msgName, uint32_t color)
{
    RTE_LOG(DEBUG, ST_PTP5, "\033[%x;1m\n",color);
    RTE_LOG(DEBUG, ST_PTP5, "\n##### PTP %s MESSAGE #####\n",msgName);
    StPtpPrintHeader((ptp_header_t*)ptpHdr);
    RTE_LOG(DEBUG, ST_PTP5, "### PTP %s MESSAGE DATA ###\n", msgName);
    StPtpPrintTimeStamp("originTimestamp", &ptpHdr->originTimestamp);
    RTE_LOG(DEBUG, ST_PTP5, "\n");
    RTE_LOG(DEBUG, ST_PTP5, "\033[39;0m\n");
}

static inline void StPtpPrintSyncMsg(ptp_sync_msg_t* ptpHdr)
{
    StPtpPrintSyncDelayReqMsg(ptpHdr, "SYNC", 0x32);
}

static inline void StPtpPrintDelayReqMsg(ptp_delay_req_msg_t* ptpHdr)
{
    StPtpPrintSyncDelayReqMsg(ptpHdr, "DELAY_REQ", 0x36);
}

static void StPtpPrintFollowUpMsg(ptp_follow_up_msg_t* ptpHdr)
{
    RTE_LOG(DEBUG, ST_PTP5, "\033[33;1m\n");
    RTE_LOG(DEBUG, ST_PTP5, "\n##### PTP FOLLOW_UP MESSAGE #####\n");
    StPtpPrintHeader((ptp_header_t*)ptpHdr);
    RTE_LOG(DEBUG, ST_PTP5, "### PTP FOLLOW_UP MESSAGE DATA ###\n");
    StPtpPrintTimeStamp("preciseOriginTimestamp", &ptpHdr->preciseOriginTimestamp);
    RTE_LOG(DEBUG, ST_PTP5, "\n");
    RTE_LOG(DEBUG, ST_PTP5, "\033[39;0m\n");
}

static void StPtpPrintDellayResMsg(ptp_delay_resp_msg_t* ptpHdr)
{
    RTE_LOG(DEBUG, ST_PTP5, "\033[34;1m\n");
    RTE_LOG(DEBUG, ST_PTP5, "\n##### PTP DELAY_RESP MESSAGE #####\n");
    StPtpPrintHeader((ptp_header_t*)ptpHdr);
    RTE_LOG(DEBUG, ST_PTP5, "### PTP DELAY_RESP MESSAGE DATA ###\n");
    StPtpPrintTimeStamp("receiveTimestamp", &ptpHdr->receiveTimestamp);
    StPtpPrintPortIdentity("requestingPortIdentity", (port_id_t *)&ptpHdr->requestingPortIdentity);
    RTE_LOG(DEBUG, ST_PTP5, "\n");
    RTE_LOG(DEBUG, ST_PTP5, "\033[39;0m\n");
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
#define StPtpPrintDellayResMsg(...)
#endif

#if 0
static struct timeval
ns_to_timeval(int64_t nsec)
{
	struct timeval t = { nsec / NS_PER_S, (nsec % NS_PER_S) / 1000 };
	if (t.tv_usec < 0)
	{
		t.tv_usec += MS_PER_S;
		t.tv_sec--;
	}
	return t;
}
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

static long double hpetPeriod = 1e0l; // ns
static uint64_t epochRteAdj; // time beetwen 1970 and boot time and adjust
static uint64_t curHPetClk;
static inline void
StPtpCalcHpetPeriod(void)
{
	hpetPeriod = 1e9l / rte_get_timer_hz();
}

static inline uint64_t
StPtpTimeFromRteCalc(uint64_t cyc)
{
	long double adjust = hpetPeriod * cyc;
	return epochRteAdj + adjust;
}

static uint64_t
StPtpTimeFromRte()
{
	return StPtpTimeFromRteCalc(rte_get_timer_cycles());
}

#if 0
static uint64_t
StPtpTimeFromRtc()
{
	struct timespec time;
	if (clock_gettime(CLOCK_REALTIME, &time) == 0)
	{
		uint64_t ns = timespec64_to_ns(&time);
		ns += timezone * NS_PER_S;
		return ns;
	}
	RTE_LOG(WARNING, ST_PTP, "clock_gettime fail\n");
	return 0;
}
#endif

static uint64_t
StPtpTimeFromEth()
{
	struct timespec spec;

        StPtpLock();
	rte_eth_timesync_read_time(ptpState.portId, &spec);
        StPtpUnlock();
	return spec.tv_sec * GIGA + spec.tv_nsec;
}

static void
StPtpPrepClockIdentityFromMac(clock_id_t *clockId, struct rte_ether_addr *mac)
{
    uint16_t ptpMagicClockId = ST_PTP_CLOCK_IDENTITY_MAGIC;
    memcpy(&clockId->id[0], &mac->addr_bytes[0], 3);
    memcpy(&clockId->id[3], &ptpMagicClockId,2);
    memcpy(&clockId->id[5], &mac->addr_bytes[3], 3);
}

static void
StPtpPrepMacFromClockIdentity(struct rte_ether_addr *mac, clock_id_t *clockId)
{
    memcpy(&mac->addr_bytes[0], &clockId->id[0], 3);
    memcpy(&mac->addr_bytes[3], &clockId->id[5], 3);
}

static int
StPtpIsInitializedAndIsOurMaster(const ptp_header_t* ptpHdr)
{
    if (ptpState.state != PTP_INITIALIZED)
    {
        RTE_LOG(WARNING, ST_PTP,"PTP slave not initialized yet\n");
        return -1;
    }
    if (StPtpComparePortIdentities(&ptpHdr->sourcePortIdentity, &ptpState.masterPortIdentity ) != 0)
    {
        StPtpPrintPortIdentity("notKnownMaster",&ptpHdr->sourcePortIdentity);
        return -1;
    }
    return 0;
}

static void
StPtpParseSyncMsg(ptp_sync_msg_t const *ptpHdr, uint16_t portid, uint16_t rxTmstampIdx, uint64_t *tm)
{
    int isSoft = 0, ret;
    struct timespec timestamp;

    if (StPtpIsInitializedAndIsOurMaster(&ptpHdr->hdr) != 0) return;

    StPtpLock();
    ret = rte_eth_timesync_read_rx_timestamp(portid, &timestamp, rxTmstampIdx);
    StPtpUnlock();

    if (!ret)
    {
	/* TODO if timesync HW is supported, it should not be used */
        /*use software timestamp */
        timestamp = ns_to_timespec(*tm);
        isSoft = 1;
    }
    ptpState.ist2Soft = isSoft;
    ptpState.t2 = timespec64_to_ns(&timestamp);
    ptpState.t2HPet = StPtpTimeFromRteCalc(curHPetClk);
    ptpState.syncSeqId = ptpHdr->hdr.sequenceId;
    ptpState.howSyncInAnnouce++;

    if (ptpState.t1HPetFreqStart  == 0 )  ptpState.t1HPetFreqClk = curHPetClk;
    ptpState.t1HPetFreqClkNext = curHPetClk;
    RTE_LOG(DEBUG, ST_PTP4,"SYNC save time\n");
}

static const ptp_delay_req_msg_t delayReqMsgPat =
{
	{.messageType = DELAY_REQ,
	.transportSpecific = 0,
	.versionPTP = 2,
	.reserved0 = 0,
	//.messageLength = htobe16(sizeof(ptp_delay_req_msg_t)),
	.reserved1 = 0,
	.flagField = 0,
	.correctionField = 0,
	.reserved2 = 0,
	.controlField = 0,
	.logMessageInterval = 0},
	.originTimestamp = {0},
};

static void
StPtpParseFollowUpMsg(ptp_follow_up_msg_t const *ptpHdr, uint16_t portid)
{

    ptp_delay_req_msg_t *ptpMsg;

    if (StPtpIsInitializedAndIsOurMaster(&ptpHdr->hdr) != 0)
    {
        RTE_LOG(DEBUG, ST_PTP5,"FOLLOW_UP - not our master\n");
        return;
    }
    if (ptpHdr->hdr.sequenceId != ptpState.syncSeqId)
    {
        RTE_LOG(DEBUG, ST_PTP5, "FOLLOW_UP sequenceId different than SYNC\n");
        return;
    }
    struct rte_mbuf *reqPkt = rte_pktmbuf_alloc(ptpState.mbuf);
    if (reqPkt == NULL)
    {
        RTE_LOG(WARNING, ST_PTP, "FOLLOW_UP rte_pktmbuf_alloc fail\n");
        return;
    }
    //copy t1 time
    ptpState.t1 = net_tstamp_to_ns(&ptpHdr->preciseOriginTimestamp);
    if (ptpState.t1HPetFreqStart  == 0 )   ptpState.t1HPetFreqStart = ptpState.t1;

    struct rte_ether_hdr *ethHdr = rte_pktmbuf_mtod(reqPkt, struct rte_ether_hdr *);

    if (ptpState.vlanRx) {
        reqPkt->pkt_len = reqPkt->data_len = sizeof(struct rte_ether_hdr) 
		 + sizeof(ptp_delay_req_msg_t) + sizeof(struct rte_vlan_hdr);
	ethHdr->ether_type = htons(RTE_ETHER_TYPE_VLAN);
	struct rte_vlan_hdr* vlan_header = (struct rte_vlan_hdr*)((char *)&ethHdr->ether_type + 2);
	vlan_header->vlan_tci = htons(ptpState.vlanTci);
	vlan_header->eth_proto  = htons(PTP_PROTOCOL);
        ptpMsg = rte_pktmbuf_mtod_offset(reqPkt, ptp_delay_req_msg_t *,
                sizeof(struct rte_ether_hdr) + sizeof(struct rte_vlan_hdr));
    }
    else {
        reqPkt->pkt_len = reqPkt->data_len = sizeof(struct rte_ether_hdr) + sizeof(ptp_delay_req_msg_t);
        ethHdr->ether_type = htons(PTP_PROTOCOL);

        reqPkt->ol_flags |= PKT_TX_IEEE1588_TMST;
        if (ptpState.vlanTci != 0)
        {
            reqPkt->ol_flags |= PKT_TX_VLAN;
            reqPkt->vlan_tci = ptpState.vlanTci;
        }
        ptpMsg = rte_pktmbuf_mtod_offset(reqPkt, ptp_delay_req_msg_t *,
                sizeof(struct rte_ether_hdr));

    }


    rte_eth_macaddr_get(portid, &ethHdr->s_addr);
    if (ptpState.addrMode == ST_PTP_MULTICAST_ADDR)
    {
        static struct rte_ether_addr const multicast
            = { { 0x01, 0x1b, 0x19, 0x0, 0x0, 0x0 } };
        rte_ether_addr_copy(&multicast, &ethHdr->d_addr);
    }
    else
    {
        static struct rte_ether_addr addr;
        StPtpPrepMacFromClockIdentity(&addr, &ptpState.masterPortIdentity.clockIdentity);
        rte_ether_addr_copy(&addr, &ethHdr->d_addr);
    }

    memcpy((void*)ptpMsg, (void*)&delayReqMsgPat, sizeof(ptp_delay_req_msg_t));
    ptpMsg->hdr.messageLength = htons(sizeof(ptp_delay_req_msg_t));
    ptpMsg->hdr.domainNumber = ptpHdr->hdr.domainNumber;
    ptpMsg->hdr.sourcePortIdentity = ptpState.ourPortIdentity;
    ptpState.delayReqId++;
    ptpMsg->hdr.sequenceId = ptpState.delayReqId;
    ptpState.delReqPkt = reqPkt;
    pthread_mutex_unlock(&ptpState.isDo);
}

static void
StPtpParseDellayResMsg(ptp_delay_resp_msg_t const *ptpHdr, uint16_t portid)
{
    if (ptpState.state != PTP_INITIALIZED)
    {
        RTE_LOG(WARNING, ST_PTP4,"PTP slave not initialized yet\n");
        return;
    }
    ptpState.howDelayResInAnnouce++;
    int ret = StPtpComparePortIdentities(&ptpHdr->requestingPortIdentity, &ptpState.ourPortIdentity);
    RTE_LOG(WARNING, ST_PTP4,"StPtpParseDellayResMsg ret= %d\n", ret);
    if (ret != 0)
    {
        if (ret > 0) ptpState.howHigherPortIdentity++;
        RTE_LOG(WARNING, ST_PTP6,"StPtpParseDellayResMsg ptpState.howHigherPortIdentity %d\n",
            ptpState.howHigherPortIdentity);
        RTE_LOG(WARNING, ST_PTP6,"\n\nDELAY_RESP not for us START\n");
        StPtpPrintPortIdentity("requestingPortIdentity",&ptpHdr->requestingPortIdentity);
        StPtpPrintPortIdentity("ourPortIdentity",&ptpState.ourPortIdentity);
        RTE_LOG(WARNING, ST_PTP6,"\nDELAY_RESP not for us END\n\n");
        return;
    }
    if (ptpState.delayReqId != ptpHdr->hdr.sequenceId)
    {
        RTE_LOG(WARNING, ST_PTP6, "DELAY_RESP not DELAY_REQ id\n");
        return;
    }
    ptpState.howDelayResOurInAnnouce++;
    struct timespec tmt3;
#ifdef DEBUG_PTP
    uint64_t t3Soft = ptpState.t3;
#endif

    StPtpLock();
    ret = rte_eth_timesync_read_tx_timestamp(portid, &tmt3);
    StPtpUnlock();
    if (ret == 0)
    {
        ptpState.t3 = timespec64_to_ns(&tmt3);
        ptpState.ist3Soft = 0;
    }
    else
    {
        RTE_LOG(WARNING, ST_PTP4,"DELAY_RESP timestamp %ld:%ld ret=%d\n",
            tmt3, tmt3.tv_nsec, ret);
        ptpState.ist3Soft = 1;
    }
    RTE_LOG(WARNING, ST_PTP6, "DELAY_RESP ptpState.ist3Soft=%d\n", ptpState.ist3Soft);
    ptpState.t4 = net_tstamp_to_ns(&ptpHdr->receiveTimestamp);
    int64_t delta = (((int64_t)ptpState.t4 - (int64_t)ptpState.t3)
        - ((int64_t)ptpState.t2 - (int64_t)ptpState.t1))/2;
#ifdef DEBUG_PTP
    int64_t tp =  (((int64_t)ptpState.t4 - (int64_t)ptpState.t3)
            + ((int64_t)ptpState.t2 - (int64_t)ptpState.t1))/2;
#endif

    RTE_LOG(WARNING, ST_PTP6,"t1=%ld t2=%ld t3=%ld t4=%ld\n",
        ptpState.t1, ptpState.t2, ptpState.t3, ptpState.t4);
    RTE_LOG(WARNING, ST_PTP6,"t1=%ld t2HPet=%ld t3HPet=%ld t4=%ld\n",
        ptpState.t1, ptpState.t2HPet, ptpState.t3HPet, ptpState.t4);

     /* useful info for debug */
    RTE_LOG(WARNING, ST_PTP6,"%s: t2-t1=%ld t4-t3=%ld\n", ptpState.vlanRx?"VLAN":"VLAN strip", ptpState.t2 -ptpState.t1, ptpState.t4 - ptpState.t3);
    RTE_LOG(WARNING, ST_PTP6,"t4-t1=%ld t3-t2=%ld\n", ptpState.t4 -ptpState.t1, ptpState.t3 - ptpState.t2);

    StPtpLock();
    if (rte_eth_timesync_adjust_time(portid, delta) == 0)
    {
        if (ptpState.clkSrc == ST_PTP_CLOCK_SRC_ETH || ptpState.clkSrc == ST_PTP_CLOCK_SRC_AUTO)
            StPtpGetTime = StPtpTimeFromEth;
        RTE_LOG(WARNING, ST_PTP4,"delta: %ld\n", delta);
        RTE_LOG(WARNING, ST_PTP4,"   tp: %ld\n", tp);
        RTE_LOG(WARNING, ST_PTP4,"ist2Soft %ld  ist3Soft: %ld\n", ptpState.ist2Soft, ptpState.ist3Soft);
    }
    else
    {
        RTE_LOG(WARNING, ST_PTP, "rte_eth_timesync_adjust_time fail\n");
    }
    StPtpUnlock();

    int64_t deltaHpet = (((int64_t)ptpState.t4 - (int64_t)(ptpState.t3HPet))
        - ((int64_t)ptpState.t2HPet - (int64_t)ptpState.t1))/2;
#ifdef DEBUG_PTP
    int64_t delPath = ptpState.t3 - t3Soft;
    int64_t tpHPet =  (((int64_t)ptpState.t4 - (int64_t)(ptpState.t3HPet))
            + ((int64_t)ptpState.t2HPet - (int64_t)ptpState.t1))/2;
#endif
    RTE_LOG(WARNING, ST_PTP6,"t2HPet-t1=%ld t4-t3HPet=%ld\n", ptpState.t2HPet -ptpState.t1, ptpState.t4 - ptpState.t3HPet);
    RTE_LOG(WARNING, ST_PTP6,"t4-t1=%ld t3HPet-t2HPet=%ld\n", ptpState.t4 -ptpState.t1, ptpState.t3HPet - ptpState.t2HPet);

    epochRteAdj += deltaHpet;
    long double newHPetPeriod;
    uint64_t curHPetDel = ptpState.t1 - ptpState.t1HPetFreqStart;

    if (curHPetDel >= MIN_FREQ_MES_TIME)
    {
        newHPetPeriod = (long double)curHPetDel / (ptpState.t1HPetFreqClkNext - ptpState.t1HPetFreqClk);
        hpetPeriod = newHPetPeriod;
        ptpState.t1HPetFreqStart = 0;
        ptpState.t1HPetFreqClk = 0;
    }

    RTE_LOG(WARNING, ST_PTP4,"   curHPetDel: %ld\n", curHPetDel);
    RTE_LOG(WARNING, ST_PTP4,"    deltaHpet: %ld\n", deltaHpet);
    RTE_LOG(WARNING, ST_PTP4,"       tpHpet: %ld\n", tpHPet);
    RTE_LOG(WARNING, ST_PTP4,"   hpetPeriod: %Lf\n", hpetPeriod);
    RTE_LOG(WARNING, ST_PTP4,"newHpetPeriod: %Lf\n", newHPetPeriod);
    RTE_LOG(WARNING, ST_PTP4,"  t3 - t3Soft: %ld\n", delPath);

    //here sync other clocks
#if 0
    if (t3 == 0)
    { // TODO rte_eth_timesync_read_rx_timestamp
        t3 = StPtpGetTime();
        RTE_LOG(DEBUG, ST_PTP, "followUpTmstamp!\n");
    }
    RTE_LOG(DEBUG, ST_PTP, "%lu.%09lu %lu.%09lu %li\n", t3 / GIGA, t3 % GIGA, t1 / GIGA,
            t1 % GIGA, t3 - t1);
    uint64_t t4 = timespec64_to_ns(timestamp);
    if (t4 == 0)
        t4 = t3;
    { // Adjust time in DPDK/HPET
        CalcHpetPeriod();
        uint64_t t2 = TimeFromRte();
        int64_t delta = (t4 - t3 ? t3 : StPtpGetTime()) - (t2 - t1);
        //if ((delta < stTreshold)||(stTreshold > -delta))
        {
            delta/=2;
        }
        RTE_LOG(DEBUG,ST_PTP,"epochRte+=%li\n", delta);
        epochRte += delta;
    }


    uint64_t const ns=StPtpGetTime();
    time_t const s=ns/1000000000ull;
    RTE_LOG(DEBUG,ST_PTP,"PTP 0.%09llu + %s", ns%1000000000ull, ctime(&s)); // ctime contain '\n'
#endif
}

static st_status_t
StPtpGetPortClockIdentity(uint16_t portId, clock_id_t *clockId)
{
    int ret;
    struct rte_ether_addr mac;
    ret = rte_eth_macaddr_get(portId, &mac);
    if (ret !=0 ) return ST_GENERAL_ERR;
    StPtpPrepClockIdentityFromMac(clockId, &mac);
    return ST_OK;
}

static void
StPtpParseAnnouceMsg(struct rte_mbuf *m, ptp_announce_msg_t* ptpAnMsg, uint16_t portId)
{
    //first prepare actual clock identity
    port_id_t ourPortIdentity;
    ourPortIdentity.portNumber = htons(1); //now allways
    if (StPtpGetPortClockIdentity(portId, &ourPortIdentity.clockIdentity) != 0)
    {
        ptpState.state = PTP_NOT_INITIALIZED;
        return;
    }
    if (ptpState.state ==  PTP_NOT_INITIALIZED)
    {
        //copy our clock id and maybe master id
        ptpState.ourPortIdentity = ourPortIdentity;
        if (ptpState.masterChooseMode == ST_PTP_FIRST_KNOWN_MASTER)
            ptpState.masterPortIdentity = ptpAnMsg->hdr.sourcePortIdentity;
#if 0
        else if (ptpState.masterChooseMode == ST_PTP_SET_MASTER)
        {
            ptpState.masterChooseMode = ST_PTP_FIRST_KNOWN_MASTER;
            //not implemented

        }
        else if (ptpState.masterChooseMode == ST_PTP_BEST_KNOWN_MASTER)
        {
            ptpState.masterChooseMode = ST_PTP_FIRST_KNOWN_MASTER;
            //not implemented
        }
#endif
        else return; //difrent not supported
        ptpState.state = PTP_INITIALIZED;
        StPtpPrintPortIdentity("masterPortIdentity", &ptpState.masterPortIdentity);
        StPtpPrintPortIdentity("ourPortIdentity", &ptpState.ourPortIdentity);
    }
    else if (ptpState.state ==  PTP_INITIALIZED)
    {
        RTE_LOG(WARNING, ST_PTP4,"Before pauseToSendDelayReq %d\n", ptpState.pauseToSendDelayReq);
        if (ptpState.howSyncInAnnouce != 0)
        {
            ptpState.howDifDelayReqDelayRes += ptpState.howSyncInAnnouce -
                ptpState.howDelayResOurInAnnouce;
            int howDelPer;
            if (ptpState.howDelayResInAnnouce == ptpState.howDelayResOurInAnnouce)
                howDelPer = rand() % 10;
            else
                howDelPer = ptpState.howHigherPortIdentity / ptpState.howSyncInAnnouce;
            ptpState.pauseToSendDelayReq =  PAUSE_TO_SEND_FIRST_DELAY_REQ + howDelPer * ORDER_WAIT_TIME;
        }
        else
            ptpState.pauseToSendDelayReq = PAUSE_TO_SEND_FIRST_DELAY_REQ;

        RTE_LOG(WARNING, ST_PTP4,"howSyncInAnnouce %d\n", ptpState.howSyncInAnnouce);
        RTE_LOG(WARNING, ST_PTP4,"howHigherPortIdentity %d\n", ptpState.howHigherPortIdentity);
        RTE_LOG(WARNING, ST_PTP4,"howDifDelayReqDelayRes %d\n", ptpState.howDifDelayReqDelayRes);
        RTE_LOG(WARNING, ST_PTP4,"howDelayResInAnnouce %d\n", ptpState.howDelayResInAnnouce);
        RTE_LOG(WARNING, ST_PTP4,"howDelayResOurInAnnouce %d\n", ptpState.howDelayResOurInAnnouce);

        RTE_LOG(WARNING, ST_PTP4,"After pauseToSendDelayReq %d\n\n", ptpState.pauseToSendDelayReq);
    }
    //all other now ignore
    ptpState.howSyncInAnnouce = 0;
    ptpState.howDelayResInAnnouce = 0;
    ptpState.howDelayResOurInAnnouce = 0;
    ptpState.howHigherPortIdentity = 0;

    if (m->ol_flags & PKT_RX_VLAN)
    {
        ptpState.vlanTci = m->vlan_tci;
    }
    else
    {
        ptpState.vlanTci = 0;
    }
}


static void
StPtpParsePtp(struct rte_mbuf *m, uint16_t portid, uint16_t rxTmstampIdx)
{
    curHPetClk = rte_get_timer_cycles();
	uint64_t tm = StPtpGetTime();
	ptp_header_t *ptpHdr;

        if (ptpState.vlanRx)
            ptpHdr = rte_pktmbuf_mtod_offset(m, ptp_header_t *, sizeof(struct rte_ether_hdr)
			     + sizeof(struct rte_vlan_hdr));
	else
	    ptpHdr = rte_pktmbuf_mtod_offset(m, ptp_header_t *, sizeof(struct rte_ether_hdr));

	switch (ptpHdr->messageType)
	{
	case SYNC:
		StPtpParseSyncMsg((ptp_sync_msg_t *)ptpHdr, portid, rxTmstampIdx, &tm);
		break;
	case FOLLOW_UP:
		StPtpParseFollowUpMsg((ptp_follow_up_msg_t *)ptpHdr, portid);
		break;
	case DELAY_RESP:
		StPtpParseDellayResMsg((ptp_delay_resp_msg_t *)ptpHdr, portid);
		break;
	case ANNOUNCE:
                StPtpParseAnnouceMsg(m, (ptp_announce_msg_t*) ptpHdr, portid);
                break;
	case DELAY_REQ:
                StPtpPrintDelayReqMsg((ptp_delay_req_msg_t*)ptpHdr);
		break;
	default:
		RTE_LOG(DEBUG, ST_PTP4, "Unknown %02x PTP4L frame type\n", ptpHdr->messageType);
		return;
	}
}

void ParseArp(struct rte_arp_ipv4 const *header, uint16_t portid);


st_status_t
StParseEthernet(uint16_t portid, struct rte_mbuf *m)
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
		vlan_header = (struct rte_vlan_hdr*)((void*)&ethHdr->ether_type + 2);
		ptpState.vlanTci = ntohs(vlan_header->vlan_tci);
		ether_type = ntohs(vlan_header->eth_proto);
		vlan = 1;
        }

	switch (ether_type)
	{
	case PTP_PROTOCOL:
                ptpState.vlanRx = vlan;
		StPtpParsePtp(m, portid,  m->timesync);
		break;
	case ARP_PROTOCOL:
//		ParseArp(rte_pktmbuf_mtod_offset(m, struct rte_arp_ipv4 *, sizeof(struct rte_ether_hdr)),
//				 portid);
		break;
	case IPv4_PROTOCOL:
		ParseIp(rte_pktmbuf_mtod_offset(m, ipv4Hdr_t *, sizeof(struct rte_ether_hdr)), m, portid);
		break;
	default:
//        RTE_LOG(DEBUG, ST_PTP4, "Unknow %04x frame type %s\n", ntohs(ethHdr->ether_type), t);
		return ST_NOT_SUPPORTED;
	}
	return ST_OK;
}

uint64_t (*StPtpGetTime)(void) = StPtpTimeFromEth;//TimeFromRtc;

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

#if 0
int
rte_eth_timesync_adjust_freq(uint16_t port_id, int64_t delta)
{
	struct rte_eth_dev *dev;

	RTE_ETH_VALID_PORTID_OR_ERR_RET(port_id, -ENODEV);
	dev = &rte_eth_devices[port_id];

	RTE_FUNC_PTR_OR_ERR_RET(*dev->dev_ops->timesync_adjust_time, -ENOTSUP);
	return eth_err(port_id, (*dev->dev_ops->timesync_adjust_time)(dev,
								      delta));
}
#endif

static void *
StPtpDelayReqThread(void *arg)
{
    int ret;
    while (rte_atomic32_read(&ptpState.isStop) == 0)
    {
        struct timespec ts;
        pthread_mutex_lock(&ptpState.isDo);
        if (rte_atomic32_read(&ptpState.isStop) > 0)
        {
            rte_pktmbuf_free((struct rte_mbuf *)ptpState.delReqPkt);
            break;
        }
        rte_delay_us_sleep (ptpState.pauseToSendDelayReq);
        rte_eth_timesync_read_tx_timestamp(ptpState.portId, &ts);

        if (rte_eth_tx_burst(ptpState.portId, ptpState.txRingId, (struct rte_mbuf **)&ptpState.delReqPkt, 1) == 0)
        //if (rte_ring_sp_enqueue(ptpState.txRing, (void *)ptpState.delReqPkt))
        // not remove under line - I'll be it test yet
        {
            rte_pktmbuf_free((struct rte_mbuf *)ptpState.delReqPkt);
            RTE_LOG(WARNING, ST_PTP4, "delReqPkt didn't send\n");
            continue;
        }
        ptpState.howDelayReqSent++;
        ret = rte_eth_timesync_read_time(ptpState.portId,&ts); //soft time - if we here should be 0
        if (ret != 0) continue;
        ptpState.t3 = timespec64_to_ns(&ts); //temp soft
        ptpState.t3HPet = StPtpTimeFromRte();
        RTE_LOG(WARNING, ST_PTP4, "delReqPkt sent\n");
    }
    return NULL;
}

st_status_t StPtpInit(uint16_t portId, struct rte_mempool *mbuf, uint16_t txRingId, struct rte_ring *txRing)
{
    int ret;
    ret = rte_eth_timesync_enable(portId);
    if (ret != 0)
    {
        RTE_LOG(ERR, ST_PTP, "Cannot clock on port %d\n", portId);
        return ST_PTP_GENERAL_ERR;
    }
    ptpState.portId = portId;
    ptpState.txRingId = txRingId;
    ptpState.txRing = txRing;
    ptpState.mbuf = mbuf;
    ptpState.delReqPkt = NULL;
    rte_atomic32_init(&ptpState.isStop);
    pthread_mutex_init(&ptpState.isDo, NULL);
    pthread_mutex_lock(&ptpState.isDo);
    srand (time(NULL));
    ptpState.pauseToSendDelayReq =  PAUSE_TO_SEND_FIRST_DELAY_REQ + (rand() % 10 ) * ORDER_WAIT_TIME;
    RTE_LOG(WARNING, ST_PTP, "ptpState.pauseToSendDelayReq: %d\n",ptpState.pauseToSendDelayReq);
    ptpState.state = PTP_NOT_INITIALIZED;
    if (StPtpGetPortClockIdentity(ptpState.portId, &ptpState.ourPortIdentity.clockIdentity) != 0)
    {
        RTE_LOG(ERR, ST_PTP, "Cannot init portclockid PTP, not valid MAC?\n");
        return ST_PTP_GENERAL_ERR;
    }
    tzset();
    StPtpCalcHpetPeriod();

	struct timespec time;
	if (clock_gettime(CLOCK_REALTIME, &time) != 0)
	{
        RTE_LOG(ERR, ST_PTP, "Cannot read  CLOCK_REALTIME\n");
        return ST_PTP_GENERAL_ERR;
	}
    uint64_t ns = timespec64_to_ns(&time);
    ns += timezone * NS_PER_S;
    time = ns_to_timespec(ns);
    if (rte_eth_timesync_write_time(portId, &time))
    {
        RTE_LOG(ERR, ST_PTP, "Cannot write sync CLOCK_REALTIME to Eth\n");
        return ST_PTP_GENERAL_ERR;
    }
    ret = pthread_create(&ptpState.ptpDelayReqThread, NULL, (void*(*)(void*))StPtpDelayReqThread, NULL);
    if (ret != 0)
    {
        RTE_LOG(ERR, ST_PTP, "Cannot init PTP - cannot create thread\n");
        return ST_PTP_GENERAL_ERR;
    }
    StSetClockSource(ST_PTP_CLOCK_SRC_AUTO);
    return ST_OK;
}

st_status_t StSetClockSource(st_ptp_clocksource_t clkSrc)
{
    switch (clkSrc)
    {
    case ST_PTP_CLOCK_SRC_AUTO:
    case ST_PTP_CLOCK_SRC_ETH:
        StPtpGetTime = StPtpTimeFromEth;
        break;
    case ST_PTP_CLOCK_SRC_RTE:
        StPtpGetTime = StPtpTimeFromRte;
        break;
    case ST_PTP_CLOCK_SRC_RTC:
    default:
        return ST_PTP_NOT_VALID_CLK_SRC;
    }
    ptpState.clkSrc = clkSrc;
    return ST_OK;
}


#if 0
static uint64_t
TimeFromRtc()
{
	struct timespec time;
	if (clock_gettime(CLOCK_REALTIME, &time) == 0)
	{
		uint64_t ns = timespec64_to_ns(&time);
		CalcHpetPeriod();
		double adjust = hpetPeriod;
		adjust *= rte_get_timer_cycles();
		tzset();
		ns += timezone * NS_PER_S;
		epochRte = ns - adjust;
		StPtpGetTime = TimeFromRte;
		time.tv_nsec = ns%NS_PER_S;
		time.tv_sec = ns/NS_PER_S;
		if (rte_eth_timesync_enable(stSendDevice.dev.port[0]) ||
            rte_eth_ti mesync_write_time(stSendDevice.dev.port[0], &time) != 0)
		{
			RTE_LOG(WARNING, ST_PTP, "TimeFromRtc rte_eth_timesync_write_time fail\n");
		}
		RTE_LOG(WARNING, ST_PTP, "TimeFromRtc rte_eth_timesync_write_time OK: %ld:%ld\n",time.tv_sec,time.tv_nsec);
		return ns;
	}
	RTE_LOG(WARNING, ST_PTP, "clock_gettime fail\n");
	return 0;
}
#endif

st_status_t StPtpDeInit(uint16_t portId)
{
    rte_atomic32_add(&ptpState.isStop, 1);
    pthread_mutex_unlock(&ptpState.isDo);
    pthread_join(ptpState.ptpDelayReqThread, NULL);
    return ST_OK;
}

st_status_t StPtpIsSync(uint16_t portId)
{
    return -1;
}


