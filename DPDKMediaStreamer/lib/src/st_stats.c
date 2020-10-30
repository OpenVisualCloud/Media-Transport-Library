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

#include "st_stats.h"

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

#define MIN_START_PER (4.0)
#define MIN_PER (9.8)
#define MIN_PER_FOR_MIN (15.0)
#define RATE_UNIT (1e6)
#define MIN_RATE_NOTVALID (1e50)

static int StStsInit_(uint16_t portId);
static void StStsPrint_(uint16_t portId);

void
StStsTask(uint16_t portId)
{
	if (StStsInit_(portId) < 0)
		return;
	StStsPrint_(portId);
}

struct rte_eth_stats glbStats;
static uint64_t firstTicks;
static uint64_t lastTicks;
static uint64_t freqTicks;
static int isInit = 0;
static int nbRead = 1;

static double oMinRate = MIN_RATE_NOTVALID, iMinRate = MIN_RATE_NOTVALID;
static double oMaxRate = 0.0, iMaxRate = 0.0;

static int
StStsInit_(uint16_t portId)
{
	if (isInit)
		return 0;
	freqTicks = rte_get_tsc_hz();
	if (freqTicks == 0)
		return -1;
	lastTicks = rte_get_tsc_cycles();
	if (lastTicks == 0)
		return -1;
	if (firstTicks == 0)
	{
		firstTicks = lastTicks;
		return -1;
	}
	double per = 1.0 / freqTicks * (lastTicks - firstTicks);
	if (per < MIN_START_PER)
		return -1;
	if (rte_eth_stats_reset(portId))
		return -1;
	firstTicks = lastTicks;
	isInit = 1;
	return -1;
}

static void
StStsPrint_(uint16_t portId)
{
	struct rte_eth_stats stats;
	uint64_t currTicks = rte_get_tsc_cycles();
	double per = 1.0 / freqTicks * (currTicks - lastTicks);

	if (per < MIN_PER)
		return;

	double perGlob = 1.0 / freqTicks * (currTicks - firstTicks);

	lastTicks = currTicks;

	rte_eth_stats_get(portId, &stats);
	rte_eth_stats_reset(portId);

	double oRate = stats.obytes * 8, iRate = stats.ibytes * 8;
	oRate /= per;
	iRate /= per;

	glbStats.obytes += stats.obytes;
	glbStats.ibytes += stats.ibytes;

	double oMidRate = glbStats.obytes * 8, iMidRate = glbStats.ibytes * 8;
	oMidRate /= perGlob;
	iMidRate /= perGlob;

	if (perGlob > MIN_PER_FOR_MIN)
	{
		if (oMinRate > oRate)
			oMinRate = oRate;
		if (iMinRate > iRate)
			iMinRate = iRate;
	}

	if (oMaxRate < oRate)
		oMaxRate = oRate;
	if (iMaxRate < iRate)
		iMaxRate = iRate;

	printf("\n* * * *    B I T   R A T E S    * * * * \n");
	printf("NB: %d\n", nbRead);
	printf("Last 10s Tx: %10.2lf [Mb/s]\n", oRate / RATE_UNIT);
	printf("Last 10s Rx: %10.2lf [Mb/s]\n", iRate / RATE_UNIT);
	if (perGlob > MIN_PER_FOR_MIN && oMinRate < MIN_RATE_NOTVALID)
		printf("Min Tx:      %10.2lf [Mb/s]\n", oMinRate / RATE_UNIT);
	else
		printf("Min Tx:      %17s\n", "NOT VALID");
	if (perGlob > MIN_PER_FOR_MIN && iMinRate < MIN_RATE_NOTVALID)
		printf("Min Rx:      %10.2lf [Mb/s]\n", iMinRate / RATE_UNIT);
	else
		printf("Min Rx:      %17s\n", "NOT VALID");

	printf("Avr Tx:      %10.2lf [Mb/s]\n", oMidRate / RATE_UNIT);
	printf("Avr Rx:      %10.2lf [Mb/s]\n", iMidRate / RATE_UNIT);
	printf("Max Tx:      %10.2lf [Mb/s]\n", oMaxRate / RATE_UNIT);
	printf("Max Rx:      %10.2lf [Mb/s]\n", iMaxRate / RATE_UNIT);
	printf("* *    E N D    B I T   R A T E S   * * \n\n");
	nbRead++;
	return;
}
