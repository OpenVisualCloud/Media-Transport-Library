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

#include <rte_eal.h>

#include "st_api.h"
#include "st_pkt.h"
#include "st_stats.h"

#include <fcntl.h>
#include <numa.h>
#include <rvrtp_main.h>
#include <st_api.h>
#include <st_kni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

const static st_nic_rate_params_t stNicParamsTable[NIC_RATE_SPEED_COUNT] =
{
    {
        ST_NIC_RATE_SPEED_10GBPS,
        ST_MAX_SESSIONS_25FPS_10GBPS,
        ST_MAX_SESSIONS_29FPS_10GBPS,
        ST_MAX_SESSIONS_50FPS_10GBPS,
        ST_MAX_SESSIONS_59FPS_10GBPS,
        ST_MAX_TX_RINGS_10GBPS,
        ST_MAX_RX_RINGS_10GBPS,
        ST_MAX_SCH_THREADS_10GBPS,
        ST_MAX_ENQ_THREADS_10GBPS,
        ST_MAX_RCV_THREADS_10GBPS
    },
    {
        ST_NIC_RATE_SPEED_25GBPS,
        ST_MAX_SESSIONS_25FPS_25GBPS,
        ST_MAX_SESSIONS_29FPS_25GBPS,
        ST_MAX_SESSIONS_50FPS_25GBPS,
        ST_MAX_SESSIONS_59FPS_25GBPS,
        ST_MAX_TX_RINGS_25GBPS,
        ST_MAX_RX_RINGS_25GBPS,
        ST_MAX_SCH_THREADS_25GBPS,
        ST_MAX_ENQ_THREADS_25GBPS,
        ST_MAX_RCV_THREADS_25GBPS
    },
    {
        ST_NIC_RATE_SPEED_40GBPS,
        ST_MAX_SESSIONS_25FPS_40GBPS,
        ST_MAX_SESSIONS_29FPS_40GBPS,
        ST_MAX_SESSIONS_50FPS_40GBPS,
        ST_MAX_SESSIONS_59FPS_40GBPS,
        ST_MAX_TX_RINGS_40GBPS,
        ST_MAX_RX_RINGS_40GBPS,
        ST_MAX_SCH_THREADS_40GBPS,
        ST_MAX_ENQ_THREADS_40GBPS,
        ST_MAX_RCV_THREADS_40GBPS
    },
    {
        ST_NIC_RATE_SPEED_100GBPS,
        ST_MAX_SESSIONS_25FPS_100GBPS,
        ST_MAX_SESSIONS_29FPS_100GBPS,
        ST_MAX_SESSIONS_50FPS_100GBPS,
        ST_MAX_SESSIONS_59FPS_100GBPS,
        ST_MAX_TX_RINGS_100GBPS,
        ST_MAX_RX_RINGS_100GBPS,
        ST_MAX_SCH_THREADS_100GBPS,
        ST_MAX_ENQ_THREADS_100GBPS,
        ST_MAX_RCV_THREADS_100GBPS
    }
};

const st_nic_rate_params_t *
StDevFindDevConf(uint8_t nicSpeedRate, const st_nic_rate_params_t *nicParamsTable)
{
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
	strncpy(pcieAddr, portInName, ST_PCIE_ADDR_LEN);
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
StDevGetCPUs(int32_t soc, int32_t *lowMn, int32_t *lowMx, int32_t *highMn, int32_t *highMx)
{
	FILE *fp;
	char cl[64];
	snprintf(cl, sizeof(cl), "/sys/devices/system/node/node%d/cpulist", soc);
	fp = fopen(cl, "r");
	if (fp == NULL)
		return ST_DEV_CANNOT_READ_CPUS;
	if(fgets(cl, sizeof(cl), fp) == NULL)
	{
		return ST_GENERAL_ERR;
	}
	fclose(fp);
	if (sscanf(cl, "%d-%d,%d-%d", lowMn, lowMx, highMn, highMx) < 4)
		return ST_DEV_CANNOT_READ_CPUS;
	return ST_OK;
}

#if 0
static st_status_t
StDevIsBinded(const char *portName)
{
	FILE *fp;
	char buf[150];
	snprintf(buf, sizeof(buf),
			 "dpdk-devbind.py -s | grep %s | grep -E -e \"(drv=mlx)|(drv=igb_uio)\"", portName);
	fp = popen(buf, "r");
	if (fp == NULL)
		return ST_GENERAL_ERR;
	buf[0] = 0;
	fgets(buf, sizeof(buf), fp);
	pclose(fp);
	if (strlen(buf) == 0)
		return ST_DEV_MOD_NOT_BINDED;
	return ST_OK;
}
#endif

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

#if 0
static st_status_t
StDevModUnLoad(const char *drvName)
{
	FILE *fp;
	char buf[32];
	snprintf(buf, sizeof(buf), "rmmod %s", drvName);
	fp = popen(buf, "r");
	if (fp == NULL)
		return ST_GENERAL_ERR;
	if (pclose(fp) < 0)
		return ST_DEV_CANNOT_UNLOAD_MOD;
	return ST_OK;
}
#endif

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
	char *argv[16];
	char coreList[20];
} st_eal_args_t;

typedef enum
{
	ST_DEV_TYPE_PRODUCER_USED = 1 << ST_DEV_TYPE_PRODUCER,
	ST_DEV_TYPE_CONSUMER_USED = 1 << ST_DEV_TYPE_CONSUMER,
} st_dev_used_flags;

typedef struct st_used_dev_info
{
	unsigned int isDevTypesPrep;
	st_port_info_t port[2];
} st_used_dev_info_t;

static st_used_dev_info_t usedPortInfo;

st_kni_ms_conf_t *kni = NULL;
uint32_t currlCore = -1;
bool isSchActive = false;
bool isKniActive = false;

static st_status_t
StDevTryGetEthLinkSpeed(const char *portName, int *speed, char *eth)
{
	FILE *fp;
	char buf[512];

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
	snprintf(buf, sizeof(buf), "ip link set %s up ", eth);
	fp = popen(buf, "r");
	if (fp == NULL)
		return ST_INVALID_PARAM;
	if (pclose(fp) < 0)
		return ST_INVALID_PARAM;
	snprintf(buf, sizeof(buf), "/sys/class/net/%s/speed", eth);
	do
	{
		usleep(200000);
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
		return ST_INVALID_PARAM;
	buf[0] = 0;
	ln = fgets(buf, sizeof(buf), fp);
	if (pclose(fp) < 0)
		return ST_INVALID_PARAM;
	for (i = 0; i < sizeof(krnDrvNames) / sizeof(krnDrvNames[0]); i++)
	{
		ln = strstr(buf, krnDrvNames[i]);
		if (ln != NULL)
			break;
	}
	if (i >= sizeof(krnDrvNames) / sizeof(krnDrvNames[0]))
		return ST_INVALID_PARAM;
	if (StDevBind(krnDrvNames[i], portName) < 0)
		return ST_INVALID_PARAM;
	if (StDevTryGetEthLinkSpeed(portName, speed, eth) < 0)
		return ST_INVALID_PARAM;
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

#if 0
static st_status_t
StDevGetNumaSize(int soc)
{
    FILE * fp;
    st_status_t res = ST_GENERAL_ERR;
    char buf[150];
    return res;
}


static st_status_t
StDevSetNumaSize(int soc, int sizeGB)
{
    FILE * fp;
    st_status_t res = ST_GENERAL_ERR;
    char buf[150];
    return res;
}
#endif

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

static char ethName[RTE_KNI_NAMESIZE];

const char *
StDevGetKniInterName(void)
{
	if (ethName[0] == 0)
		return NULL;
	return ethName;
}
const st_nic_rate_params_t *stDevParams = NULL;

static char namePrg[] = "InitMediaStreamerLibrary";
static char socketMemVal[] = "1024,1024";
static char socketMemPar[] = "--socket-mem";
static char portPar[] = "-w";
static char procListPar[] = "-l";

static st_status_t
StDevInitParams(st_eal_args_t *a, st_used_dev_info_t *p)
{
	int loMin, loMax, hiMin, hiMax;
	uint8_t bus;

	a->argc = 0;
	a->argv[a->argc] = namePrg;
	a->argc++;
	a->argv[a->argc] = portPar;
	a->argc++;
	a->argv[a->argc] = p->port[0].normName;
	a->argc++;
	if (strcmp(p->port[0].normName, p->port[1].normName) != 0)
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

	if (StDevGetPcieDevBus(p->port[0].normName, &bus) != ST_OK)
		return ST_DEV_GENERAL_ERR;
	if (StDevGetCPUs(bus < ST_PCIE_SEC_SOC_BUS ? 0 : 1, &loMin, &loMax, &hiMin, &hiMax) != ST_OK)
		return ST_DEV_GENERAL_ERR;
	stDevParams = StDevFindDevConf(p->port[0].speed / 1000, stNicParamsTable);
	if (stDevParams == NULL)
		return ST_DEV_NOT_FIND_SPEED_CONF;
	int lcCount = 1 + stDevParams->maxEnqThrds + stDevParams->maxSchThrds + stDevParams->maxRcvThrds
				  + ST_KNI_THEARD;
	if (lcCount > (loMax + 1 + hiMax - hiMin - 2 * ST_FREE_LCORES_ON_CPU))
		return ST_DEV_GENERAL_ERR;
	// TODO: ? Maybe -> different strategy of lcores
	if ((lcCount + ST_FREE_LCORES_ON_CPU) <= (hiMax - hiMin + 1))
	{
		// get from high lcores
		lcCount = hiMax - lcCount + 1;
		snprintf(a->coreList, sizeof(a->coreList), "%d-%d", lcCount, hiMax);
	}
	else
	{
		// get from low and next from high
		int loLc = loMin + ST_FREE_LCORES_ON_CPU;
		lcCount -= loMax - loLc + 1;
		int hiLc = hiMax - lcCount + 1;

		//        int hiLc = hiMin + ST_FREE_LCORES_ON_CPU;
		//        lcCount -= hiMax - hiLc + 1;
		//        int loLc = loMax - lcCount + 1;
		snprintf(a->coreList, sizeof(a->coreList), "%d,%d-%d,%d-%d", hiMax, loLc, loMax, hiLc,
				 hiMax - 1);
	}
	a->argv[a->argc] = a->coreList;
	a->argc++;
	return ST_OK;
}

static st_status_t
StDevInitDevs(st_used_dev_info_t *p)
{
	st_eal_args_t args = { 0 };
	if (StDevPrepMods() != ST_OK)
		return ST_GENERAL_ERR;
	if (StDevTryGetEthLinkSpeed(p->port[0].normName, &p->port[0].speed, ethName) != ST_OK)
	{
		if (StDevGetEthLinkSpeed(p->port[0].normName, &p->port[0].speed, ethName) != ST_OK)
		{
			memset(ethName, 0, sizeof(ethName));
			return ST_DEV_BAD_PORT_NAME;
		}
	}
	StDevDownNetClass(ethName);
	StDevUnbind(p->port[0].normName);
	if (StDevBind(StDevGetDpdkCardDrvName(p->port[0].normName), p->port[0].normName) != ST_OK)
		return ST_DEV_BAD_PORT_NAME;
	;
	if (strcmp(p->port[0].normName, p->port[1].normName) != 0)
	{
		if (StDevTryGetEthLinkSpeed(p->port[1].normName, &p->port[1].speed, ethName) != ST_OK)
		{
			if (StDevGetEthLinkSpeed(p->port[1].normName, &p->port[1].speed, ethName) != ST_OK)
				return ST_DEV_BAD_PORT_NAME;
		}
		StDevDownNetClass(ethName);
		StDevUnbind(p->port[1].normName);
		if (StDevBind(StDevGetDpdkCardDrvName(p->port[1].normName), p->port[1].normName) != ST_OK)
			return ST_DEV_BAD_PORT_NAME;
	}
	else
		p->port[1].speed = 0;
	st_status_t res = StDevTestNuma();
	if (res < 0)
		return res;
	if (StDevInitParams(&args, p) < 0)
		return ST_DEV_BAD_PORT_NAME;
	args.argv[args.argc++] = "-v";
	if (stMainParams.dpdkParams != 0)
	{
		args.argv[args.argc++] = stMainParams.dpdkParams;
	}
	args.argv[args.argc] = NULL;
	if (rte_eal_init(args.argc, args.argv) < 0)
		return ST_DEV_BAD_PORT_NAME;
	return ST_OK;
}

static st_status_t
StDevPrepMBuf(rvrtp_device_t *d)
{
	struct rte_mempool *mbufPool;
	/* Creates a new mempool in memory to hold the mbufs. */
	mbufPool = rte_pktmbuf_pool_create("MBUF_POOL", (1 << 18), MBUF_CACHE_SIZE, 0,
									   RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
	if (mbufPool == NULL)
		return ST_DEV_CANNOT_PREPARE_MBUF;
	d->mbufPool = mbufPool;
	return ST_OK;
}

static const struct rte_eth_conf portConf = { .rxmode = { .max_rx_pkt_len = RTE_ETHER_MAX_LEN } };

static const struct rte_eth_txconf txPortConf = { .tx_rs_thresh = 1, .tx_free_thresh = 1 };
static const struct rte_eth_rxconf rxPortConf = { .rx_free_thresh = 1 };

static st_status_t
StDevInitRtePort(uint16_t port, rvrtp_device_t *d)
{
	struct rte_eth_conf port_conf = portConf;
	struct rte_eth_dev_info devInfo;
	const uint16_t rxQueues = stDevParams->maxRxRings;
	const uint16_t txQueues = stDevParams->maxTxRings + 1;	// allocate one more for PTP
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

	struct rte_eth_fc_conf fcConf;

	ret = rte_eth_dev_flow_ctrl_get(port, &fcConf);
	if (ret != 0) {
		rte_exit(ST_GENERAL_ERR, "Error upon rte_eth_dev_flow_ctrl_get port %d info %s\n",
				 port, strerror(-ret));
	}

#if 0
	//possible value for fcConf.mode = RTE_FC_FULL/RTE_FC_TX_PAUSE/RTE_FC_NONE
	fcConf.mode = RTE_FC_TX_PAUSE;//RTE_FC_FULL;//TX FC at min
	fcConf.mac_ctrl_frame_fwd = 1;

	ret = rte_eth_dev_flow_ctrl_set(port, &fcConf);
	if (ret != 0) {
		rte_exit(ST_GENERAL_ERR, "Error upon rte_eth_dev_flow_ctrl_set port %d info %s\n",
				 port, strerror(-ret));
	}
#endif

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

static void
StDevInitRxTx(rvrtp_device_t *d)
{
	// rvrtp_device_t *recvDev = NULL;
	d->dev.ver.major = ST_VERSION_MAJOR_CURRENT;
	d->dev.ver.major = ST_VERSION_MINOR_CURRENT;
	d->dev.maxSt21Sessions = ST_MAX_SESSIONS_MAX;
	d->dev.maxSt30Sessions = ST_MAX_SESSIONS_MAX;
	d->dev.mtu = 1500;
	d->dev.rateGbps = stDevParams->nicSpeed;
	d->dev.pacerType = ST_2110_21_TPN;
}

static st_status_t
StDevGetPortIds(rvrtp_device_t *d, st_used_dev_info_t *p)
{
	uint16_t portId = 0;

	for (int i = 0; i < 2; i++)
	{
		int res = rte_eth_dev_get_port_by_name(p->port[i].normName, &portId);
		if (res < 0)
		{
			RTE_LOG(ERR, ST_DEV, ST_DEV_ERROR " Cannot find port %s\n", p->port[i].normName);
			return ST_DEV_GENERAL_ERR;
		}
		d->dev.port[i] = portId;
	}
	if (d->dev.port[0] == d->dev.port[1])
	{
		if (StDevInitRtePort(d->dev.port[0], d) != 0)
		{
			rte_exit(EXIT_FAILURE, "Cannot init port %" PRIu8 "\n", d->dev.port[0]);
			return ST_DEV_GENERAL_ERR;
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
StDevRvRtpInitRecv(st_main_params_t *mp, rvrtp_device_t *d)
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

	mp->snCount = MIN(mp->snCount, d->dev.maxSt21Sessions);

	d->snTable = rte_malloc_socket("snTable", d->dev.maxSt21Sessions * sizeof(rvrtp_session_t *),
								   RTE_CACHE_LINE_SIZE, rte_socket_id());

	if (!d->snTable)
	{
		rte_exit(ST_NO_MEMORY, "StDevRvRtpInitRecv cannot allocate few bytes");
	}
	memset(d->snTable, 0, d->dev.maxSt21Sessions * sizeof(rvrtp_session_t *));

	d->mbufPool = mp->mbufPool;
	d->rxOnly = mp->rxOnly;

	struct rte_ether_addr srcMac;
	rte_eth_macaddr_get(mp->rxPortId, &srcMac);

#ifdef TX_RINGS_DEBUG
	RTE_LOG(INFO, USER1, "RX SRC MAC address %02x:%02x:%02x:%02x:%02x:%02x\n", srcMac.addr_bytes[0],
			srcMac.addr_bytes[1], srcMac.addr_bytes[2], srcMac.addr_bytes[3], srcMac.addr_bytes[4],
			srcMac.addr_bytes[5]);

#endif

	memcpy(&d->srcMacAddr[0][0], &srcMac, ETH_ADDR_LEN);

	StDevInitRxThreads(mp, d);

	return ST_OK;
}

static st_status_t
StDevRvRtpInitSend(st_main_params_t *mp, rvrtp_device_t *d)
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
		RTE_LOG(INFO, USER1,
			"Requested number of TX sessions (%d) is higher than allowed maximum sessions (%d). "
			"Number of sessions set to the %d sessions.\n",
			mp->snCount, d->dev.maxSt21Sessions, d->dev.maxSt21Sessions);
	}

	mp->snCount = MIN(mp->snCount, d->dev.maxSt21Sessions);

	d->snTable = rte_malloc_socket("snTable", d->dev.maxSt21Sessions * sizeof(rvrtp_session_t *),
								   RTE_CACHE_LINE_SIZE, rte_socket_id());

	d->timeTable = rte_malloc_socket("timeTable", d->dev.maxSt21Sessions * sizeof(uint32_t),
									 RTE_CACHE_LINE_SIZE, rte_socket_id());

	d->txRing = rte_malloc_socket("txRing", d->maxRings * sizeof(struct rte_ring *),
								  RTE_CACHE_LINE_SIZE, rte_socket_id());

	d->txPktSizeL1 = rte_malloc_socket("txPktSizeL1", d->maxRings * sizeof(uint64_t),
									   RTE_CACHE_LINE_SIZE, rte_socket_id());

	d->packetsTx = rte_malloc_socket("packetsTx", (d->maxRings + 1) * sizeof(uint64_t),
									 RTE_CACHE_LINE_SIZE, rte_socket_id());

	d->pausesTx = rte_malloc_socket("pausesTx", (d->maxRings + 1) * sizeof(uint64_t),
									RTE_CACHE_LINE_SIZE, rte_socket_id());

	if ((!d->snTable) || (!d->timeTable) || (!d->txRing) || (!d->txPktSizeL1) || (!d->packetsTx)
		|| (!d->pausesTx))
	{
		rte_exit(ST_NO_MEMORY, "RvRtpInitSendDevice cannot allocate few bytes");
	}

	memset(d->snTable, 0, d->dev.maxSt21Sessions * sizeof(rvrtp_session_t *));
	memset(d->timeTable, 0, d->dev.maxSt21Sessions * sizeof(uint32_t));
	memset(d->txRing, 0, d->maxRings * sizeof(struct rte_ring *));
	memset(d->txPktSizeL1, 0, d->maxRings * sizeof(uint64_t));
	memset(d->packetsTx, 0, (d->maxRings + 1) * sizeof(uint64_t));
	memset(d->pausesTx, 0, (d->maxRings + 1) * sizeof(uint64_t));

	struct rte_ether_addr srcMac;
	rte_eth_macaddr_get(mp->txPortId, &srcMac);

	RTE_LOG(INFO, USER1, "TX SRC MAC address %02x:%02x:%02x:%02x:%02x:%02x\n", srcMac.addr_bytes[0],
			srcMac.addr_bytes[1], srcMac.addr_bytes[2], srcMac.addr_bytes[3], srcMac.addr_bytes[4],
			srcMac.addr_bytes[5]);

	memcpy(&d->srcMacAddr[0][0], &srcMac, ETH_ADDR_LEN);

	for (uint32_t i = 0; i < d->maxRings; i++)
	{
		char ringName[32];

		snprintf(ringName, 32, "SMPTE-RING-%u", i);

		/* Create per session ring to the TPRS scheduler */
		struct rte_ring *smpteRing = 
		    rte_ring_create(ringName, 0x1 << 10, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);

		d->txRing[i] = smpteRing;
#ifdef TX_RINGS_DEBUG
		RTE_LOG(INFO, USER1, "RvRtpInitSendDevice %s %p\n", ringName, smpteRing);
#endif
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
			d->txPktSizeL1[i] = d->snTable[i]->fmt.pktSize + ST_PHYS_PKT_ADD;
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

static rte_atomic32_t isStopBkgTask;
static pthread_t devBkgTaskTid;

extern void ArpRequest(uint16_t portid, uint32_t ip);

#define ST_BKG_TICK (100 * 1000)  // us
#define ST_BKG_STS_PER (5 * 1000 * 1000)  // us
#define ST_BKG_ARP_PER (5 * 1000 * 1000)  // us
#define ST_BKG_KNI_PER (2 * 1000 * 1000)  // us

#define ST_TEST_PER_AND_DO(curStamp, taskStamp, taskPer, task)                                     \
	do                                                                                             \
	{                                                                                              \
		if (taskStamp > curStamp)                                                                  \
			break;                                                                                 \
		if ((curStamp - taskStamp) < taskPer)                                                      \
			break;                                                                                 \
		taskStamp = curStamp;                                                                      \
		task;                                                                                      \
	} while (0)

static void *
StDevBkgTasks(void *arg)
{
	uint64_t curStamp, stsStamp, arpStamp, kniStamp;
	curStamp = StPtpGetTime();
	stsStamp = curStamp;
	arpStamp = curStamp;
	kniStamp = curStamp;
	while (rte_atomic32_read(&isStopBkgTask) == 0)
	{
		rte_delay_us_sleep(ST_BKG_TICK);
		curStamp = StPtpGetTime();
		ST_TEST_PER_AND_DO(curStamp, stsStamp, ST_BKG_STS_PER, StStsTask(stMainParams.rxPortId));
		ST_TEST_PER_AND_DO(curStamp, arpStamp, ST_BKG_ARP_PER,
						   ArpRequest(stMainParams.rxPortId, *(uint32_t *)stMainParams.ipAddr));
		ST_TEST_PER_AND_DO(curStamp, kniStamp, ST_BKG_KNI_PER, StKniBkgTask());
	}
	return NULL;
}

static st_status_t
StDevInitBkgTasks(void)
{
	rte_atomic32_set(&isStopBkgTask, 0);
	int ret = rte_ctrl_thread_create(&devBkgTaskTid, "Dev ", NULL, StDevBkgTasks, NULL);
	if (ret < 0)
		return ST_GENERAL_ERR;
	return ST_OK;
}

static st_status_t
StDevStopBkgTasks(void)
{

	rte_atomic32_set(&isStopBkgTask, 1);
	pthread_join(devBkgTaskTid, NULL);
	return ST_OK;
}

st_status_t
StStartDevice(st_device_t *dev)
{
	st_status_t status = ST_OK;

	status = RvRtpValidateDevice(dev);
	if (status != ST_OK)
	{
		return status;
	}

	rvrtp_device_t *d = (rvrtp_device_t *)dev;

	uint32_t enqThrdId = 0;
	uint32_t schThrdId = 0;
	uint32_t rcvThrdId = 0;

	if (!isSchActive)
	{
		if (stDevParams->nicSpeed == ST_NIC_RATE_SPEED_100GBPS)
		{
			for (int i = rte_get_next_lcore(currlCore, 1, 0); i < RTE_MAX_LCORE;
				 i = rte_get_next_lcore(i, 1, 0))
			{
				if (schThrdId < stMainParams.maxSchThrds)
				{
					int ret = rte_eal_remote_launch(LcoreMainTransmitterBulk,
													(void *)((U64)schThrdId), i);
					if (ret != 0)
					{
						RTE_LOG(INFO, USER1, "Run TransmitterBulk not possible. Lcore not ready\n");
					}
					schThrdId++;
					currlCore = i;
					isSchActive = true;
					continue;
				}
			}
		}
		else
		{
			for (int i = rte_get_next_lcore(currlCore, 1, 0); i < RTE_MAX_LCORE;
				 i = rte_get_next_lcore(i, 1, 0))
			{
				if (schThrdId < stMainParams.maxSchThrds)
				{
					int ret = rte_eal_remote_launch(LcoreMainTransmitterSingle,
													(void *)((U64)schThrdId), i);
					if (ret != 0)
					{
						RTE_LOG(INFO, USER1, "Run TransmitterSingle not possible. Lcore not ready\n");
					}
					schThrdId++;
					currlCore = i;
					isSchActive = true;
					continue;
				}
			}
		}
	}

	if (d->dev.type == ST_DEV_TYPE_PRODUCER)
	{
		for (int i = rte_get_next_lcore(currlCore, 1, 0); i < RTE_MAX_LCORE;
			 i = rte_get_next_lcore(i, 1, 0))
		{
			if ((enqThrdId < stMainParams.maxEnqThrds) && ((stMainParams.rxOnly == 0)))
			{
				int ret = rte_eal_remote_launch(LcoreMainPktRingEnqueue, (void *)((U64)enqThrdId), i);
				if (ret != 0)
				{
					RTE_LOG(INFO, USER1, "Run RingEuqueue not possible. Lcore not ready\n");
				}
				enqThrdId++;
				currlCore = i;
				continue;
			}
		}
	}
	else
	{
		for (int i = rte_get_next_lcore(currlCore, 1, 0); i < RTE_MAX_LCORE;
			 i = rte_get_next_lcore(i, 1, 0))
		{
			if ((rcvThrdId < stMainParams.maxRcvThrds) && (stMainParams.txOnly == 0))
			{
				int ret = rte_eal_remote_launch(LcoreMainReceiver, (void *)((U64)rcvThrdId), i);
				if (ret != 0)
				{
					RTE_LOG(INFO, USER1, "Run Receiver not possible. Lcore not ready\n");
				}
				rcvThrdId++;
				currlCore = i;
				continue;
			}
		}
	}
	return status;
}

st_status_t
StCreateDevice(
    st_device_t *inDev,
    const char *port1Name,
    const char *port2Name,
    st_device_t **outDev)
{
	if (!inDev || !port1Name || !port2Name || !outDev)
	{
		return ST_INVALID_PARAM;
	}

	st_status_t res;
	st_used_dev_info_t locUsedPort = { 0 };
	rvrtp_device_t *d;

	stMainParams.schedStart = 0;
	stMainParams.ringStart = 0;
	stMainParams.ringBarrier1 = 0;
	stMainParams.ringBarrier2 = 0;

	rte_atomic32_set(&isTxDevToDestroy, 0);
	rte_atomic32_set(&isRxDevToDestroy, 0);

	//validate device a bit
	if ((inDev->maxSt21Sessions > ST_MAX_SESSIONS_MAX) || (inDev->maxSt30Sessions > ST_MAX_SESSIONS_MAX) ||
		(inDev->maxSt40Sessions > ST_MAX_SESSIONS_MAX))
	{
		return ST_DEV_MAX_ERR;
	}

	res = StTestExacRate(inDev);
	if (res != ST_OK)
	{
		return res;
	}

	if ((StDevNormPcieAddr(port1Name, locUsedPort.port[0].normName) != ST_OK) ||
		(StDevNormPcieAddr(port2Name, locUsedPort.port[1].normName) != ST_OK))
	{
		return ST_DEV_BAD_PORT_NAME;
	}
	if (strncmp(locUsedPort.port[0].normName, locUsedPort.port[1].normName, ST_PCIE_ADDR_LEN) != 0)
	{
		RTE_LOG(ERR, ST_DEV, ST_DEV_ERROR " Both port must be the same\n");
		return ST_DEV_BAD_PORT_NAME;
	}

	if (usedPortInfo.isDevTypesPrep)
	{
		if (strncmp(locUsedPort.port[0].normName, usedPortInfo.port[0].normName, ST_PCIE_ADDR_LEN)
			!= 0)
		{
			RTE_LOG(ERR, ST_DEV, ST_DEV_ERROR " Both port must be the same - decond initialization\n");
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

	if (usedPortInfo.isDevTypesPrep == 0)
	{
		res = StDevInitDevs(&usedPortInfo);
		if (res < 0)
			return res;
	}
	StDevInitRxTx(d);
	if (usedPortInfo.isDevTypesPrep == 0)
	{
		res = StDevPrepMBuf(d);
		if (res < 0)
			return res;

		stMainParams.mbufPool = d->mbufPool;
		stMainParams.rxPortId = d->dev.port[0];
		stMainParams.txPortId = d->dev.port[1];

		if (StDevGetPortIds(d, &usedPortInfo) < 0)
			return ST_DEV_GENERAL_ERR;
	}
	// in future need remove - MainParams must be compatibyle yet
	d->mbufPool = stMainParams.mbufPool;
	d->dev.port[0] = stMainParams.rxPortId;
	d->dev.port[1] = stMainParams.txPortId;
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

		if (!d->txRing[d->dev.maxSt21Sessions])
		{
			rte_exit(ST_GENERAL_ERR, "KNI ring is not initialized");
		}
		StInitKni(1);

		kni = StInitKniConf(d->dev.port[0], d->mbufPool, 0, 6, d->txRing[d->dev.maxSt21Sessions]);
		if (!kni)
		{
			rte_exit(ST_GENERAL_ERR, "Fail of KNI. Try run `insmod $RTE_SDK/$RTE_TARGET/kmod/rte_kni.ko carrier=on`\n");
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

	status = RvRtpValidateDevice(dev);
	if (status != ST_OK)
	{
		return status;
	}

	if (dev->type == ST_DEV_TYPE_PRODUCER)
	{
		rte_atomic32_set(&isTxDevToDestroy, 1);
	}
	else if (dev->type == ST_DEV_TYPE_CONSUMER)
	{
		rte_atomic32_set(&isRxDevToDestroy, 1);
	}

	StDevStopBkgTasks();
	StStopKni(kni);

	return status;
}
