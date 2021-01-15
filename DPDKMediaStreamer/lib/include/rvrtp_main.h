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

#ifndef _RVRTP_MAIN_H
#define _RVRTP_MAIN_H

/* General includes */
#include <inttypes.h>
#include <linux/if_ether.h>
#include <netinet/udp.h>
#include <stdbool.h>
#include <stdint.h>

/* DPDK includes */
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_ring.h>

/* Self includes */
#include "st_api_internal.h"
#include "st_pkt.h"

#define LIB_VERSION_MAJOR 1
#define LIB_VERSION_MINOR 0
#define LIB_VERSION_LAST 11


#define MBUF_CACHE_SIZE 128
#define RX_RING_SIZE 8192
#define TX_RING_SIZE 4096
#define MAX_PAUSE_FRAMES (ST_MAX_SESSIONS_MAX * 2)
#define MIN_PKT_SIZE 64
#define IP_ADDR_LEN 4
#define MAC_ADDR_LEN 6
#define MAX_STR_LEN 80

typedef struct st_hw_caps
{
	uint32_t nicHwTmstamp : 1;
	uint32_t nicHwTimesync : 1;
	uint32_t reserved : 30;
} st_hw_caps_t;

typedef struct st_main_params
{
	volatile uint64_t schedStart;
	volatile uint64_t ringStart;
	volatile uint64_t ringBarrier0;
	volatile uint64_t ringBarrier1;
	volatile uint64_t ringBarrier2;
	st_hw_caps_t hwCaps;
	uint32_t maxEnqThrds;
	st_thrd_params_t enqThrds[ST_MAX_ENQ_THREADS_MAX];
	uint32_t maxRcvThrds;
	st_thrd_params_t rcvThrds[ST_MAX_RCV_THREADS_MAX];
	uint32_t maxSchThrds;
	uint16_t txPortId;
	uint16_t rxPortId;
	struct rte_mempool *mbufPool;

	// Input Parameters
	uint8_t macAddr[MAC_ADDR_LEN];							/**< destination MAC */
	uint8_t ipAddr[IP_ADDR_LEN];	                        /**< destination IP */
	uint8_t sipAddr[IP_ADDR_LEN];							/**< source IP */
	bool isEbuCheck;
	uint32_t rxOnly;
	uint32_t txOnly;
	uint32_t rate;
	uint32_t interlaced;
	uint32_t fmtIndex;
	uint32_t snCount;
	uint16_t udpPort;
	st21_buf_fmt_t bufFormat;

	char outPortName[MAX_STR_LEN];
	char inPortName[MAX_STR_LEN];
	char dpdkParams[MAX_STR_LEN]; /**< parameters after empty --  */
} st_main_params_t;

/*
 * Utility functions for device creation
 */
void StDevInitTxThreads(st_main_params_t *mp, rvrtp_device_t *dev);
void StDevInitRxThreads(st_main_params_t *mp, rvrtp_device_t *dev);
st_status_t StDevCalculateBudgets(rvrtp_device_t *d);

/* Defines */
//#define DEBUG

/* Globals */
extern struct tprs_scheduler sch;
extern st_main_params_t stMainParams;
extern const st_nic_rate_params_t *stDevParams;

extern volatile uint64_t pktsBuild;
extern volatile uint64_t pktsQueued;

extern rte_atomic32_t isTxDevToDestroy;
extern rte_atomic32_t isRxDevToDestroy;

extern void ShowWelcomeBanner();
extern int ParseArgs(int argc, char *argv[]);

void PrintVersion();
void PrintHelp();

// forward declarations
int LcoreMainPktRingEnqueue(void *args);
int LcoreMainReceiver(void *args);
// int LcoreMainTransmitter(void* args);
int LcoreMainTransmitterBulk(void *args);
int LcoreMainTransmitterSingle(void *args);
int LcoreMainTransmitterDual(void *args);

#endif
