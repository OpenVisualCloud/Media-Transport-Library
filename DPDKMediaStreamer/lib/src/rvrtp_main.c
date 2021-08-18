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
 *	Media streamer based on DPDK
 *
 */

#include "rvrtp_main.h"

#include "st_flw_cls.h"
#include "st_kni.h"
#include "st_stats.h"

#include <signal.h>
#include <unistd.h>

/* Globals */
st_main_params_t stMainParams;

rte_atomic32_t isTxDevToDestroy;
rte_atomic32_t isRxDevToDestroy;
rte_atomic32_t isStopMainThreadTasks;

st_kni_ms_conf_t *kniDev1 = NULL;
st_kni_ms_conf_t *kniDev2 = NULL;

#define MAX_VER_SRING 64
char *stBuildVersionStr = BUILD;
char stLibVersionStr[MAX_STR_LEN];

extern st_enqueue_stats_t enqStats[RTE_MAX_LCORE];
extern st_rcv_stats_t rxThreadStats[RTE_MAX_LCORE];
extern st_rcv_stats_t rxThreadAudioStats[RTE_MAX_LCORE];
extern st_rcv_stats_t rxThreadAncilStats[RTE_MAX_LCORE];

struct rateAdjust {
	int port_num;
	int session_num;
	int frame_rate;
	int gbps;
	int adjust;
};

st_status_t
StGetParam(st_param_t prm, st_param_val_t *val)
{
	if (!val)
	{
		return ST_INVALID_PARAM;
	}

	switch (prm)
	{
	case ST_BUILD_ID:
		(*val).valueU64 = atoi(stBuildVersionStr);
		break;
	case ST_LIB_VERSION:
		snprintf(stLibVersionStr, sizeof(stLibVersionStr), "%d.%d.%d", LIB_VERSION_MAJOR,
				 LIB_VERSION_MINOR, LIB_VERSION_LAST);
		(*val).strPtr = stLibVersionStr;
		break;
	case ST_SOURCE_IP:
		memcpy(&val->valueU32, stMainParams.sipAddr[ST_PPORT], IP_ADDR_LEN);
		break;
	case ST_RSOURCE_IP:
		memcpy(&val->valueU32, stMainParams.sipAddr[ST_RPORT], IP_ADDR_LEN);
		break;
	case ST_LIB_SCOREID:
		val->strPtr = stMainParams.lib_cid;
		break;
	default:
		RTE_LOG(INFO, USER1, "Unknown param: %d\n", prm);
		return ST_BAD_PARAM_ID;
	}

	return ST_OK;
}

st_status_t
StSetParam(st_param_t prm, st_param_val_t val)
{
	switch (prm)
	{
	case ST_SOURCE_IP:
		memcpy(stMainParams.sipAddr[ST_PPORT], (uint8_t *)&(val.valueU32), IP_ADDR_LEN);
		break;
	case ST_RSOURCE_IP:
		memcpy(stMainParams.sipAddr[ST_RPORT], (uint8_t *)&(val.valueU32), IP_ADDR_LEN);
		break;
	case ST_DESTINATION_IP_TX:
		memcpy(stMainParams.ipAddr[ST_PPORT][ST_TX], (uint8_t *)&(val.valueU32), IP_ADDR_LEN);
		break;
	case ST_RDESTINATION_IP_TX:
		memcpy(stMainParams.ipAddr[ST_RPORT][ST_TX], (uint8_t *)&(val.valueU32), IP_ADDR_LEN);
		break;
	case ST_DESTINATION_IP_RX:
		memcpy(stMainParams.ipAddr[ST_PPORT][ST_RX], (uint8_t *)&(val.valueU32), IP_ADDR_LEN);
		break;
	case ST_RDESTINATION_IP_RX:
		memcpy(stMainParams.ipAddr[ST_RPORT][ST_RX], (uint8_t *)&(val.valueU32), IP_ADDR_LEN);
		break;
	case ST_EBU_TEST:
		stMainParams.isEbuCheck = val.valueU64;
		break;
	case ST_SN_COUNT:
		stMainParams.snCount = val.valueU64;
		break;
	case ST_SN30_COUNT:
		stMainParams.sn30Count = val.valueU64;
		break;
	case ST_SN40_COUNT:
		stMainParams.sn40Count = val.valueU64;
		break;
	case ST_TX_FROM_P:
		stMainParams.pTx = val.valueU64;
		break;
	case ST_RX_FROM_P:
		stMainParams.pRx = val.valueU64;
		break;
	case ST_TX_FROM_R:
		stMainParams.rTx = val.valueU64;
		break;
	case ST_RX_FROM_R:
		stMainParams.rRx = val.valueU64;
		break;
	case ST_P_PORT:
		snprintf(stMainParams.inPortName[ST_PPORT], sizeof(stMainParams.inPortName[ST_PPORT]), "%s",
				 val.strPtr);
		snprintf(stMainParams.outPortName[ST_PPORT], sizeof(stMainParams.outPortName[ST_PPORT]),
				 "%s", val.strPtr);
		break;
	case ST_R_PORT:
		snprintf(stMainParams.inPortName[ST_RPORT], sizeof(stMainParams.inPortName[ST_RPORT]), "%s",
				 val.strPtr);
		snprintf(stMainParams.outPortName[ST_RPORT], sizeof(stMainParams.outPortName[ST_RPORT]),
				 "%s", val.strPtr);
		break;
	case ST_FMT_INDEX:
		stMainParams.fmtIndex = val.valueU64;
		break;
	case ST_AUDIOFMT_INDEX:
		stMainParams.audioFmtIndex = val.valueU64;
		break;
	case ST_AUDIO_FRAME_SIZE:
		stMainParams.audioFrameSize = val.valueU32;
		break;
	case ST_DPDK_PARAMS:
		snprintf(stMainParams.dpdkParams, sizeof(stMainParams.dpdkParams), "%s", val.strPtr);
		break;
	case ST_BULK_NUM:
		stMainParams.txBulkNum = val.valueU64;
		break;
	case ST_ENQUEU_THREADS:
		stMainParams.maxEnqThrds = val.valueU64;
		break;
	case ST_NUM_PORT:
		stMainParams.numPorts = val.valueU32;
		break;
	case ST_PACING_TYPE:
		if (!strcmp(val.strPtr, "pause"))
			StSetPacing(ST_PACING_PAUSE);
		else if (!strcmp(val.strPtr, "tsc"))
			StSetPacing(ST_PACING_TSC);
		else
			RTE_LOG(WARNING, USER1, "%s, unknown pacing: %s\n", __func__, val.strPtr);
		break;
	case ST_TSC_HZ:
		StSetTscTimeHz(val.valueU64);
		break;
	case ST_LIB_SCOREID:
		strncpy(stMainParams.lib_cid, val.strPtr, sizeof(stMainParams.lib_cid) - 1);
		break;
	case ST_USER_TMSTAMP:
		stMainParams.userTmstamp = val.valueBool;
		break;
	default:
		RTE_LOG(INFO, USER1, "Unknown param: %d\n", prm);
		return ST_INVALID_PARAM;
	}

	return ST_OK;
}

void
StDisplayExitStats(void)
{
	extern uint64_t adjustCount[6];

	st_device_impl_t *dRx = &stRecvDevice;
	st_device_impl_t *dTx = &stSendDevice;

	printf("----------------------------------------\n");
	printf("ALIGNMENT COUNTS: \n");
	printf(" adjustCount[0] %lu adjustCount[1] %lu adjustCount[2] %lu\n", adjustCount[0],
		   adjustCount[1], adjustCount[2]);
	printf(" adjustCount[3] %lu adjustCount[4] %lu adjustCount[5] %lu\n", adjustCount[3],
		   adjustCount[4], adjustCount[5]);
	printf("----------------------------------------\n");

	printf("----------------------------------------\n");
	printf("SN TABLE: \n");
	for (uint32_t i = 0; i < dRx->dev.maxSt21Sessions; i++)
	{
		if (!dRx->snTable[i])
			continue;

		printf(" RX sn %u pRx %lu pDrop %lu fRx %lu fDrop %lu fixes %lu\n", i,
			   dRx->snTable[i]->sn.pktsRecv, dRx->snTable[i]->pktsDrop,
			   dRx->snTable[i]->sn.frmsRecv, dRx->snTable[i]->frmsDrop, dRx->snTable[i]->frmsFixed);
	}
	printf("----------------------------------------\n");

	printf("----------------------------------------\n");
	printf("TX scheduler stats:\n");
	st_enqueue_stats_t total = { 0 };
	unsigned int core = 0;
	RTE_LCORE_FOREACH(core)
	{
		total.pktsQueued += enqStats[core].pktsQueued;
		total.pktsBuild += enqStats[core].pktsBuild;

		total.pktsPriAllocFail += enqStats[core].pktsPriAllocFail;
		total.pktsExtAllocFail += enqStats[core].pktsExtAllocFail;
		total.pktsRedAllocFail += enqStats[core].pktsRedAllocFail;
		total.pktsQueuePriFail += enqStats[core].pktsQueuePriFail;
		total.pktsQueueRedFail += enqStats[core].pktsQueueRedFail;
		total.sessionLkpFail += enqStats[core].sessionLkpFail;
		total.sessionStateFail += enqStats[core].sessionStateFail;
		total.pktsChainPriFail += enqStats[core].pktsChainPriFail;
		total.pktsChainRedFail += enqStats[core].pktsChainRedFail;
	}

	printf("=== TX Packet Enqueue Stats ===\n");
	printf(" TX rings pktsBuild %lu, pktsEnq %lu\n", total.pktsBuild, total.pktsQueued);
	printf("=== TX Packet Enqueue Error Stats ===\n");
	printf(" BUFF: Primary %lu, External %lu, Redudant %lu\n", total.pktsPriAllocFail,
		   total.pktsExtAllocFail, total.pktsRedAllocFail);
	printf(" QUEUE: Primary %u, Redudant %u\n", total.pktsQueuePriFail, total.pktsQueueRedFail);
	printf(" SESSION: Lookup %u, State %u\n", total.sessionLkpFail, total.sessionStateFail);
	printf(" PKT-CHAIN: Primary %u, Redudant %u\n", total.pktsChainPriFail, total.pktsChainRedFail);
	printf("----------------------------------------\n");

	printf("----------------------------------------\n");
	printf("RX video stats:\n");
	st_rcv_stats_t rx_total = { 0 };
	st_rcv_stats_t rx_audio_total = { 0 };
	st_rcv_stats_t rx_ancil_total = { 0 };
	core = 0;

	RTE_LCORE_FOREACH(core)
	{
		/* video stats */
		rx_total.badIpUdp 		+= rxThreadStats[core].badIpUdp;
		rx_total.badIpUdpR 		+= rxThreadStats[core].badIpUdpR;
		rx_total.badRtp 		+= rxThreadStats[core].badRtp;
		rx_total.badRtpR 		+= rxThreadStats[core].badRtpR;
		rx_total.tmpstampDone 		+= rxThreadStats[core].tmpstampDone;
		rx_total.tmpstampDoneR 		+= rxThreadStats[core].tmpstampDoneR;
		rx_total.outOfOrder 		+= rxThreadStats[core].outOfOrder;
		rx_total.outOfOrderR 		+= rxThreadStats[core].outOfOrderR;
		rx_total.rtpTmstampOverflow	+= rxThreadStats[core].rtpTmstampOverflow;
		rx_total.rtpTmstampOverflowR	+= rxThreadStats[core].rtpTmstampOverflowR;
		rx_total.rtpTmstampLess 	+= rxThreadStats[core].rtpTmstampLess;
		rx_total.rtpTmstampLessR 	+= rxThreadStats[core].rtpTmstampLessR;

		rx_total.restartAsNewFrame	+= rxThreadStats[core].restartAsNewFrame;
		rx_total.restartAsNewFrameR	+= rxThreadStats[core].restartAsNewFrameR;

		rx_total.firstPacketGood 	+= rxThreadStats[core].firstPacketGood;
		rx_total.firstPacketGoodR 	+= rxThreadStats[core].firstPacketGoodR;
		rx_total.nonFirstPacketGood	+= rxThreadStats[core].nonFirstPacketGood;
		rx_total.nonFirstPacketGoodR	+= rxThreadStats[core].nonFirstPacketGoodR;
		rx_total.lastPacketGood 	+= rxThreadStats[core].lastPacketGood;
		rx_total.lastPacketGoodR 	+= rxThreadStats[core].lastPacketGoodR;
		rx_total.nonFirstPacketPendGood	+= rxThreadStats[core].nonFirstPacketPendGood;
		rx_total.nonFirstPacketPendGoodR+= rxThreadStats[core].nonFirstPacketPendGoodR;
		rx_total.lastPacketPendGood 	+= rxThreadStats[core].lastPacketPendGood;
		rx_total.lastPacketPendGoodR 	+= rxThreadStats[core].lastPacketPendGoodR;

		rx_total.fastCopyFail 		+= rxThreadStats[core].fastCopyFail;
		rx_total.fastCopyFailR 		+= rxThreadStats[core].fastCopyFailR;
		rx_total.fastCopyFailErr 	+= rxThreadStats[core].fastCopyFailErr;
		rx_total.fastCopyFailErrR	+= rxThreadStats[core].fastCopyFailErrR;

		rx_total.userNotifyLine 	+= rxThreadStats[core].userNotifyLine;
		rx_total.userNotifyPendLine 	+= rxThreadStats[core].userNotifyPendLine;
		rx_total.userNotifyFrame 	+= rxThreadStats[core].userNotifyFrame;
		rx_total.userNotifyPendFrame 	+= rxThreadStats[core].userNotifyPendFrame;

		rx_total.completeFrames 	+= rxThreadStats[core].completeFrames;
		rx_total.completePendFrames 	+= rxThreadStats[core].completePendFrames;
		rx_total.incompleteFrameDone	+= rxThreadStats[core].incompleteFrameDone;
		rx_total.incompletePendFrameDone+= rxThreadStats[core].incompletePendFrameDone;

		rx_total.forcePendBuffOut 	+= rxThreadStats[core].forcePendBuffOut;
		rx_total.forcePendBuffOutR 	+= rxThreadStats[core].forcePendBuffOutR;
		rx_total.forceCurrBuffOut 	+= rxThreadStats[core].forceCurrBuffOut;
		rx_total.forceCurrBuffOutR 	+= rxThreadStats[core].forceCurrBuffOutR;

		/* audio stats */
		rx_audio_total.badIpUdp 		+= rxThreadAudioStats[core].badIpUdp;
		rx_audio_total.badIpUdpR 		+= rxThreadAudioStats[core].badIpUdpR;
		rx_audio_total.badRtp 			+= rxThreadAudioStats[core].badRtp;
		rx_audio_total.badRtpR 			+= rxThreadAudioStats[core].badRtpR;
		rx_audio_total.tmpstampDone 		+= rxThreadAudioStats[core].tmpstampDone;
		rx_audio_total.tmpstampDoneR 		+= rxThreadAudioStats[core].tmpstampDoneR;
		rx_audio_total.outOfOrder 		+= rxThreadAudioStats[core].outOfOrder;
		rx_audio_total.outOfOrderR 		+= rxThreadAudioStats[core].outOfOrderR;
		rx_audio_total.rtpTmstampOverflow	+= rxThreadAudioStats[core].rtpTmstampOverflow;
		rx_audio_total.rtpTmstampOverflowR	+= rxThreadAudioStats[core].rtpTmstampOverflowR;
		rx_audio_total.rtpTmstampLess 		+= rxThreadAudioStats[core].rtpTmstampLess;
		rx_audio_total.rtpTmstampLessR 		+= rxThreadAudioStats[core].rtpTmstampLessR;

		rx_audio_total.restartAsNewFrame	+= rxThreadAudioStats[core].restartAsNewFrame;
		rx_audio_total.restartAsNewFrameR	+= rxThreadAudioStats[core].restartAsNewFrameR;

		rx_audio_total.firstPacketGood 		+= rxThreadAudioStats[core].firstPacketGood;
		rx_audio_total.firstPacketGoodR 	+= rxThreadAudioStats[core].firstPacketGoodR;
		rx_audio_total.nonFirstPacketGood	+= rxThreadAudioStats[core].nonFirstPacketGood;
		rx_audio_total.nonFirstPacketGoodR	+= rxThreadAudioStats[core].nonFirstPacketGoodR;
		rx_audio_total.lastPacketGood 		+= rxThreadAudioStats[core].lastPacketGood;
		rx_audio_total.lastPacketGoodR 		+= rxThreadAudioStats[core].lastPacketGoodR;
		rx_audio_total.nonFirstPacketPendGood	+= rxThreadAudioStats[core].nonFirstPacketPendGood;
		rx_audio_total.nonFirstPacketPendGoodR	+= rxThreadAudioStats[core].nonFirstPacketPendGoodR;
		rx_audio_total.lastPacketPendGood 	+= rxThreadAudioStats[core].lastPacketPendGood;
		rx_audio_total.lastPacketPendGoodR 	+= rxThreadAudioStats[core].lastPacketPendGoodR;

		rx_audio_total.fastCopyFail 		+= rxThreadAudioStats[core].fastCopyFail;
		rx_audio_total.fastCopyFailR 		+= rxThreadAudioStats[core].fastCopyFailR;
		rx_audio_total.fastCopyFailErr 		+= rxThreadAudioStats[core].fastCopyFailErr;
		rx_audio_total.fastCopyFailErrR		+= rxThreadAudioStats[core].fastCopyFailErrR;

		rx_audio_total.userNotifyLine 		+= rxThreadAudioStats[core].userNotifyLine;
		rx_audio_total.userNotifyPendLine 	+= rxThreadAudioStats[core].userNotifyPendLine;
		rx_audio_total.userNotifyFrame 		+= rxThreadAudioStats[core].userNotifyFrame;
		rx_audio_total.userNotifyPendFrame 	+= rxThreadAudioStats[core].userNotifyPendFrame;

		rx_audio_total.completeFrames 		+= rxThreadAudioStats[core].completeFrames;
		rx_audio_total.completePendFrames 	+= rxThreadAudioStats[core].completePendFrames;
		rx_audio_total.incompleteFrameDone	+= rxThreadAudioStats[core].incompleteFrameDone;
		rx_audio_total.incompletePendFrameDone	+= rxThreadAudioStats[core].incompletePendFrameDone;

		rx_audio_total.forcePendBuffOut 	+= rxThreadAudioStats[core].forcePendBuffOut;
		rx_audio_total.forcePendBuffOutR 	+= rxThreadAudioStats[core].forcePendBuffOutR;
		rx_audio_total.forceCurrBuffOut 	+= rxThreadAudioStats[core].forceCurrBuffOut;
		rx_audio_total.forceCurrBuffOutR 	+= rxThreadAudioStats[core].forceCurrBuffOutR;

		/* ancilary stats */
		rx_ancil_total.badIpUdp 		+= rxThreadAncilStats[core].badIpUdp;
		rx_ancil_total.badIpUdpR 		+= rxThreadAncilStats[core].badIpUdpR;
		rx_ancil_total.badRtp 			+= rxThreadAncilStats[core].badRtp;
		rx_ancil_total.badRtpR 			+= rxThreadAncilStats[core].badRtpR;
		rx_ancil_total.tmpstampDone 		+= rxThreadAncilStats[core].tmpstampDone;
		rx_ancil_total.tmpstampDoneR 		+= rxThreadAncilStats[core].tmpstampDoneR;
		rx_ancil_total.outOfOrder 		+= rxThreadAncilStats[core].outOfOrder;
		rx_ancil_total.outOfOrderR 		+= rxThreadAncilStats[core].outOfOrderR;
		rx_ancil_total.rtpTmstampOverflow	+= rxThreadAncilStats[core].rtpTmstampOverflow;
		rx_ancil_total.rtpTmstampOverflowR	+= rxThreadAncilStats[core].rtpTmstampOverflowR;
		rx_ancil_total.rtpTmstampLess 		+= rxThreadAncilStats[core].rtpTmstampLess;
		rx_ancil_total.rtpTmstampLessR 		+= rxThreadAncilStats[core].rtpTmstampLessR;

		rx_ancil_total.restartAsNewFrame	+= rxThreadAncilStats[core].restartAsNewFrame;
		rx_ancil_total.restartAsNewFrameR	+= rxThreadAncilStats[core].restartAsNewFrameR;

		rx_ancil_total.firstPacketGood 		+= rxThreadAncilStats[core].firstPacketGood;
		rx_ancil_total.firstPacketGoodR 	+= rxThreadAncilStats[core].firstPacketGoodR;
		rx_ancil_total.nonFirstPacketGood	+= rxThreadAncilStats[core].nonFirstPacketGood;
		rx_ancil_total.nonFirstPacketGoodR	+= rxThreadAncilStats[core].nonFirstPacketGoodR;
		rx_ancil_total.lastPacketGood 		+= rxThreadAncilStats[core].lastPacketGood;
		rx_ancil_total.lastPacketGoodR 		+= rxThreadAncilStats[core].lastPacketGoodR;
		rx_ancil_total.nonFirstPacketPendGood	+= rxThreadAncilStats[core].nonFirstPacketPendGood;
		rx_ancil_total.nonFirstPacketPendGoodR	+= rxThreadAncilStats[core].nonFirstPacketPendGoodR;
		rx_ancil_total.lastPacketPendGood 	+= rxThreadAncilStats[core].lastPacketPendGood;
		rx_ancil_total.lastPacketPendGoodR 	+= rxThreadAncilStats[core].lastPacketPendGoodR;

		rx_ancil_total.fastCopyFail 		+= rxThreadAncilStats[core].fastCopyFail;
		rx_ancil_total.fastCopyFailR 		+= rxThreadAncilStats[core].fastCopyFailR;
		rx_ancil_total.fastCopyFailErr 		+= rxThreadAncilStats[core].fastCopyFailErr;
		rx_ancil_total.fastCopyFailErrR		+= rxThreadAncilStats[core].fastCopyFailErrR;

		rx_ancil_total.userNotifyLine 		+= rxThreadAncilStats[core].userNotifyLine;
		rx_ancil_total.userNotifyPendLine 	+= rxThreadAncilStats[core].userNotifyPendLine;
		rx_ancil_total.userNotifyFrame 		+= rxThreadAncilStats[core].userNotifyFrame;
		rx_ancil_total.userNotifyPendFrame 	+= rxThreadAncilStats[core].userNotifyPendFrame;

		rx_ancil_total.completeFrames 		+= rxThreadAncilStats[core].completeFrames;
		rx_ancil_total.completePendFrames 	+= rxThreadAncilStats[core].completePendFrames;
		rx_ancil_total.incompleteFrameDone	+= rxThreadAncilStats[core].incompleteFrameDone;
		rx_ancil_total.incompletePendFrameDone	+= rxThreadAncilStats[core].incompletePendFrameDone;

		rx_ancil_total.forcePendBuffOut 	+= rxThreadAncilStats[core].forcePendBuffOut;
		rx_ancil_total.forcePendBuffOutR 	+= rxThreadAncilStats[core].forcePendBuffOutR;
		rx_ancil_total.forceCurrBuffOut 	+= rxThreadAncilStats[core].forceCurrBuffOut;
		rx_ancil_total.forceCurrBuffOutR 	+= rxThreadAncilStats[core].forceCurrBuffOutR;
	}

	printf("--- RX VIDEO THREAD STATS --\n");
	printf("\n");
	printf("--- LIBRARY ---\n");
	printf("-----------------------------------------------------------------------------------------------\n");
	printf("| %12s | %30s | %20s | %20s |\n", "Error Type", "Category", "Primary", "Redundant");
	printf("-----------------------------------------------------------------------------------------------\n");
	printf("| %12s | %30s | %20lu | %20lu |\n", "packet-err", "bad Ip|Udp", rx_total.badIpUdp, rx_total.badIpUdpR); 
	printf("| %12s | %30s | %20lu | %20lu |\n", "packet-err", "bad Rtp", rx_total.badRtp, rx_total.badRtpR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-err", "out of Order", rx_total.outOfOrder, rx_total.outOfOrderR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-err", "incorrect tmstamp", rx_total.rtpTmstampLess, rx_total.rtpTmstampLessR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "switch-err", "Force Pending Frames", rx_total.forcePendBuffOut, rx_total.forcePendBuffOutR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "switch-err", "Force Current Frames", rx_total.forceCurrBuffOut, rx_total.forceCurrBuffOutR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "switch-err", "Historgram Err pkt", rx_total.fastCopyFailErr, rx_total.fastCopyFailErrR);
	printf("^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^\n");
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-ok", "tmstamp Done", rx_total.tmpstampDone, rx_total.tmpstampDoneR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-ok", "tmstamp Overflow", rx_total.rtpTmstampOverflow, rx_total.rtpTmstampOverflowR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-ok", "First Frame Pkt", rx_total.firstPacketGood, rx_total.firstPacketGoodR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-ok", "CURR Frame middle Pkt", rx_total.nonFirstPacketGood, rx_total.nonFirstPacketGoodR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-ok", "PEND Frame middle Pkt", rx_total.nonFirstPacketPendGood, rx_total.nonFirstPacketPendGoodR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-ok", "CURR Frame last Pkt", rx_total.lastPacketGood, rx_total.lastPacketGoodR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-ok", "PEND Frame last Pkt", rx_total.lastPacketPendGood, rx_total.lastPacketPendGoodR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "library-ok", "Restart as new Frame", rx_total.restartAsNewFrame, rx_total.restartAsNewFrameR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "library-ok", "Histogram Redundant pkt", rx_total.fastCopyFail, rx_total.fastCopyFailR);
	printf("-----------------------------------------------------------------------------------------------\n");
	printf("\n");
	printf("--- USER NOTIFICATION ---\n");
	printf("------------------------------------------------------------------------\n");
	printf("| %12s | %30s | %20s | \n", "Error Type", "Category", "Count");
	printf("------------------------------------------------------------------------\n");
	printf("| %12s | %30s | %20lu |\n", "unexpected", "incomplete Curr-Frame", rx_total.incompleteFrameDone);
	printf("| %12s | %30s | %20lu |\n", "unexpected", "incomplete Pend-Frame", rx_total.incompletePendFrameDone);
	printf("^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^\n");
	printf("| %12s | %30s | %20lu |\n", "normal", "Notify N lines of CURR", rx_total.userNotifyLine);
	printf("| %12s | %30s | %20lu |\n", "normal", "Notify N lines of PEND", rx_total.userNotifyPendLine);
	printf("| %12s | %30s | %20lu |\n", "normal", "Notify Frame of CURR", rx_total.userNotifyFrame);
	printf("| %12s | %30s | %20lu |\n", "normal", "Notify Frame of PEND", rx_total.userNotifyPendFrame);
	printf("| %12s | %30s | %20lu |\n", "normal", "complete CURR Frame", rx_total.completeFrames);
	printf("| %12s | %30s | %20lu |\n", "normal", "complete PEND Frame", rx_total.completePendFrames);
	printf("------------------------------------------------------------------------\n");

	printf("--- RX AUDIO THREAD STATS --\n");
	printf("\n");
	printf("--- LIBRARY ---\n");
	printf("-----------------------------------------------------------------------------------------------\n");
	printf("| %12s | %30s | %20s | %20s |\n", "Error Type", "Category", "Primary", "Redundant");
	printf("-----------------------------------------------------------------------------------------------\n");
	printf("| %12s | %30s | %20lu | %20lu |\n", "packet-err", "bad Ip|Udp", 		rx_audio_total.badIpUdp, rx_audio_total.badIpUdpR); 
	printf("| %12s | %30s | %20lu | %20lu |\n", "packet-err", "bad Rtp", 			rx_audio_total.badRtp, rx_audio_total.badRtpR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-err", "out of Order", 		rx_audio_total.outOfOrder, rx_audio_total.outOfOrderR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-err", "incorrect tmstamp", 	rx_audio_total.rtpTmstampLess, rx_audio_total.rtpTmstampLessR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "switch-err", "Force Pending Frames", 	rx_audio_total.forcePendBuffOut, rx_audio_total.forcePendBuffOutR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "switch-err", "Force Current Frames", 	rx_audio_total.forceCurrBuffOut, rx_audio_total.forceCurrBuffOutR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "switch-err", "Historgram Err pkt", 	rx_audio_total.fastCopyFailErr, rx_audio_total.fastCopyFailErrR);
	printf("^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^\n");
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-ok", "tmstamp Done", 		rx_audio_total.tmpstampDone, rx_audio_total.tmpstampDoneR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-ok", "tmstamp Overflow", 		rx_audio_total.rtpTmstampOverflow, rx_audio_total.rtpTmstampOverflowR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-ok", "First Frame Pkt", 		rx_audio_total.firstPacketGood, rx_audio_total.firstPacketGoodR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-ok", "CURR Frame middle Pkt", 	rx_audio_total.nonFirstPacketGood, rx_audio_total.nonFirstPacketGoodR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-ok", "PEND Frame middle Pkt", 	rx_audio_total.nonFirstPacketPendGood, rx_audio_total.nonFirstPacketPendGoodR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-ok", "CURR Frame last Pkt", 	rx_audio_total.lastPacketGood, rx_audio_total.lastPacketGoodR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-ok", "PEND Frame last Pkt", 	rx_audio_total.lastPacketPendGood, rx_audio_total.lastPacketPendGoodR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "library-ok", "Restart as new Frame", 	rx_audio_total.restartAsNewFrame, rx_audio_total.restartAsNewFrameR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "library-ok", "Histogram Redundant pkt", 	rx_audio_total.fastCopyFail, rx_audio_total.fastCopyFailR);
	printf("-----------------------------------------------------------------------------------------------\n");
	printf("\n");
	printf("--- USER NOTIFICATION ---\n");
	printf("------------------------------------------------------------------------\n");
	printf("| %12s | %30s | %20s | \n", "Error Type", "Category", "Count");
	printf("------------------------------------------------------------------------\n");
	printf("| %12s | %30s | %20lu |\n", "unexpected", "incomplete Curr-Frame", rx_audio_total.incompleteFrameDone);
	printf("| %12s | %30s | %20lu |\n", "unexpected", "incomplete Pend-Frame", rx_audio_total.incompletePendFrameDone);
	printf("^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^\n");
	printf("| %12s | %30s | %20lu |\n", "normal", "Notify N lines of CURR", rx_audio_total.userNotifyLine);
	printf("| %12s | %30s | %20lu |\n", "normal", "Notify N lines of PEND", rx_audio_total.userNotifyPendLine);
	printf("| %12s | %30s | %20lu |\n", "normal", "Notify Frame of CURR", rx_audio_total.userNotifyFrame);
	printf("| %12s | %30s | %20lu |\n", "normal", "Notify Frame of PEND", rx_audio_total.userNotifyPendFrame);
	printf("| %12s | %30s | %20lu |\n", "normal", "complete CURR Frame", rx_audio_total.completeFrames);
	printf("| %12s | %30s | %20lu |\n", "normal", "complete PEND Frame", rx_audio_total.completePendFrames);
	printf("------------------------------------------------------------------------\n");

	printf("--- RX ANCILARY THREAD STATS --\n");
	printf("\n");
	printf("--- LIBRARY ---\n");
	printf("-----------------------------------------------------------------------------------------------\n");
	printf("| %12s | %30s | %20s | %20s |\n", "Error Type", "Category", "Primary", "Redundant");
	printf("-----------------------------------------------------------------------------------------------\n");
	printf("| %12s | %30s | %20lu | %20lu |\n", "packet-err", "bad Ip|Udp", rx_ancil_total.badIpUdp, rx_ancil_total.badIpUdpR); 
	printf("| %12s | %30s | %20lu | %20lu |\n", "packet-err", "bad Rtp", rx_ancil_total.badRtp, rx_ancil_total.badRtpR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-err", "out of Order", rx_ancil_total.outOfOrder, rx_ancil_total.outOfOrderR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-err", "incorrect tmstamp", rx_ancil_total.rtpTmstampLess, rx_ancil_total.rtpTmstampLessR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "switch-err", "Force Pending Frames", rx_ancil_total.forcePendBuffOut, rx_ancil_total.forcePendBuffOutR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "switch-err", "Force Current Frames", rx_ancil_total.forceCurrBuffOut, rx_ancil_total.forceCurrBuffOutR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "switch-err", "Historgram Err pkt", rx_ancil_total.fastCopyFailErr,	rx_ancil_total.fastCopyFailErrR);
	printf("^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^\n");
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-ok", "tmstamp Done", rx_ancil_total.tmpstampDone, rx_ancil_total.tmpstampDoneR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-ok", "tmstamp Overflow", rx_ancil_total.rtpTmstampOverflow, rx_ancil_total.rtpTmstampOverflowR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-ok", "First Frame Pkt", rx_ancil_total.firstPacketGood, rx_ancil_total.firstPacketGoodR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-ok", "CURR Frame middle Pkt", rx_ancil_total.nonFirstPacketGood, rx_ancil_total.nonFirstPacketGoodR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-ok", "PEND Frame middle Pkt", rx_ancil_total.nonFirstPacketPendGood, rx_ancil_total.nonFirstPacketPendGoodR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-ok", "CURR Frame last Pkt", rx_ancil_total.lastPacketGood, rx_ancil_total.lastPacketGoodR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "rtp-hdr-ok", "PEND Frame last Pkt", rx_ancil_total.lastPacketPendGood, rx_ancil_total.lastPacketPendGoodR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "library-ok", "Restart as new Frame", rx_ancil_total.restartAsNewFrame, rx_ancil_total.restartAsNewFrameR);
	printf("| %12s | %30s | %20lu | %20lu |\n", "library-ok", "Histogram Redundant pkt", rx_ancil_total.fastCopyFail, rx_ancil_total.fastCopyFailR);
	printf("-----------------------------------------------------------------------------------------------\n");
	printf("\n");
	printf("--- USER NOTIFICATION ---\n");
	printf("------------------------------------------------------------------------\n");
	printf("| %12s | %30s | %20s | \n", "Error Type", "Category", "Count");
	printf("------------------------------------------------------------------------\n");
	printf("| %12s | %30s | %20lu |\n", "unexpected", "incomplete Curr-Frame", rx_ancil_total.incompleteFrameDone);
	printf("| %12s | %30s | %20lu |\n", "unexpected", "incomplete Pend-Frame", rx_ancil_total.incompletePendFrameDone);
	printf("^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^\n");
	printf("| %12s | %30s | %20lu |\n", "normal", "Notify N lines of CURR", rx_ancil_total.userNotifyLine);
	printf("| %12s | %30s | %20lu |\n", "normal", "Notify N lines of PEND", rx_ancil_total.userNotifyPendLine);
	printf("| %12s | %30s | %20lu |\n", "normal", "Notify Frame of CURR", rx_ancil_total.userNotifyFrame);
	printf("| %12s | %30s | %20lu |\n", "normal", "Notify Frame of PEND", rx_ancil_total.userNotifyPendFrame);
	printf("| %12s | %30s | %20lu |\n", "normal", "complete CURR Frame", rx_ancil_total.completeFrames);
	printf("| %12s | %30s | %20lu |\n", "normal", "complete PEND Frame", rx_ancil_total.completePendFrames);
	printf("------------------------------------------------------------------------\n");

	if ((dTx->packetsTx) && (dTx->pausesTx))
	{
		for (uint32_t i = 0; i < stMainParams.numPorts; i++)
		{
			if (i == 0 && stMainParams.pTx != 1)
				continue;
			for (uint32_t j = 0; j <= dTx->maxRings; j++)
			{
				printf(" TX port %u ring %u packetsTx %lu pausesTx %lu\n", i, j,
					   dTx->packetsTx[i][j], dTx->pausesTx[i][j]);
			}
		}
	}

	if (stSendDevice.snTable && stSendDevice.snTable[0])
	{
		for (int p = 0; p < stMainParams.numPorts; ++p)
		{
			if (p == 0 && stMainParams.pTx != 1)
				continue;
			fprintf(stderr, "DEST_MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
					stSendDevice.snTable[0]->hdrPrint[p].singleHdr.eth.d_addr.addr_bytes[0],
					stSendDevice.snTable[0]->hdrPrint[p].singleHdr.eth.d_addr.addr_bytes[1],
					stSendDevice.snTable[0]->hdrPrint[p].singleHdr.eth.d_addr.addr_bytes[2],
					stSendDevice.snTable[0]->hdrPrint[p].singleHdr.eth.d_addr.addr_bytes[3],
					stSendDevice.snTable[0]->hdrPrint[p].singleHdr.eth.d_addr.addr_bytes[4],
					stSendDevice.snTable[0]->hdrPrint[p].singleHdr.eth.d_addr.addr_bytes[5]);
		}
	}

	return;
}

uint32_t
StDevGetNicMaxSessions(st_device_impl_t *d)
{
	if (!d)
		ST_ASSERT;
	if (!stDevParams)
		ST_ASSERT;

	switch (d->dev.exactRate)
	{
	case ST_DEV_RATE_P_25_00:
		if (d->fmtIndex == ST21_FMT_P_INTEL_720_25 || d->fmtIndex == ST21_FMT_P_AYA_720_25)
			return stDevParams->maxSt21Sn25Fps * 2;
		else if (d->fmtIndex == ST21_FMT_P_INTEL_2160_25 || d->fmtIndex == ST21_FMT_P_AYA_2160_25)
			return stDevParams->maxSt21Sn25Fps / 4;
		else
			return stDevParams->maxSt21Sn25Fps;

	case ST_DEV_RATE_P_29_97:
		if (d->fmtIndex == ST21_FMT_P_INTEL_720_29 || d->fmtIndex == ST21_FMT_P_AYA_720_29)
			return stDevParams->maxSt21Sn29Fps * 2;
		else if (d->fmtIndex == ST21_FMT_P_INTEL_2160_29 || d->fmtIndex == ST21_FMT_P_AYA_2160_29)
			return stDevParams->maxSt21Sn29Fps / 4;
		else
			return stDevParams->maxSt21Sn29Fps;

	case ST_DEV_RATE_P_50_00:
		if (d->fmtIndex == ST21_FMT_P_INTEL_720_50 || d->fmtIndex == ST21_FMT_P_AYA_720_50)
			return stDevParams->maxSt21Sn50Fps * 2;
		else if (d->fmtIndex == ST21_FMT_P_INTEL_2160_50 || d->fmtIndex == ST21_FMT_P_AYA_2160_50)
			return stDevParams->maxSt21Sn50Fps / 4;
		else
			return stDevParams->maxSt21Sn50Fps;

	case ST_DEV_RATE_P_59_94:
		if (d->fmtIndex == ST21_FMT_P_INTEL_720_59 || d->fmtIndex == ST21_FMT_P_AYA_720_59)
			return stDevParams->maxSt21Sn59Fps * 2;
		else if (d->fmtIndex == ST21_FMT_P_INTEL_2160_59 || d->fmtIndex == ST21_FMT_P_AYA_2160_59)
			return stDevParams->maxSt21Sn59Fps / 4;
		else
			return stDevParams->maxSt21Sn59Fps;

	case ST_DEV_RATE_I_25_00:
		if (d->fmtIndex == ST21_FMT_I_INTEL_720_25 || d->fmtIndex == ST21_FMT_I_AYA_720_25)
			return stDevParams->maxSt21Sn25Fps * 4;
		else if (d->fmtIndex == ST21_FMT_I_INTEL_2160_25 || d->fmtIndex == ST21_FMT_I_AYA_2160_25)
			return stDevParams->maxSt21Sn25Fps / 2;
		else
			return stDevParams->maxSt21Sn25Fps * 2;

	case ST_DEV_RATE_I_29_97:
		if (d->fmtIndex == ST21_FMT_I_INTEL_720_29 || d->fmtIndex == ST21_FMT_I_AYA_720_29)
			return stDevParams->maxSt21Sn29Fps * 4;
		else if (d->fmtIndex == ST21_FMT_I_INTEL_2160_29 || d->fmtIndex == ST21_FMT_I_AYA_2160_29)
			return stDevParams->maxSt21Sn29Fps / 2;
		else
			return stDevParams->maxSt21Sn29Fps * 2;

	case ST_DEV_RATE_I_50_00:
		if (d->fmtIndex == ST21_FMT_I_INTEL_720_50 || d->fmtIndex == ST21_FMT_I_AYA_720_50)
			return stDevParams->maxSt21Sn50Fps * 4;
		else if (d->fmtIndex == ST21_FMT_I_INTEL_2160_50 || d->fmtIndex == ST21_FMT_I_AYA_2160_50)
			return stDevParams->maxSt21Sn50Fps / 2;
		else
			return stDevParams->maxSt21Sn50Fps * 2;

	case ST_DEV_RATE_I_59_94:
		if (d->fmtIndex == ST21_FMT_I_INTEL_720_59 || d->fmtIndex == ST21_FMT_I_AYA_720_59)
			return stDevParams->maxSt21Sn59Fps * 4;
		else if (d->fmtIndex == ST21_FMT_I_INTEL_2160_59 || d->fmtIndex == ST21_FMT_I_AYA_2160_59)
			return stDevParams->maxSt21Sn59Fps / 2;
		else
			return stDevParams->maxSt21Sn59Fps * 2;
	}
	ST_ASSERT;
	return 0;
}

uint32_t
StDevGetFrameTime(st_exact_rate_t videoRate)
{
	switch (videoRate)
	{
	case ST_DEV_RATE_P_25_00:
	case ST_DEV_RATE_I_25_00:
		return 40000000;

	case ST_DEV_RATE_P_29_97:
	case ST_DEV_RATE_I_29_97:
		return (uint32_t)((MEGA * 1001) / 30);

	case ST_DEV_RATE_P_50_00:
	case ST_DEV_RATE_I_50_00:
		return 20000000;

	case ST_DEV_RATE_P_59_94:
	case ST_DEV_RATE_I_59_94:
		return (uint32_t)((MEGA * 1001) / 60);
	}

	ST_ASSERT;
	return 0;
}

int
StDevGetInterlaced(st_exact_rate_t videoRate)
{
	switch (videoRate)
	{
	case ST_DEV_RATE_I_29_97:
	case ST_DEV_RATE_I_25_00:
	case ST_DEV_RATE_I_50_00:
	case ST_DEV_RATE_I_59_94:
		return 1;

	case ST_DEV_RATE_P_25_00:
	case ST_DEV_RATE_P_29_97:
	case ST_DEV_RATE_P_50_00:
	case ST_DEV_RATE_P_59_94:
		return 0;
	}

	ST_ASSERT;
	return 0;
}

struct rateAdjust adjust_table[] = {
	{1, 2, ST_DEV_RATE_P_59_94, 25, 15000},
	{1, 3, ST_DEV_RATE_P_59_94, 25, 14000},
	{1, 2, ST_DEV_RATE_P_50_00, 25, 18000},
	{1, 3, ST_DEV_RATE_P_50_00, 25, 17500},
	{1, 1, ST_DEV_RATE_P_29_97, 25, 33000},
	{1, 2, ST_DEV_RATE_P_29_97, 25, 30000},
	{1, 3, ST_DEV_RATE_P_29_97, 25, 20000},
	{1, 3, ST_DEV_RATE_P_59_94, 40, 1500},
	{1, 3, ST_DEV_RATE_P_29_97, 40, 3000},
	{2, 7, ST_DEV_RATE_P_59_94, 100, 6000},
};

st_status_t
StDevGetAdjust(st_device_impl_t *d, int num_port)
{
	for (int idx = 0; idx < sizeof(adjust_table) / sizeof(struct rateAdjust); idx++)
	{
		if (num_port == adjust_table[idx].port_num
			&& stMainParams.snCount <= adjust_table[idx].session_num
			&& d->dev.exactRate == adjust_table[idx].frame_rate
			&& d->dev.rateGbps == adjust_table[idx].gbps)
		{
			d->adjust = adjust_table[idx].adjust;
			return ST_OK;
		}
	}
	d->adjust = 0;
	return ST_OK;
}

/* TODO
 * did not consider ST30 and ST40, and need to refine it
 */
st_status_t
StDevCalculateBudgets(st_device_impl_t *d, int num_port)
{
	// check number of sessions and other fields for valid values
	if (d->dev.maxSt21Sessions == 0)
		return ST_INVALID_PARAM;

	uint32_t quotAdjust;
	switch (d->dev.rateGbps)
	{
	case 10:
		quotAdjust = ST_ADJUST_10GBPS;
		break;
	case 25:
		quotAdjust = ST_ADJUST_25GBPS;
		break;
	case 40:
		quotAdjust = ST_ADJUST_40GBPS;
		break;
	case 100:
		quotAdjust = ST_ADJUST_100GBPS;
		break;
	default:
		return ST_DEV_BAD_NIC_RATE;
	}

	/* calcuate available quot */
	uint32_t frameTime = StDevGetFrameTime(d->dev.exactRate);  // in ns
	uint64_t quotBase = ((uint64_t)frameTime * d->dev.rateGbps * quotAdjust);
	uint32_t pktSlotsInFrame;

	// dispatch pacing
	switch (d->dev.pacerType)
	{
	case ST_2110_21_TPN:
		pktSlotsInFrame = ST_DEFAULT_PKTS_IN_FRAME_GAPPED;
		break;
	case ST_2110_21_TPNL:
	case ST_2110_21_TPW:
		pktSlotsInFrame = ST_DEFAULT_PKTS_IN_FRAME_LINEAR;
		break;
	default:
		return ST_DEV_BAD_PACING;
	}

	quotBase = quotBase / num_port;

	if (StDevGetInterlaced(d->dev.exactRate) || d->fmtIndex == 0)
	{
		pktSlotsInFrame /= 2;
	}

	d->quot = (quotBase / 8 / pktSlotsInFrame) / ST_DENOM_DEFAULT;
	d->remaind = (quotBase / 8 / pktSlotsInFrame) % ST_DENOM_DEFAULT;
	if (num_port == MAX_RXTX_PORTS)
	    d->dev.maxSt21Sessions = MIN(d->dev.maxSt21Sessions, (StDevGetNicMaxSessions(d) + 1)/MAX_RXTX_PORTS);
	else
	    d->dev.maxSt21Sessions = MIN(d->dev.maxSt21Sessions, StDevGetNicMaxSessions(d));

	uint32_t maxRings;
	if (d->quot < d->dev.maxSt21Sessions * ST_HD_422_10_SLN_L1_SZ)
	    maxRings = d->dev.maxSt21Sessions;
	else
	    maxRings
		= d->dev.maxSt21Sessions
		  + (d->quot - d->dev.maxSt21Sessions * ST_HD_422_10_SLN_L1_SZ) / ST_DEFAULT_PKT_L1_SZ;

	if (maxRings == d->dev.maxSt21Sessions)
	{
		d->dev.maxSt21Sessions--;
		RTE_LOG(
			INFO, USER1,
			"StDevCalculateBudgets adjust maxSessions to %u since maxRings is too small of %u\n",
			d->dev.maxSt21Sessions, maxRings);
	}

	int outOfBoundRingBytes = (int)d->quot - (int)(d->dev.maxSt21Sessions * ST_HD_422_10_SLN_L1_SZ)
							  - (int)((maxRings - d->dev.maxSt21Sessions) * ST_DEFAULT_PKT_L1_SZ);

	RTE_LOG(INFO, USER1, "ST21 Sessions Out of bound ring budget: %d\n", outOfBoundRingBytes);
	d->outOfBoundRing = outOfBoundRingBytes < 0 ? 0: 1;
	d->maxRings = maxRings;

	/* TODO just hardcode the audio session number */
	/* need to re-compute if audio and video essence does not match */
	d->dev.maxSt30Sessions = d->dev.maxSt21Sessions;
	d->dev.maxSt40Sessions = d->dev.maxSt21Sessions;
	RTE_LOG(INFO, USER1,
			"ST21 Sessions max count is %u Rings count is %u, Out of bound ring is %s\n",
			d->dev.maxSt21Sessions, d->maxRings, d->outOfBoundRing ? "on" : "off");

	StDevGetAdjust(d, num_port);

	return ST_OK;
}

void
SigHandler(int signo)
{
	RTE_LOG(INFO, USER1, "%s, signal %d\n", __func__, signo);
	switch (signo)
	{
	case SIGINT: /* Interrupt from keyboard */
		rte_atomic32_set(&isStopMainThreadTasks, 1);
		break;
	}

	return;
}

void
StDevInitTxThreads(st_main_params_t *mp, st_device_impl_t *dev)
{
	if ((!dev) || (!mp))
		ST_ASSERT;
	if (!stDevParams)
		ST_ASSERT;

	// count lcores to initialize threading structures
	uint32_t thrdCount = 0;
	int i = 0;
	RTE_LCORE_FOREACH_WORKER(i)
	{
		thrdCount++;
	}

	thrdCount -= ST_KNI_THEARD;

	if (mp->pTx == 1 || mp->rTx == 1)
	{

		mp->maxSchThrds = stDevParams->maxSchThrds;
		if (mp->pTx == 1 && mp->rTx == 1)
		    mp->maxSchThrds = 1;
		if (mp->pacing == ST_PACING_TSC)
		{
			if (mp->snCount <= dev->dev.maxSt21Sessions/2)
				mp->maxSchThrds = 1;
		}

		RTE_LOG(INFO, USER1, "%s, snCount %d enqueue threads %d\n", __func__, mp->snCount, mp->maxEnqThrds);
		uint32_t perThrdSnCount = RTE_MIN(dev->dev.maxSt21Sessions, mp->snCount) / mp->maxEnqThrds;
		uint32_t countRemaind = RTE_MIN(dev->dev.maxSt21Sessions, mp->snCount) % mp->maxEnqThrds;

		if ((perThrdSnCount == 0) && (countRemaind == 0))
			rte_exit(ST_INVALID_PARAM, "Minimum expected video session is 1!\n");

		if (perThrdSnCount == 0)
		{
			perThrdSnCount = 1;
			mp->maxEnqThrds = countRemaind;
			countRemaind = 0;
		}
		int nextSn = 0;
		for (int i = 0; i < mp->maxEnqThrds; ++i)
		{
			if (i == (mp->maxEnqThrds - countRemaind))
				perThrdSnCount++;
			mp->enqThrds[i].thrdSnFirst = nextSn;
			mp->enqThrds[i].thrdSnLast = nextSn + perThrdSnCount;
			nextSn += perThrdSnCount;
		}

		for (uint32_t i = 0; i < mp->maxEnqThrds; i++)
		{
			mp->enqThrds[i].pktsCount
				= ST_DEFAULT_PKTS_IN_LN
				  * (mp->enqThrds[i].thrdSnLast - mp->enqThrds[i].thrdSnFirst);
		}
	}
}

void
StDevInitRxThreads(st_main_params_t *mp, st_device_impl_t *dev)
{
	if ((!dev) || (!mp))
		ST_ASSERT;
	if (!stDevParams)
		ST_ASSERT;

	mp->maxRcvThrds = stDevParams->maxRcvThrds;
	mp->maxAudioRcvThrds = stDevParams->maxAudioRcvThrds;
	mp->maxAncRcvThrds = stDevParams->maxAncRcvThrds;

	if (mp->pRx == 1 || mp->rRx == 1)
	{
		uint32_t perThrdSnCount = RTE_MIN(dev->dev.maxSt21Sessions, mp->snCount) / mp->maxRcvThrds;
		uint32_t countRemaind = RTE_MIN(dev->dev.maxSt21Sessions, mp->snCount) % mp->maxRcvThrds;
		if ((perThrdSnCount == 0) && (countRemaind == 0))
			rte_exit(ST_INVALID_PARAM, "Minimum expected video session is 1!\n");

		if (perThrdSnCount == 0)
		{
			perThrdSnCount = 1;
			mp->maxRcvThrds = countRemaind;
			countRemaind = 0;
		}
		int nextSn = 0;
		for (int i = 0; i < mp->maxRcvThrds; ++i)
		{
			if (i == (mp->maxRcvThrds - countRemaind))
				perThrdSnCount++;
			mp->rcvThrds[i].thrdSnFirst = nextSn;
			mp->rcvThrds[i].thrdSnLast = nextSn + perThrdSnCount;
			nextSn += perThrdSnCount;
		}

		perThrdSnCount = RTE_MIN(dev->dev.maxSt30Sessions, mp->sn30Count) / mp->maxAudioRcvThrds;
		countRemaind = RTE_MIN(dev->dev.maxSt30Sessions, mp->sn30Count) % mp->maxAudioRcvThrds;

		if ((perThrdSnCount == 0) && (countRemaind))
		{
			perThrdSnCount = 1;
			mp->maxAudioRcvThrds = countRemaind;
			countRemaind = 0;
		}
		nextSn = 0;
		for (int i = 0; (perThrdSnCount) && (i < mp->maxAudioRcvThrds); ++i)
		{
			if (i == (mp->maxAudioRcvThrds - countRemaind))
				perThrdSnCount++;
			mp->audioRcvThrds[i].thrdSnFirst = nextSn;
			mp->audioRcvThrds[i].thrdSnLast = nextSn + perThrdSnCount;
			nextSn += perThrdSnCount;
		}

		perThrdSnCount = RTE_MIN(dev->dev.maxSt40Sessions, mp->sn40Count) / mp->maxAncRcvThrds;
		countRemaind = RTE_MIN(dev->dev.maxSt40Sessions, mp->sn40Count) % mp->maxAncRcvThrds;

		if ((perThrdSnCount == 0) && (countRemaind))
		{
			perThrdSnCount = 1;
			mp->maxAncRcvThrds = countRemaind;
			countRemaind = 0;
		}
		nextSn = 0;
		for (int i = 0; (perThrdSnCount) && (i < mp->maxAncRcvThrds); ++i)
		{
			if (i == (mp->maxAncRcvThrds - countRemaind))
				perThrdSnCount++;
			mp->ancRcvThrds[i].thrdSnFirst = nextSn;
			mp->ancRcvThrds[i].thrdSnLast = nextSn + perThrdSnCount;
			nextSn += perThrdSnCount;
		}
	}

	if (signal(SIGINT, SigHandler) == SIG_ERR)
		printf("\ncan't catch SIGINT\n");
	signal(SIGUSR1, SigHandler);
}
