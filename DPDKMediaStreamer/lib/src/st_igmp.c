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

/*
 *
 *	Media streamer based on DPDK
 *
 */

/*
*	Implementation of the Internet Group Management Protocol (Version 2 and 3)
*	for multicast support is compatible with RFC 2236 and 3376
*/

#include "st_igmp.h"

#include "rvrtp_main.h"

#include <stdatomic.h>
#include <unistd.h>

#define IGMP_PROTOCOL 0x02
#define IPv4_PROTOCOL 0x0800

#define IP_DONT_FRAGMENT_FLAG 0x0040
#define IP_IGMP_DSCP_VALUE 0xc0

#define MIN_NUMBER_OF_ENTIRES 64

pthread_t querierThread;
atomic_int_least8_t isQuerierStopped = 1;

static st_igmp_params_t igmpParams[MAX_RXTX_PORTS]
	= { { .state = IGMP_NOT_INITIALIZED, .numberOfSources = 0 } };

uint32_t
IgmpSetPktSize(uint32_t sizeOfIgmpHdr)
{
	uint32_t pktSize = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeOfIgmpHdr;

	// Align to the 60 bytes because of requirements of NIC's (e.g. Intel(R) Fortville, support packets with minimum size 60 bytes)
	if (pktSize < 60)
	{
		pktSize += 60 - pktSize;
	}

	return pktSize;
}

void
IgmpBuildIpHdr(struct rte_ipv4_hdr *ipHdr, uint32_t sourceAddress, uint32_t sizeOfIgmpHdr)
{
	if (!ipHdr)
	{
		return;
	}

	ipHdr->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
	ipHdr->time_to_live = 1;
	ipHdr->type_of_service = IP_IGMP_DSCP_VALUE;
	ipHdr->fragment_offset = IP_DONT_FRAGMENT_FLAG;
	ipHdr->hdr_checksum = 0;

	uint16_t tLen = sizeof(struct rte_ipv4_hdr) + sizeOfIgmpHdr;
	ipHdr->total_length = htons(tLen);
	ipHdr->next_proto_id = IGMP_PROTOCOL;
	ipHdr->src_addr = sourceAddress;
}

uint16_t
IgmpCalculateChecksum(igmp_message_type_t messageType, void *igmpMessage)
{
	if (!igmpMessage)
	{
		return 0;
	}

	uint8_t count = 0;
	uint16_t *val_1 = (uint16_t *)(igmpMessage);
	uint16_t sum = 0;

	switch (messageType)
	{
	case MEMBERSHIP_QUERY:
		count = sizeof(st_membership_query_t) / sizeof(uint16_t);
		break;
	case MEMBERSHIP_REPORT_V1:
		break;
	case MEMBERSHIP_REPORT_V2:
		count = sizeof(st_membership_report_v2_t) / sizeof(uint16_t);
		;
		break;
	case MEMBERSHIP_REPORT_V3:
		count = sizeof(st_membership_report_v3_t) / sizeof(uint16_t);
		break;
	default:
		RTE_LOG(ERR, USER1, "Unknown message type: %d", messageType);
		return 0;
	}

	while (count > 0)
	{
		sum = sum + ntohs(*val_1);
		val_1++;
		count--;
	}
	sum = ~sum;

	return sum;
}

st_status_t
StCreateMembershipQueryV3(uint32_t sourceAddress, uint32_t groupAddress, int portid)
{
	if (!igmpParams[portid].mbuf)
	{
		return ST_BUFFER_NOT_READY;
	}
	rte_mbuf_refcnt_set(igmpParams[portid].mbuf, MBUF_LIFE_TIME);

	st_membership_query_t *membershipQuery
		= rte_pktmbuf_mtod_offset(igmpParams[portid].mbuf, st_membership_query_t *,
								  sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));

	membershipQuery->type = MEMBERSHIP_QUERY;
	membershipQuery->maxRespCode = QUERY_RESPONSE_INTERVAL;
	membershipQuery->checksum = 0x00;  // set to zero for the calculation purpose
	membershipQuery->resv = 0x00;
	membershipQuery->s = 0x00;
	membershipQuery->qrv = 0x00;
	membershipQuery->qqic = 0x08;

	igmpParams[portid].mbuf->pkt_len = IgmpSetPktSize(sizeof(st_membership_query_t));
	igmpParams[portid].mbuf->data_len = IgmpSetPktSize(sizeof(st_membership_query_t));
	igmpParams[portid].mbuf->ol_flags = PKT_TX_IPV4;

	igmpParams[portid].mbuf->l2_len = sizeof(struct rte_ether_hdr);
	igmpParams[portid].mbuf->l3_len = sizeof(struct rte_ipv4_hdr);
	igmpParams[portid].mbuf->ol_flags |= PKT_TX_IP_CKSUM;

	struct rte_ether_hdr *ethHdr
		= rte_pktmbuf_mtod(igmpParams[portid].mbuf, struct rte_ether_hdr *);

	rte_eth_macaddr_get(igmpParams[portid].portId, &ethHdr->s_addr);
	ethHdr->ether_type = htons(IPv4_PROTOCOL);

	struct rte_ipv4_hdr *ipHdr = rte_pktmbuf_mtod_offset(
		igmpParams[portid].mbuf, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));

	IgmpBuildIpHdr(ipHdr, sourceAddress, sizeof(st_membership_query_t));

	static struct rte_ether_addr multicast = { 0 };
	multicast.addr_bytes[0] = 0x01;
	multicast.addr_bytes[1] = 0x00;
	multicast.addr_bytes[2] = 0x5e;
	uint32_t tmpMacChunk = 0;

	switch (igmpParams[portid].queryMessageType)
	{
	case GENERAL_MEMBERSHIP_QUERY:
		ipHdr->dst_addr = igmpParams[portid].queryIpAddress;
		membershipQuery->groupAddress = 0;
		membershipQuery->numberOfSources = 0;
		membershipQuery->sourceAddress = 0;
		multicast.addr_bytes[3] = 0x00;
		multicast.addr_bytes[4] = 0x00;
		multicast.addr_bytes[5] = 0x01;
		break;
	case GROUP_SPECIFIC_MEMBERSHIP_QUERY:
		ipHdr->dst_addr = groupAddress;
		membershipQuery->groupAddress = groupAddress;
		membershipQuery->numberOfSources = 0;
		membershipQuery->sourceAddress = 0;
		tmpMacChunk = (groupAddress >> 8) & 0xFFFFFF7F;
		memcpy(&multicast.addr_bytes[3], &tmpMacChunk, sizeof(uint8_t) * 3);
		break;
	case GROUP_AND_SOURCE_MEMBERSHIP_QUERY:
		ipHdr->dst_addr = groupAddress;
		membershipQuery->groupAddress = groupAddress;
		membershipQuery->numberOfSources = htons(igmpParams[portid].numberOfSources);
		membershipQuery->sourceAddress = sourceAddress;
		tmpMacChunk = (groupAddress >> 8) & 0xFFFFFF7F;
		memcpy(&multicast.addr_bytes[3], &tmpMacChunk, sizeof(uint8_t) * 3);
		break;
	default:
		break;
	}

	rte_ether_addr_copy(&multicast, &ethHdr->d_addr);

	uint16_t checksum = IgmpCalculateChecksum(MEMBERSHIP_QUERY, membershipQuery);

	if (checksum <= 0)
	{
		return ST_IGMP_WRONG_CHECKSUM;
	}

	membershipQuery->checksum = htons(checksum);

	igmpParams[portid].queryPkt = igmpParams[portid].mbuf;

	return ST_OK;
}

st_status_t
StCreateMembershipReportV2(uint32_t sourceAddress, uint32_t groupAddress, igmp_message_type_t type,
						   int portid)
{
	if (!igmpParams[portid].mbuf)
	{
		return ST_BUFFER_NOT_READY;
	}
	rte_mbuf_refcnt_set(igmpParams[portid].mbuf, MBUF_LIFE_TIME);

	st_membership_report_v2_t *membershipReport
		= rte_pktmbuf_mtod_offset(igmpParams[portid].mbuf, st_membership_report_v2_t *,
								  sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));

	membershipReport->type = type;
	membershipReport->maxRespTime = QUERY_RESPONSE_INTERVAL;
	membershipReport->checksum = 0;

	igmpParams[portid].mbuf->pkt_len = IgmpSetPktSize(sizeof(st_membership_report_v2_t));
	igmpParams[portid].mbuf->data_len = IgmpSetPktSize(sizeof(st_membership_report_v2_t));
	igmpParams[portid].mbuf->ol_flags = PKT_TX_IPV4;

	igmpParams[portid].mbuf->l2_len = sizeof(struct rte_ether_hdr);
	igmpParams[portid].mbuf->l3_len = sizeof(struct rte_ipv4_hdr);

	struct rte_ether_hdr *ethHdr
		= rte_pktmbuf_mtod(igmpParams[portid].mbuf, struct rte_ether_hdr *);
	rte_eth_macaddr_get(igmpParams[portid].portId, &ethHdr->s_addr);

	ethHdr->ether_type = htons(IPv4_PROTOCOL);

	struct rte_ipv4_hdr *ipHdr = rte_pktmbuf_mtod_offset(
		igmpParams[portid].mbuf, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
	ipHdr->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
	ipHdr->time_to_live = 1;
	ipHdr->type_of_service = IP_IGMP_DSCP_VALUE;
	ipHdr->fragment_offset = IP_DONT_FRAGMENT_FLAG;

	igmpParams[portid].mbuf->ol_flags |= PKT_TX_IP_CKSUM;
	ipHdr->hdr_checksum = 0;

	uint16_t tLen = sizeof(struct rte_ipv4_hdr) + sizeof(st_membership_report_v2_t);
	ipHdr->total_length = htons(tLen);
	ipHdr->next_proto_id = IGMP_PROTOCOL;

	static struct rte_ether_addr multicast_dst = { 0 };
	multicast_dst.addr_bytes[0] = 0x01;
	multicast_dst.addr_bytes[1] = 0x00;
	multicast_dst.addr_bytes[2] = 0x5e;
	uint32_t tmpMacChunk = 0;

	if (type == MEMBERSHIP_QUERY)
	{
		ipHdr->src_addr = sourceAddress;
		switch (igmpParams[portid].queryMessageType)
		{
		case GENERAL_MEMBERSHIP_QUERY:
			membershipReport->groupAddress = 0;
			ipHdr->dst_addr = igmpParams[portid].queryIpAddress;
			multicast_dst.addr_bytes[3] = 0x00;
			multicast_dst.addr_bytes[4] = 0x00;
			multicast_dst.addr_bytes[5] = 0x01;
			break;
		case GROUP_SPECIFIC_MEMBERSHIP_QUERY:
			membershipReport->groupAddress = groupAddress;
			ipHdr->dst_addr = groupAddress;
			tmpMacChunk = (groupAddress >> 8) & 0xFFFFFF7F;
			memcpy(&multicast_dst.addr_bytes[3], &tmpMacChunk, sizeof(uint8_t) * 3);
			break;
		default:
			break;
		}
	}
	else if (type == MEMBERSHIP_REPORT_V2 || type == LEAVE_GROUP_V2)
	{
		ipHdr->src_addr = sourceAddress;
		membershipReport->groupAddress = groupAddress;
		ipHdr->dst_addr = groupAddress;
		tmpMacChunk = (groupAddress >> 8) & 0xFFFFFF7F;
		memcpy(&multicast_dst.addr_bytes[3], &tmpMacChunk, sizeof(uint8_t) * 3);
	}

	rte_ether_addr_copy(&multicast_dst, &ethHdr->d_addr);

	uint16_t checksum = IgmpCalculateChecksum(MEMBERSHIP_QUERY, membershipReport);

	if (checksum <= 0)
	{
		return ST_IGMP_WRONG_CHECKSUM;
	}

	if (type == MEMBERSHIP_QUERY)
	{
		membershipReport->checksum = htons(checksum);
	}
	else if (type == MEMBERSHIP_REPORT_V2 || type == LEAVE_GROUP_V2)
	{
		membershipReport->checksum = htons(checksum - 1);
	}

	igmpParams[portid].reportPkt = igmpParams[portid].mbuf;

	return ST_OK;
}

st_group_record_t
StCreateGroupRecord(uint16_t numOfSrcs, uint32_t groupAddress, uint32_t sourceAddress,
					qroup_record_type_t type)
{
	st_group_record_t grOfRec;

	grOfRec.recordType = type;
	grOfRec.auxDataLen = 0;	 // According to the RFC 3376, the field must be 0
	grOfRec.numberOfSources = htons(numOfSrcs);
	grOfRec.multicastAddress = groupAddress;
	grOfRec.sourceAddress = sourceAddress;	// Change in case of handling more than one source
	grOfRec.auxiliaryData = 0;				// According to the RFC 3376, the field must be 0

	return grOfRec;
}

st_status_t
StCreateMembershipReportV3(uint32_t groupAddress, uint32_t sourceAddress, qroup_record_type_t type,
						   uint16_t numberOfGroupRecords, int portid)
{
	if (!igmpParams[portid].mbuf)
	{
		return ST_BUFFER_NOT_READY;
	}
	rte_mbuf_refcnt_set(igmpParams[portid].mbuf, MBUF_LIFE_TIME);

	st_membership_report_v3_t *membershipReport
		= rte_pktmbuf_mtod_offset(igmpParams[portid].mbuf, st_membership_report_v3_t *,
								  sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));

	membershipReport->type = MEMBERSHIP_REPORT_V3;
	membershipReport->reserved_1 = 0x00;
	membershipReport->checksum = 0x00;	// set to zero for the calculation purpose
	membershipReport->reserved_2 = 0x00;
	membershipReport->numberOfGroupRecords = htons(numberOfGroupRecords);
	membershipReport->groupRecords = StCreateGroupRecord(
		0, groupAddress, 0, type);	// Change in case of handling more than one source

	igmpParams[portid].mbuf->pkt_len = IgmpSetPktSize(sizeof(st_membership_report_v3_t));
	igmpParams[portid].mbuf->data_len = IgmpSetPktSize(sizeof(st_membership_report_v3_t));
	igmpParams[portid].mbuf->ol_flags = PKT_TX_IPV4;

	igmpParams[portid].mbuf->l2_len = sizeof(struct rte_ether_hdr);
	igmpParams[portid].mbuf->l3_len = sizeof(struct rte_ipv4_hdr);

	struct rte_ether_hdr *ethHdr
		= rte_pktmbuf_mtod(igmpParams[portid].mbuf, struct rte_ether_hdr *);
	rte_eth_macaddr_get(igmpParams[portid].portId, &ethHdr->s_addr);
	static struct rte_ether_addr const multicast_dst = { { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x16 } };

	rte_ether_addr_copy(&multicast_dst, &ethHdr->d_addr);

	ethHdr->ether_type = htons(IPv4_PROTOCOL);

	struct rte_ipv4_hdr *ipHdr = rte_pktmbuf_mtod_offset(
		igmpParams[portid].mbuf, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
	ipHdr->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
	ipHdr->time_to_live = 1;
	ipHdr->type_of_service = IP_IGMP_DSCP_VALUE;
	ipHdr->fragment_offset = IP_DONT_FRAGMENT_FLAG;

	igmpParams[portid].mbuf->ol_flags |= PKT_TX_IP_CKSUM;
	ipHdr->hdr_checksum = 0;

	uint16_t tLen = sizeof(struct rte_ipv4_hdr) + sizeof(st_membership_report_v3_t);
	ipHdr->total_length = htons(tLen);
	ipHdr->next_proto_id = IGMP_PROTOCOL;
	ipHdr->src_addr = sourceAddress;
	ipHdr->dst_addr = igmpParams[portid].reportIpAddress;

	uint16_t checksum = IgmpCalculateChecksum(MEMBERSHIP_QUERY, membershipReport);

	if (checksum <= 0)
	{
		return ST_IGMP_WRONG_CHECKSUM;
	}

	membershipReport->checksum = htons(checksum - 1);

	igmpParams[portid].reportPkt = igmpParams[portid].mbuf;

	return ST_OK;
}

st_status_t
StSendMembershipQuery(int portid)
{
	if (!igmpParams[portid].queryPkt)
	{
		return ST_BUFFER_NOT_READY;
	}

	int retryCount = 3;

	do
	{
		if (unlikely(igmpParams[portid].state != IGMP_INITIALIZED))
		{
			return ST_IGMP_NOT_READY;
		}

		if (rte_eth_tx_burst(igmpParams[portid].portId, igmpParams[portid].ringId,
							 (struct rte_mbuf **)&igmpParams[portid].queryPkt, 1)
			> 0)
		{
			return ST_OK;
		}
		retryCount--;
	} while (retryCount != 0);

	RTE_LOG(DEBUG, USER1, "Membership Query was not send for portid %d\n", portid);
	return ST_IGMP_SEND_QUERY_FAILED;
}

st_status_t
StSendMembershipReport(int portid)
{
	if (!igmpParams[portid].reportPkt)
	{
		return ST_BUFFER_NOT_READY;
	}

	int retryCount = 3;

	do
	{
		if (unlikely(igmpParams[portid].state != IGMP_INITIALIZED))
		{
			return ST_IGMP_NOT_READY;
		}

		if (rte_eth_tx_burst(igmpParams[portid].portId, igmpParams[portid].ringId,
							 (struct rte_mbuf **)&igmpParams[portid].reportPkt, 1)
			> 0)
		{
			return ST_OK;
		}
		retryCount--;
	} while (retryCount != 0);

	RTE_LOG(DEBUG, USER1, "Membership Report was not send for portid %d\n", portid);
	return ST_IGMP_SEND_REPORT_FAILED;
}

st_status_t
StIgmpQuerierStart()
{
	if (isQuerierStopped == 0)
	{
		return ST_OK;
	}

	isQuerierStopped = 0;
	pthread_create(&querierThread, NULL, &IgmpQuerierLoop, NULL);

	return ST_OK;
}

st_status_t
StIgmpQuerierStop()
{
	if (isQuerierStopped == 1)
	{
		return ST_OK;
	}

	for (int i = 0; i < stMainParams.numPorts; i++)
	{
		igmpParams[i].state = IGMP_STOPPED;
	}

	isQuerierStopped = 1;
	pthread_join(querierThread, NULL);

	for (int i = 0; i < stMainParams.numPorts; i++)
	{
		if (igmpParams[i].sourceAddressesList)
		{
			free(igmpParams[i].sourceAddressesList);
			igmpParams[i].sourceAddressesList = NULL;
		}
		igmpParams[i].state = IGMP_NOT_INITIALIZED;
	}

	RTE_LOG(INFO, USER1, "IGMP querier STOPPED\n");

	return ST_OK;
}

st_status_t
StIgmpInit(uint16_t portId, struct rte_mempool *mempool, uint32_t *srcIpAddr,
		   uint32_t *multicastIpAddr, uint16_t ringId)
{
	if (!mempool || !srcIpAddr || !multicastIpAddr)
	{
		return ST_INVALID_PARAM;
	}
    if (igmpParams[portId].state == IGMP_INITIALIZED)
	{
		return ST_OK;
	}
	igmpParams[portId].igmp_version = IGMP_V3;
	igmpParams[portId].portId = portId;
	igmpParams[portId].queryMessageType
		= GENERAL_MEMBERSHIP_QUERY;	 // At this moment only general query is sent,
									 // environment is prepared for support other
									 // message types but it is not used.
	igmpParams[portId].sourceAddressesList = malloc(MIN_NUMBER_OF_ENTIRES * sizeof(uint32_t));
	igmpParams[portId].maxNumberOfSources = MIN_NUMBER_OF_ENTIRES;
	igmpParams[portId].numberOfSources = 0;
	igmpParams[portId].srcIpAddress = *srcIpAddr;
	igmpParams[portId].groupIpAddress[ST_TX] = multicastIpAddr[ST_TX];
	igmpParams[portId].groupIpAddress[ST_RX] = multicastIpAddr[ST_RX];

	igmpParams[portId].mbuf = rte_pktmbuf_alloc(mempool);
	igmpParams[portId].ringId = ringId;
	if (igmpParams[portId].mbuf == NULL)
	{
		RTE_LOG(ERR, USER1, "IGMP rte_pktmbuf_alloc failed\n");
		return ST_NO_MEMORY;
	}
	rte_mbuf_refcnt_set(igmpParams[portId].mbuf, 3 * MBUF_LIFE_TIME);

	if (inet_pton(AF_INET, "224.0.0.1", &igmpParams[portId].queryIpAddress) != 1)
	{
		RTE_LOG(ERR, USER1, "Translate IP query address error\n");
	}
	if (inet_pton(AF_INET, "224.0.0.22", &igmpParams[portId].reportIpAddress) != 1)
	{
		RTE_LOG(ERR, USER1, "Translate IP report address error\n");
	}
	if (inet_pton(AF_INET, "224.0.0.0", &igmpParams[portId].minMulitcastIpAddress) != 1)
	{
		RTE_LOG(ERR, USER1, "Translate IP minMulitcast address error\n");
	}
	if (inet_pton(AF_INET, "239.255.255.255", &igmpParams[portId].maxMulitcastIpAddress) != 1)
	{
		RTE_LOG(ERR, USER1, "Translate IP maxMulitcast address error\n");
	}

	igmpParams[portId].state = IGMP_INITIALIZED;

	if (stMainParams.pTx == 1 || stMainParams.rTx == 1)
	{
		StIgmpQuerierStart();
	}

	return ST_OK;
}

st_status_t
StUpdateSourcesList(uint32_t sourceAddress, int portid)
{

	if (igmpParams[portid].numberOfSources >= igmpParams[portid].maxNumberOfSources)
	{
		igmpParams[portid].sourceAddressesList = realloc(
			igmpParams[portid].sourceAddressesList,
			(igmpParams[portid].numberOfSources + MIN_NUMBER_OF_ENTIRES) * sizeof(uint32_t));
		igmpParams[portid].maxNumberOfSources
			= igmpParams[portid].numberOfSources + MIN_NUMBER_OF_ENTIRES;
	}

	igmpParams[portid].numberOfSources++;

	igmpParams[portid].sourceAddressesList[igmpParams[portid].numberOfSources] = sourceAddress;

	return ST_OK;
}

void *
IgmpQuerierLoop(void *arg)
{
	st_status_t status = ST_OK;

	RTE_LOG(INFO, USER1, "IGMP querier STARTED\n");

	while (!isQuerierStopped)
	{
		for (int portid = 0; portid < stMainParams.numPorts; portid++)
		{
			if (igmpParams[portid].igmp_version == IGMP_V2)
			{
				for (int i = 0; i < MAX_RXTX_TYPES; i++)
				{
					if (igmpParams[portid].groupIpAddress[i] != 0)
					{
						status = StCreateMembershipReportV2(igmpParams[portid].srcIpAddress,
															igmpParams[portid].groupIpAddress[i],
															MEMBERSHIP_QUERY, portid);
						if (status != ST_OK)
						    break;
					}
				}
			}
			else if (igmpParams[portid].igmp_version == IGMP_V3)
			{
				for (int i = 0; i < MAX_RXTX_TYPES; i++)
				{
					if (igmpParams[portid].groupIpAddress[i] != 0)
					{
						status = StCreateMembershipQueryV3(igmpParams[portid].srcIpAddress,
														   igmpParams[portid].groupIpAddress[i],
														   portid);
						if (status != ST_OK)
							break;
					}
				}
			}

			if (status != ST_OK)
			{
				RTE_LOG(ERR, USER1, "StCreateMembershipQuery FAILED, ErrNo: %d\n", status);
				return NULL;
			}

			if (igmpParams[portid].igmp_version == IGMP_V2)
			{
				status = StSendMembershipReport(portid);
			}
			else if (igmpParams[portid].igmp_version == IGMP_V3)
			{
				status = StSendMembershipQuery(portid);
			}

			if (status != ST_OK)
			{
				RTE_LOG(ERR, USER1, "StSendMembershipQuery FAILED, ErrNo: %d\n", status);
			}
		}
		for (int i = 0; i < QUERY_INTERVAL; i++) {
			if (isQuerierStopped)
				break;
			sleep(1);
		}
	}
	for (int portid = 0; portid < stMainParams.numPorts; portid++)
	{
		rte_pktmbuf_free(igmpParams[portid].mbuf);
	}
	return NULL;
}

void
ParseIp(ipv4Hdr_t const *ipHdr, struct rte_mbuf *m, uint16_t portid)
{
	if (ipHdr->next_proto_id != IGMP_PROTOCOL || !ipHdr || !m)
	{
		return;
	}

	st_status_t status = ST_OK;

	st_membership_query_t *recvQuery = rte_pktmbuf_mtod_offset(
		m, st_membership_query_t *, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));

	switch (recvQuery->type)
	{
	case MEMBERSHIP_QUERY:
		if (igmpParams[portid].igmp_version == IGMP_V2)
		{
			for (int i = 0; i < MAX_RXTX_TYPES; i++)
			{
				status = StCreateMembershipReportV2(igmpParams[portid].srcIpAddress,
													igmpParams[portid].groupIpAddress[i],
													MEMBERSHIP_REPORT_V2, portid);
				if (status != ST_OK)
					break;
			}
		}
		else if (igmpParams[portid].igmp_version == IGMP_V3)
		{
			for (int i = 0; i < MAX_RXTX_TYPES; i++)
			{
				status = StCreateMembershipReportV3(igmpParams[portid].groupIpAddress[i],
													igmpParams[portid].srcIpAddress,
													MODE_IS_EXCLUDE, 1, portid);
				if (status != ST_OK)
					break;
			}
		}
		else
		{
			/* unsupported IGMP version */
			RTE_LOG(DEBUG, USER1, "Unsupported IGMP version %d\n", igmpParams[portid].igmp_version);
			return;
		}
		if (status != ST_OK)
		{
			RTE_LOG(ERR, USER1, "StCreateMembershipReport FAILED, ErrNo: %d\n", status);
			return;
		}
		status = StSendMembershipReport(portid);
		if (status != ST_OK)
		{
			RTE_LOG(ERR, USER1, "StSendMembershipReport FAILED, ErrNo: %d\n", status);
			return;
		}
		break;
	case MEMBERSHIP_REPORT_V1:
		break;
	case MEMBERSHIP_REPORT_V2:
	case LEAVE_GROUP_V2:
		break;
	case MEMBERSHIP_REPORT_V3:
		break;
	default:
		break;
	}
}
