/* * Copyright (C) 2020-2021 Intel Corporation.
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

#define _GNU_SOURCE

#include "dpdk_common.h"
#include "st_api.h"
#include "st_arp.h"
#include "st_pkt.h"
#include "st_stats.h"

#include <fcntl.h>
#include <numa.h>
#include <rvrtp_main.h>
#include <sched.h>
#include <st_api.h>
#include <st_kni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#define ST_PCIE_TEMPL ("SSSS:BB:DD.F")
#define ST_PCIE_ADDR_LEN sizeof(ST_PCIE_TEMPL)
#define ST_PCIE_ADDR_FORM "%04x:%02x:%02x.%01x"
#define ST_PCIE_SEC_SOC_BUS (0x80)
#define ST_FREE_LCORES_ON_CPU (2)
#define ST_MIN_NUMA_PAGE_SIZE (1024ll * 1024 * 1024)
#define ST_MIN_NUMA_PAGES (2)
#define ST_MIN_NUMA_SIZE (ST_MIN_NUMA_PAGES * ST_MIN_NUMA_PAGE_SIZE)

#define UIO_MOD "uio"
#define IGB_UIO_MOD "igb_uio"
#define RTE_KNI_MOD "rte_kni"
#define MLX4_CORE_MOD "mlx4_core"
#define MLX5_CORE_MOD "mlx5_core"

#define PATH_MOD "/lib/modules/$(uname -r)/extra/dpdk"
#define MOD_EXT ".ko"
#define CMD_LINE_LEN (1024)
#define ST_DEV_CRIT_ERR(err, info)                                                                 \
	rte_exit(err, "%s\n    File: %s, function: %s, line %u\n", info, __FILE__, __FUNCTION__,       \
			 __LINE__)

#define RTE_LOGTYPE_ST_DEV (RTE_LOGTYPE_USER1)
#define ST_DEV_ERROR "[ERROR] "

userargs_t func_args[RTE_MAX_LCORE];
/* array to hold dynamic offset for timestamp support NIC */
int hwts_dynfield_offset[RTE_MAX_ETHPORTS];
static lcore_transmitter_args_t transmitter_thread_args[RTE_MAX_LCORE];

const static st_nic_rate_params_t stNicParamsTable[NIC_RATE_SPEED_COUNT]
	= { { ST_NIC_RATE_SPEED_10GBPS, ST_MAX_SESSIONS_25FPS_10GBPS, ST_MAX_SESSIONS_29FPS_10GBPS,
		  ST_MAX_SESSIONS_50FPS_10GBPS, ST_MAX_SESSIONS_59FPS_10GBPS, ST_MAX_TX_RINGS_10GBPS,
		  ST_MAX_RX_RINGS_10GBPS, ST_MAX_SCH_THREADS_10GBPS, ST_MAX_ENQ_THREADS_10GBPS,
		  ST_MAX_RCV_THREADS_10GBPS, ST_MAX_AUDIO_RCV_THREADS_10GBPS, ST_MAX_ANC_RCV_THREADS_10GBPS,
		  ST_MAX_TX_BULK_NUM_10GBPS },
		{ ST_NIC_RATE_SPEED_25GBPS, ST_MAX_SESSIONS_25FPS_25GBPS, ST_MAX_SESSIONS_29FPS_25GBPS,
		  ST_MAX_SESSIONS_50FPS_25GBPS, ST_MAX_SESSIONS_59FPS_25GBPS, ST_MAX_TX_RINGS_25GBPS,
		  ST_MAX_RX_RINGS_25GBPS, ST_MAX_SCH_THREADS_25GBPS, ST_MAX_ENQ_THREADS_25GBPS,
		  ST_MAX_RCV_THREADS_25GBPS, ST_MAX_AUDIO_RCV_THREADS_25GBPS, ST_MAX_ANC_RCV_THREADS_25GBPS,
		  ST_MAX_TX_BULK_NUM_25GBPS },
		{ ST_NIC_RATE_SPEED_40GBPS, ST_MAX_SESSIONS_25FPS_40GBPS, ST_MAX_SESSIONS_29FPS_40GBPS,
		  ST_MAX_SESSIONS_50FPS_40GBPS, ST_MAX_SESSIONS_59FPS_40GBPS, ST_MAX_TX_RINGS_40GBPS,
		  ST_MAX_RX_RINGS_40GBPS, ST_MAX_SCH_THREADS_40GBPS, ST_MAX_ENQ_THREADS_40GBPS,
		  ST_MAX_RCV_THREADS_40GBPS, ST_MAX_AUDIO_RCV_THREADS_40GBPS, ST_MAX_ANC_RCV_THREADS_40GBPS,
		  ST_MAX_TX_BULK_NUM_40GBPS },
		{ ST_NIC_RATE_SPEED_100GBPS, ST_MAX_SESSIONS_25FPS_100GBPS, ST_MAX_SESSIONS_29FPS_100GBPS,
		  ST_MAX_SESSIONS_50FPS_100GBPS, ST_MAX_SESSIONS_59FPS_100GBPS, ST_MAX_TX_RINGS_100GBPS,
		  ST_MAX_RX_RINGS_100GBPS, ST_MAX_SCH_THREADS_100GBPS, ST_MAX_ENQ_THREADS_100GBPS,
		  ST_MAX_RCV_THREADS_100GBPS, ST_MAX_AUDIO_RCV_THREADS_100GBPS,
		  ST_MAX_ANC_RCV_THREADS_100GBPS, ST_MAX_TX_BULK_NUM_100GBPS } };

static unsigned short getNicNuma(char *nicAddr);
static int getPowerCore(unsigned short core, char *result, uint8_t resultLen);
static int isNumaCore(unsigned short core, uint8_t numa);
static unsigned int freeHugeNuma(unsigned short numa);
int getCore(rte_cpuset_t *libcore, uint16_t flags);

const st_nic_rate_params_t *
StDevFindDevConf(uint8_t nicSpeedRate, const st_nic_rate_params_t *nicParamsTable)
{
	if (stMainParams.numPorts == MAX_RXTX_PORTS)
	{
		switch (nicSpeedRate)
		{
		case ST_NIC_RATE_SPEED_100GBPS:
			nicSpeedRate = ST_NIC_RATE_SPEED_40GBPS;
			break;
		case ST_NIC_RATE_SPEED_40GBPS:
			nicSpeedRate = ST_NIC_RATE_SPEED_25GBPS;
			break;
		default:
			break;
		}
	}
	for (int i = 0; i < NIC_RATE_SPEED_COUNT; i++)
	{
		if (nicParamsTable[i].nicSpeed == nicSpeedRate)
		{
			return &nicParamsTable[i];
		}
	}
	return NULL;
}

/**
 * Normalize PCIe address on PCI bus to SSSS:BB:DD.F
 */
static st_status_t
StDevNormPcieAddr(const char *portInName, char *portOutName)
{
	char pcieAddr[ST_PCIE_ADDR_LEN];
	uint32_t s, b, d, f;
	if (portInName == NULL || portOutName == NULL)
		return ST_DEV_BAD_PORT_NAME;

	snprintf(pcieAddr, ST_PCIE_ADDR_LEN, "%s", portInName);
	if (sscanf(portInName, "%x:%x:%x.%x", &s, &b, &d, &f) < 4)
	{
		s = 0;
		if (sscanf(portInName, "%x:%x.%x", &b, &d, &f) < 3)
			return ST_DEV_BAD_PORT_NAME;
	}
	snprintf(portOutName, ST_PCIE_ADDR_LEN, ST_PCIE_ADDR_FORM, s, b, d, f);
	return ST_OK;
}

static st_status_t
StDevGetPcieDevBus(const char *portName, uint8_t *bus)
{
	uint32_t s, b, d, f;
	*bus = -1;
	if (sscanf(portName, "%x:%x:%x.%x", &s, &b, &d, &f) != 4)
		return ST_DEV_BAD_PORT_NAME;
	*bus = (uint8_t)b;
	return ST_OK;
}

static st_status_t
StDevGetCPUs(int32_t soc, int16_t *lowMn, int16_t *lowMx, int16_t *highMn, int16_t *highMx)
{
	FILE *fp;
	char cl[64];
	snprintf(cl, sizeof(cl), "/sys/devices/system/node/node%d/cpulist", soc);
	fp = fopen(cl, "r");
	if (fp == NULL)
		return ST_DEV_CANNOT_READ_CPUS;
	if (fgets(cl, sizeof(cl), fp) == NULL)
	{
		return ST_GENERAL_ERR;
	}
	fclose(fp);
	*lowMn = -1;
	*lowMx = -1;
	*highMn = -1;
	*highMx = -1;
	if (sscanf(cl, "%hd-%hd,%hd-%hd", lowMn, lowMx, highMn, highMx) < 2)
		return ST_DEV_CANNOT_READ_CPUS;
	return ST_OK;
}

static st_status_t
StDevIsModLoad(const char *drvName)
{
	FILE *fp;
	char buf[150];
	snprintf(buf, sizeof(buf), "lsmod | grep \"^%s \"", drvName);
	fp = popen(buf, "r");
	if (fp == NULL)
		return ST_GENERAL_ERR;
	buf[0] = 0;
	if ((fgets(buf, sizeof(buf), fp)) == NULL)
	{
		return ST_GENERAL_ERR;
	}
	pclose(fp);
	if (strlen(buf) == 0)
		return ST_DEV_MOD_NOT_LOADED;
	return ST_OK;
}

static st_status_t
StDevPrepCmd(const char *c, char *ret, size_t *retSize)
{
	FILE *fp;
	fp = popen(c, "r");
	if (fp == NULL)
		return ST_GENERAL_ERR;
	if (ret != NULL)
	{
		ret[0] = 0;
		if (fgets(ret, *retSize, fp) != NULL)
		{
			if (retSize != NULL)
				*retSize = strnlen(ret, *retSize);
		}
	}
	if (pclose(fp) < 0)
		return ST_GENERAL_ERR;
	return ST_OK;
}

static st_status_t
StDevDpdkInsMod(const char *drvName, const char *params)
{
	char c[120];
	if (params == NULL)
		snprintf(c, sizeof(c), "insmod /lib/modules/$(uname -r)/extra/dpdk/%s.ko", drvName);
	else
		snprintf(c, sizeof(c), "insmod /lib/modules/$(uname -r)/extra/dpdk/%s.ko %s", drvName,
				 params);
	return StDevPrepCmd(c, NULL, NULL);
}

static st_status_t
StDevModProb(const char *drvName)
{
	char c[80];
	snprintf(c, sizeof(c), "modprobe %s", drvName);
	return StDevPrepCmd(c, NULL, NULL);
}

static st_status_t
StDevBind(const char *drvName, const char *portName)
{
	FILE *fp;
	char buf[64];
	snprintf(buf, sizeof(buf), "dpdk-devbind.py -b %s %s ", drvName, portName);
	fp = popen(buf, "r");
	if (fp == NULL)
		return ST_GENERAL_ERR;
	if (pclose(fp) < 0)
		return ST_DEV_CANNOT_LOAD_MOD;
	return ST_OK;
}

static st_status_t
StDevUnbind(const char *portName)
{
	st_status_t res;
	char buf[120];
	size_t retSize;
	snprintf(buf, sizeof(buf), "dpdk-devbind.py -u %s ", portName);
	res = StDevPrepCmd(buf, NULL, NULL);
	if (res < 0)
		return res;
	snprintf(buf, sizeof(buf), "dpdk-devbind.py -s | grep %s | grep drv=", portName);
	retSize = sizeof(retSize);
	res = StDevPrepCmd(buf, buf, &retSize);
	if (res < 0)
		return res;
	if (retSize > 1)
		return retSize;
	return ST_OK;
}

static const char *dpdkDrvNames[] = {
	MLX4_CORE_MOD,
	MLX5_CORE_MOD,
	IGB_UIO_MOD,
};
static const char *
StDevGetDpdkCardDrvName(const char *portName)
{
	FILE *fp;
	char buf[150];
	snprintf(buf, sizeof(buf), "dpdk-devbind.py -s | grep %s", portName);
	fp = popen(buf, "r");
	if (fp == NULL)
		return NULL;
	if (fgets(buf, sizeof(buf), fp) == NULL)
		return NULL;
	if (pclose(fp) < 0)
		return NULL;
	for (int i = 0; i < sizeof(dpdkDrvNames) / sizeof(dpdkDrvNames[0]); i++)
		if (strstr(buf, dpdkDrvNames[i]) != NULL)
			return dpdkDrvNames[i];
	return NULL;
}

// TODO: Have to read register from MMIO
static const char *krnDrvNames[] = {
	"ixgbe", "ice", "i40e", "mlx4_core", "mlx5_core",
};

typedef struct st_port_info
{
	char normName[ST_PCIE_ADDR_LEN];
	int32_t speed;
} st_port_info_t;
typedef struct st_eal_args
{
	int argc;
	char *argv[32];
	char coreList[512];
} st_eal_args_t;

typedef enum
{
	ST_DEV_TYPE_PRODUCER_USED = 1 << ST_DEV_TYPE_PRODUCER,
	ST_DEV_TYPE_CONSUMER_USED = 1 << ST_DEV_TYPE_CONSUMER,
} st_dev_used_flags;

typedef struct st_used_dev_info
{
	unsigned int isDevTypesPrep;
	st_port_info_t port[MAX_RXTX_PORTS];
} st_used_dev_info_t;

static st_used_dev_info_t usedPortInfo;

st_kni_ms_conf_t *kni[MAX_RXTX_PORTS] = { NULL };
static uint32_t currlCore = -1;
bool isSchActive = false;
bool isKniActive = false;

static st_status_t
StDevTryGetEthLinkSpeed(const char *portName, int *speed, char *eth)
{
	FILE *fp;
	char buf[512];
	rte_delay_us_sleep(1000000);
	snprintf(buf, sizeof(buf), "ls -l /sys/class/net/ | grep %s ", portName);
	fp = popen(buf, "r");
	if (fp == NULL)
		return ST_INVALID_PARAM;
	buf[0] = 0;
	char *ln = fgets(buf, sizeof(buf), fp);
	if (pclose(fp) < 0)
		return ST_INVALID_PARAM;
	if (ln == NULL)
		return ST_INVALID_PARAM;
	if (sscanf(strstr(buf, "/net/"), "/net/%s/", eth) <= 0)
		return ST_INVALID_PARAM;
	int linkUpCnt = 0;
	do
	{
		if (linkUpCnt++ > 9)
			return ST_DEV_UNPLUGED_CABLE_ERR;
		snprintf(buf, sizeof(buf), "ip link set %s up ", eth);
		fp = popen(buf, "r");
		if (fp == NULL)
			return ST_INVALID_PARAM;
		if (pclose(fp) < 0)
			return ST_INVALID_PARAM;
		snprintf(buf, sizeof(buf), "/sys/class/net/%s/speed", eth);
		rte_delay_us_sleep(1000000);
		fp = fopen(buf, "rb");
		*speed = -1;
		if (fp == NULL)
			return ST_INVALID_PARAM;
		if (fscanf(fp, "%d", speed) == 0)
		{
			return ST_GENERAL_ERR;
		}
		fclose(fp);
	} while (*speed < 0);
	return ST_OK;
}

static st_status_t
StDevGetEthLinkSpeed(const char *portName, int *speed, char *eth)
{
	FILE *fp;
	char *ln;
	int i = 0;
	char buf[512];
	StDevUnbind(portName);	// ignore result
	for (i = 0; i < sizeof(krnDrvNames) / sizeof(krnDrvNames[0]); i++)
		StDevModProb(krnDrvNames[i]);
	snprintf(buf, sizeof(buf), "dpdk-devbind.py -s | grep %s ", portName);
	fp = popen(buf, "r");
	if (fp == NULL)
		return ST_DEV_BAD_PORT_NAME;
	buf[0] = 0;
	ln = fgets(buf, sizeof(buf), fp);
	if (pclose(fp) < 0)
		return ST_DEV_BAD_PORT_NAME;
	for (i = 0; i < sizeof(krnDrvNames) / sizeof(krnDrvNames[0]); i++)
	{
		ln = strstr(buf, krnDrvNames[i]);
		if (ln != NULL)
			break;
	}
	if (i >= sizeof(krnDrvNames) / sizeof(krnDrvNames[0]))
		return ST_DEV_BAD_PORT_NAME;
	st_status_t res;
	if ((res = StDevBind(krnDrvNames[i], portName)) < 0)
		return res;
	if ((res = StDevTryGetEthLinkSpeed(portName, speed, eth)) < 0)
		return res;
	return ST_OK;
}

static st_status_t
StDevDownNetClass(char *eth)
{
	FILE *fp;
	char buf[512];
	int32_t delCnt = 0;
	sleep(1);
	snprintf(buf, sizeof(buf), "ip link set %s down", eth);
	fp = popen(buf, "r");
	if (fp == NULL)
		return ST_GENERAL_ERR;
	if (fgets(buf, sizeof(buf), fp) == NULL)
	{
		return ST_GENERAL_ERR;
	}
	if (pclose(fp) < 0)
		return ST_GENERAL_ERR;
	do
	{
		snprintf(buf, sizeof(buf), "ip addr show dev %s", eth);
		fp = popen(buf, "r");
		if (fp == NULL)
			break;
		if (fgets(buf, sizeof(buf), fp) == NULL)
		{
			return ST_GENERAL_ERR;
		}
		pclose(fp);
		if (strstr(buf, "inet ") == NULL)
			return ST_OK;
		usleep(200000);
		delCnt++;
		if (delCnt > (10000000) / (200000))
			return ST_GENERAL_ERR;
	} while (1);
	return ST_GENERAL_ERR;
	;
}

static st_status_t
StDevTestNuma()
{
	FILE *fp;
	char buf[128];
	int hugeSize;
	int nodePages;
	if (numa_available() < 0)
		return ST_DEV_NO_NUMA;
	fp = popen("cat /proc/meminfo  | grep Hugepagesize", "r");
	if (fp == NULL)
		return ST_DEV_GENERAL_ERR;
	if (fscanf(fp, "Hugepagesize:%d kB", &hugeSize) < 1)
	{
		pclose(fp);
		return ST_DEV_NO_NUMA;
	}
	pclose(fp);
	if (hugeSize < ST_MIN_NUMA_PAGE_SIZE / 1024)
		return ST_DEV_NO_1GB_PAGE;
	for (int nmNod = 0; nmNod < numa_max_node(); nmNod++)
	{
		snprintf(buf, sizeof(buf),
				 "/sys/devices/system/node/node%d"
				 "/hugepages/hugepages-1048576kB/nr_hugepages",
				 nmNod);
		fp = fopen(buf, "r");
		if (fp == NULL)
			return ST_DEV_GENERAL_ERR;
		if (fscanf(fp, "%d", &nodePages) < 1)
		{
			fclose(fp);
			return ST_DEV_GENERAL_ERR;
		}
		fclose(fp);
		if (nodePages < ST_MIN_NUMA_PAGES)
			return ST_DEV_NO_MIN_NUMA;
	}
	return ST_OK;
}

static st_status_t
StDevPrepMods(void)
{
	if (StDevIsModLoad(UIO_MOD) != ST_OK)
	{
		if (StDevModProb(UIO_MOD) != ST_OK)
			return ST_DEV_CANNOT_LOAD_MOD;
	}
	if (StDevIsModLoad(IGB_UIO_MOD) != ST_OK)
	{
		if (StDevDpdkInsMod(IGB_UIO_MOD, NULL))
			return ST_DEV_CANNOT_LOAD_MOD;
	}
	if (StDevIsModLoad(RTE_KNI_MOD) != ST_OK)
	{
		if (StDevDpdkInsMod(RTE_KNI_MOD, "kthread_mode=multiple carrier=on"))
		{
			return ST_DEV_CANNOT_LOAD_MOD;
		}
	}
	return ST_OK;
}

static char ethName[MAX_RXTX_PORTS][RTE_KNI_NAMESIZE];

const char *
StDevGetKniInterName(int portId)
{
	if (ethName[portId] == 0)
		return NULL;
	return ethName[portId];
}
const st_nic_rate_params_t *stDevParams = NULL;

static char namePrg[] = "InitMediaStreamerLibrary";
static char socketMemVal[64] = "1024,1024";
static char socketMemPar[] = "--socket-mem";
static char filePrefix[] = "--file-prefix";
static char inMemory[] = "--in-memory";
static char matchAllocations[] = "--match-allocations";
static char portPar[] =
#if (RTE_VER_YEAR < 21)
	"-w";
#else
	"-a";
#endif
static char procListPar[] = "-l";

static st_status_t
StDevInitParams(st_eal_args_t *a, st_used_dev_info_t *p)
{
	short int loMin, loMax, hiMin, hiMax;
	uint8_t bus;
	char dpdkName[35] = ST_PREFIX_APPNAME;

	rte_cpuset_t libraryCores;
	/* identify the NUMA for Primary Port */
	int numaPrimary = -1, numaRedudant = -1;

	numaPrimary = getNicNuma(p->port[ST_PPORT].normName);
	/* identify the NUMA for Redudant Port */
	if ((p->port[ST_RPORT].normName) && (stMainParams.numPorts == 2))
		numaRedudant = getNicNuma(p->port[ST_RPORT].normName);

	if (numaPrimary != -1)
	{
		RTE_LOG(INFO, USER1, "primary port (%s) is on NUMA (%d)\n", p->port[ST_PPORT].normName,
				numaPrimary);
		if ((p->port[ST_RPORT].normName) && (stMainParams.numPorts == 2))
		{
			RTE_LOG(INFO, USER1, "Redudant port (%s) is on NUMA (%d)\n", p->port[ST_RPORT].normName,
					numaRedudant);
			/* check if both are on same NUMA */
			if (numaPrimary != numaRedudant)
			{
				RTE_LOG(ERR, USER1,
						"Primary port (%s) and Redudant Port (%s) are not in same NUMA\n",
						p->port[ST_PPORT].normName, p->port[ST_RPORT].normName);
				return ST_DEV_GENERAL_ERR;
			}
		}

		/* for single NUMA, numa node returened is -1 */
		if (numaPrimary == 65535)
			numaPrimary = 0;

		/* fetching performance cores on the same NUMA */
		uint16_t numaFlag = (numaPrimary == 0)
								? 16
								: (numaPrimary == 1)
									  ? 32
									  : (numaPrimary == 2) ? 64 : (numaPrimary == 3) ? 128 : 0;
		uint16_t coreFlag = 1;

		/* prepare socketMemVal */
		snprintf(socketMemVal, sizeof(socketMemVal), "%s",
				 (numaPrimary == 0)
					 ? "2048,0,0,0"
					 : (numaPrimary == 1)
						   ? "0,2048,0,0"
						   : (numaPrimary == 2) ? "0,0,2048,0"
												: (numaPrimary == 3) ? "0,0,0,2048" : "0,0,0,0");

		if (getCore(&libraryCores, numaFlag + coreFlag) != 0)
		{
			RTE_LOG(ERR, USER1, "Failed to get performance core on NUMA (%d)\n", numaPrimary);
			return ST_DEV_GENERAL_ERR;
		}

		/* check if we have minimum */
		if (CPU_COUNT(&libraryCores) < 5)
		{
			RTE_LOG(ERR, USER1, "Insufficent performance cores, current cores are %u\n", CPU_COUNT(&libraryCores));
			return ST_DEV_NOT_ENOUGH_CORES;
		}

		/* prepare core mask */
		int cpuCount = 0;

		for (int cpuIndex = 0;
			 ((cpuIndex < RTE_MAX_LCORE) && (cpuCount < CPU_COUNT(&libraryCores))); cpuIndex++)
		{
			if (CPU_ISSET(cpuIndex, &libraryCores))
			{
				snprintf(a->coreList + strlen(a->coreList),
						 sizeof(a->coreList) - strlen(a->coreList), "%d,", cpuIndex);
				cpuCount += 1;
			}
		}
		a->coreList[strlen(a->coreList) - 1] = '\0';

		RTE_LOG(INFO, USER1, "CPU core List (%s)\n", a->coreList);
	}
	else
	{
		RTE_LOG(ERR, USER1, "Primary Port (%s) NUMA not found\n", p->port[ST_PPORT].normName);
		return ST_DEV_GENERAL_ERR;
	}

	a->argc = 0;
	a->argv[a->argc] = namePrg;
	a->argc++;
	a->argv[a->argc] = filePrefix;
	a->argc++;
	a->argv[a->argc] = dpdkName;
	a->argc++;
	a->argv[a->argc] = inMemory;
	a->argc++;
	a->argv[a->argc] = matchAllocations;
	a->argc++;
	a->argv[a->argc] = portPar;
	a->argc++;
	a->argv[a->argc] = p->port[0].normName;
	a->argc++;
	if (stMainParams.numPorts == 2)
	{
		a->argv[a->argc] = portPar;
		a->argc++;
		a->argv[a->argc] = p->port[1].normName;
		a->argc++;
	}
	a->argv[a->argc] = socketMemPar;
	a->argc++;
	a->argv[a->argc] = socketMemVal;
	a->argc++;
	a->argv[a->argc] = procListPar;
	a->argc++;

	st_status_t res;
	if ((res = StDevGetPcieDevBus(p->port[ST_PPORT].normName, &bus)) != ST_OK)
		return res;
	if ((res = StDevGetCPUs(bus < ST_PCIE_SEC_SOC_BUS ? 0 : 1, &loMin, &loMax, &hiMin, &hiMax))
		!= ST_OK)
		return res;
	stDevParams = StDevFindDevConf(p->port[ST_PPORT].speed / 1000, stNicParamsTable);
	if (stDevParams == NULL)
		return ST_DEV_NOT_FIND_SPEED_CONF;

	/* default thread for DPDK */
	short int lcCount = 1 /* main thread */ + ST_KNI_THEARD;

	if (!stMainParams.rxOnly)
	{
		lcCount += stDevParams->maxEnqThrds + (stDevParams->maxSchThrds * stMainParams.numPorts);
		if (stMainParams.sn30Count > 0)
			lcCount += 1;
		if (stMainParams.sn40Count > 0)
			lcCount += 1;
	}
	if (!stMainParams.txOnly)
	{
		lcCount += stDevParams->maxRcvThrds;
		if (stMainParams.sn30Count > 0)
			lcCount += stDevParams->maxAudioRcvThrds;
		if (stMainParams.sn40Count > 0)
			lcCount += stDevParams->maxAncRcvThrds;
	}

	if (lcCount > (loMax + 1 + hiMax - hiMin - 2 * ST_FREE_LCORES_ON_CPU))
		return ST_DEV_GENERAL_ERR;

	a->argv[a->argc] = a->coreList;
	a->argc++;

	return ST_OK;
}

static st_status_t
StDevGetLocalIp(const char *eth, int portidx)
{
	if (*(uint32_t *)stMainParams.sipAddr[portidx] != 0)
		return ST_OK;

	st_status_t res;
	char buf[200];
	char result[40];
	size_t retSize;
	snprintf(buf, sizeof(buf), "ip addr show %s | grep inet | awk '{print $2}' | cut -d/ -f1", eth);
	retSize = 40;
	res = StDevPrepCmd(buf, result, &retSize);
	if (res < 0)
		return res;
	result[retSize - 1] = '\0';
	if (inet_pton(AF_INET, result, stMainParams.sipAddr[portidx]) != 1)
	{
		RTE_LOG(ERR, USER1, "not valid ip(%s) found, len = %ld\n", result, retSize);
		return ST_BAD_SRC_IPADDR;
	}
	return ST_OK;
}

static st_status_t
StDevInitDevs(st_used_dev_info_t *p)
{
	st_eal_args_t args = { 0 };
	st_status_t res;
	if ((res = StDevPrepMods()) != ST_OK)
		return res;
	for (int k = 0; k < stMainParams.numPorts; ++k)
	{
		ethName[k][RTE_KNI_NAMESIZE - 1] = '\0';
		if ((res = StDevTryGetEthLinkSpeed(p->port[k].normName, &p->port[k].speed, ethName[k]))
			!= ST_OK)
		{
			if ((res = StDevGetEthLinkSpeed(p->port[k].normName, &p->port[k].speed, ethName[k]))
				!= ST_OK)
			{
				memset(ethName[k], k, sizeof(ethName[k]));
				return res;
			}
		}
		if (StDevGetLocalIp(ethName[k], k) != ST_OK)
		{
			RTE_LOG(ERR, USER1, "Can not find local ip for eth: %s\n", ethName[k]);
			RTE_LOG(ERR, USER1, "Please config IP for it or manually set by --sip xx.xx.xx.xx\n");
			return ST_BAD_SRC_IPADDR;
		}
		StDevDownNetClass(ethName[k]);
		StDevUnbind(p->port[k].normName);
		if (StDevBind(StDevGetDpdkCardDrvName(p->port[k].normName), p->port[k].normName) != ST_OK)
			return ST_DEV_BAD_PORT_NAME;
	}
	res = StDevTestNuma();
	if (res < 0)
		return res;
	if (StDevInitParams(&args, p) < 0)
		return ST_DEV_BAD_PORT_NAME;

	args.argv[args.argc++] = "-v";
	if (strlen(stMainParams.dpdkParams) > 2)
	{
		args.argv[args.argc++] = stMainParams.dpdkParams;
	}
	args.argv[args.argc++] = "--";
	if (rte_eal_init(args.argc, args.argv) < 0)
		return ST_DEV_BAD_PORT_NAME;

	return ST_OK;
}

/* Bind the port back to kernel drv */
static st_status_t
StDevBindToKernel(char *portName)
{
	char buf[512];
	FILE *fp;
	int krnDrvSize = sizeof(krnDrvNames) / sizeof(krnDrvNames[0]);
	int i;

	/* Get all available drv */
	snprintf(buf, sizeof(buf), "dpdk-devbind.py -s | grep %s ", portName);
	fp = popen(buf, "r");
	if (!fp)
		return ST_DEV_BAD_PORT_NAME;
	buf[0] = 0;
	if (!fgets(buf, sizeof(buf), fp))
		return ST_DEV_BAD_PORT_NAME;
	if (pclose(fp) < 0)
		return ST_DEV_BAD_PORT_NAME;

	/* Search the kernel drv */
	for (i = 0; i < krnDrvSize; i++)
	{
		if (strstr(buf, krnDrvNames[i]))
			break;
	}
	if (i >= krnDrvSize)
		return ST_DEV_BAD_PORT_NAME;

	RTE_LOG(INFO, USER1, "%s, bind %s back to kernel drv %s\n", __func__, portName, krnDrvNames[i]);
	return StDevBind(krnDrvNames[i], portName);
}

static st_status_t
StDevExitDevs(st_used_dev_info_t *p)
{
	char *portName;
	uint16_t portId;

	for (int k = 0; k < stMainParams.numPorts; ++k)
	{
		portName = p->port[k].normName;

		if (rte_eth_dev_get_port_by_name(portName, &portId) != 0)
			return ST_DEV_BAD_PORT_NAME;
		rte_eth_dev_close(portId); /* Close the port */

		StDevBindToKernel(portName); /* Bind back to kernel */
	}

	return ST_OK;
}

static st_status_t
StDevPrepMBuf(st_device_impl_t *d)
{
	struct rte_mempool *mbufPool;
	/* Creates a new mempool in memory to hold the mbufs. */
	mbufPool = rte_pktmbuf_pool_create_by_ops("MBUF_POOL", (1 << 18), MBUF_CACHE_SIZE,
#if (RTE_VER_YEAR < 21)
											  0,
#else
											  sizeof(pktpriv_data_t),
#endif
											  RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id(), "stack");

	if (mbufPool == NULL)
		return ST_DEV_CANNOT_PREPARE_MBUF;
	d->mbufPool = mbufPool;
	return ST_OK;
}

static const struct rte_eth_conf portConf = { .rxmode = { .max_rx_pkt_len = RTE_ETHER_MAX_LEN },
											  .txmode = { .offloads = DEV_TX_OFFLOAD_MULTI_SEGS } };

static const struct rte_eth_txconf txPortConf = { .tx_rs_thresh = 1, .tx_free_thresh = 1 };
static const struct rte_eth_rxconf rxPortConf = { .rx_free_thresh = 1 };

static uint16_t
mbuf_parse(uint16_t port __rte_unused, uint16_t qidx __rte_unused, struct rte_mbuf **pkts,
		   uint16_t nb_pkts, uint16_t max_pkts __rte_unused, void *_ __rte_unused)
{
	if (unlikely(nb_pkts == 0))
		return 0;

	uint64_t ptpTime = 0;
	if (stMainParams.isEbuCheck)
	{
		struct timespec spec;
		rte_eth_timesync_read_time(port, &spec);
		ptpTime = spec.tv_sec * GIGA + spec.tv_nsec;
	}

	struct rte_mbuf *replace[nb_pkts];
	int k = 0, j = 0;
	for (int i = 0; i < nb_pkts; i++)
	{
		struct rte_mbuf *m = pkts[i];

		if (unlikely(((m->packet_type & RTE_PTYPE_L4_MASK) != RTE_PTYPE_L4_UDP)
					 || ((m->packet_type & RTE_PTYPE_L4_FRAG) == RTE_PTYPE_L4_FRAG)))
		{
			replace[k] = m;
			k += 1;
			continue;
		}

#if (RTE_VER_YEAR < 21)
		m->timestamp = ptpTime;
#else
		/* No access to portid, hence we have rely on pktpriv_data */
		pktpriv_data_t *ptr = rte_mbuf_to_priv(m);
		ptr->timestamp = ptpTime;
#endif

		pkts[j] = m;
		j += 1;

		m->l2_len
			= 14
			  + (((m->packet_type & RTE_PTYPE_L2_MASK) == RTE_PTYPE_L2_ETHER_QINQ)
					 ? 8
					 : ((m->packet_type & RTE_PTYPE_L2_MASK) == RTE_PTYPE_L2_ETHER_VLAN) ? 4 : 0);
		/* we rely on NIC filter to get ipv4-udp */
		struct rte_ipv4_hdr *ipv4_hdr
			= (struct rte_ipv4_hdr *)((char *)(rte_pktmbuf_mtod(m, struct rte_ether_hdr *))
									  + m->l2_len);
		m->l3_len = (ipv4_hdr->version_ihl & RTE_IPV4_HDR_IHL_MASK) * 4;
		//struct rte_udp_hdr *udp = (struct rte_udp_hdr *)((char *) ipv4_hdr + )m->l3_len;
		m->l4_len = 8;
	}

	for (int index = 0; index < k; index++)
		rte_pktmbuf_free(replace[index]);

	return (nb_pkts);
}

#if ST_NIC_DRIVER_WA
/* Sample check for malicious code debugging  */
static uint16_t
StPreCheckPkts(uint8_t port __rte_unused, uint16_t qidx __rte_unused, struct rte_mbuf **pkts,
			   uint16_t nb_pkts, void *_ __rte_unused)
{
	if (unlikely(nb_pkts == 0))
		return nb_pkts;

	int i, j, k;
	struct rte_mbuf *ptr;
	struct rte_mbuf *replace[nb_pkts];

	for (i = 0, j = 0, k = 0; i < nb_pkts; i++)
	{
		ptr = pkts[i];
		if ((ptr->pkt_len < ST_NIC_DRIVER_WA_PKT_LEN_17)
			|| (ptr->nb_segs > ST_NIC_DRIVER_WA_NB_SEG_8)
			|| (ptr->pkt_len > ST_NIC_DRIVER_WA_PKT_LEN_9728))
		{
			replace[k++] = ptr;
			continue;
		}
		pkts[j++] = ptr;
	}

	rte_pktmbuf_free_bulk(replace, k);
	return j;
}
#endif	//ST_NIC_DRIVER_WA

static st_status_t
StDevInitRtePort(uint16_t port, st_device_impl_t *d)
{
	struct rte_eth_conf port_conf = portConf;
	struct rte_eth_dev_info devInfo;
	const uint16_t rxQueues = stDevParams->maxAudioRcvThrds + stDevParams->maxRcvThrds
							  + stDevParams->maxAncRcvThrds
							  + 1;	// One RxQ more for Audio for each Rx Thread
	const uint16_t txQueues
		= stDevParams->maxTxRings + 1 + 1;	// allocate one more for PTP and one more for IGMP
	uint16_t rxDesc = RX_RING_SIZE;
	uint16_t txDesc = TX_RING_SIZE;
	int ret;
	uint16_t q;

	if (port >= rte_eth_dev_count_avail())
		return ST_GENERAL_ERR;

	ret = rte_eth_dev_info_get(port, &devInfo);
	if (ret != 0)
	{
		rte_exit(ST_GENERAL_ERR, "Error during getting device (port %u) info: %s\n", port,
				 strerror(-ret));
	}

	RTE_LOG(INFO, USER1, "CAPABILITIES: dev_info.tx_offload_capa = %llx\n",
			(U64)devInfo.tx_offload_capa);
	RTE_LOG(INFO, USER1, "CAPABILITIES: dev_info.rx_offload_capa = %llx\n",
			(U64)devInfo.rx_offload_capa);

	if (devInfo.rx_offload_capa & DEV_RX_OFFLOAD_TIMESTAMP)
	{
		RTE_LOG(ERR, USER1, "DEV_RX_OFFLOAD_TIMESTAMP is supported on port %d\n", port);
		port_conf.rxmode.offloads |= DEV_RX_OFFLOAD_TIMESTAMP;
		stMainParams.hwCaps.nicHwTmstamp = 1;
	}
	else
	{
		RTE_LOG(ERR, USER1, "DEV_RX_OFFLOAD_TIMESTAMP is NOT supported on port %d\n", port);
	}

	/* Configure the Ethernet device. */
	ret = rte_eth_dev_configure(port, rxQueues, txQueues, &port_conf);
	if (ret != 0)
	{
		rte_exit(ST_GENERAL_ERR, "Error upon rte_eth_dev_configure port %u info: %s\n", port,
				 strerror(-ret));
	}

	ret = rte_eth_dev_adjust_nb_rx_tx_desc(port, &rxDesc, &txDesc);
	if (ret != 0)
	{
		rte_exit(ST_GENERAL_ERR, "Error upon rte_eth_dev_adjust_nb_rx_tx_desc port %d info %s\n",
				 port, strerror(-ret));
	}

	/* Allocate and set up 1 RX queue per Ethernet port. */
	for (q = 0; q < rxQueues; q++)
	{
		ret = rte_eth_rx_queue_setup(port, q, rxDesc, rte_eth_dev_socket_id(port), &rxPortConf,
									 d->mbufPool);
		if (ret < 0)
		{
			rte_exit(ST_GENERAL_ERR, "Error upon rte_eth_rx_queue_setup port %d info %s\n", port,
					 strerror(-ret));
		}
		if (q && rte_eth_add_rx_callback(port, q, mbuf_parse, NULL) == NULL)
		{
			rte_exit(ST_GENERAL_ERR, "Failed to add Rx callback for port %d q %d\n", port, q);
		}
	}

	/* Allocate and set up 1 TX queue per Ethernet port. */
	for (q = 0; q < txQueues; q++)
	{
		ret = rte_eth_tx_queue_setup(port, q, txDesc, rte_eth_dev_socket_id(port), &txPortConf);
		if (ret < 0)
		{
			rte_exit(ST_GENERAL_ERR, "Error upon rte_eth_tx_queue_setup port %d info %s\n", port,
					 strerror(-ret));
		}
	}

	/* enable PTYPE for packet classification by NIC */
	{
		uint32_t ptypes[16];
		uint32_t set_ptypes[16];
		uint32_t ptype_mask = RTE_PTYPE_L2_ETHER_TIMESYNC | RTE_PTYPE_L2_ETHER_ARP
							  | RTE_PTYPE_L2_ETHER_VLAN | RTE_PTYPE_L2_ETHER_QINQ
							  | RTE_PTYPE_L4_ICMP | RTE_PTYPE_L3_IPV4 | RTE_PTYPE_L4_UDP
							  | RTE_PTYPE_L4_FRAG;

		int num_ptypes
			= rte_eth_dev_get_supported_ptypes(port, ptype_mask, ptypes, RTE_DIM(ptypes));
		for (int i = 0; i < num_ptypes; i += 1)
		{
			set_ptypes[i] = ptypes[i];
		}

		if (num_ptypes >= 5)
		{
			if (rte_eth_dev_set_ptypes(port, ptype_mask, set_ptypes, num_ptypes) != 0)
				rte_exit(EXIT_FAILURE, " failed to set the fetched ptypes!");
			printf(" PTYPE enabled for port (%d)!", port);
		}
		else
		{
			rte_exit(EXIT_FAILURE, "failed to setup all ptype, only %d supported!", num_ptypes);
		}
	}
#if ST_NIC_DRIVER_WA
	for (uint16_t q = 0; q < txQueues; q++)
	{
		if (rte_eth_add_tx_callback(port, q, (rte_tx_callback_fn)StPreCheckPkts, NULL) == NULL)
			rte_panic("failed to set rte_eth_add_tx_callback!");
	}
#endif	// ST_NIC_DRIVER_WA

#if (RTE_VER_YEAR >= 21)
	rte_mbuf_dyn_rx_timestamp_register(&hwts_dynfield_offset[port], NULL);
	if (hwts_dynfield_offset[port] < 0)
	{
		RTE_LOG(ERR, USER1, " Failed to register timestamp field for port(%d:%s)\n", port,
				devInfo.driver_name);
	}
#endif

	/* Start the Ethernet port. */
	ret = rte_eth_dev_start(port);
	if (ret < 0)
	{
		rte_exit(ST_GENERAL_ERR, "Error upon rte_eth_dev_start port %d info %s\n", port,
				 strerror(-ret));
	}

	ret = rte_eth_timesync_enable(port);
	if (ret < 0)
	{
		RTE_LOG(ERR, USER1, "TIMESYNC is NOT supported on port %d\n", port);
		stMainParams.hwCaps.nicHwTimesync = 0;
	}
	else
	{
		RTE_LOG(ERR, USER1, "TIMESYNC is supported on port %d\n", port);
		stMainParams.hwCaps.nicHwTimesync = 1;
	}

	/* Enable RX in promiscuous mode for the Ethernet device. */
	rte_eth_promiscuous_enable(port);
	RTE_LOG(DEBUG, USER1, "%i rte_eth_allmulticast_enable\n", rte_eth_allmulticast_enable(port));
	return ST_OK;
}

st_status_t
StDevInitRxTx(st_device_impl_t *d)
{
	if (!stDevParams || (stDevParams->nicSpeed <= 0))
	{
		return ST_DEV_BAD_NIC_RATE;
	}

	// st_device_impl_t *recvDev = NULL;
	d->dev.ver.major = ST_VERSION_MAJOR_CURRENT;
	d->dev.ver.major = ST_VERSION_MINOR_CURRENT;
	d->dev.maxSt21Sessions = ST_MAX_SESSIONS_MAX;
	d->dev.maxSt30Sessions = ST_MAX_SESSIONS_MAX;
	d->dev.maxSt40Sessions = ST_MAX_SESSIONS_MAX;
	d->dev.mtu = 1500;
	d->dev.rateGbps = stDevParams->nicSpeed;
	d->dev.pacerType = ST_2110_21_TPN;

	return ST_OK;
}

static st_status_t
StDevGetPortIds(st_device_impl_t *d, st_used_dev_info_t *p)
{
	uint16_t portId = 0;

	for (int i = 0; i < d->numPorts; i++)
	{
		st_status_t res;
		if ((res = rte_eth_dev_get_port_by_name(p->port[i].normName, &portId)) != ST_OK)
		{
			RTE_LOG(ERR, ST_DEV, ST_DEV_ERROR " Cannot find port %s\n", p->port[i].normName);
			return res;
		}
		d->dev.port[i] = portId;
		if ((res = StDevInitRtePort(d->dev.port[i], d)) != ST_OK)
		{
			rte_exit(EXIT_FAILURE, "Cannot init port %" PRIu8 "\n", d->dev.port[0]);
			return res;
		}
	}
	return ST_OK;
}

static st_status_t
StTestExacRate(st_device_t *d)
{
	switch (d->exactRate)
	{
	case ST_DEV_RATE_P_25_00:
	case ST_DEV_RATE_P_29_97:
	case ST_DEV_RATE_P_50_00:
	case ST_DEV_RATE_P_59_94:
	case ST_DEV_RATE_I_25_00:
	case ST_DEV_RATE_I_29_97:
	case ST_DEV_RATE_I_50_00:
	case ST_DEV_RATE_I_59_94:
		break;
	default:
		return ST_DEV_BAD_EXACT_RATE;
	}
	return ST_OK;
}

static st_status_t
StDevRvRtpInitRecv(st_main_params_t *mp, st_device_impl_t *d)
{
	st_status_t res = ST_OK;

	d->snCount = 0;

	if (d->dev.rateGbps == 0)
		d->dev.rateGbps = stDevParams->nicSpeed;

	if ((res = StDevCalculateBudgets(d)) != ST_OK)
		return res;

	if (mp->snCount > d->dev.maxSt21Sessions)
	{
		RTE_LOG(
			INFO, USER1,
			"Requested number of RX sessions (%d) is higher than allowed maximum sessions (%d). "
			"Number of sessions set to the %d sessions.\n",
			mp->snCount, d->dev.maxSt21Sessions, d->dev.maxSt21Sessions);
	}

	if (mp->sn30Count > d->dev.maxSt30Sessions)
	{
		RTE_LOG(
			INFO, USER1,
			"Requested number of RX sessions (%d) is higher than allowed maximum sessions (%d). "
			"Number of sessions set to the %d sessions.\n",
			mp->sn30Count, d->dev.maxSt30Sessions, d->dev.maxSt30Sessions);
	}

	mp->snCount = MIN(mp->snCount, d->dev.maxSt21Sessions);
	mp->sn30Count = MIN(mp->sn30Count, d->dev.maxSt30Sessions);

	d->snTable = rte_malloc_socket("snTable", d->dev.maxSt21Sessions * sizeof(st_session_impl_t *),
								   RTE_CACHE_LINE_SIZE, rte_socket_id());

	d->sn30Table
		= rte_malloc_socket("sn30Table", d->dev.maxSt30Sessions * sizeof(st_session_impl_t *),
							RTE_CACHE_LINE_SIZE, rte_socket_id());

	d->sn40Table
		= rte_malloc_socket("sn40Table", d->dev.maxSt40Sessions * sizeof(st_session_impl_t *),
							RTE_CACHE_LINE_SIZE, rte_socket_id());

	if (!d->snTable || !d->sn30Table || !d->sn40Table)
	{
		rte_exit(ST_NO_MEMORY, "StDevRvRtpInitRecv cannot allocate few bytes");
	}
	memset(d->snTable, 0, d->dev.maxSt21Sessions * sizeof(st_session_impl_t *));
	memset(d->sn30Table, 0, d->dev.maxSt30Sessions * sizeof(st_session_impl_t *));
	memset(d->sn40Table, 0, d->dev.maxSt40Sessions * sizeof(st_session_impl_t *));

	d->mbufPool = mp->mbufPool;
	d->rxOnly = mp->rxOnly;

	for (int p = 0; p < mp->numPorts; ++p)
	{
		struct rte_ether_addr srcMac;
		rte_eth_macaddr_get(mp->rxPortId[p], &srcMac);

#ifdef TX_RINGS_DEBUG
		RTE_LOG(INFO, USER1, "RX SRC MAC address %02x:%02x:%02x:%02x:%02x:%02x\n",
				srcMac.addr_bytes[0], srcMac.addr_bytes[1], srcMac.addr_bytes[2],
				srcMac.addr_bytes[3], srcMac.addr_bytes[4], srcMac.addr_bytes[5]);

#endif

		memcpy(&d->srcMacAddr[mp->rxPortId[p]][0], &srcMac, ETH_ADDR_LEN);
	}

	StDevInitRxThreads(mp, d);

	return ST_OK;
}

static st_status_t
StDevRvRtpInitSend(st_main_params_t *mp, st_device_impl_t *d)
{
	st_status_t res;

	d->snCount = 0;

	if (d->dev.rateGbps == 0)
		d->dev.rateGbps = stDevParams->nicSpeed;

	d->mbufPool = mp->mbufPool;
	d->rxOnly = mp->rxOnly;

	if ((res = StDevCalculateBudgets(d)) != ST_OK)
		return res;

	if (mp->snCount > d->dev.maxSt21Sessions)
	{
		RTE_LOG(
			INFO, USER1,
			"Requested number of TX sessions (%d) is higher than allowed maximum sessions (%d). "
			"Number of sessions set to the %d sessions.\n",
			mp->snCount, d->dev.maxSt21Sessions, d->dev.maxSt21Sessions);
	}

	mp->snCount = MIN(mp->snCount, d->dev.maxSt21Sessions);
	mp->sn30Count = MIN(mp->sn30Count, d->dev.maxSt30Sessions);
	mp->sn40Count = MIN(mp->sn40Count, d->dev.maxSt40Sessions);

	d->snTable = rte_malloc_socket("snTable", d->dev.maxSt21Sessions * sizeof(st_session_impl_t *),
								   RTE_CACHE_LINE_SIZE, rte_socket_id());

	d->sn30Table
		= rte_malloc_socket("sn30Table", d->dev.maxSt30Sessions * sizeof(st_session_impl_t *),
							RTE_CACHE_LINE_SIZE, rte_socket_id());

	d->sn40Table
		= rte_malloc_socket("sn40Table", d->dev.maxSt40Sessions * sizeof(st_session_impl_t *),
							RTE_CACHE_LINE_SIZE, rte_socket_id());

	d->timeTable = rte_malloc_socket("timeTable", d->dev.maxSt21Sessions * sizeof(uint32_t),
									 RTE_CACHE_LINE_SIZE, rte_socket_id());

	d->txPktSizeL1 = rte_malloc_socket("txPktSizeL1", d->maxRings * sizeof(uint64_t),
									   RTE_CACHE_LINE_SIZE, rte_socket_id());

	if ((!d->snTable) || (!d->sn30Table) || (!d->timeTable) || (!d->txRing) || (!d->txPktSizeL1))
	{
		rte_exit(ST_NO_MEMORY, "RvRtpInitSendDevice cannot allocate few bytes");
	}

	memset(d->snTable, 0, d->dev.maxSt21Sessions * sizeof(st_session_impl_t *));
	memset(d->sn30Table, 0, d->dev.maxSt30Sessions * sizeof(st_session_impl_t *));
	memset(d->sn40Table, 0, d->dev.maxSt40Sessions * sizeof(st_session_impl_t *));
	memset(d->timeTable, 0, d->dev.maxSt21Sessions * sizeof(uint32_t));
	memset(d->txPktSizeL1, 0, d->maxRings * sizeof(uint64_t));

	for (int i = 0; i < mp->numPorts; ++i)
	{
		d->txRing[i] = rte_malloc_socket("txRing", d->maxRings * sizeof(struct rte_ring *),
										 RTE_CACHE_LINE_SIZE, rte_socket_id());

		d->packetsTx[i] = rte_malloc_socket("packetsTx", (d->maxRings + 1) * sizeof(uint64_t),
											RTE_CACHE_LINE_SIZE, rte_socket_id());

		d->pausesTx[i] = rte_malloc_socket("pausesTx", (d->maxRings + 1) * sizeof(uint64_t),
										   RTE_CACHE_LINE_SIZE, rte_socket_id());

		if ((!d->txRing[i]) || (!d->packetsTx[i]) || (!d->pausesTx[i]))
		{
			rte_exit(ST_NO_MEMORY, "RvRtpInitSendDevice cannot allocate few bytes");
		}

		memset(d->txRing[i], 0, d->maxRings * sizeof(struct rte_ring *));
		memset(d->packetsTx[i], 0, (d->maxRings + 1) * sizeof(uint64_t));
		memset(d->pausesTx[i], 0, (d->maxRings + 1) * sizeof(uint64_t));

		struct rte_ether_addr srcMac;
		rte_eth_macaddr_get(mp->txPortId[i], &srcMac);

		memcpy(&d->srcMacAddr[mp->txPortId[i]][0], &srcMac, ETH_ADDR_LEN);

		for (uint32_t j = 0; j < d->maxRings; j++)
		{
			char ringName[32];
			unsigned int flags, count;
			if (j == (d->maxRings - 1))
			{
				flags = RING_F_MP_HTS_ENQ | RING_F_SC_DEQ;
				count = 0x1 << 12;
			}
			else
			{
				flags = RING_F_SP_ENQ | RING_F_SC_DEQ;
				count = 0x1 << 10;
			}

			snprintf(ringName, 32, "SMPTE-RING-%u%u", i, j);

			/* Create per session ring to the TPRS scheduler */
			struct rte_ring *smpteRing = rte_ring_create(ringName, count, rte_socket_id(), flags);

			d->txRing[i][j] = smpteRing;
#ifdef TX_RINGS_DEBUG
			RTE_LOG(INFO, USER1, "RvRtpInitSendDevice %s %p\n", ringName, smpteRing);
#endif
		}
	}

	for (uint32_t i = 0; i < d->dev.maxSt21Sessions; i++)
	{
		d->timeTable[i] = 0;
	}

	uint32_t budget = d->quot;
	for (uint32_t i = 0; i < d->dev.maxSt21Sessions; i++)
	{
		if (d->snTable[i])
		{
			d->txPktSizeL1[i] = StSessionGetPktsize(d->snTable[i]) + ST_PHYS_PKT_ADD;
		}
		else
		{
			d->txPktSizeL1[i] = ST_HD_422_10_SLN_L1_SZ;
		}
		budget -= d->txPktSizeL1[i];
	}
	for (uint32_t i = d->dev.maxSt21Sessions; i < d->maxRings; i++)
	{
		if (budget > ST_DEFAULT_PKT_L1_SZ + ST_MIN_PKT_L1_SZ)
		{
			d->txPktSizeL1[i] = ST_DEFAULT_PKT_L1_SZ;
			budget -= d->txPktSizeL1[i];
		}
		else if ((budget > ST_DEFAULT_PKT_L1_SZ) && (d->outOfBoundRing))
		{
			d->txPktSizeL1[i] = ST_DEFAULT_PKT_L1_SZ - ST_MIN_PKT_L1_SZ;
			budget -= d->txPktSizeL1[i];
		}
		else
		{
			d->txPktSizeL1[i] = MIN(budget, ST_DEFAULT_PKT_L1_SZ);
		}
	}
#ifdef TX_RINGS_DEBUG
	for (uint32_t i = 0; i < dev->maxRings; i++)
	{
		RTE_LOG(INFO, USER1, "Device Ring %u txPktSizeL1 %u\n", i, dev->txPktSizeL1[i]);
	}
#endif
	StDevInitTxThreads(mp, d);

	return ST_OK;
}

rte_atomic32_t isStopBkgTask;
static pthread_t devBkgTaskTid;

#define ST_BKG_TICK (100 * 1000)		  // us
#define ST_BKG_STS_PER (5 * 1000 * 1000)  // us
#define ST_BKG_ARP_PER (5 * 1000 * 1000)  // us
#define ST_BKG_KNI_PER (2 * 1000 * 1000)  // us

#define ST_TEST_PER_AND_DO(curStamp, taskStamp, taskPer, task)                                     \
	do                                                                                             \
	{                                                                                              \
		if (taskStamp < curStamp && (curStamp - taskStamp) < taskPer)                              \
			break;                                                                                 \
		taskStamp = curStamp;                                                                      \
		task;                                                                                      \
	} while (0)

static void *
StDevBkgTasks(void *arg)
{
	uint64_t curStamp, stsStamp, arpStamp, kniStamp;
	curStamp = StGetCpuTimeNano();
	stsStamp = curStamp;
	arpStamp = curStamp;
	kniStamp = curStamp;
	while (rte_atomic32_read(&isStopBkgTask) == 0)
	{
		rte_delay_us_sleep(ST_BKG_TICK);
		curStamp = StGetCpuTimeNano();
		ST_TEST_PER_AND_DO(curStamp, stsStamp, ST_BKG_STS_PER, StStsTask(stMainParams.numPorts));
		bool isMultiCast
			= stMainParams.ipAddr[ST_PPORT][0] >= 0xe0 && stMainParams.ipAddr[ST_PPORT][0] <= 0xef;
		if (!isMultiCast && !SearchArpHist(*(uint32_t *)stMainParams.ipAddr[ST_PPORT], NULL))
			ST_TEST_PER_AND_DO(curStamp, arpStamp, ST_BKG_ARP_PER,
							   ArpRequest(stMainParams.txPortId[ST_PPORT],
										  *(uint32_t *)stMainParams.ipAddr[ST_PPORT], *(uint32_t *)stMainParams.sipAddr[ST_PPORT]));
		if (stMainParams.numPorts == 2)
		{
			isMultiCast = stMainParams.ipAddr[ST_RPORT][0] >= 0xe0
						  && stMainParams.ipAddr[ST_RPORT][0] <= 0xef;
			if (!isMultiCast && !SearchArpHist(*(uint32_t *)stMainParams.ipAddr[ST_RPORT], NULL))
				ST_TEST_PER_AND_DO(curStamp, arpStamp, ST_BKG_ARP_PER,
								   ArpRequest(stMainParams.txPortId[ST_RPORT],
											  *(uint32_t *)stMainParams.ipAddr[ST_RPORT],*(uint32_t *)stMainParams.sipAddr[ST_RPORT]));
		}
		ST_TEST_PER_AND_DO(curStamp, kniStamp, ST_BKG_KNI_PER, StKniBkgTask());
	}
	return NULL;
}

static st_status_t
StDevInitBkgTasks(void)
{
	loadArpHist();
	rte_atomic32_set(&isStopBkgTask, 0);
	int ret = rte_ctrl_thread_create(&devBkgTaskTid, "Dev ", NULL, StDevBkgTasks, NULL);
	if (ret < 0)
		return ST_GENERAL_ERR;
	return ST_OK;
}

static st_status_t
StDevStopBkgTasks(void)
{
	storeArpHist();
	rte_atomic32_set(&isStopBkgTask, 1);
	pthread_join(devBkgTaskTid, NULL);
	return ST_OK;
}

st_status_t
StStartDevice(st_device_t *dev)
{
	st_status_t status = ST_OK;

	status = StValidateDevice(dev);
	if (status != ST_OK)
	{
		return status;
	}

	st_device_impl_t *d = (st_device_impl_t *)dev;

	uint32_t enqThrdId = 0;
	uint32_t schThrdId = 0;

	if (d->dev.type == ST_DEV_TYPE_PRODUCER)
	{
		if (stMainParams.rxOnly == 0 && !isSchActive)
		{
			do
			{
				currlCore = rte_get_next_lcore(currlCore, 1, 0);
				transmitter_thread_args[schThrdId].threadId = schThrdId;
				transmitter_thread_args[schThrdId].bulkNum
					= stMainParams.txBulkNum ? stMainParams.txBulkNum : stDevParams->maxTxBulkNum;
				int ret = rte_eal_remote_launch(LcoreMainTransmitter,
												&transmitter_thread_args[schThrdId], currlCore);
				if (ret != 0)
				{
					RTE_LOG(ERR, USER1, "LcoreMainTransmitterDual failed to launch\n");
					return ST_REMOTE_LAUNCH_FAIL;
				}
			} while (++schThrdId < stDevParams->maxSchThrds * stMainParams.numPorts);

			/* Start the video enqueue */
			do
			{
				currlCore = rte_get_next_lcore(currlCore, 1, 0);
				const uint64_t temp = enqThrdId;
				int ret = rte_eal_remote_launch(LcoreMainPktRingEnqueue, (void *)(temp), currlCore);
				if (ret != 0)
				{
					RTE_LOG(ERR, USER1, "LcoreMainPktRingEnqueue failed to launch\n");
					return ST_REMOTE_LAUNCH_FAIL;
				}
			} while (++enqThrdId < stMainParams.maxEnqThrds);

			/* Start the audio enqueue */
			if (stMainParams.sn30Count > 0)
			{
				currlCore = rte_get_next_lcore(currlCore, 1, 0);
				const uint64_t temp = enqThrdId++;
				int ret
					= rte_eal_remote_launch(LcoreMainAudioRingEnqueue, (void *)(temp), currlCore);
				if (ret != 0)
				{
					RTE_LOG(ERR, USER1, "Run RingEuqueue not possible. Lcore not ready\n");
					return ST_REMOTE_LAUNCH_FAIL;
				}
			}

			/* Start the ancillary enqueue */
			if (stMainParams.sn40Count > 0)
			{
				currlCore = rte_get_next_lcore(currlCore, 1, 0);
				const uint64_t temp = enqThrdId++;
				int ret = rte_eal_remote_launch(LcoreMainAncillaryRingEnqueue, (void *)(temp),
												currlCore);
				if (ret != 0)
				{
					RTE_LOG(ERR, USER1,
							"Run Ancillary Data RingEuqueue not possible. Lcore not ready\n");
					return ST_REMOTE_LAUNCH_FAIL;
				}
			}
			isSchActive = true;
		}
	}
	else
	{
		if (stMainParams.txOnly == 0)
		{
			uint16_t maxRcvThreads = stMainParams.maxRcvThrds;
			uint16_t maxRcv30Threads
				= (stMainParams.sn30Count == 0) ? 0 : stMainParams.maxAudioRcvThrds;
			uint16_t maxRcv40Threads
				= (stMainParams.sn40Count == 0) ? 0 : stMainParams.maxAncRcvThrds;

			uint16_t index = 0;
			do
			{
				func_args[index].sn_type = ST_ESSENCE_VIDEO;
				func_args[index].threadId = index;
				func_args[index].portP = 0;
				func_args[index].portR = (stMainParams.numPorts == 2) ? 1 : 0;
				func_args[index].qPcount = 1;
				func_args[index].qRcount = 1;
				func_args[index].queueP[0] = 1 + index;
				func_args[index].queueR[0] = 1 + index;

				index += 1;
			} while (--maxRcvThreads);

			if (stMainParams.sn30Count)
			{
				do
				{
					func_args[index].sn_type = ST_ESSENCE_AUDIO;
					func_args[index].threadId = index;
					func_args[index].portP = 0;
					func_args[index].portR = (stMainParams.numPorts == 2) ? 1 : 0;
					func_args[index].qPcount = 1;
					func_args[index].qRcount = 1;
					func_args[index].queueP[0] = 1 + index;
					func_args[index].queueR[0] = 1 + index;

					index += 1;
				} while (--maxRcv30Threads);
			}

			if (stMainParams.sn40Count)
			{
				do
				{
					func_args[index].sn_type = ST_ESSENCE_ANC;
					func_args[index].threadId = index;
					func_args[index].portP = 0;
					func_args[index].portR = (stMainParams.numPorts == 2) ? 1 : 0;
					func_args[index].qPcount = 1;
					func_args[index].qRcount = 1;
					func_args[index].queueP[0] = 1 + index;
					func_args[index].queueR[0] = 1 + index;

					index += 1;
				} while (--maxRcv40Threads);
			}

			maxRcvThreads = stMainParams.maxRcvThrds;
			maxRcv30Threads = (stMainParams.sn30Count == 0) ? 0 : stMainParams.maxAudioRcvThrds;
			maxRcv40Threads = (stMainParams.sn40Count == 0) ? 0 : stMainParams.maxAncRcvThrds;
			const int maxThreads = maxRcvThreads + maxRcv30Threads + maxRcv40Threads;

			for (index = 0; index < (maxThreads); index++)
			{
				currlCore = rte_get_next_lcore(currlCore, 1, 0);
				if (currlCore >= RTE_MAX_LCORE || !(rte_lcore_is_enabled(currlCore)))
				{
					RTE_LOG(ERR, USER1, "Lcore (%d) not valid!", currlCore);
					return ST_REMOTE_LAUNCH_FAIL;
				}
				int ret = rte_eal_remote_launch(LcoreMainReceiver, (void *)(&func_args[index]),
												currlCore);
				if (ret != 0)
				{
					RTE_LOG(ERR, USER1, "Run Receiver not possible. Lcore not ready\n");
					return ST_REMOTE_LAUNCH_FAIL;
				}
			}
		}
	}

	return status;
}

st_status_t
StCreateDevice(st_device_t *inDev, const char *port1Name, const char *port2Name,
			   st_device_t **outDev)
{
	if (!inDev || !port1Name || !outDev)
	{
		return ST_INVALID_PARAM;
	}

	st_status_t res;
	st_used_dev_info_t locUsedPort = { 0 };
	st_device_impl_t *d;

	printf("Ports: %s %s\n", port1Name, port2Name);
	stMainParams.schedStart = 0;
	stMainParams.ringStart = 0;
	stMainParams.ringBarrier1 = 0;
	stMainParams.ringBarrier2 = 0;

	rte_atomic32_set(&isTxDevToDestroy, 0);
	rte_atomic32_set(&isRxDevToDestroy, 0);
	rte_atomic32_set(&isStopMainThreadTasks, 0);

	//validate device a bit
	if ((inDev->maxSt21Sessions > ST_MAX_SESSIONS_MAX)
		|| (inDev->maxSt30Sessions > ST_MAX_SESSIONS_MAX)
		|| (inDev->maxSt40Sessions > ST_MAX_SESSIONS_MAX))
	{
		return ST_DEV_MAX_ERR;
	}

	res = StTestExacRate(inDev);
	if (res != ST_OK)
	{
		return res;
	}

	if (((res = StDevNormPcieAddr(port1Name, locUsedPort.port[ST_PPORT].normName)) != ST_OK)
		|| ((stMainParams.numPorts == MAX_RXTX_PORTS)
			&& ((res = StDevNormPcieAddr(port2Name, locUsedPort.port[ST_RPORT].normName)) != ST_OK)))
	{
		return res;
	}
	if ((stMainParams.numPorts == MAX_RXTX_PORTS)
		&& (strncmp(locUsedPort.port[ST_PPORT].normName, locUsedPort.port[ST_RPORT].normName,
				   ST_PCIE_ADDR_LEN)
			   == 0))
	{
		RTE_LOG(ERR, ST_DEV, ST_DEV_ERROR " Primary and Redundant ports must not be the same\n");
		return ST_DEV_BAD_PORT_NAME;
	}

	if (usedPortInfo.isDevTypesPrep)
	{
		if (strncmp(locUsedPort.port[0].normName, usedPortInfo.port[0].normName, ST_PCIE_ADDR_LEN)
			!= 0)
		{
			RTE_LOG(ERR, ST_DEV,
					ST_DEV_ERROR " Both port must be the same - second initialization\n");
			return ST_DEV_BAD_PORT_NAME;
		}
	}
	else
		usedPortInfo = locUsedPort;

	if (inDev->type == ST_DEV_TYPE_PRODUCER)
	{
		if (usedPortInfo.isDevTypesPrep & ST_DEV_TYPE_PRODUCER_USED)
		{
			RTE_LOG(ERR, ST_DEV, ST_DEV_ERROR "Maximum producer achieved\n");
			return ST_DEV_PORT_MAX_TYPE_PREP;
		}
		d = &stSendDevice;
	}
	else if (inDev->type == ST_DEV_TYPE_CONSUMER)
	{
		if (usedPortInfo.isDevTypesPrep & ST_DEV_TYPE_CONSUMER_USED)
		{
			RTE_LOG(ERR, ST_DEV, ST_DEV_ERROR "Maximum consumer achieved\n");
			return ST_DEV_PORT_MAX_TYPE_PREP;
		}
		d = &stRecvDevice;
	}
	else
		return ST_INVALID_PARAM;  // next it not posible

	*(st_device_t *)d = *inDev;
	d->numPorts = stMainParams.numPorts;

	if (usedPortInfo.isDevTypesPrep == 0)
	{
		res = StDevInitDevs(&usedPortInfo);
		if (res < 0)
			return res;
	}

	res = StDevInitRxTx(d);
	if (res < 0)
	{
		return res;
	}

	if (usedPortInfo.isDevTypesPrep == 0)
	{
		res = StDevPrepMBuf(d);
		if (res < 0)
			return res;

		if ((res = StDevGetPortIds(d, &usedPortInfo)) != ST_OK)
			return res;

		stMainParams.mbufPool = d->mbufPool;
		stMainParams.rxPortId[ST_PPORT] = d->dev.port[ST_PPORT];
		stMainParams.txPortId[ST_PPORT] = d->dev.port[ST_PPORT];
		if (stMainParams.numPorts == MAX_RXTX_PORTS)
		{
			stMainParams.rxPortId[ST_RPORT] = d->dev.port[ST_RPORT];
			stMainParams.txPortId[ST_RPORT] = d->dev.port[ST_RPORT];
		}
	}
	// in future need remove - MainParams must be compatibyle yet
	d->mbufPool = stMainParams.mbufPool;
	d->dev.port[ST_PPORT] = stMainParams.rxPortId[ST_PPORT];
	if (stMainParams.numPorts == MAX_RXTX_PORTS)
		d->dev.port[ST_RPORT] = stMainParams.rxPortId[ST_RPORT];
	d->fmtIndex = stMainParams.fmtIndex;

	if (d->dev.type == ST_DEV_TYPE_PRODUCER)
	{
		res = StDevRvRtpInitSend(&stMainParams, d);
		if (res < 0)
		{
			RTE_LOG(ERR, ST_DEV, ST_DEV_ERROR "Cannot prepare producer\n");
			return ST_DEV_CANNOT_PREP_PRODUCER;
		}
		usedPortInfo.isDevTypesPrep |= ST_DEV_TYPE_PRODUCER_USED;
	}
	else
	{
		res = StDevRvRtpInitRecv(&stMainParams, d);
		if (res < 0)
		{
			RTE_LOG(ERR, ST_DEV, ST_DEV_ERROR "Cannot prepare consumer\n");
			return ST_DEV_CANNOT_PREP_CONSUMER;
		}
		usedPortInfo.isDevTypesPrep |= ST_DEV_TYPE_CONSUMER_USED;
	}

	d->dev.snCount = stMainParams.snCount;
	d->dev.sn30Count = stMainParams.sn30Count;
	d->dev.sn40Count = stMainParams.sn40Count;

	if (!isKniActive && d->txRing != NULL)
	{
		uint16_t slvCoreRx = 0xff, slvCoreTx = 0xff;
		uint32_t rcvKniId = 0;
		for (int i = rte_get_next_lcore(currlCore, 1, 0); i < RTE_MAX_LCORE;
			 i = rte_get_next_lcore(i, 1, 0))
		{
			if (rcvKniId < ST_KNI_THEARD)
			{
				if (slvCoreRx == 0xff)
					slvCoreRx = i;
				else if (slvCoreTx == 0xff)
					slvCoreTx = i;
				currlCore = i;
				rcvKniId++;
			}
		}

		StInitKni(stMainParams.numPorts);

		for (int k = 0; k < stMainParams.numPorts; ++k)
		{
			uint16_t kniPortId;
			if (rte_eth_dev_get_port_by_name(stMainParams.outPortName[k], &kniPortId) != 0)
				return ST_DEV_BAD_PORT_NAME;
			if (!d->txRing[kniPortId][d->dev.maxSt21Sessions])
			{
				rte_exit(ST_GENERAL_ERR, "KNI ring is not initialized");
			}
			kni[k] = StInitKniConf(kniPortId, d->mbufPool, 0, 6,
								   d->txRing[kniPortId][d->dev.maxSt21Sessions], k);
			if (!kni[k])
			{
				rte_exit(ST_GENERAL_ERR, "Fail of KNI. Try run `insmod "
										 "$RTE_SDK/$RTE_TARGET/kmod/rte_kni.ko carrier=on`\n");
			}
		}
		StStartKni(slvCoreRx, slvCoreTx, kni);

		printf("##### KNI TX runned on the %d lcore #####\n", slvCoreTx);
		printf("##### KNI RX runned on the %d lcore #####\n", slvCoreRx);
		isKniActive = true;

		StDevInitBkgTasks();
		printf("##### StDevInitBkgTasks #####\n");
	}

	*outDev = (st_device_t *)d;

	return ST_OK;
}

st_status_t
StDestroyDevice(st_device_t *dev)
{
	st_status_t status = ST_OK;

	status = StValidateDevice(dev);
	if (status != ST_OK)
	{
		return status;
	}

	if (dev->type == ST_DEV_TYPE_PRODUCER)
	{
		rte_atomic32_set(&isTxDevToDestroy, 1);
		usedPortInfo.isDevTypesPrep &= ~ST_DEV_TYPE_PRODUCER_USED;
	}
	else if (dev->type == ST_DEV_TYPE_CONSUMER)
	{
		rte_atomic32_set(&isRxDevToDestroy, 1);
		usedPortInfo.isDevTypesPrep &= ~ST_DEV_TYPE_CONSUMER_USED;
	}
	rte_atomic32_set(&isStopMainThreadTasks, 1);

	StDevStopBkgTasks();
	StStopKni(kni);
	if (0 == usedPortInfo.isDevTypesPrep) /* No any producer/consumer now */
		StDevExitDevs(&usedPortInfo);

	return status;
}

unsigned short
siblingCore(unsigned short core)
{
	FILE *fp = NULL;
	char cmd1[512] = { '\0' };

	snprintf(cmd1, 512, "/sys/devices/system/cpu/cpu%u/topology/thread_siblings_list", core);

	fp = fopen(cmd1, "r");
	if (fp == NULL)
		return core;

	if (fgets(cmd1, 511, fp) == NULL)
	{
		return core;
	}

	char *coresStr[2];
	if (2 == rte_strsplit(cmd1, strlen(cmd1), coresStr, 2, ','))
	{
		char siblingCoreStr[16];
		snprintf(siblingCoreStr, strlen(coresStr[1]), "%s", coresStr[1]);

		unsigned short core1 = core;
		unsigned short core2 = atoi(siblingCoreStr);

		if (core1 == core)
			return core2;
		else
			return core1;
	}

	return core;
}

void
StGetAppAffinityCores(uint16_t start_id, cpu_set_t *app_cpuset)
{
	int index = 0;

	if (app_cpuset == NULL)
		return;

	CPU_ZERO(app_cpuset);
	/* populate all threads I can use */
	for (int i = start_id; i < get_nprocs_conf(); i++)
	{
		CPU_SET(i, app_cpuset);
	}

	/* If no user input, then remove all DPDK threads */
	/* TODO can just remove library core */
	if (!start_id)
	{
		unsigned int coreId = 0;
		RTE_LCORE_FOREACH(coreId)
		{
			index = coreId;
			CPU_CLR(index, app_cpuset);
			CPU_CLR(siblingCore(index), app_cpuset);
		}
	}

	/* remove gui thread */

	/* remove OS thread */
	index = 0;
	CPU_CLR(index, app_cpuset);
	CPU_CLR(siblingCore(0), app_cpuset);

	return;
}

static unsigned short
getNicNuma(char *nicAddr)
{
	FILE *fp = NULL;
	char cmd1[512] = { '\0' };

	snprintf(cmd1, 512, "/sys/bus/pci/devices/%s/numa_node", nicAddr);
	if (access(cmd1, F_OK) == 0)
	{
		fp = fopen(cmd1, "r");
		if (fp != NULL)
		{
			if (fgets(cmd1, 511, fp) != NULL)
				return atoi(cmd1);
		}
	}

	return -1;
}

static int
isNumaCore(unsigned short core, uint8_t numa)
{
	char cmd1[512] = { '\0' };

	snprintf(cmd1, 512, "/sys/devices/system/cpu/cpu%d/node%d/", core, numa);
	return (access(cmd1, F_OK) == 0);
}

static unsigned int
freeHugeNuma(unsigned short numa)
{
	int numaMem = 0;
	FILE *fp = NULL;
	char cmd1[512] = { '\0' };

	snprintf(cmd1, 512,
			 "/sys/devices/system/node/node%u/hugepages/hugepages-1048576kB/free_hugepages", numa);
	fp = fopen(cmd1, "r");
	if (fp != NULL)
	{
		if (fgets(cmd1, 511, fp) != NULL)
		{
			numaMem = atoi(cmd1);
		}
	}

	return numaMem;
}

static int
getPowerCore(unsigned short core, char *result, uint8_t resultLen)
{
	FILE *fp = NULL;
	char cmd1[512] = { '\0' };

	memset(result, '\0', resultLen);

	snprintf(cmd1, 512, "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_governor", core);
	fp = fopen(cmd1, "r");
	if (fp == NULL)
		return -1;

	if (fgets(result, resultLen, fp) == NULL)
		return -2;

	return 0;
}

int
getCore(rte_cpuset_t *libcore, uint16_t flags)
{
	uint8_t coreSelection = flags & 0xf;
	uint8_t numaMask = (flags & 0xf0) >> 4;
	if ((libcore == NULL) || (coreSelection == 0) || (numaMask == 0))
	{
		RTE_LOG(ERR, ST_DEV, ST_DEV_ERROR "invalid params for %s!\n", __func__);
		return -1;
	}

	rte_cpuset_t powersave_cpuset, ondemand_cpuset, performance_cpuset, unknown_cpuset;
	CPU_ZERO(libcore);
	CPU_ZERO(&powersave_cpuset);
	CPU_ZERO(&ondemand_cpuset);
	CPU_ZERO(&performance_cpuset);
	CPU_ZERO(&unknown_cpuset);

	uint8_t minCores = RTE_MIN(get_nprocs_conf(), get_nprocs());
	uint8_t numaCount = numa_num_configured_nodes();

	if (numa_available() == 0)
	{
		for (int index = 0; index < minCores; index++)
		{
			char result[25];
			if (!getPowerCore(index, result, (uint8_t) sizeof(result)))
				CPU_SET(index,
						(strncmp("powersave", result, strlen("powersave")) == 0)
							? &powersave_cpuset
							: (strncmp("ondemand", result, strlen("ondemand")) == 0)
								  ? &ondemand_cpuset
								  : (strncmp("performance", result, strlen("performance")) == 0)
										? &performance_cpuset
										: &unknown_cpuset);
		}

		if (CPU_COUNT(&unknown_cpuset))
		{
			/* CPU under VM or no power governer */
			CPU_OR(&powersave_cpuset, &powersave_cpuset, &unknown_cpuset);
			CPU_OR(&ondemand_cpuset, &ondemand_cpuset, &unknown_cpuset);
			CPU_OR(&performance_cpuset, &performance_cpuset, &unknown_cpuset);
		}

		switch (coreSelection)
		{
		case 1:
			CPU_OR(libcore, libcore, &performance_cpuset);
			RTE_LOG(DEBUG, ST_DEV, "Cores in performance are (%u)\n",
					CPU_COUNT(&performance_cpuset));
			break;

		case 2:
			CPU_OR(libcore, libcore, &powersave_cpuset);
			RTE_LOG(DEBUG, ST_DEV, "Cores in powersave are (%u)\n", CPU_COUNT(&powersave_cpuset));
			break;

		case 4:
			CPU_OR(libcore, libcore, &ondemand_cpuset);
			RTE_LOG(DEBUG, ST_DEV, "Cores in ondemand are (%u)\n", CPU_COUNT(&ondemand_cpuset));
			break;

		case 8:
			CPU_OR(libcore, libcore, &performance_cpuset);
			CPU_OR(libcore, libcore, &powersave_cpuset);
			CPU_OR(libcore, libcore, &ondemand_cpuset);
			break;

		default:
			RTE_LOG(ERR, ST_DEV, ST_DEV_ERROR "CPU power options needs to passed in %s\n",
					__func__);
			break;
		}

		uint16_t countCores = CPU_COUNT(libcore);
		if (countCores)
		{
			uint8_t oneGbHuge = 0;

			rte_cpuset_t siblingPerformance_cpuset[4];
			CPU_ZERO(&siblingPerformance_cpuset[0]);
			CPU_ZERO(&siblingPerformance_cpuset[1]);
			CPU_ZERO(&siblingPerformance_cpuset[2]);
			CPU_ZERO(&siblingPerformance_cpuset[3]);

			RTE_LOG(DEBUG, ST_DEV, "NUMA mask %x\n ", numaMask);
			for (int indexNuma = 0; (numaMask != 0); numaMask = numaMask >> 1, indexNuma += 1)
			{
				if ((indexNuma + 1) > numaCount)
					return -5;

				if (numaMask & 1)
				{
					oneGbHuge = freeHugeNuma(indexNuma);
					RTE_LOG(DEBUG, ST_DEV, "NUMA %d\n ", indexNuma);
					if (oneGbHuge >= 2)
					{
						for (int index = 0; ((countCores > 0) && (index < minCores)); index++)
						{
							if (!CPU_ISSET(index, libcore) || (numa_node_of_cpu(index) != indexNuma))
								continue;

							int ret = isNumaCore(index, indexNuma);
							RTE_LOG(DEBUG, ST_DEV,
									"1GB Huge page count (%u) on NUMA (%u) CPU (%u) is same NUMA "
									"(%d)\n",
									oneGbHuge, indexNuma, index, ret);

							if (ret == 0)
								continue;

							RTE_LOG(DEBUG, ST_DEV, "NUMA %u Core %u Sibling %u\n", indexNuma, index,
									siblingCore(index));
							CPU_SET(index, &(siblingPerformance_cpuset[indexNuma]));
							CPU_SET(siblingCore(index), &(siblingPerformance_cpuset[indexNuma]));

							countCores--;
						}
					}
				}
			}

			RTE_LOG(
				DEBUG, ST_DEV, "NUMA: 0 - %d, 1 -%d, 2 - %d, 3 - %d\n",
				CPU_COUNT(&siblingPerformance_cpuset[0]), CPU_COUNT(&siblingPerformance_cpuset[1]),
				CPU_COUNT(&siblingPerformance_cpuset[2]), CPU_COUNT(&siblingPerformance_cpuset[3]));

			CPU_ZERO(libcore);
			CPU_OR(libcore, libcore, &siblingPerformance_cpuset[0]);
			CPU_OR(libcore, libcore, &siblingPerformance_cpuset[1]);
			CPU_OR(libcore, libcore, &siblingPerformance_cpuset[2]);
			CPU_OR(libcore, libcore, &siblingPerformance_cpuset[3]);

			/* removing OS cores */
			if (CPU_ISSET(0, libcore))
				CPU_CLR(0, libcore);
			if (CPU_ISSET(siblingCore(0), libcore))
				CPU_CLR(siblingCore(0), libcore);

			if (CPU_COUNT(libcore) == 0)
			{
				RTE_LOG(ERR, ST_DEV, ST_DEV_ERROR "NUMA mask %x, 1GB Huge Pages are (%d)!\n",
						numaMask, oneGbHuge);
				RTE_LOG(ERR, ST_DEV, ST_DEV_ERROR "there are no CPU cores statisfying the flag!\n");
				return -3;
			}

			RTE_LOG(DEBUG, ST_DEV, "libcore %p Flag %x cores %u\n", libcore, flags,
					CPU_COUNT(libcore));
			return 0;
		}
		else
			return -2;
	}
	return -4;
}
