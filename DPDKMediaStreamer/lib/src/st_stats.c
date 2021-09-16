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

#include "st_stats.h"
#include "st_api.h"

#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_bus_pci.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_debug.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_interrupts.h>
#include <rte_kni.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_memcpy.h>
#include <rte_memory.h>
#include <rte_mempool.h>
#include <rte_per_lcore.h>
#include <rte_string_fns.h>

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/queue.h>
#include <unistd.h>

#include "rvrtp_main.h"

#define MIN_START_PER (4.0)
#define MIN_PER (9.8)
#define MIN_PER_FOR_MIN (15.0)
#define RATE_UNIT (1e6)
#define MIN_RATE_NOTVALID (1e50)
#define MAX_RXTX_PORTS 2
static int StStsInit_(uint16_t numPorts);
static void StStsPrint_(uint16_t portId);

void
StStsTask(uint16_t numPorts)
{
	for (uint16_t portId = 0; portId < numPorts; ++portId)
	{
		if (StStsInit_(portId) < 0)
			return;
		StStsPrint_(portId);
	}
}

struct rte_eth_stats glbStats[MAX_RXTX_PORTS] __rte_cache_aligned;
static uint64_t firstTicks[MAX_RXTX_PORTS];
static uint64_t lastTicks[MAX_RXTX_PORTS];
static uint64_t freqTicks[MAX_RXTX_PORTS];
static int isInit[MAX_RXTX_PORTS] = { 0 };
static uint64_t nbRead = 1;

static double oMinRate[MAX_RXTX_PORTS] = { MIN_RATE_NOTVALID, MIN_RATE_NOTVALID };
static double iMinRate[MAX_RXTX_PORTS] = { MIN_RATE_NOTVALID, MIN_RATE_NOTVALID };
static double oMaxRate[MAX_RXTX_PORTS] = { 0.0, 0.0 }, iMaxRate[MAX_RXTX_PORTS] = { 0.0, 0.0 };

static int
StStsInit_(uint16_t portId)
{
	if (isInit[portId])
		return 0;
	freqTicks[portId] = rte_get_tsc_hz();
	if (freqTicks[portId] == 0)
		return -1;
	lastTicks[portId] = rte_get_tsc_cycles();
	if (lastTicks[portId] == 0)
		return -1;
	if (firstTicks[portId] == 0)
	{
		firstTicks[portId] = lastTicks[portId];
		return -1;
	}
	double per = 1.0 / freqTicks[portId] * (lastTicks[portId] - firstTicks[portId]);
	if (per < MIN_START_PER)
		return -1;
	if (rte_eth_stats_reset(portId))
		return -1;
	firstTicks[portId] = lastTicks[portId];
	isInit[portId] = 1;
	return -1;
}

static void
StStsPrintPacingEpochMismatch(st_device_impl_t *dTx)
{
	st_session_impl_t *s;
	rvrtp_pacing_t *pacing;

	for (uint32_t j = 0; j < stMainParams.snCount; j++)
	{
		s = dTx->snTable[j];
		if (!s)
		{
			break;
		}

		pacing = &s->pacing;
		if (pacing->epochMismatch)
		{
			printf("Session %02u, epoch mismatch %u\n", j, pacing->epochMismatch);
			pacing->epochMismatch = 0;
		}
	}
}

static void
StStsPrintTscPacing(uint16_t portId)
{
	st_device_impl_t *dTx = &stSendDevice;
	st_session_impl_t *s;
	rvrtp_pacing_t *pacing;

	for (uint32_t j = 0; j < stMainParams.snCount; j++)
	{
		s = dTx->snTable[j];
		if (!s)
		{
			break;
		}
		pacing = &s->pacing;

		if (nbRead > 6)
		{
			if (dTx->pacingDeltaMax[portId][j] > dTx->pacingUpDeltaMax[portId][j])
			{
				dTx->pacingUpDeltaMax[portId][j] = dTx->pacingDeltaMax[portId][j];
			}

			if (dTx->pacingDeltaMax[portId][j] > (pacing->vrx * pacing->trs))
			{
				dTx->pacingVrxCnt[portId][j]++;
			}
		}

		printf("Pacingdelta for TX port %u ring %02u, upMax %lu Vrx %lu, Cnt %lu Max %lu Avg %lu\n",
			portId, j,
			dTx->pacingUpDeltaMax[portId][j],
			dTx->pacingVrxCnt[portId][j],
			dTx->pacingDeltaCnt[portId][j],
			dTx->pacingDeltaMax[portId][j],
			dTx->pacingDeltaCnt[portId][j] ? dTx->pacingDeltaSum[portId][j] / dTx->pacingDeltaCnt[portId][j] : 0);

		dTx->pacingDeltaCnt[portId][j] = 0;
		dTx->pacingDeltaMax[portId][j] = 0;
		dTx->pacingDeltaSum[portId][j] = 0;
	}

	if (portId == 0)
	{
		StStsPrintPacingEpochMismatch(dTx);
	}
}

static void
StStsPrintNicRlPacing(uint16_t portId)
{
	st_device_impl_t *dTx = &stSendDevice;

	for (uint32_t j = 0; j < stMainParams.snCount; j++)
	{
		uint64_t vrx = dTx->pacingVrxCnt[portId][j];
		if (vrx)
		{
			printf("Pacinginfo for TX port %u ring %02u, Vrx %lu\n", portId, j, vrx);
		}
	}

	if (portId == 0)
	{
		StStsPrintPacingEpochMismatch(dTx);
	}
}

static void
StStsPrint_(uint16_t portId)
{
	if (rte_eth_dev_count_avail() == 0)
		return;

	struct rte_eth_stats stats;
	uint64_t currTicks = rte_get_tsc_cycles();
	double per = (double)(currTicks - lastTicks[portId]) / (double)freqTicks[portId];

	if (per < MIN_PER || portId > 1)
		return;

	double perGlob = (double)(currTicks - firstTicks[portId]) / (double)freqTicks[portId];

	lastTicks[portId] = currTicks;

	rte_eth_stats_get(portId, &stats);
	rte_eth_stats_reset(portId);

	double oRate = (double)(stats.obytes * 8), iRate = (double)(stats.ibytes * 8);
	oRate /= per;
	iRate /= per;

	glbStats[portId].obytes += stats.obytes;
	glbStats[portId].ibytes += stats.ibytes;

	double oMidRate = glbStats[portId].obytes * 8, iMidRate = glbStats[portId].ibytes * 8;
	oMidRate /= perGlob;
	iMidRate /= perGlob;

	if (perGlob > MIN_PER_FOR_MIN)
	{
		if (oMinRate[portId] > oRate)
			oMinRate[portId] = oRate;
		if (iMinRate[portId] > iRate)
			iMinRate[portId] = iRate;
	}

	if (oMaxRate[portId] < oRate)
		oMaxRate[portId] = oRate;
	if (iMaxRate[portId] < iRate)
		iMaxRate[portId] = iRate;

	printf("\n* * * *    B I T   R A T E S  Port %d  * * * * \n", portId);
	printf("NB: %ld\n", nbRead);
	printf("Last 10s Tx: %10.2lf [Mb/s]\n", oRate / RATE_UNIT);
	printf("Last 10s Rx: %10.2lf [Mb/s]\n", iRate / RATE_UNIT);
	if (perGlob > MIN_PER_FOR_MIN && oMinRate[portId] < MIN_RATE_NOTVALID)
		printf("Min Tx:      %10.2lf [Mb/s]\n", oMinRate[portId] / RATE_UNIT);
	else
		printf("Min Tx:      %17s\n", "NOT VALID");
	if (perGlob > MIN_PER_FOR_MIN && iMinRate[portId] < MIN_RATE_NOTVALID)
		printf("Min Rx:      %10.2lf [Mb/s]\n", iMinRate[portId] / RATE_UNIT);
	else
		printf("Min Rx:      %17s\n", "NOT VALID");

	printf("Avr Tx:      %10.2lf [Mb/s]\n", oMidRate / RATE_UNIT);
	printf("Avr Rx:      %10.2lf [Mb/s]\n", iMidRate / RATE_UNIT);
	printf("Max Tx:      %10.2lf [Mb/s]\n", oMaxRate[portId] / RATE_UNIT);
	printf("Max Rx:      %10.2lf [Mb/s]\n", iMaxRate[portId] / RATE_UNIT);
	printf("Status: imissed %ld ierrors %ld oerrors %ld rx_nombuf %ld\n",
			stats.imissed, stats.ierrors, stats.oerrors, stats.rx_nombuf);
	if ((stMainParams.pTx == 1 || stMainParams.rTx == 1))
	{
		if (StIsTscPacing())
		{
			StStsPrintTscPacing(portId);
		}
		else if (StIsNicRlPacing())
		{
			StStsPrintNicRlPacing(portId);
		}
	}
	printf("* *    E N D    B I T   R A T E S   * * \n\n");
	nbRead++;
	return;
}
