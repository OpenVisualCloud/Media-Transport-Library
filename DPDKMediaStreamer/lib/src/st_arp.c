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

#include <rte_arp.h>
#include <rte_ethdev.h>

#include <rvrtp_main.h>

extern st_main_params_t stMainParams;

void ArpAnswer(uint16_t portid, uint32_t ip, struct rte_ether_addr const *mac);

void
ArpRequest(uint16_t portid, uint32_t ip)
{
	// RTE_LOG(INFO, USER1, "ArpRequest not implemented\n");
	ArpAnswer(portid, *(uint32_t *)stMainParams.ipAddr, (struct rte_ether_addr *)stMainParams.macAddr);
}

static void
ArpReceiveRequest(struct rte_arp_hdr const *request, uint16_t portid)
{
	if (!request)
	{
		return;
	}

	if (ntohs(request->arp_hardware) == 1 &&	 // ethernet
		ntohs(request->arp_protocol) == 0x800 && // IP protocol
		request->arp_hlen == 6 &&				 // size of MAC
		request->arp_plen == 4)					 // sizeof IP
	{
		if (request->arp_data.arp_tip == *(uint32_t *)stMainParams.sipAddr)
		{
			struct rte_mbuf *reqPkt = rte_pktmbuf_alloc(stMainParams.mbufPool);
			if (reqPkt)
			{
				reqPkt->pkt_len = reqPkt->data_len
					= sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);
				struct rte_ether_hdr *eth = rte_pktmbuf_mtod(reqPkt, struct rte_ether_hdr *);
				rte_eth_macaddr_get(portid, &eth->s_addr);
				rte_ether_addr_copy(&request->arp_data.arp_sha, &eth->d_addr);
				eth->ether_type = htons(0x0806); // ARP_PROTOCOL
				struct rte_arp_hdr *arp = rte_pktmbuf_mtod_offset(
					reqPkt, struct rte_arp_hdr *, sizeof(struct rte_ether_hdr));
				arp->arp_hardware = htons(1);
				arp->arp_protocol = htons(0x800); // IP protocol
				arp->arp_hlen = 6;				  // size of MAC
				arp->arp_plen = 4;				  // size of fo IP
				arp->arp_opcode = htons(RTE_ARP_OP_REPLY);
				arp->arp_data.arp_tha = request->arp_data.arp_sha;
				arp->arp_data.arp_tip = request->arp_data.arp_sip;
				rte_eth_macaddr_get(portid, &arp->arp_data.arp_sha);
				arp->arp_data.arp_sip = *(uint32_t *)stMainParams.sipAddr;
				if (rte_eth_tx_burst(portid, stDevParams->maxTxRings, &reqPkt, 1) <= 0)
				{
					RTE_LOG(WARNING, USER1, "rte_eth_tx_burst fail\n");
					rte_pktmbuf_free(reqPkt);
				}
				else
				{
					RTE_LOG(DEBUG, USER1, "ARP reply\n");
				}
			}
			else
			{
				RTE_LOG(WARNING, USER1, "rte_pktmbuf_alloc\n");
			}
		}
	}
}

void
ParseArp(struct rte_arp_hdr const *header, uint16_t portid)
{
	if (!header)
	{
		return;
	}

	switch (htons(header->arp_opcode))
	{
	case RTE_ARP_OP_REQUEST:
		ArpReceiveRequest(header, portid);
		break;
	default:
		RTE_LOG(DEBUG, USER1, "ParseArp %04x uninplemented\n", ntohs(header->arp_opcode));
		return;
	}
}
