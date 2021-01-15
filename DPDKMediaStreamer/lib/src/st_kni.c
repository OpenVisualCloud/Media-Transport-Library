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
 *    Media streamer based on DPDK
 *
 */

#include "st_kni.h"

#include <rte_atomic.h>
#include <rte_common.h>
#include <rte_debug.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_kni.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memcpy.h>
#include <rte_memory.h>
#include <rte_mempool.h>
#include <rte_per_lcore.h>

#include "st_igmp.h"

#include <net/if.h>
#include <st_ptp.h>
#include <sys/ioctl.h>
#include <time.h>

#define ST_KNI_NAME "vStKNI"
#define RTE_LOGTYPE_ST_KNI (RTE_LOGTYPE_USER1)
#define ST_KNI_ERROR "[ERROR] "
#define STATS_PEROD (20)

/* Max size of a single packet */
#define MAX_PACKET_SZ 2048
/* How many packets to attempt to read from NIC in one go */
#define PKT_BURST_SZ 32
/* How many objects (mbufs) to keep in per-lcore mempool cache */
#define MEMPOOL_CACHE_SZ PKT_BURST_SZ
/* Number of RX ring descriptors */
#define NB_RXD 1024
/* Number of TX ring descriptors */
#define NB_TXD 1024
/* Total octets in ethernet header */
#define KNI_ENET_HEADER_SIZE 14
/* Total octets in the FCS */
#define KNI_ENET_FCS_SIZE 4

struct st_kni_ms_conf
{
	uint16_t ethPortId;
	uint16_t rxRingNb;

	uint32_t txThread;
	struct rte_ring *txRing;

	int32_t isUp;

	struct rte_mempool *mbufPool;

	uint32_t slvCoreRx;
	uint32_t slvCoreTx;
	struct rte_kni_conf kniConf;
	struct rte_kni *kni;

	rte_atomic32_t isStop;
	rte_atomic32_t lnkUp;

	uint64_t rxPcks;
	uint64_t txPcks;
	uint64_t rxDrpd;
	uint64_t txDrpd;

	struct timespec rxTmr;
	struct timespec txTmr;
};

static st_kni_ms_conf_t *kniItfs = NULL;
static uint16_t firstEmptyKni = 0;
static uint32_t nbKni = 0;
extern st_status_t ParseEthernet(uint16_t portid, struct rte_mbuf *m);

#ifdef ST_KNI_DEBUG

static void
StPrintRecvStats(st_kni_ms_conf_t *c)
{
	struct timespec cur;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cur);
	if ((cur.tv_sec - c->rxTmr.tv_sec) > (STATS_PEROD - 1))
	{
		clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c->rxTmr);
		RTE_LOG(INFO, ST_KNI,
				"\n"
				"*** Receiver stats ***\n"
				"    ethPortId: %d, rxRingNb: %d\n"
				"    rxPcks: %lu\n"
				"    rxDrpd: %lu\n\n",
				c->ethPortId, c->rxRingNb, c->rxPcks, c->rxDrpd);
	}
}

static void
StPrintTranStats(st_kni_ms_conf_t *c)
{
	struct timespec cur;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cur);
	if ((cur.tv_sec - c->txTmr.tv_sec) > (STATS_PEROD - 1))
	{
		clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c->txTmr);
		RTE_LOG(INFO, ST_KNI,
				"\n"
				"*** Transmiter stats ***\n"
				"    txPcks: %lu\n"
				"    txDrpd: %lu\n\n",
				c->txPcks, c->txDrpd);
	}
}

#endif

static void
StKniBurstFreeMbufs(struct rte_mbuf **pkts, uint32_t num)
{
	uint32_t i;

	if (pkts == NULL)
		return;

	for (i = 0; i < num; i++)
	{
		rte_pktmbuf_free(pkts[i]);
		pkts[i] = NULL;
	}
}

static void
StKniRxLcore(st_kni_ms_conf_t *c)
{
	uint16_t portId;
	uint16_t rxRing;
	int32_t ret;
	int vlanOffload;

	if (c == NULL)
		return;

	portId = c->ethPortId;
	rxRing = c->rxRingNb;

	RTE_LOG(INFO, ST_KNI, "StKniRxLcore ethPortId: %d, rxRingNb %d - START\n", portId, rxRing);
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c->rxTmr);

        vlanOffload = rte_eth_dev_get_vlan_offload(portId);
	vlanOffload |= ETH_VLAN_STRIP_OFFLOAD;
	rte_eth_dev_set_vlan_offload(portId, vlanOffload);

	while (rte_atomic32_read(&c->isStop) == 0)
	{
		/* Burst rx from eth */
		struct rte_mbuf *pktsBurst[PKT_BURST_SZ];
		unsigned nbRx, num;
		rte_kni_handle_request(c->kni);
		if (!rte_atomic32_read(&c->lnkUp))
		{
			rte_delay_ms(100);
			continue;
		}

		nbRx = rte_eth_rx_burst(portId, rxRing, pktsBurst, PKT_BURST_SZ);
		if (unlikely(nbRx > PKT_BURST_SZ))
		{
			RTE_LOG(INFO, ST_KNI, "Error receiving from eth\n");
			continue;
		}
		for (unsigned i = 0; i < nbRx; ++i)
			StParseEthernet(c->ethPortId, pktsBurst[i]);

		/* Burst tx to kni */
		num = rte_kni_tx_burst(c->kni, pktsBurst, nbRx);

		if (num)
			c->rxPcks += num;

		ret = rte_kni_handle_request(c->kni);
		if (ret)
		{
			RTE_LOG(INFO, ST_KNI, "Error %d\n", ret);
		}
		if (unlikely(num < nbRx))
		{
			/* Free mbufs not tx to kni interface */
			StKniBurstFreeMbufs(&pktsBurst[num], nbRx - num);
			c->rxDrpd += nbRx - num;
		}
#ifdef ST_KNI_DEBUG
		StPrintRecvStats(c);
#endif
	}
	RTE_LOG(INFO, ST_KNI, "StKniRxLcore ethPortId: %d, rxRingNb %d - STOP\n", portId, rxRing);
}

static void
StKniTxLcore(st_kni_ms_conf_t *c)
{
	unsigned nb_tx, num;
	struct rte_ring *txRing;
	struct rte_mbuf *pktsBurst[PKT_BURST_SZ];

	if (!c)
	{
		ST_ASSERT;
	}

	txRing = c->txRing;
	RTE_LOG(INFO, ST_KNI, " StKniTxLcore txRing - START\n");

	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c->txTmr);

	while (rte_atomic32_read(&c->isStop) == 0)
	{
		if (!rte_atomic32_read(&c->lnkUp))
		{
			rte_delay_ms(100);
			continue;
		}
		/* Burst rx from kni */
		num = rte_kni_rx_burst(c->kni, pktsBurst, PKT_BURST_SZ);
		if (unlikely(num > PKT_BURST_SZ))
		{
			RTE_LOG(INFO, ST_KNI, "Error receiving from KNI\n");
			continue;
		}
		/* Burst tx to eth */
		nb_tx = rte_ring_sp_enqueue_bulk(txRing, (void **)pktsBurst, num, NULL);
		if (nb_tx)
			c->txPcks += nb_tx;
		if (unlikely(nb_tx < num))
		{
			/* Free mbufs not tx to NIC */
			StKniBurstFreeMbufs(&pktsBurst[nb_tx], num - nb_tx);
			c->txDrpd += num - nb_tx;
		}
#ifdef ST_KNI_DEBUG
		StPrintTranStats(c);
#endif
	}
	RTE_LOG(INFO, ST_KNI, " StKniTxLcore txRing - STOP\n");
}

static st_kni_ms_conf_t *
StFindKniEthIntf(uint16_t ethPortId)
{
	st_kni_ms_conf_t *c = NULL;
	int32_t idx = firstEmptyKni - 1;
	for (; idx >= 0; idx--)
	{
		if (kniItfs[idx].ethPortId == ethPortId)
		{
			c = &kniItfs[idx];
			break;
		}
	}
	return c;
}

static int
StKniConfNetInt(uint16_t portId, uint8_t ifUp)
{
	st_kni_ms_conf_t *c = NULL;
	int ret = 0;
	c = StFindKniEthIntf(portId);
	if (!c)
	{
		RTE_LOG(ERR, ST_KNI, ST_KNI_ERROR "Not find KNI interface for %d\n", portId);
		return -1;
	}
	rte_atomic32_set(&c->lnkUp, 0);
	rte_eth_dev_stop(portId);
	if (ifUp != 0)
		ret = rte_eth_dev_start(portId);
	if (!ret)
		rte_atomic32_set(&c->lnkUp, ifUp);
	return 0;
}

static void
StPrintMac(const char *n, struct rte_ether_addr *m)
{
	char buf[RTE_ETHER_ADDR_FMT_SIZE];
	rte_ether_format_addr(buf, RTE_ETHER_ADDR_FMT_SIZE, m);
	RTE_LOG(INFO, ST_KNI, "\t%s%s\n", n, buf);
}

/* Callback for request of configuring mac address */
static int
StKniCfgMacAddr(uint16_t portId, uint8_t macAddr[])
{
	st_kni_ms_conf_t *c = NULL;
	c = StFindKniEthIntf(portId);
	if (c == NULL)
	{
		RTE_LOG(ERR, ST_KNI, ST_KNI_ERROR "Not find KNI interface for %d\n", portId);
		return -EINVAL;
	}
	if (!rte_eth_dev_is_valid_port(portId))
	{
		RTE_LOG(ERR, ST_KNI, "Invalid port id %d\n", portId);
		return -EINVAL;
	}
	RTE_LOG(INFO, ST_KNI, "Configure mac address of %d\n", portId);
	StPrintMac("Address:", (struct rte_ether_addr *)macAddr);
	rte_memcpy(&c->kniConf.mac_addr, macAddr, sizeof(c->kniConf.mac_addr));
	int ret = rte_eth_dev_default_mac_addr_set(portId, (struct rte_ether_addr *)macAddr);
	if (ret < 0)
		RTE_LOG(ERR, ST_KNI, "Failed to config mac_addr for port %d\n", portId);
	return ret;
}

static int
StKniChangeMtu(uint16_t portId, unsigned int newMtu)
{
	return ST_NOT_IMPLEMENTED;
}
static int
StKniConfigPromiscusity(uint16_t portId, uint8_t toOn)
{
	return ST_NOT_IMPLEMENTED;
}

static int
StKniConfigAllMulticast(uint16_t portId, uint8_t toOn)
{
	return ST_NOT_IMPLEMENTED;
}

st_kni_ms_conf_t *
StInitKniConf(int32_t ethPortId, struct rte_mempool *mbufPool, uint16_t rxRingNb, uint32_t txThread,
			  struct rte_ring *txRing)
{
	st_kni_ms_conf_t *c = NULL;
	struct rte_eth_dev_info devInfo;
	int32_t ret;
	if (kniItfs == NULL)
	{
		RTE_LOG(ERR, ST_KNI, ST_KNI_ERROR "Initalize KNI subsystem using StPrepKni\n");
		return NULL;
	}
	if (firstEmptyKni >= nbKni)
	{
		RTE_LOG(ERR, ST_KNI, ST_KNI_ERROR "No epmty KNI slots\n");
		return NULL;
	}
	if (mbufPool == NULL)
	{
		RTE_LOG(ERR, ST_KNI, ST_KNI_ERROR "mbufPool must be set\n");
		return NULL;
	}

	c = kniItfs + firstEmptyKni;
	firstEmptyKni++;
	memset(c, 0, sizeof(struct st_kni_ms_conf));
	c->ethPortId = ethPortId;
	c->rxRingNb = rxRingNb;
	c->txRing = txRing;
	c->txThread = txThread;
	c->mbufPool = mbufPool;
	if (txRing == NULL)
	{
		RTE_LOG(ERR, ST_KNI, ST_KNI_ERROR "Both txRing must be set\n");
		return NULL;
	}
	ret = rte_eth_dev_info_get(ethPortId, &devInfo);

	if (ret != 0)
	{
		RTE_LOG(ERR, ST_KNI, ST_KNI_ERROR "During getting device (port %u) info: %s\n",
				c->ethPortId, strerror(-ret));
		firstEmptyKni--;
		return NULL;
	}
	ret = rte_eth_macaddr_get(c->ethPortId, (struct rte_ether_addr *)&c->kniConf.mac_addr);
	if (ret != 0)
	{
		RTE_LOG(ERR, ST_KNI, ST_KNI_ERROR "Failed to get MAC address (port %u): %s\n", c->ethPortId,
				rte_strerror(-ret));
		firstEmptyKni--;
		return NULL;
	}
	const char *kniName = StDevGetKniInterName();
	if (kniName == NULL)
		snprintf(c->kniConf.name, RTE_KNI_NAMESIZE, ST_KNI_NAME "%u", c->ethPortId);
	else
		strncpy(c->kniConf.name, kniName, RTE_KNI_NAMESIZE);
	c->kniConf.group_id = ethPortId;
	c->kniConf.mbuf_size = MAX_PACKET_SZ;
	rte_eth_dev_get_mtu(ethPortId, &c->kniConf.mtu);
	c->kniConf.min_mtu = devInfo.min_mtu;
	c->kniConf.max_mtu = devInfo.max_mtu;
	rte_atomic32_init(&c->isStop);
	return c;
}

int32_t
StStartKni(unsigned slvCoreRx, unsigned slvCoreTx, st_kni_ms_conf_t *c)
{
	struct rte_kni *kni;
	struct rte_kni_ops ops;
	RTE_LOG(INFO, ST_KNI, "slvCoreRx: %d, slvCoreTx: %d\n", slvCoreRx, slvCoreTx);
	c->slvCoreRx = slvCoreRx;
	c->slvCoreTx = slvCoreTx;
	if (c->mbufPool == NULL)
	{
		RTE_LOG(ERR, ST_KNI, ST_KNI_ERROR "No mbuf set\n");
		return ST_INVALID_PARAM;
	}
	memset(&ops, 0, sizeof(ops));
	ops.port_id = c->ethPortId;
	ops.change_mtu = StKniChangeMtu;  // kni_change_mtu;
	ops.config_network_if = StKniConfNetInt;
	ops.config_mac_address = StKniCfgMacAddr;
	ops.config_promiscusity = StKniConfigPromiscusity;
	ops.config_allmulticast = StKniConfigAllMulticast;
	kni = rte_kni_alloc(c->mbufPool, &c->kniConf, &ops);
	if (!kni)
	{
		RTE_LOG(ERR, ST_KNI,
				ST_KNI_ERROR "Fail to create kni for "
							 "port: %d\n",
				c->ethPortId);
		return ST_KNI_CANNOT_PREPARE;
	}
	c->kni = kni;
	StPtpInit(c->ethPortId, c->mbufPool, stDevParams->maxTxRings, c->txRing);
	StIgmpQuerierInit(c->ethPortId, c->mbufPool, c->txRing, (uint32_t *)stMainParams.sipAddr,
					  (uint32_t *)stMainParams.ipAddr);
	rte_eal_remote_launch((lcore_function_t *)StKniTxLcore, c, c->slvCoreTx);
	rte_eal_remote_launch((lcore_function_t *)StKniRxLcore, c, c->slvCoreRx);
	{  // Assign IP to KNI
		int const sock = socket(AF_INET, SOCK_DGRAM, 0);
		if (sock != -1)
		{
			struct ifreq ifr;
			strncpy(ifr.ifr_name, c->kniConf.name, sizeof(ifr.ifr_name) - 1);
			ifr.ifr_ifru.ifru_addr.sa_family = AF_INET;
			((struct sockaddr_in *)&ifr.ifr_ifru.ifru_addr)->sin_port = 0;
			memcpy(&((struct sockaddr_in *)&ifr.ifr_ifru.ifru_addr)->sin_addr.s_addr,
				   &stMainParams.sipAddr, 4);

			if (ioctl(sock, SIOCSIFADDR, &ifr))
			{
				RTE_LOG(ERR, USER1, "Cannot assign IP to %s\n", ifr.ifr_name);
			}
			close(sock);
		}
		else
		{
			RTE_LOG(ERR, USER1, "socket AF_INET fail\n");
		}
	}
	return ST_OK;
}

int32_t
StStopKni(st_kni_ms_conf_t *c)
{
	RTE_LOG(INFO, ST_KNI, "Release ST_KNI\n");
	rte_atomic32_inc(&c->isStop);
	StIgmpQuerierStop();
	rte_eal_wait_lcore(c->slvCoreRx);
	rte_eal_wait_lcore(c->slvCoreTx);
	rte_kni_release(c->kni);
	rte_eth_dev_stop(c->ethPortId);
	return 0;
}

// initialise virtual kni ports for each ports
int32_t
StInitKni(int32_t nbp)
{
	RTE_LOG(INFO, ST_KNI, "Prepare %d " ST_KNI_NAME " devices\n", nbp);
	if (kniItfs != NULL)
	{
		RTE_LOG(ERR, ST_KNI, ST_KNI_ERROR ST_KNI_NAME " interface already prepared\n");
		return ST_KNI_ALREADY_PREPARED;
	}
	kniItfs = rte_zmalloc("StKNI ports params", nbp * sizeof(struct st_kni_ms_conf),
						  RTE_CACHE_LINE_SIZE);
	if (kniItfs == NULL)
	{
		RTE_LOG(ERR, ST_KNI,
				ST_KNI_ERROR "Cannot reserve memory for " ST_KNI_NAME "parameters struct\n");
		return ST_KNI_GENERAL_ERR;
	}
	if (rte_kni_init(nbp) != 0)
	{
		rte_free(kniItfs);
		kniItfs = NULL;
		RTE_LOG(ERR, ST_KNI,
				ST_KNI_ERROR "Cannot prepare KNI. "
							 "Propably kni driver is not loaded\n");
		return ST_KNI_CANNOT_PREPARE;
	}
	firstEmptyKni = 0;
	nbKni = nbp;
	return ST_OK;
}

st_status_t
StKniBkgTask(void)
{
	uint16_t portId;
	struct rte_eth_link link;
	st_kni_ms_conf_t *c = NULL;
	int ret;
	RTE_ETH_FOREACH_DEV(portId)
	{
		c = StFindKniEthIntf(portId);
		if (c == NULL)
			continue;
		memset(&link, 0, sizeof(link));
		ret = rte_eth_link_get_nowait(portId, &link);
		if (ret < 0)
			continue;
		rte_kni_update_link(c->kni, link.link_status);
	}
	return ST_OK;
}
