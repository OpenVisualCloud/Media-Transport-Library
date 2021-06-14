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

#ifndef _ST_FLW_CLS_H
#define _ST_FLW_CLS_H

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_flow.h>
#include <rte_flow_classify.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_table_acl.h>

#include "st_api.h"
#include "st_api_internal.h"

#include <getopt.h>
#include <inttypes.h>
#include <stdint.h>

#define MAX_PATTERN_NUM 4
#define MAX_ACTION_NUM 2

#define RTE_LOGTYPE_ST_CLS (RTE_LOGTYPE_USER1)

struct st_udp_flow_conf
{
	uint32_t srcIp;
	uint32_t srcMask;
	uint16_t srcPort;
	uint16_t srcPortMask;
	uint32_t dstIp;
	uint32_t dstMask;
	uint16_t dstPort;
	uint16_t dstPortMask;
	struct rte_ether_addr dstMac;
	struct rte_ether_addr srcMac;
	uint16_t etherType;
	struct rte_ether_addr dstMacMask;
	struct rte_ether_addr srcMacMask;
	uint16_t etherTypeMask;
};

struct st_classify_app_pars
{
	uint16_t portId;
	uint16_t rxQ;
	uint32_t flConfCount;
	struct st_udp_flow_conf *flConf;
	const char *nameFltrCore;
};

extern struct rte_flow *StSetUDPFlow(uint16_t portId, uint16_t rxQ, struct st_udp_flow_conf *flConf,
									 struct rte_flow_error *err);

#endif	// _ST_FLW_CLS_H
