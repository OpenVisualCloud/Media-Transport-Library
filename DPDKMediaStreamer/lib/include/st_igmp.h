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

#ifndef ST_IGMP_H
#define ST_IGMP_H

#include <rte_ip.h>

#include "st_api.h"

typedef struct rte_ipv4_hdr ipv4Hdr_t;

typedef struct rte_ether_hdr ethernetHeader_t;

typedef enum
{
	IGMP_NOT_INITIALIZED = 0x00,
	IGMP_INITIALIZED = 0x01
} igmp_state_t;

typedef enum
{
	IGMP_V1 = 0x01,
	IGMP_V2 = 0x02,
	IGMP_V3 = 0x03
} igmp_version_t;

typedef enum igmp_timers
{
	ROBUSTNESS_VARIABLE = 2,
	QUERY_INTERVAL = 20,			// set to 20 seconds / standard is 125 seconds (value 1 second)
	QUERY_RESPONSE_INTERVAL = 100,	// 10 seconds (value 1/10 second)
	GROUP_MEMBERSHIP_INTERVAL = (ROBUSTNESS_VARIABLE * QUERY_INTERVAL) + QUERY_RESPONSE_INTERVAL,
	STARTUP_QUERY_INTERVAL = QUERY_INTERVAL / 4
} igmp_timers_t;

typedef enum igmp_message_type
{
	MEMBERSHIP_QUERY = 0x11,
	MEMBERSHIP_REPORT_V1 = 0x12,
	MEMBERSHIP_REPORT_V2 = 0x16,
	MEMBERSHIP_REPORT_V3 = 0x22,
	LEAVE_GROUP_V2 = 0x17
} igmp_message_type_t;

typedef enum query_message_type
{
	GENERAL_MEMBERSHIP_QUERY = 0x01,
	GROUP_SPECIFIC_MEMBERSHIP_QUERY = 0x02,
	GROUP_AND_SOURCE_MEMBERSHIP_QUERY = 0x03
} query_message_type_t;

typedef enum qroup_record_type
{
	MODE_IS_INCLUDE = 0x01,
	MODE_IS_EXCLUDE = 0x02,
	CHANGE_TO_INCLUDE_MODE = 0x03,
	CHANGE_TO_EXCLUDE_MODE = 0x04,
	ALLOW_NEW_SOURCES = 0x05,
	BLOCK_OLD_SOURCES = 0x06
} qroup_record_type_t;

typedef struct st_igmp_params
{
	igmp_state_t state;
	igmp_version_t igmp_version;
	struct rte_mempool *mbuf;
	volatile struct rte_mbuf *queryPkt;
	volatile struct rte_mbuf *reportPkt;
	uint16_t portId;
	struct rte_ring *txRing;
	uint32_t minMulitcastIpAddress;
	uint32_t maxMulitcastIpAddress;
	uint32_t srcIpAddress;
	uint32_t groupIpAddress;
	uint32_t reportIpAddress;
	uint32_t queryIpAddress;
	query_message_type_t queryMessageType;
	uint16_t numberOfSources;
	uint16_t maxNumberOfSources;
	uint32_t *sourceAddressesList;
} st_igmp_params_t;

struct st_membership_query
{
	uint8_t type;
	uint8_t maxRespCode;
	uint16_t checksum;
	uint32_t groupAddress;
	struct
	{
		uint8_t qrv : 3;
		uint8_t s : 1;
		uint8_t resv : 4;
	};
	uint8_t qqic;
	uint16_t numberOfSources;
	uint32_t sourceAddress;
} __attribute__((__packed__));
typedef struct st_membership_query st_membership_query_t;

struct st_group_record
{
	uint8_t recordType;
	uint8_t auxDataLen;
	uint16_t numberOfSources;
	uint32_t multicastAddress;
	uint32_t sourceAddress;
	uint32_t auxiliaryData;
} __attribute__((__packed__));
typedef struct st_group_record st_group_record_t;

struct st_membership_report_v1
{
	struct
	{
		uint8_t version : 4;
		uint8_t type : 4;
	};
	uint8_t unused;
	uint16_t checksum;
	uint32_t groupAddress;
} __attribute__((__packed__));
typedef struct st_membership_report_v1 st_membership_report_v1_t;

struct st_membership_report_v2
{
	uint8_t type;
	uint8_t maxRespTime;
	uint16_t checksum;
	uint32_t groupAddress;
} __attribute__((__packed__));
typedef struct st_membership_report_v2 st_membership_report_v2_t;

struct st_membership_report_v3
{
	uint8_t type;
	uint8_t reserved_1;
	uint16_t checksum;
	uint16_t reserved_2;
	uint16_t numberOfGroupRecords;
	st_group_record_t groupRecords;
} __attribute__((__packed__));
typedef struct st_membership_report_v3 st_membership_report_v3_t;

extern st_status_t StIgmpQuerierInit(uint16_t portId, struct rte_mempool *mbuf,
									 struct rte_ring *txRing, uint32_t *srcIpAddr,
									 uint32_t *multicastIpAddr);
st_status_t StCreateMembershipQueryV3(uint32_t sourceAddress, uint32_t groupAddress);
st_status_t StCreateMembershipReportV2(uint32_t groupAddress, igmp_message_type_t type);
st_group_record_t StCreateGroupRecord(uint16_t numOfSrcs, uint32_t destinationAddress,
									  uint32_t sourceAddress, qroup_record_type_t type);
st_status_t StCreateMembershipReportV3(uint32_t groupAddress, uint32_t sourceAddress,
									   qroup_record_type_t type, uint16_t numberOfGroupRecords);
st_status_t StSendMembershipQuery();
st_status_t StSendMembershipReport();
st_status_t StIgmpQuerierStart();
st_status_t StIgmpQuerierStop();
st_status_t StUpdateSourcesList(uint32_t sourceAddress);

void *StIgmpQuerierLoop(void *arg);
extern void ParseIp(ipv4Hdr_t const *ipHdr, struct rte_mbuf *m, uint16_t portid);

#endif	// ST_IGMP_H
