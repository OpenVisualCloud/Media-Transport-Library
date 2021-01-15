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
atomic_int_least8_t isQuerierStopped;

static st_igmp_params_t igmpParams = { .state = IGMP_NOT_INITIALIZED, .numberOfSources = 0 };

st_status_t
StCreateMembershipQueryV3(uint32_t sourceAddress, uint32_t groupAddress)
{
	struct rte_mbuf *queryPktTmp = rte_pktmbuf_alloc(igmpParams.mbuf);
	if (queryPktTmp == NULL)
	{
		RTE_LOG(ERR, USER1, "Create Membership Query rte_pktmbuf_alloc failed\n");
		return ST_NO_MEMORY;
	}
	st_membership_query_t *membershipQuery
		= rte_pktmbuf_mtod_offset(queryPktTmp, st_membership_query_t *,
								  sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));

	membershipQuery->type = MEMBERSHIP_QUERY;
	membershipQuery->maxRespCode = QUERY_RESPONSE_INTERVAL;
	membershipQuery->checksum = 0x00;  // set to zero for the calculation purpose
	membershipQuery->resv = 0x00;
	membershipQuery->s = 0x00;
	membershipQuery->qrv = 0x00;
	membershipQuery->qqic = 0x08;

	queryPktTmp->pkt_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr)
						   + sizeof(st_membership_query_t);
	queryPktTmp->data_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr)
							+ sizeof(st_membership_query_t);
	queryPktTmp->ol_flags = PKT_TX_IPV4;

	queryPktTmp->l2_len = sizeof(struct rte_ether_hdr);
	queryPktTmp->l3_len = sizeof(struct rte_ipv4_hdr);

	struct rte_ether_hdr *ethHdr = rte_pktmbuf_mtod(queryPktTmp, struct rte_ether_hdr *);
	rte_eth_macaddr_get(igmpParams.portId, &ethHdr->s_addr);

	ethHdr->ether_type = htons(IPv4_PROTOCOL);

	struct rte_ipv4_hdr *ipHdr
		= rte_pktmbuf_mtod_offset(queryPktTmp, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
	ipHdr->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
	ipHdr->time_to_live = 1;
	ipHdr->type_of_service = IP_IGMP_DSCP_VALUE;
	ipHdr->fragment_offset = IP_DONT_FRAGMENT_FLAG;

	queryPktTmp->ol_flags |= PKT_TX_IP_CKSUM;
	ipHdr->hdr_checksum = 0;

	uint16_t tLen = sizeof(struct rte_ipv4_hdr) + sizeof(st_membership_query_t);
	ipHdr->total_length = htons(tLen);
	ipHdr->next_proto_id = IGMP_PROTOCOL;
	ipHdr->src_addr = sourceAddress;

	static struct rte_ether_addr multicast = { 0 };
	multicast.addr_bytes[0] = 0x01;
	multicast.addr_bytes[1] = 0x00;
	multicast.addr_bytes[2] = 0x5e;
	uint32_t tmpMacChunk = 0;

	switch (igmpParams.queryMessageType)
	{
	case GENERAL_MEMBERSHIP_QUERY:
		ipHdr->dst_addr = igmpParams.queryIpAddress;
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
		membershipQuery->numberOfSources = htons(igmpParams.numberOfSources);
		membershipQuery->sourceAddress = sourceAddress;
		tmpMacChunk = (groupAddress >> 8) & 0xFFFFFF7F;
		memcpy(&multicast.addr_bytes[3], &tmpMacChunk, sizeof(uint8_t) * 3);
		break;
	default:
		break;
	}

	rte_ether_addr_copy(&multicast, &ethHdr->d_addr);

	// calculate checksum
	uint8_t count = sizeof(st_membership_query_t) / sizeof(uint16_t);
	uint16_t *val_1 = (uint16_t *)(membershipQuery);
	uint16_t sum = 0;

	while (count > 0)
	{
		sum = sum + ntohs(*val_1);
		val_1++;
		count--;
	}
	sum = ~sum;

	membershipQuery->checksum = htons(sum);

	igmpParams.queryPkt = queryPktTmp;

	return ST_OK;
}

st_status_t
StCreateMembershipReportV2(uint32_t groupAddress, igmp_message_type_t type)
{
	struct rte_mbuf *reportPktTmp = rte_pktmbuf_alloc(igmpParams.mbuf);
	if (reportPktTmp == NULL)
	{
		RTE_LOG(ERR, USER1, "Create Membership Report rte_pktmbuf_alloc failed\n");
		return ST_NO_MEMORY;
	}

	st_membership_report_v2_t *membershipReport
		= rte_pktmbuf_mtod_offset(reportPktTmp, st_membership_report_v2_t *,
								  sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));

	membershipReport->type = type;
	membershipReport->maxRespTime = QUERY_RESPONSE_INTERVAL;
	membershipReport->checksum = 0;

	reportPktTmp->pkt_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr)
							+ sizeof(st_membership_report_v2_t);
	reportPktTmp->data_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr)
							 + sizeof(st_membership_report_v2_t);
	reportPktTmp->ol_flags = PKT_TX_IPV4;

	reportPktTmp->l2_len = sizeof(struct rte_ether_hdr);
	reportPktTmp->l3_len = sizeof(struct rte_ipv4_hdr);

	struct rte_ether_hdr *ethHdr = rte_pktmbuf_mtod(reportPktTmp, struct rte_ether_hdr *);
	rte_eth_macaddr_get(igmpParams.portId, &ethHdr->s_addr);

	ethHdr->ether_type = htons(IPv4_PROTOCOL);

	struct rte_ipv4_hdr *ipHdr = rte_pktmbuf_mtod_offset(reportPktTmp, struct rte_ipv4_hdr *,
														 sizeof(struct rte_ether_hdr));
	ipHdr->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
	ipHdr->time_to_live = 1;
	ipHdr->type_of_service = IP_IGMP_DSCP_VALUE;
	ipHdr->fragment_offset = IP_DONT_FRAGMENT_FLAG;

	reportPktTmp->ol_flags |= PKT_TX_IP_CKSUM;
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
		ipHdr->src_addr = igmpParams.srcIpAddress;
		switch (igmpParams.queryMessageType)
		{
		case GENERAL_MEMBERSHIP_QUERY:
			membershipReport->groupAddress = 0;
			ipHdr->dst_addr = igmpParams.queryIpAddress;
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
		ipHdr->src_addr = 0x0;
		membershipReport->groupAddress = groupAddress;
		ipHdr->dst_addr = groupAddress;
		tmpMacChunk = (groupAddress >> 8) & 0xFFFFFF7F;
		memcpy(&multicast_dst.addr_bytes[3], &tmpMacChunk, sizeof(uint8_t) * 3);
	}

	rte_ether_addr_copy(&multicast_dst, &ethHdr->d_addr);

	// calculate checksum
	uint8_t count = sizeof(st_membership_report_v2_t) / sizeof(uint16_t);
	uint16_t *val_1 = (uint16_t *)(membershipReport);
	uint16_t sum = 0;

	while (count > 0)
	{
		sum = sum + ntohs(*val_1);
		val_1++;
		count--;
	}
	sum = ~sum;

	if (type == MEMBERSHIP_QUERY)
	{
		membershipReport->checksum = htons(sum);
	}
	else if (type == MEMBERSHIP_REPORT_V2 || type == LEAVE_GROUP_V2)
	{
		membershipReport->checksum = htons(sum - 1);
	}

	igmpParams.reportPkt = reportPktTmp;

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
						   uint16_t numberOfGroupRecords)
{
	struct rte_mbuf *reportPktTmp = rte_pktmbuf_alloc(igmpParams.mbuf);
	if (reportPktTmp == NULL)
	{
		RTE_LOG(ERR, USER1, "Create Membership Report rte_pktmbuf_alloc failed\n");
		return ST_NO_MEMORY;
	}

	st_membership_report_v3_t *membershipReport
		= rte_pktmbuf_mtod_offset(reportPktTmp, st_membership_report_v3_t *,
								  sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));

	membershipReport->type = MEMBERSHIP_REPORT_V3;
	membershipReport->reserved_1 = 0x00;
	membershipReport->checksum = 0x00;	// set to zero for the calculation purpose
	membershipReport->reserved_2 = 0x00;
	membershipReport->numberOfGroupRecords = htons(numberOfGroupRecords);
	membershipReport->groupRecords = StCreateGroupRecord(
		0, groupAddress, 0, type);	// Change in case of handling more than one source

	reportPktTmp->pkt_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr)
							+ sizeof(st_membership_report_v3_t);
	reportPktTmp->data_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr)
							 + sizeof(st_membership_report_v3_t);
	reportPktTmp->ol_flags = PKT_TX_IPV4;

	reportPktTmp->l2_len = sizeof(struct rte_ether_hdr);
	reportPktTmp->l3_len = sizeof(struct rte_ipv4_hdr);

	struct rte_ether_hdr *ethHdr = rte_pktmbuf_mtod(reportPktTmp, struct rte_ether_hdr *);
	rte_eth_macaddr_get(igmpParams.portId, &ethHdr->s_addr);
	static struct rte_ether_addr const multicast_dst = { { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x16 } };

	rte_ether_addr_copy(&multicast_dst, &ethHdr->d_addr);

	ethHdr->ether_type = htons(IPv4_PROTOCOL);

	struct rte_ipv4_hdr *ipHdr = rte_pktmbuf_mtod_offset(reportPktTmp, struct rte_ipv4_hdr *,
														 sizeof(struct rte_ether_hdr));
	ipHdr->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
	ipHdr->time_to_live = 1;
	ipHdr->type_of_service = IP_IGMP_DSCP_VALUE;
	ipHdr->fragment_offset = IP_DONT_FRAGMENT_FLAG;

	reportPktTmp->ol_flags |= PKT_TX_IP_CKSUM;
	ipHdr->hdr_checksum = 0;

	uint16_t tLen = sizeof(struct rte_ipv4_hdr) + sizeof(st_membership_report_v3_t);
	ipHdr->total_length = htons(tLen);
	ipHdr->next_proto_id = IGMP_PROTOCOL;
	ipHdr->src_addr = 0x0;
	ipHdr->dst_addr = igmpParams.reportIpAddress;

	// calculate checksum
	uint8_t count = sizeof(st_membership_report_v3_t) / sizeof(uint16_t);
	uint16_t *val_1 = (uint16_t *)(membershipReport);
	uint16_t sum = 0;

	while (count > 0)
	{
		sum = sum + ntohs(*val_1);
		val_1++;
		count--;
	}
	sum = ~sum;

	membershipReport->checksum = htons(sum - 1);

	igmpParams.reportPkt = reportPktTmp;

	return ST_OK;
}

st_status_t
StSendMembershipQuery()
{
	if (rte_ring_sp_enqueue(igmpParams.txRing, (void *)igmpParams.queryPkt) < 0)
	{
		RTE_LOG(ERR, USER1,
				"ERROR: Not enough room in the ring to enqueue; object Membership Query is not "
				"enqueued\n");
		return ST_NO_MEMORY;
	}

	return ST_OK;
}

st_status_t
StSendMembershipReport()
{
	if (rte_ring_sp_enqueue(igmpParams.txRing, (void *)igmpParams.reportPkt) < 0)
	{
		RTE_LOG(ERR, USER1,
				"ERROR: Not enough room in the ring to enqueue; object Membership Report is not "
				"enqueued\n");
		return ST_NO_MEMORY;
	}

	return ST_OK;
}

st_status_t
StIgmpQuerierStart()
{
	if (igmpParams.state != IGMP_INITIALIZED)
	{
		return ST_IGMP_QUERIER_NOT_READY;
	}

	isQuerierStopped = 0;
	pthread_create(&querierThread, NULL, &StIgmpQuerierLoop, NULL);

	return ST_OK;
}

st_status_t
StIgmpQuerierStop()
{
	if (igmpParams.state != IGMP_INITIALIZED)
	{
		return ST_IGMP_QUERIER_NOT_READY;
	}

	isQuerierStopped = 1;
	pthread_join(querierThread, NULL);

	free(igmpParams.sourceAddressesList);

	igmpParams.state = IGMP_NOT_INITIALIZED;

	RTE_LOG(INFO, USER1, "IGMP querier STOPPED\n");

	return ST_OK;
}

st_status_t
StIgmpQuerierInit(uint16_t portId, struct rte_mempool *mbuf, struct rte_ring *txRing,
				  uint32_t *srcIpAddr, uint32_t *multicastIpAddr)
{
	igmpParams.igmp_version = IGMP_V3;
	igmpParams.mbuf = mbuf;
	igmpParams.portId = portId;
	igmpParams.txRing = txRing;
	igmpParams.queryMessageType = GENERAL_MEMBERSHIP_QUERY;
	igmpParams.sourceAddressesList = malloc(MIN_NUMBER_OF_ENTIRES * sizeof(uint32_t));
	igmpParams.maxNumberOfSources = MIN_NUMBER_OF_ENTIRES;
	igmpParams.numberOfSources = 0;
	igmpParams.srcIpAddress = *srcIpAddr;
	igmpParams.groupIpAddress = *multicastIpAddr;

	if (inet_pton(AF_INET, "224.0.0.1", &igmpParams.queryIpAddress) != 1)
	{
		RTE_LOG(ERR, USER1, "Translate IP query address error\n");
	}
	if (inet_pton(AF_INET, "224.0.0.22", &igmpParams.reportIpAddress) != 1)
	{
		RTE_LOG(ERR, USER1, "Translate IP report address error\n");
	}
	if (inet_pton(AF_INET, "224.0.0.0", &igmpParams.minMulitcastIpAddress) != 1)
	{
		RTE_LOG(ERR, USER1, "Translate IP minMulitcast address error\n");
	}
	if (inet_pton(AF_INET, "239.255.255.255", &igmpParams.maxMulitcastIpAddress) != 1)
	{
		RTE_LOG(ERR, USER1, "Translate IP maxMulitcast address error\n");
	}

	igmpParams.state = IGMP_INITIALIZED;

	if (stMainParams.rxOnly != 1)
	{
		StIgmpQuerierStart();
	}

	return ST_OK;
}

st_status_t
StUpdateSourcesList(uint32_t sourceAddress)
{

	if (igmpParams.numberOfSources >= igmpParams.maxNumberOfSources)
	{
		igmpParams.sourceAddressesList
			= realloc(igmpParams.sourceAddressesList,
					  (igmpParams.numberOfSources + MIN_NUMBER_OF_ENTIRES) * sizeof(uint32_t));
		igmpParams.maxNumberOfSources = igmpParams.numberOfSources + MIN_NUMBER_OF_ENTIRES;
	}

	igmpParams.numberOfSources++;

	igmpParams.sourceAddressesList[igmpParams.numberOfSources] = sourceAddress;

	return ST_OK;
}

void *
StIgmpQuerierLoop(void *arg)
{
	st_status_t status = ST_OK;

	RTE_LOG(INFO, USER1, "IGMP querier STARTED\n");

	while (!isQuerierStopped)
	{
		if (igmpParams.igmp_version == IGMP_V2)
		{
			status = StCreateMembershipReportV2(igmpParams.groupIpAddress, MEMBERSHIP_QUERY);
		}
		else if (igmpParams.igmp_version == IGMP_V3)
		{
			status = StCreateMembershipQueryV3(igmpParams.srcIpAddress, igmpParams.groupIpAddress);
		}

		if (status != ST_OK)
		{
			RTE_LOG(ERR, USER1, "StCreateMembershipQuery FAILED, ErrNo: %d\n", status);
			return NULL;
		}

		if (igmpParams.igmp_version == IGMP_V2)
		{
			status = StSendMembershipReport();
		}
		else if (igmpParams.igmp_version == IGMP_V3)
		{
			status = StSendMembershipQuery();
		}

		if (status != ST_OK)
		{
			RTE_LOG(ERR, USER1, "StSendMembershipQuery FAILED, ErrNo: %d\n", status);
			return NULL;
		}
		sleep(QUERY_INTERVAL);
	}

	return NULL;
}

void
ParseIp(ipv4Hdr_t const *ipHdr, struct rte_mbuf *m, uint16_t portid)
{
	if (ipHdr->next_proto_id != IGMP_PROTOCOL)
	{
		return;
	}
	st_status_t status = ST_OK;

	st_membership_query_t *recvQuery = rte_pktmbuf_mtod_offset(
		m, st_membership_query_t *, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));

	switch (recvQuery->type)
	{
	case MEMBERSHIP_QUERY:
		if (igmpParams.igmp_version == IGMP_V2)
		{
			status = StCreateMembershipReportV2(igmpParams.groupIpAddress, MEMBERSHIP_REPORT_V2);
		}
		else if (igmpParams.igmp_version == IGMP_V3)
		{
			status = StCreateMembershipReportV3(igmpParams.groupIpAddress, igmpParams.srcIpAddress,
									   MODE_IS_EXCLUDE, 1);
		}
		if (status != ST_OK)
		{
			RTE_LOG(ERR, USER1, "StCreateMembershipReport FAILED, ErrNo: %d\n", status);
			return;
		}
		status = StSendMembershipReport();
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
