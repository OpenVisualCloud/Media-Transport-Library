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

#include "st_flw_cls.h"

#include "rvrtp_main.h"

#include <unistd.h>

struct rte_flow *
StSetUDPFlow(uint16_t portId, uint16_t rxQ, struct st_udp_flow_conf *flConf,
			 struct rte_flow_error *err)
{
	struct rte_flow_attr attr;
	struct rte_flow_item pattern[MAX_PATTERN_NUM];
	struct rte_flow_action action[MAX_ACTION_NUM];
	struct rte_flow *flow = NULL;
	struct rte_flow_action_queue queue = { .index = rxQ };

	struct rte_flow_item_eth ethSpec;
	struct rte_flow_item_eth ethMask;

	struct rte_flow_item_ipv4 ipv4Spec;
	struct rte_flow_item_ipv4 ipv4Mask;

	struct rte_flow_item_udp udpSpec;
	struct rte_flow_item_udp udpMask;

	int res;

	memset(&ethSpec, 0, sizeof(ethSpec));
	memset(&ethMask, 0, sizeof(ethMask));

	memset(&ipv4Spec, 0, sizeof(ipv4Spec));
	ipv4Spec.hdr.next_proto_id = IPPROTO_UDP;
	ipv4Spec.hdr.src_addr = flConf->srcIp;
	ipv4Spec.hdr.dst_addr = flConf->dstIp;

	memset(&ipv4Mask, 0, sizeof(ipv4Mask));

	/* For i40e only: disable 802.3 PAUSE Frames filtering  */
	struct rte_eth_dev_info devInfo = { 0 };
	res = rte_eth_dev_info_get(portId, &devInfo);

	if ((res == 0)
		&& ((strcmp(devInfo.driver_name, "net_i40e") == 0)
			|| (strcmp(devInfo.driver_name, "net_ice") == 0)))
	{
		// newest net_i40e does not accept proto filter when it is defined later as
		// protocol item (such as UDP), same issue with latest net_ice drivers
		ipv4Mask.hdr.next_proto_id = 0x0;
	}
	else
	{
		ipv4Mask.hdr.next_proto_id = 0xff;
	}

	ipv4Mask.hdr.src_addr = flConf->srcMask;
	ipv4Mask.hdr.dst_addr = flConf->dstMask;

	memset(&udpSpec, 0, sizeof(udpSpec));
	udpSpec.hdr.src_port = flConf->srcPort;
	udpSpec.hdr.dst_port = flConf->dstPort;
	udpSpec.hdr.dgram_len = 0;
	udpSpec.hdr.dgram_cksum = 0;

	memset(&udpMask, 0x0, sizeof(udpMask));
	udpMask.hdr.src_port = flConf->srcPortMask;
	udpMask.hdr.dst_port = flConf->dstPortMask;
	udpMask.hdr.dgram_len = 0;
	udpMask.hdr.dgram_cksum = 0;

	memset(pattern, 0, sizeof(pattern));
	memset(action, 0, sizeof(action));

	memset(&attr, 0, sizeof(struct rte_flow_attr));
	attr.ingress = 1;

	action[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
	action[0].conf = &queue;
	action[1].type = RTE_FLOW_ACTION_TYPE_END;

	pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
	pattern[0].spec = &ethSpec;
	pattern[0].mask = &ethMask;
	pattern[1].type = RTE_FLOW_ITEM_TYPE_IPV4;
	pattern[1].spec = &ipv4Spec;
	pattern[1].mask = &ipv4Mask;
	pattern[2].type = RTE_FLOW_ITEM_TYPE_UDP;
	pattern[2].spec = &udpSpec;
	pattern[2].mask = &udpMask;
	pattern[3].type = RTE_FLOW_ITEM_TYPE_END;

	res = rte_flow_validate(portId, &attr, pattern, action, err);
	if (!res)
	{
		flow = rte_flow_create(portId, &attr, pattern, action, err);
	}
	else
	{
		RTE_LOG(ERR, USER2, "Flow validation failed with error: %s\n", err->message);
	}
	return flow;
}

#ifdef DEBUG
static void
StPrintPartFilter(char *n, uint32_t ip, uint32_t mip, uint16_t p, uint16_t mp)
{
	RTE_LOG(INFO, ST_CLS,
			"\n%s\n"
			"      ip: %d.%d.%d.%d, mip: %d.%d.%d.%d\n"
			"       p: %04x mp: %04x\n",
			n, (ip >> 0) & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF,
			(mip >> 0) & 0xFF, (mip >> 8) & 0xFF, (mip >> 16) & 0xFF, (mip >> 24) & 0xFF, p, mp);
}
#endif
