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

#include "st_arp.h"

#include "st_api.h"

enum arp_entry_type
{
	ARP_ENTRY_DYNAMIC,
	ARP_ENTRY_STATIC
};

typedef struct arp_element
{
	uint32_t ipAddr;			   /**< dest IP */
	uint8_t macAddr[ETH_ADDR_LEN]; /**< destination MAC */
	uint8_t type;
} arp_element;

#define MAX_HIS 10
static arp_element arp_hist[MAX_HIS];
static int arp_element_size = 0;
static int update_idx = 0;
extern st_main_params_t stMainParams;
pthread_mutex_t arp_table_mutex;
void
ArpRequest(uint16_t portid, uint32_t ip, uint32_t sip)
{
	struct rte_mbuf *reqPkt = rte_pktmbuf_alloc(stMainParams.mbufPool);
	if (reqPkt)
	{
		reqPkt->pkt_len = reqPkt->data_len
			= sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);
		struct rte_ether_hdr *eth = rte_pktmbuf_mtod(reqPkt, struct rte_ether_hdr *);
		rte_eth_macaddr_get(portid, &eth->s_addr);
		memset(&eth->d_addr, 0xFF, RTE_ETHER_ADDR_LEN);
		eth->ether_type = htons(0x0806);  // ARP_PROTOCOL
		struct rte_arp_hdr *arp
			= rte_pktmbuf_mtod_offset(reqPkt, struct rte_arp_hdr *, sizeof(struct rte_ether_hdr));
		arp->arp_hardware = htons(1);
		arp->arp_protocol = htons(0x800);  // IP protocol
		arp->arp_hlen = ETH_ADDR_LEN;	   // size of MAC
		arp->arp_plen = 4;				   // size of fo IP
		arp->arp_opcode = htons(RTE_ARP_OP_REQUEST);
		arp->arp_data.arp_tip = ip;
		arp->arp_data.arp_sip = sip;
		rte_eth_macaddr_get(portid, &arp->arp_data.arp_sha);
		memset(&arp->arp_data.arp_tha, 0, RTE_ETHER_ADDR_LEN);

		if (rte_eth_tx_burst(portid, stDevParams->maxTxRings, &reqPkt, 1) <= 0)
		{
			RTE_LOG(WARNING, USER1, "rte_eth_tx_burst fail\n");
			rte_pktmbuf_free(reqPkt);
		}
		else
		{
			RTE_LOG(DEBUG, USER1, "ARP Request send\n");
		}
	}
}

static void
ArpReceiveRequest(struct rte_arp_hdr const *request, uint16_t portid)
{
	if (!request)
	{
		return;
	}

	if (ntohs(request->arp_hardware) == 1 &&	  // ethernet
		ntohs(request->arp_protocol) == 0x800 &&  // IP protocol
		request->arp_hlen == ETH_ADDR_LEN &&	  // size of MAC
		request->arp_plen == 4)					  // sizeof IP
	{
		int userport = 0;
		for (userport = 0; userport < MAX_RXTX_PORTS; userport++)
		{
			if(stMainParams.txPortId[userport] == portid)
			    break;
		}
		if (request->arp_data.arp_tip == *(uint32_t *)stMainParams.sipAddr[userport])
		{
			struct rte_mbuf *reqPkt = rte_pktmbuf_alloc(stMainParams.mbufPool);
			if (reqPkt)
			{
				reqPkt->pkt_len = reqPkt->data_len
					= sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);
				struct rte_ether_hdr *eth = rte_pktmbuf_mtod(reqPkt, struct rte_ether_hdr *);
				rte_eth_macaddr_get(portid, &eth->s_addr);
				rte_ether_addr_copy(&request->arp_data.arp_sha, &eth->d_addr);
				eth->ether_type = htons(0x0806);  // ARP_PROTOCOL
				struct rte_arp_hdr *arp = rte_pktmbuf_mtod_offset(reqPkt, struct rte_arp_hdr *,
																  sizeof(struct rte_ether_hdr));
				arp->arp_hardware = htons(1);
				arp->arp_protocol = htons(0x800);  // IP protocol
				arp->arp_hlen = ETH_ADDR_LEN;	   // size of MAC
				arp->arp_plen = 4;				   // size of fo IP
				arp->arp_opcode = htons(RTE_ARP_OP_REPLY);
				rte_ether_addr_copy(&request->arp_data.arp_sha, &arp->arp_data.arp_tha);
				arp->arp_data.arp_tip = request->arp_data.arp_sip;
				rte_eth_macaddr_get(portid, &arp->arp_data.arp_sha);
				arp->arp_data.arp_sip = *(uint32_t *)stMainParams.sipAddr[userport];
				if (rte_eth_tx_burst(portid, stDevParams->maxTxRings, &reqPkt, 1) <= 0)
				{
					RTE_LOG(WARNING, USER1, "rte_eth_tx_burst fail\n");
					rte_pktmbuf_free(reqPkt);
				}
				else
				{
					RTE_LOG(DEBUG, USER1, "ARP request reply send\n");
				}
			}
			else
			{
				RTE_LOG(WARNING, USER1, "rte_pktmbuf_alloc\n");
			}
		}
	}
}

static void
ArpReceiveReply(struct rte_arp_hdr const *reply, uint16_t portid)
{
	if (!reply)
	{
		return;
	}
	if (ntohs(reply->arp_hardware) == 1 &&		// ethernet
		ntohs(reply->arp_protocol) == 0x800 &&	// IP protocol
		reply->arp_hlen == ETH_ADDR_LEN &&		// size of MAC
		reply->arp_plen == 4)					// sizeof IP
	{
		int userport = 0;
		for (userport = 0; userport < MAX_RXTX_PORTS; userport++)
		{
			if (stMainParams.txPortId[userport] == portid)
				break;
		}
		if (reply->arp_data.arp_tip == *(uint32_t *)stMainParams.sipAddr[userport])
		{
			bool found = SearchArpHist(reply->arp_data.arp_sip, NULL);

			if (!found)
			{
				pthread_mutex_lock(&arp_table_mutex);
				if (arp_element_size == MAX_HIS)
				{
					arp_hist[update_idx].ipAddr = reply->arp_data.arp_sip;
					memcpy(arp_hist[update_idx].macAddr, reply->arp_data.arp_sha.addr_bytes,
						   ETH_ADDR_LEN);
					arp_hist[update_idx].type = ARP_ENTRY_DYNAMIC;
					update_idx++;
					if (update_idx >= MAX_HIS)
						update_idx = 0;
				}
				else
				{
					arp_hist[arp_element_size].ipAddr = reply->arp_data.arp_sip;
					memcpy(arp_hist[arp_element_size].macAddr, reply->arp_data.arp_sha.addr_bytes,
						   ETH_ADDR_LEN);
					arp_hist[arp_element_size].type = ARP_ENTRY_DYNAMIC;
					arp_element_size++;
				}
				pthread_mutex_unlock(&arp_table_mutex);
			}

			RTE_LOG(INFO, USER1, "receive arp reply\n");
		}
	}
}

void
ParseArp(struct rte_arp_hdr const *header, unsigned short portid)
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
	case RTE_ARP_OP_REPLY:
		ArpReceiveReply(header, portid);
		break;
	default:
		RTE_LOG(DEBUG, USER1, "ParseArp %04x uninplemented\n", ntohs(header->arp_opcode));
		return;
	}

	return;
}

void
loadArpHist()
{
	FILE *fp = fopen("arp_hist.bin", "rb");
	if (fp == NULL)
		return;
	fseek(fp, 0, SEEK_END);
	int size = ftell(fp);
	arp_element_size = size / sizeof(arp_element);
	RTE_LOG(INFO, USER1, "got %d arp history\n", arp_element_size);
	fseek(fp, 0, SEEK_SET);

	size_t readBytes = fread(arp_hist, sizeof(arp_element), arp_element_size, fp);
	if (0 == readBytes)
		RTE_LOG(INFO, USER1, "No ARP history!\n");

	fclose(fp);
	pthread_mutex_init(&arp_table_mutex, NULL);
}

void
storeArpHist()
{
	FILE *fp = fopen("arp_hist.bin", "wb");
	if (fp == NULL)
		return;
	pthread_mutex_lock(&arp_table_mutex);
	fwrite(arp_hist, sizeof(arp_element), arp_element_size, fp);
	fclose(fp);
	pthread_mutex_unlock(&arp_table_mutex);
}

bool
SearchArpHist(uint32_t ip, uint8_t *dstMac)
{
	bool found = false;
	pthread_mutex_lock(&arp_table_mutex);
	for (int i = 0; i < arp_element_size; i++)
	{
		if (arp_hist[i].ipAddr == ip)
		{
			if (dstMac)
				memcpy(dstMac, arp_hist[i].macAddr, ETH_ADDR_LEN);
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&arp_table_mutex);
	return found;
}

st_status_t
StSetStaticArpEntry(st_session_t *sn, uint16_t nicPort, uint8_t *macAddr, uint8_t *ipAddr)
{
	char ipAddrString[INET_ADDRSTRLEN];

	if (!macAddr || !ipAddr)
	{
		return ST_INVALID_PARAM;
	}

	st_status_t status = StValidateSession(sn);
	if (status != ST_OK)
	{
		return status;
	}

	st_session_impl_t *s = (st_session_impl_t *)sn;

	bool found = SearchArpHist(*(uint32_t *)ipAddr, NULL);
	inet_ntop(AF_INET, ipAddr, ipAddrString, INET_ADDRSTRLEN);

	if (!found)
	{
		pthread_mutex_lock(&arp_table_mutex);
		if (arp_element_size == MAX_HIS)
		{
			memcpy(&arp_hist[update_idx].ipAddr, ipAddr, IP_ADDR_LEN);
			memcpy(arp_hist[update_idx].macAddr, macAddr, ETH_ADDR_LEN);
			memcpy(s->fl[nicPort].dstMac, macAddr, ETH_ADDR_LEN);
			arp_hist[update_idx].type = ARP_ENTRY_STATIC;
			update_idx++;
			if (update_idx >= MAX_HIS)
				update_idx = 0;
		}
		else
		{
			memcpy(&arp_hist[arp_element_size].ipAddr, ipAddr, IP_ADDR_LEN);
			memcpy(arp_hist[arp_element_size].macAddr, macAddr, ETH_ADDR_LEN);
			memcpy(s->fl[nicPort].dstMac, macAddr, ETH_ADDR_LEN);
			arp_hist[arp_element_size].type = ARP_ENTRY_STATIC;
			arp_element_size++;
		}
		pthread_mutex_unlock(&arp_table_mutex);
	}

	RTE_LOG(INFO, USER1,
			"ARP entry (static) added with IP: %s and MAC: %hhx:%hhx:%hhx:%hhx:%hhx:%hhx.\n",
			ipAddrString, macAddr[0], macAddr[1], macAddr[2], macAddr[3], macAddr[4], macAddr[5]);

	return ST_OK;
}

st_status_t
StGetArpTable()
{
	char ipAddrString[INET_ADDRSTRLEN];

	printf("IP Address	 Phys Address	 Type\n");
	printf("---------------	---------------	 --------\n");
	for (int i = 0; i < arp_element_size; i++)
	{
		inet_ntop(AF_INET, &arp_hist[i].ipAddr, ipAddrString, INET_ADDRSTRLEN);

		char *typeString = arp_hist[i].type == ARP_ENTRY_STATIC ? "Static" : "Dynamic";

		printf("%s	%hhx:%hhx:%hhx:%hhx:%hhx:%hhx %s\n", ipAddrString, arp_hist[i].macAddr[0],
			   arp_hist[i].macAddr[1], arp_hist[i].macAddr[2], arp_hist[i].macAddr[3],
			   arp_hist[i].macAddr[4], arp_hist[i].macAddr[5], typeString);
	}
	printf("\n");

	return ST_OK;
}
