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

st_kni_ms_conf_t *kniDev1 = NULL;
st_kni_ms_conf_t *kniDev2 = NULL;

#define MAX_VER_SRING 64
char *stBuildVersionStr = BUILD;
char stLibVersionStr[MAX_STR_LEN];


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
		snprintf(stLibVersionStr, sizeof(stLibVersionStr), "%d.%d.%d", 
			     LIB_VERSION_MAJOR, LIB_VERSION_MINOR, LIB_VERSION_LAST);
		(*val).strPtr = stLibVersionStr;
		break;
	default:
		RTE_LOG(INFO, USER1, "Unknown param: %d\n", prm);
		return ST_INVALID_PARAM;
	}

	return ST_OK;
}

st_status_t
StSetParam(st_param_t prm, st_param_val_t val)
{
	switch (prm)
	{
	case ST_SOURCE_IP:
		memcpy(stMainParams.sipAddr, (uint8_t *)&(val.valueU32), IP_ADDR_LEN);
		break;
	case ST_DESTINATION_IP:
		memcpy(stMainParams.ipAddr, (uint8_t *)&(val.valueU32), IP_ADDR_LEN);
		break;
	case ST_EBU_TEST:
		stMainParams.isEbuCheck = val.valueU64;
		break;
	case ST_SN_COUNT:
		stMainParams.snCount = val.valueU64;
		break;
	case ST_TX_ONLY:
		stMainParams.txOnly = val.valueU64;
		break;
	case ST_RX_ONLY:
		stMainParams.rxOnly = val.valueU64;
		break;
	case ST_MAC:
		memcpy(stMainParams.macAddr, (uint8_t *)&val.valueU64, MAC_ADDR_LEN);
		break;
	case ST_OUT_PORT:
		snprintf(stMainParams.outPortName, sizeof(stMainParams.outPortName), "%s", val.strPtr);
		break;
	case ST_IN_PORT:
		snprintf(stMainParams.inPortName, sizeof(stMainParams.inPortName), "%s", val.strPtr);
		break;
	case ST_FMT_INDEX:
		stMainParams.fmtIndex = val.valueU64;
		break;
	case ST_DPDK_PARAMS:
		snprintf(stMainParams.dpdkParams, sizeof(stMainParams.dpdkParams), "%s", val.strPtr);
		break;
	default:
		RTE_LOG(INFO, USER1, "Unknown param: %d\n", prm);
		return ST_INVALID_PARAM;
	}

	return ST_OK;
}

uint32_t
StDevGetNicMaxSessions(rvrtp_device_t *d)
{
	if (!d)	ST_ASSERT;
	if (!stDevParams) ST_ASSERT;

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

st_status_t
StDevCalculateBudgets(rvrtp_device_t *d)
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

	if (StDevGetInterlaced(d->dev.exactRate) || d->fmtIndex == 0)
	{
		pktSlotsInFrame /= 2;
	}

	d->quot = (quotBase / 8 / pktSlotsInFrame) / ST_DENOM_DEFAULT;
	d->remaind = (quotBase / 8 / pktSlotsInFrame) % ST_DENOM_DEFAULT;

	d->dev.maxSt21Sessions = MIN(d->dev.maxSt21Sessions, StDevGetNicMaxSessions(d));

	// now calcuate ring count
	uint32_t maxRings
		= d->dev.maxSt21Sessions
		  + (d->quot - d->dev.maxSt21Sessions * ST_HD_422_10_SLN_L1_SZ) / ST_DEFAULT_PKT_L1_SZ;

	if (maxRings == d->dev.maxSt21Sessions)
	{
		d->dev.maxSt21Sessions--;
		RTE_LOG(INFO, USER1, "StDevCalculateBudgets adjust maxSessions to %u since maxRings is too small of %u\n",
			d->dev.maxSt21Sessions, maxRings);
	}

	int outOfBoundRingBytes = (int)d->quot - (int)(d->dev.maxSt21Sessions * ST_HD_422_10_SLN_L1_SZ)
		- (int)((maxRings - d->dev.maxSt21Sessions) * ST_DEFAULT_PKT_L1_SZ);

	RTE_LOG(INFO, USER1, "ST21 Sessions Out of bound ring budget: %d\n", outOfBoundRingBytes);
	d->outOfBoundRing = 1;
	d->maxRings = maxRings;

	RTE_LOG(INFO, USER1, "ST21 Sessions max count is %u Rings count is %u, Out of bound ring is %s\n",
		d->dev.maxSt21Sessions, d->maxRings, d->outOfBoundRing ? "on" : "off");

	return ST_OK;
}

void
SigHandler(int signo)
{
extern uint64_t adjustCount[6];

	rvrtp_device_t *dRx = &stRecvDevice;
	rvrtp_device_t *dTx = &stSendDevice;

	printf("----------------------------------------\n");
	printf("ALIGNMENT COUNTS: \n");
	printf(" adjustCount[0] %lu adjustCount[1] %lu adjustCount[2] %lu\n", adjustCount[0], adjustCount[1], adjustCount[2]);
	printf(" adjustCount[3] %lu adjustCount[4] %lu adjustCount[5] %lu\n", adjustCount[3], adjustCount[4], adjustCount[5]);
	printf("----------------------------------------\n");
	{
		printf("SN TABLE: \n");
		for (uint32_t i = 0; i < dRx->dev.maxSt21Sessions; i++)
		{
			if (!dRx->snTable[i])
				continue;

			printf(" RX sn %u pRx %lu pDrop %lu fRx %lu fDrop %lu fixes %lu\n", i,
				   dRx->snTable[i]->sn.pktsRecv, dRx->snTable[i]->pktsDrop,
				   dRx->snTable[i]->sn.frmsRecv, dRx->snTable[i]->frmsDrop,
				   dRx->snTable[i]->frmsFixed);
		}
		printf("TX scheduler stats:\n");
		printf(" TX rings pktsBuild %lu, pktsEnq %lu\n", pktsBuild, pktsQueued);
		if ((dTx->packetsTx) && (dTx->pausesTx))
		{
			for (uint32_t i = 0; i <= dTx->maxRings; i++)
			{
				printf(" TX ring %u packetsTx %lu pausesTx %lu\n", i, dTx->packetsTx[i],
					   dTx->pausesTx[i]);
			}
		}
	}
	if (stSendDevice.snTable && stSendDevice.snTable[0] && !stMainParams.rxOnly)
	{
		fprintf(stderr, "DEST_MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
				stSendDevice.snTable[0]->hdrPrint.dualHdr.eth.d_addr.addr_bytes[0],
				stSendDevice.snTable[0]->hdrPrint.dualHdr.eth.d_addr.addr_bytes[1],
				stSendDevice.snTable[0]->hdrPrint.dualHdr.eth.d_addr.addr_bytes[2],
				stSendDevice.snTable[0]->hdrPrint.dualHdr.eth.d_addr.addr_bytes[3],
				stSendDevice.snTable[0]->hdrPrint.dualHdr.eth.d_addr.addr_bytes[4],
				stSendDevice.snTable[0]->hdrPrint.dualHdr.eth.d_addr.addr_bytes[5]);
	}
	switch (signo)
	{
	case SIGUSR1:
		signal(SIGUSR1, SigHandler);
		return;
	}
	return;
}

void
StDevInitTxThreads(st_main_params_t *mp, rvrtp_device_t *dev)
{
	if ((!dev) || (!mp))
		ST_ASSERT;
	if (!stDevParams)
		ST_ASSERT;

	// count lcores to initialize threading structures
	uint32_t thrdCount = 0;
	int i = 0;
	RTE_LCORE_FOREACH_SLAVE(i) { thrdCount++; }

	thrdCount -= ST_KNI_THEARD;

	mp->maxSchThrds = stDevParams->maxSchThrds;
	mp->maxEnqThrds = thrdCount - mp->maxSchThrds - stDevParams->maxRcvThrds;

	if (mp->maxEnqThrds != stDevParams->maxEnqThrds)
	{
		rte_exit(ST_INVALID_PARAM,
			"Invalid number of enq threads of %u for available number of sessions, shall be %u\n",
			mp->maxEnqThrds, stDevParams->maxEnqThrds);
	}

	uint32_t perThrdSnCount = dev->dev.maxSt21Sessions / mp->maxEnqThrds;
	uint32_t countRemaind = dev->dev.maxSt21Sessions % mp->maxEnqThrds;

	if (countRemaind > perThrdSnCount)
	{
		rte_exit(ST_INVALID_PARAM,
				 "Invalid number of enq threads of %u per remaind of %u for available number of "
				 "sessions\n",
				 mp->maxEnqThrds, countRemaind);
	}
	if (countRemaind >= mp->maxEnqThrds/2) perThrdSnCount++;

	for (uint32_t i = 0; i < mp->maxEnqThrds; i++)
	{
		mp->enqThrds[i].thrdSnFirst = i * perThrdSnCount;
		mp->enqThrds[i].thrdSnLast = MIN((i + 1) * perThrdSnCount, dev->dev.maxSt21Sessions);
	}
	if (mp->enqThrds[mp->maxEnqThrds - 1].thrdSnLast < dev->dev.maxSt21Sessions)
	{
		mp->enqThrds[mp->maxEnqThrds - 1].thrdSnLast = dev->dev.maxSt21Sessions;
	}
	for (uint32_t i = 0; i < mp->maxEnqThrds; i++)
	{
		mp->enqThrds[i].pktsCount =
			ST_DEFAULT_PKTS_IN_LN * (mp->enqThrds[i].thrdSnLast - mp->enqThrds[i].thrdSnFirst);
	}
}

void
StDevInitRxThreads(st_main_params_t *mp, rvrtp_device_t *dev)
{
	if ((!dev) || (!mp))
		ST_ASSERT;
	if (!stDevParams)
		ST_ASSERT;

	mp->maxRcvThrds = stDevParams->maxRcvThrds;

	uint32_t perThrdSnCount = dev->dev.maxSt21Sessions / mp->maxRcvThrds;
	uint32_t countRemaind = dev->dev.maxSt21Sessions % mp->maxRcvThrds;

	if (countRemaind >= mp->maxRcvThrds/2) perThrdSnCount++;

	for (uint32_t i = 0; i < mp->maxRcvThrds; i++)
	{
		mp->rcvThrds[i].thrdSnFirst = i * perThrdSnCount;
		mp->rcvThrds[i].thrdSnLast = MIN((i + 1) * perThrdSnCount, dev->dev.maxSt21Sessions);

		RTE_LOG(INFO, USER1, "Receiver thread of %u firstSn %u last Sn %u\n", i,
				mp->rcvThrds[i].thrdSnFirst, mp->rcvThrds[i].thrdSnLast);
	}
	if (mp->rcvThrds[mp->maxRcvThrds - 1].thrdSnLast < dev->dev.maxSt21Sessions)
	{
		mp->rcvThrds[mp->maxRcvThrds - 1].thrdSnLast = dev->dev.maxSt21Sessions;
	}

	if (signal(SIGINT, SigHandler) == SIG_ERR)
		printf("\ncan't catch SIGINT\n");
	signal(SIGUSR1, SigHandler);

}