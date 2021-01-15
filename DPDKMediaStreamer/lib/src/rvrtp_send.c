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

#include <rte_malloc.h>

#include "rvrtp_main.h"

#include <stdio.h>
#include <string.h>

static inline void *RvRtpBuildIpHeader(rvrtp_session_t *s, struct rte_ipv4_hdr *ip);
static inline void *RvRtpUpdateIpHeader(rvrtp_session_t *s, struct rte_ipv4_hdr *ip);
static inline void *RvRtpBuildUdpHeader(rvrtp_session_t *s, struct rte_udp_hdr *udp);
static inline void *RvRtpBuildDualLinePacket(rvrtp_session_t *s, void *hdr);
static inline void *RvRtpBuildSingleLinePacket(rvrtp_session_t *s, void *hdr);
static inline void *RvRtpBuildL2Packet(rvrtp_session_t *s, struct rte_ether_hdr *l2);

extern int St21GetExtIndex(st21_session_t *sn, uint8_t* addr);
//#define TX_RINGS_DEBUG 1
//#define ST_MULTICAST_TEST

rvrtp_device_t stSendDevice;

st_status_t
RvRtpDummyRecvPacket(rvrtp_session_t *s, struct rte_mbuf *rxbuf)
{
	return ST_OK;
}

int32_t
RvRtpGetTrOffsetTimeslot(rvrtp_device_t *dev, uint32_t pktTime, uint32_t tprs, uint32_t *timeslot)
{
	if ((tprs < pktTime) || (dev->snCount == dev->dev.maxSt21Sessions))
		return -1;

	if (dev->timeQuot == 0)
	{
		//initialize with the 1st TPRS
		dev->timeQuot = tprs;  // *dev->dev.rateGbps;
		dev->timeTable[0] = pktTime;
		*timeslot = 0;
#ifdef TX_RINGS_DEBUG
		RTE_LOG(INFO, USER2, "RvRtpGetTimeslot devQuot %u tprs %u\n", dev->timeQuot, tprs);
#endif
		dev->lastAllocSn = 0;
		return 0;
	}
	uint32_t usedTimeQuot = 0;
	if (dev->dev.maxSt21Sessions >= 32)
	{
		for (uint32_t i = 0; i < dev->dev.maxSt21Sessions; i++)
		{
			usedTimeQuot += dev->timeTable[i];
		}
#ifdef TX_RINGS_DEBUG
		RTE_LOG(INFO, USER2, "RvRtpGetTimeslot usedTimeQuot %u\n", usedTimeQuot);
#endif
		if (dev->timeQuot >= usedTimeQuot + pktTime)
		{
			uint32_t snId = dev->lastAllocSn + 8;
			if (snId >= dev->dev.maxSt21Sessions)
			{
				snId = (snId + 1) % dev->dev.maxSt21Sessions;
			}
			uint8_t found = 0;
			uint32_t tries = 0;
			while ((!found) && (tries++ < (dev->dev.maxSt21Sessions - dev->snCount)))
			{
				if (dev->timeTable[snId] == 0)
				{
					found = 1;
					dev->timeTable[snId] = pktTime;
#ifdef TX_RINGS_DEBUG
					RTE_LOG(INFO, USER2, "RvRtpGetTimeslot pktTime %u usedTimeQuot %u\n", pktTime,
							usedTimeQuot);
#endif
					*timeslot = snId;
					dev->lastAllocSn = snId;
					return usedTimeQuot;
				}
				snId += 8;
				if (snId >= dev->dev.maxSt21Sessions)
				{
					snId = (snId + 1) % dev->dev.maxSt21Sessions;
				}
			}
		}
	}
	else
	{
		for (uint32_t i = 0; i < dev->snCount; i++)
		{
			usedTimeQuot += dev->timeTable[i];
		}
		RTE_LOG(INFO, USER2, "RvRtpGetTimeslot usedTimeQuot %u\n", usedTimeQuot);

		if (dev->timeQuot >= usedTimeQuot + pktTime)
		{
			dev->timeTable[dev->snCount] = pktTime;
			RTE_LOG(INFO, USER2, "RvRtpGetTimeslot pktTime %u usedTimeQuot %u\n", pktTime,
					usedTimeQuot);
			*timeslot = dev->snCount;
			return usedTimeQuot;
		}
	}
	RTE_LOG(INFO, USER2,
			"RvRtpGetTimeslot failed since pktTime %u + usedTimeQuot %u > quot of %u\n", pktTime,
			usedTimeQuot, dev->timeQuot);
	return (-1);
}

extern st_main_params_t stMainParams;

void
ArpAnswer(uint16_t portid, uint32_t ip, struct rte_ether_addr const *mac)
{
	//RTE_LOG(DEBUG, USER1, "ArpAnswer %u %08x %u\n", portid, ip, stSendDevice.snCount);
	for (rvrtp_session_t **i = stSendDevice.snTable + stSendDevice.snCount;
		 stSendDevice.snTable <= --i;)
	{
		//RTE_LOG(DEBUG, USER1, "ArpAnswer %u %08x %p\n", portid, ip, *i);
		if (*i)
		{
			if (ip == (*i)->hdrPrint.singleHdr.ip.dst_addr)
			{
				memcpy((*i)->hdrPrint.singleHdr.eth.d_addr.addr_bytes, mac,
					   sizeof((*i)->hdrPrint.singleHdr.eth.d_addr.addr_bytes));
			}
		}
	}
}

void
RvRtpInitPacketCtx(rvrtp_session_t *s, uint32_t ring)
{
	s->ctx.tmstampOddInc = (uint32_t)(((uint64_t)s->fmt.clockRate * (uint64_t)s->fmt.frmRateDen)
									  / (uint64_t)s->fmt.frmRateMul);
	s->ctx.tmstampEvenInc = s->ctx.tmstampOddInc;
	if ((s->ctx.tmstampOddInc & 0x3) == 1)
	{
		s->ctx.tmstampEvenInc++;
	}
	s->ctx.alignTmstamp = 0;
	s->ctx.line1PixelGrpSize = s->fmt.pixelGrpSize;
	s->ctx.line1Offset = 0;
	s->ctx.line1Number = 0;
	s->ctx.line1Length = s->fmt.pixelsInPkt / s->fmt.pixelsInGrp * s->ctx.line1PixelGrpSize;
	s->ctx.line1Size = s->fmt.width / s->fmt.pixelsInGrp * s->ctx.line1PixelGrpSize;

	struct rte_ipv4_hdr *ip;
	struct rte_udp_hdr *udp;

	switch (s->fmt.pktFmt)
	{
	case ST_INTEL_DLN_RFC4175_PKT:
		s->ctx.line2Offset = 0;
		s->ctx.line2Number = 1;
		s->ctx.line2PixelGrpSize = s->fmt.pixelGrpSize;
		s->ctx.line2Size = s->fmt.width / s->fmt.pixelsInGrp * s->ctx.line2PixelGrpSize;
		s->ctx.line2Length = s->fmt.pixelsInPkt / s->fmt.pixelsInGrp * s->ctx.line2PixelGrpSize;
		ip = (struct rte_ipv4_hdr *)RvRtpBuildL2Packet(s, &s->hdrPrint.dualHdr.eth);
		udp = (struct rte_udp_hdr *)RvRtpBuildIpHeader(s, ip);
		{
			st_rfc4175_rtp_dual_hdr_t *rtp
				= (st_rfc4175_rtp_dual_hdr_t *)RvRtpBuildUdpHeader(s, udp);
			RvRtpBuildDualLinePacket(s, rtp);
		}
		break;

	case ST_INTEL_SLN_RFC4175_PKT:
		ip = (struct rte_ipv4_hdr *)RvRtpBuildL2Packet(s, &s->hdrPrint.singleHdr.eth);
		udp = (struct rte_udp_hdr *)RvRtpBuildIpHeader(s, ip);
		{
			st_rfc4175_rtp_dual_hdr_t *rtp
				= (st_rfc4175_rtp_dual_hdr_t *)RvRtpBuildUdpHeader(s, udp);
			RvRtpBuildSingleLinePacket(s, rtp);
		}
		break;
	default:
		break;
	}
#ifdef TX_RINGS_DEBUG
	RTE_LOG(INFO, USER2, "RvRtpInitPacketCtx line1Length %u line2Length %u\n", s->ctx.line1Length,
			s->ctx.line2Length);
#endif
}

st_status_t
RvRtpCreateTxSession(rvrtp_device_t *dev, st21_session_t *sin, st21_format_t *fmt,
					 rvrtp_session_t **sout)
{
	if ((!dev) || (!sin) || (!fmt))
		return ST_INVALID_PARAM;

	uint32_t tmstampTime;
	st21_session_t sn = *sin;

    sn.trOffset = (uint32_t)((uint64_t)(fmt->frameTime * fmt->trOffsetLines) / (uint64_t)fmt->totalLines);
    sn.frameSize = ((uint64_t)fmt->width * fmt->height * fmt->pixelGrpSize) / fmt->pixelsInGrp;

	uint32_t pktsInGappedMode = fmt->pktsInLine * fmt->totalLines;

	if (fmt->pktFmt == ST_INTEL_DLN_RFC4175_PKT)
	{
		pktsInGappedMode /= 2;	//have 8 pkts in line so need to adjust
	}
	if ((fmt->vscan == ST21_2160I) || (fmt->vscan == ST21_1080I) || (fmt->vscan == ST21_720I))
	{
		pktsInGappedMode /= 2;	//have a half packets to send
	}
	else if ((fmt->vscan != ST21_2160P) && (fmt->vscan != ST21_1080P) && (fmt->vscan != ST21_720P))
	{
		return ST_FMT_ERR_BAD_VSCAN;
	}

    switch (dev->dev.pacerType)
    {
    case ST_2110_21_TPN:
        sn.tprs = fmt->frameTime / pktsInGappedMode;
        break;
    case ST_2110_21_TPNL:
    case ST_2110_21_TPW:
        sn.tprs = fmt->frameTime / fmt->pktsInFrame;
        break;
    default:
        return ST_DEV_BAD_PACING;
    }

	switch (fmt->clockRate)
	{
	case 90000:	 //90kHz
		tmstampTime = 11111;
		break;
	default:
		return ST_FMT_ERR_BAD_CLK_RATE;
	}

    sn.pktTime = ((fmt->pktSize + ST_PHYS_PKT_ADD) * 8) / dev->dev.rateGbps;
    uint32_t remaind = ((fmt->pktSize + ST_PHYS_PKT_ADD) * 8) % dev->dev.rateGbps;
    if (remaind >= (dev->dev.rateGbps / 2)) sn.pktTime++;

    int32_t trOffset = RvRtpGetTrOffsetTimeslot(dev, sn.pktTime, sn.tprs, &sn.timeslot);
    if (trOffset < 0)
    {
        RTE_LOG(INFO, USER1, "failed RvRtpGetTrOffsetTimeslot %u %u\n", sn.pktTime, sn.tprs);

        return ST_SN_ERR_NO_TIMESLOT;//impossible to find timeslot for the producer
    }
    sn.trOffset += trOffset;

#ifdef TX_RINGS_DEBUG
	RTE_LOG(INFO, USER1, "RvRtpGetTrOffsetTimeslot troffste %u timeslot %u\n", sn.trOffset,
			sn.timeslot);
#endif

	rvrtp_session_t *s = malloc(sizeof(rvrtp_session_t));
	if (s)
	{
		memset(s, 0x0, sizeof(rvrtp_session_t));

		s->fmt = *fmt;
		s->dev = dev;
		s->sn = sn;
		s->tmstampTime = tmstampTime;

		//have a half lines to send in interlaced cases
		switch (fmt->vscan)
		{
		case ST21_2160I:
		case ST21_1080I:
		case ST21_720I:
			s->ctx.fieldId = 0;
			s->UpdateRtpPkt = RvRtpUpdateInterlacedPacket;
			break;
		default:
			switch (s->fmt.pktFmt)
			{
			case ST_INTEL_DLN_RFC4175_PKT:
				s->UpdateRtpPkt = RvRtpUpdateDualLinePacket;
				break;
			case ST_INTEL_SLN_RFC4175_PKT:
				s->UpdateRtpPkt = RvRtpUpdateSingleLinePacket;
				break;
			default:
				RTE_LOG(INFO, USER2, "Not supported format on transmitter\n");
				free(s);
				return ST_FMT_ERR_NOT_SUPPORTED_ON_TX;
			}
			break;
		}
		s->RecvRtpPkt = RvRtpDummyRecvPacket;

		switch (dev->dev.rateGbps)
		{
		case ST_NIC_RATE_SPEED_10GBPS:
			s->nicTxTime = 35000;
			break;
		case ST_NIC_RATE_SPEED_25GBPS:
			s->nicTxTime = 25000;
			break;
		case ST_NIC_RATE_SPEED_40GBPS:
			s->nicTxTime = 15000;
			break;
		case ST_NIC_RATE_SPEED_100GBPS:
			s->nicTxTime = 9000;
			break;
		}

		*sout = s;
		return ST_OK;
	}
	return ST_NO_MEMORY;
}

/*****************************************************************************
 *
 * RvRtpSessionCheckRunState - check if session is in run state that permits
 * packets to be trasnmitted
 *
 * Returns: nothing but session state is internally updated
 *
 * SEE ALSO:
 */
int
RvRtpSessionCheckRunState(rvrtp_session_t *s)
{
	// lock session
	RvRtpSessionLock(s);

	if (s->state != ST_SN_STATE_RUN)  // state is protected by the lock as well as prodBuf
	{
		if (s->state == ST_SN_STATE_NO_NEXT_FRAME)
		{
			s->prodBuf = s->prod.St21GetNextFrameBuf(s->prod.appHandle, s->prodBuf,
													 s->prod.frameSize, s->ctx.fieldId);
			if (s->prodBuf != NULL)
			{
				s->state = ST_SN_STATE_RUN;
				s->ctx.sliceOffset = 0;
				s->sliceOffset = s->prod.St21GetNextSliceOffset(s->prod.appHandle, s->prodBuf, 0,
																s->ctx.fieldId);
			}
			else
			{
				RTE_LOG(INFO, USER2, "ST_SN_STATE_NO_NEXT_FRAME: for session %u prodBuf %p\n",
						s->sn.timeslot, s->prodBuf);
			}
		}
		else if (s->state == ST_SN_STATE_NO_NEXT_SLICE)
		{
			uint32_t nextOffset = s->prod.St21GetNextSliceOffset(s->prod.appHandle, s->prodBuf,
																 s->sliceOffset, s->ctx.fieldId);
			if (nextOffset > s->sliceOffset)
			{
				s->sliceOffset = nextOffset;
				s->state = ST_SN_STATE_RUN;
			}
			else
			{
				RTE_LOG(INFO, USER2, "ST_SN_STATE_NO_NEXT_SLICE: for session %u sliceOffset %u\n",
						s->sn.timeslot, nextOffset);
			}
		}
	}
	//leave critical section
	RvRtpSessionUnlock(s);

	return (s->state == ST_SN_STATE_RUN);
}

/*****************************************************************************
 *
 * RvRtpBuildIpHeader -IP header constructor routine.
 *
 * DESCRIPTION
 * Constructs the IP header
 *
 * RETURNS: UDP header location
 */
static inline void *
RvRtpBuildIpHeader(rvrtp_session_t *s, struct rte_ipv4_hdr *ip)
{
	uint16_t tlen;

	/* Zero out the header space */
	memset((char *)ip, 0, sizeof(struct rte_ipv4_hdr));

	ip->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);

	ip->time_to_live = 64;
	ip->type_of_service = s->fl[0].tos;

#define IP_DONT_FRAGMENT_FLAG 0x0040
	ip->fragment_offset = IP_DONT_FRAGMENT_FLAG;

	tlen = s->fmt.pktSize - s->etherSize;
	ip->total_length = htons(tlen);
	ip->next_proto_id = 17;
	ip->src_addr = s->fl[0].src.addr4.sin_addr.s_addr;
	ip->dst_addr = s->fl[0].dst.addr4.sin_addr.s_addr;

	return &ip[1];
}

/*****************************************************************************
 *
 * RvRtpUpdateIpHeader -IP header constructor routine.
 *
 * DESCRIPTION
 * Constructs the IP header
 *
 * RETURNS: UDP header location
 */
static inline void *
RvRtpUpdateIpHeader(rvrtp_session_t *s, struct rte_ipv4_hdr *ip)
{
#ifdef ST_LATE_SN_CONNECT
	ip->src_addr = s->fl[0].src.addr4.sin_addr.s_addr;
	ip->dst_addr = s->fl[0].dst.addr4.sin_addr.s_addr;
#endif

	ip->packet_id = htons(s->ctx.ipPacketId++);

#ifdef ST_LATE_SN_CONNECT
	if (unlikely((s->ofldFlags & ST_OFLD_HW_IP_CKSUM) != ST_OFLD_HW_IP_CKSUM))
	{
		ip->hdr_checksum = 0;
		ip->hdr_checksum = rte_ipv4_cksum((const struct rte_ipv4_hdr *)ip);
	}
#endif
	return &ip[1];
}

/*****************************************************************************
 *
 * RvRtpBuildUdpHeader -UDP header constructor routine.
 *
 * DESCRIPTION
 * Constructs the UDP header
 *
 * RETURNS: RTP header location
 */
static inline void *
RvRtpBuildUdpHeader(rvrtp_session_t *s, struct rte_udp_hdr *udp)
{
	uint16_t tlen;

	tlen = s->fmt.pktSize - (s->etherSize + sizeof(struct rte_ipv4_hdr));
	udp->dgram_len = htons(tlen);
	udp->src_port = s->fl[0].src.addr4.sin_port;
	udp->dst_port = s->fl[0].dst.addr4.sin_port;
	// UDP checksum
	udp->dgram_cksum = 0;
	return &udp[1];
}

/*****************************************************************************
 *
 * RvRtpBuildDualLinePacket - UDP RAW Video RTP header constructor routine.
 *
 * DESCRIPTION
 * Constructs the UDP RFC 4175 packet with 2 lines of video
 * and headers in a packer buffer.
 *
 * RETURNS: IP header location
 *
 * SEE ALSO:
 */
static inline void *
RvRtpBuildDualLinePacket(rvrtp_session_t *s, void *hdr)
{
	/* Create the IP & UDP header */
	struct st_rfc4175_rtp_dual_hdr *rtp = (struct st_rfc4175_rtp_dual_hdr *)hdr;

	rtp->version = RVRTP_VERSION_2;
	rtp->padding = 0;
	rtp->marker = 0;
	rtp->csrcCount = 0;
	rtp->payloadType = RVRTP_PAYLOAD_TYPE_RAW_VIDEO;

	rtp->ssrc = htonl(s->sn.ssid);

	rtp->line1Length = htons(s->ctx.line1Length);
	rtp->line2Length = htons(s->ctx.line2Length);
	rtp->line1Number = htons(s->ctx.line1Number);
	rtp->line2Number = htons(s->ctx.line2Number);
	rtp->line1Offset = htons(s->ctx.line1Offset);
	rtp->line2Offset = htons(s->ctx.line2Offset);

	/* Return the original pointer for IP hdr */
	return hdr;
}

/*****************************************************************************
 *
 * RvRtpBuildSingleLinePacket - UDP RAW Video RTP header constructor routine.
 *
 * DESCRIPTION
 * Constructs the UDP RFC 4175 packet with single line of video
 * and headers in a packer buffer.
 *
 * RETURNS: IP header location
 *
 * SEE ALSO:
 */
static inline void *
RvRtpBuildSingleLinePacket(rvrtp_session_t *s, void *hdr)
{
	/* Create the IP & UDP header */
	struct st_rfc4175_rtp_single_hdr *rtp = (struct st_rfc4175_rtp_single_hdr *)hdr;

	rtp->version = RVRTP_VERSION_2;
	rtp->padding = 0;
	rtp->marker = 0;
	rtp->csrcCount = 0;
	rtp->payloadType = RVRTP_PAYLOAD_TYPE_RAW_VIDEO;
	rtp->ssrc = htonl(s->sn.ssid);
	rtp->line1Length = htons(s->ctx.line1Length);
	rtp->line1Number = htons(s->ctx.line1Number);
	rtp->line1Offset = htons(s->ctx.line1Offset);
	return hdr;
}

/*****************************************************************************
 *
 * RvRtpBuildL2Packet - L2 header constructor routine.
 *
 * DESCRIPTION
 * Constructs the l2 packet
 *
 * RETURNS: IP header location
 *
 * SEE ALSO:
 */
static inline void *
RvRtpBuildL2Packet(rvrtp_session_t *s, struct rte_ether_hdr *l2)
{
	struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)&l2[1];

	l2->ether_type = htons(0x0800);
	memcpy(&l2->d_addr, &s->fl[0].dstMac, ETH_ADDR_LEN);
	memcpy(&l2->s_addr, &s->fl[0].srcMac, ETH_ADDR_LEN);
	return ip;
}

/*****************************************************************************
 *
 * RvRtpUpdateDualLinePacket - UDP RAW Video RTP header constructor routine.
 *
 * DESCRIPTION
 * Constructs the UDP RFC 4175 packet with 2 lines of video
 * and headers in a packer buffer.
 *
 * RETURNS: IP header location
 *
 * SEE ALSO:
 */
void *
RvRtpUpdateDualLinePacket(rvrtp_session_t *s, void *hdr, struct rte_mbuf *extMbuf)
{
	/* Create the IP & UDP header */
	struct rte_ipv4_hdr *ip = hdr;
	struct rte_udp_hdr *udp = (struct rte_udp_hdr *)RvRtpUpdateIpHeader(s, ip);

	struct st_rfc4175_rtp_dual_hdr *rtp = (struct st_rfc4175_rtp_dual_hdr *)&udp[1];

	//if (unlikely(s->ctx.tmstamp == 0))
	//{
	//	s->ctx.tmstamp = s->prod.St21GetFrameTmstamp(s->prod.appHandle);
	//}

    /* is frame complete ?*/
    if (unlikely(((s->ctx.line2Number + 1) == s->fmt.height) && (s->ctx.line2Offset >= s->fmt.width - s->fmt.pixelsInPkt)))
    {
        rtp->marker = 1;
    }
    else
    {
        rtp->marker = 0;
    }
    rtp->seqNumber = htons((uint16_t)s->ctx.seqNumber.lohi.seqLo);
    rtp->seqNumberExt = htons((uint16_t)s->ctx.seqNumber.lohi.seqHi);
    rtp->tmstamp = htonl(s->ctx.tmstamp);
    rtp->line1Number = htons(s->ctx.line1Number);
    rtp->line2Number = htons(s->ctx.line2Number);
    rtp->line1Offset = htons(s->ctx.line1Offset | 0x8000); //set line1Continue bit
    rtp->line2Offset = htons(s->ctx.line2Offset);

    /* copy payload */
    uint8_t *payload = (uint8_t *)&rtp[1];

    uint32_t byteLn1Offset = s->ctx.line1Number  * s->ctx.line1Size +
        (uint32_t)s->ctx.line1Offset / s->fmt.pixelsInGrp * s->ctx.line1PixelGrpSize;
    uint32_t byteLn2Offset = s->ctx.line2Number  * s->ctx.line2Size +
        (uint32_t)s->ctx.line2Offset / s->fmt.pixelsInGrp * s->ctx.line2PixelGrpSize;

    /* line 0 */
    memcpy(payload, &s->prodBuf[byteLn1Offset], s->ctx.line1Length);
    /* line 1 */
    memcpy(&payload[s->ctx.line1Length], &s->prodBuf[byteLn2Offset], s->ctx.line2Length);

    extMbuf->data_len = extMbuf->pkt_len = 0; // Not used
    // UDP checksum 
    udp->dgram_cksum = 0;
    if (unlikely((s->ofldFlags & ST_OFLD_HW_UDP_CKSUM) != ST_OFLD_HW_UDP_CKSUM))
    {
        udp->dgram_cksum = rte_ipv4_udptcp_cksum(ip, (const void *)udp);
        if (udp->dgram_cksum == 0) udp->dgram_cksum = 0xFFFF;
    }

    //iterate to next packet
    s->ctx.line1Offset += s->fmt.pixelsInPkt;
    s->ctx.line2Offset += s->fmt.pixelsInPkt;
    s->ctx.seqNumber.sequence++;
    if ((rtp->marker == 0) && (s->ctx.line2Offset >= s->fmt.width))
    {
        s->ctx.line1Offset = 0;
        s->ctx.line2Offset = 0;
        s->ctx.line1Number += 2;
        s->ctx.line2Number += 2;
        s->ctx.sliceOffset = byteLn2Offset + s->ctx.line2Length;
		s->ctx.alignTmstamp = 1;

        uint32_t currentOffset = __sync_fetch_and_or(&s->sliceOffset, 0);

        if (s->ctx.sliceOffset >= currentOffset)
        {
            RvRtpSessionLock(s);

			uint32_t sliceOffset = s->prod.St21GetNextSliceOffset(s->prod.appHandle, s->prodBuf,
																  currentOffset, s->ctx.fieldId);
			if (sliceOffset == currentOffset)
			{
				RTE_LOG(INFO, USER2, "St21GetNextSliceOffset logical error of offset %u == %u\n",
						s->ctx.sliceOffset, currentOffset);
				s->state = ST_SN_STATE_NO_NEXT_SLICE;
			}
			s->sliceOffset = sliceOffset;

			RvRtpSessionUnlock(s);
		}
	}
	s->sn.pktsSend++;
	if (unlikely(rtp->marker == 1))
	{
		s->sn.frmsSend++;
		s->ctx.tmstamp = 0;	 //renew tmstamp at the next round
		s->ctx.line1Offset = 0;
		s->ctx.line2Offset = 0;
		s->ctx.line1Number = 0;
		s->ctx.line2Number = 1;

		s->ctx.sliceOffset = 0;

		//critical section
		RvRtpSessionLock(s);

		s->sliceOffset = 0x0;

		s->prodBuf = s->prod.St21GetNextFrameBuf(s->prod.appHandle, s->prodBuf, s->sn.frameSize,
												 s->ctx.fieldId);
		if (unlikely(s->prodBuf == NULL))
		{
			s->state = ST_SN_STATE_NO_NEXT_FRAME;
		}
		else
		{
			uint32_t nextOffset
				= s->prod.St21GetNextSliceOffset(s->prod.appHandle, s->prodBuf, 0, s->ctx.fieldId);
			s->sliceOffset += nextOffset;
			if (nextOffset == 0)
			{
				RTE_LOG(INFO, USER2, "St21GetNextSliceOffset logical error of offset %u\n",
						nextOffset);
				s->state = ST_SN_STATE_NO_NEXT_SLICE;
			}
		}

		//unlock session and sliceOffset
		RvRtpSessionUnlock(s);
	}
	/* Return the original pointer for IP hdr */
	return hdr;
}

/*****************************************************************************
 *
 * RvRtpUpdateSingleLinePacket - UDP RAW Video RTP header constructor routine.
 *
 * DESCRIPTION
 * Constructs the UDP RFC 4175 packet with single line of video
 * and headers in a packer buffer.
 *
 * RETURNS: IP header location
 *
 * SEE ALSO:
 */
void *
RvRtpUpdateSingleLinePacket(rvrtp_session_t *s, void *hdr, struct rte_mbuf *extMbuf)
{
	/* Create the IP & UDP header */
	struct rte_ipv4_hdr *ip = hdr;
	struct rte_udp_hdr *udp = (struct rte_udp_hdr *)RvRtpUpdateIpHeader(s, ip);

	struct st_rfc4175_rtp_single_hdr *rtp = (struct st_rfc4175_rtp_single_hdr *)&udp[1];

	/* is frame complete ?*/
	if (unlikely(((s->ctx.line1Number + 1) == s->fmt.height)
				 && (s->ctx.line1Offset >= s->fmt.width - s->fmt.pixelsInPkt)))
	{
		rtp->marker = 1;
	}
	else
	{
		rtp->marker = 0;
	}
	rtp->seqNumber = htons((uint16_t)s->ctx.seqNumber.lohi.seqLo);
	rtp->seqNumberExt = htons((uint16_t)s->ctx.seqNumber.lohi.seqHi);

    rtp->tmstamp = htonl(s->ctx.tmstamp);
    rtp->line1Number = htons(s->ctx.line1Number);
    rtp->line1Offset = htons(s->ctx.line1Offset);
	uint32_t lengthLeft = 
		MIN(s->ctx.line1Length, s->ctx.line1Size - (uint32_t)s->ctx.line1Offset / s->fmt.pixelsInGrp * s->ctx.line1PixelGrpSize);
	rtp->line1Length = htons(lengthLeft);

	/* copy payload */
	uint32_t byteLn1Offset =
		s->ctx.line1Number * s->ctx.line1Size + (uint32_t)s->ctx.line1Offset / s->fmt.pixelsInGrp * s->ctx.line1PixelGrpSize;

	/* line 1 */
	int idx = St21GetExtIndex(&s->sn, s->prodBuf);
	rte_iova_t bufIova = s->sn.extMem.bufIova[idx] + byteLn1Offset;
	struct rte_mbuf_ext_shared_info *shInfo = s->sn.extMem.shInfo[idx];
	/* Attach extbuf to mbuf */
	rte_pktmbuf_attach_extbuf(extMbuf, &s->prodBuf[byteLn1Offset], bufIova, lengthLeft, shInfo);
	rte_mbuf_ext_refcnt_update(shInfo, 1);
	extMbuf->data_len = extMbuf->pkt_len = lengthLeft;

	// UDP checksum
	udp->dgram_cksum = 0;
	if (unlikely((s->ofldFlags & ST_OFLD_HW_UDP_CKSUM) != ST_OFLD_HW_UDP_CKSUM))
	{
		udp->dgram_cksum = rte_ipv4_udptcp_cksum(ip, (const void *)udp);
		if (udp->dgram_cksum == 0)
			udp->dgram_cksum = 0xFFFF;
	}

	//iterate to next packet
	s->ctx.line1Offset += s->fmt.pixelsInPkt;
	s->ctx.seqNumber.sequence++;
	if ((rtp->marker == 0) && (s->ctx.line1Offset >= s->fmt.width))
	{
		s->ctx.line1Offset = 0;
		s->ctx.line1Number++;
		s->ctx.alignTmstamp = (s->ctx.line1Number & 0x1);
		s->ctx.sliceOffset = byteLn1Offset + s->ctx.line1Length;

		uint32_t currentOffset = __sync_fetch_and_or(&s->sliceOffset, 0);

		if (s->ctx.sliceOffset >= currentOffset)
		{
			RvRtpSessionLock(s);

            uint32_t sliceOffset = 
				s->prod.St21GetNextSliceOffset(s->prod.appHandle, s->prodBuf, currentOffset, s->ctx.fieldId);
            if (sliceOffset == currentOffset)
            {
                RTE_LOG(INFO, USER2, "St21GetNextSliceOffset logical error of offset %u == %u\n", s->ctx.sliceOffset, currentOffset);
                s->state = ST_SN_STATE_NO_NEXT_SLICE;
            }
            s->sliceOffset = sliceOffset;

            RvRtpSessionUnlock(s);
        }
    }
    s->sn.pktsSend++;
    if (unlikely(rtp->marker == 1))
    {
        s->sn.frmsSend++;
        s->ctx.tmstamp = 0;//renew tmstamp at the next round
        s->ctx.line1Offset = 0;
        s->ctx.line1Number = 0;
        s->ctx.sliceOffset = 0;

        //critical section
        RvRtpSessionLock(s);

        s->sliceOffset = 0x0;

		s->prodBuf = s->prod.St21GetNextFrameBuf(s->prod.appHandle, s->prodBuf, s->sn.frameSize,
												 s->ctx.fieldId);
		if (s->prodBuf == NULL)
		{
			s->state = ST_SN_STATE_NO_NEXT_FRAME;
		}
		else
		{
			uint32_t nextOffset
				= s->prod.St21GetNextSliceOffset(s->prod.appHandle, s->prodBuf, 0, s->ctx.fieldId);
			s->sliceOffset += nextOffset;
			if (nextOffset == 0)
			{
				RTE_LOG(INFO, USER2, "St21GetNextSliceOffset logical error of offset %u\n",
						nextOffset);
				s->state = ST_SN_STATE_NO_NEXT_SLICE;
			}
		}
		//unlock session and sliceOffset
		RvRtpSessionUnlock(s);
	}
	/* Return the original pointer for IP hdr */
	return hdr;
}

/*****************************************************************************
 *
 * RvRtpUpdateInterlacedPacket - UDP RAW Video RTP header constructor routine.
 *
 * DESCRIPTION
 * Constructs the UDP RFC 4175 packet with single line of interlaced video
 * and headers in a packer buffer.
 *
 * RETURNS: IP header location
 *
 * SEE ALSO:
 */
void *
RvRtpUpdateInterlacedPacket(rvrtp_session_t *s, void *hdr, struct rte_mbuf *extMbuf)
{
	/* Create the IP & UDP header */
	struct rte_ipv4_hdr *ip = hdr;
	struct rte_udp_hdr *udp = (struct rte_udp_hdr *)RvRtpUpdateIpHeader(s, ip);

	struct st_rfc4175_rtp_single_hdr *rtp = (struct st_rfc4175_rtp_single_hdr *)&udp[1];

	/* is frame complete ?*/
	if (unlikely(((s->ctx.line1Number + 1) == s->fmt.height / 2)
				 && (s->ctx.line1Offset >= s->fmt.width - s->fmt.pixelsInPkt)))
	{
		rtp->marker = 1;
	}
	else
	{
		rtp->marker = 0;
	}
	rtp->seqNumber = htons((uint16_t)s->ctx.seqNumber.lohi.seqLo);
	rtp->seqNumberExt = htons((uint16_t)s->ctx.seqNumber.lohi.seqHi);

    rtp->tmstamp = htonl(s->ctx.tmstamp);
    rtp->line1Number = htons(s->ctx.line1Number | (s->ctx.fieldId << 15)); //set Field value and line number;
    rtp->line1Offset = htons(s->ctx.line1Offset);
	uint32_t lengthLeft = 
		MIN(s->ctx.line1Length, s->ctx.line1Size - (uint32_t)s->ctx.line1Offset / s->fmt.pixelsInGrp * s->ctx.line1PixelGrpSize);

	/* copy payload */
	uint32_t byteLn1Offset =
		(s->ctx.line1Number * 2 + s->ctx.fieldId) * s->ctx.line1Size + 
		(uint32_t)s->ctx.line1Offset / s->fmt.pixelsInGrp * s->ctx.line1PixelGrpSize;

	/* line 1 */
	int idx = St21GetExtIndex(&s->sn, s->prodBuf);
	rte_iova_t bufIova = s->sn.extMem.bufIova[idx] + byteLn1Offset;
	struct rte_mbuf_ext_shared_info *shInfo = s->sn.extMem.shInfo[idx];
	/* Attach extbuf to mbuf */
	rte_pktmbuf_attach_extbuf(extMbuf, &s->prodBuf[byteLn1Offset], bufIova, lengthLeft, shInfo);
	rte_mbuf_ext_refcnt_update(shInfo, 1);
	extMbuf->data_len = extMbuf->pkt_len = lengthLeft;

    // UDP checksum 
    udp->dgram_cksum = 0;
    if (unlikely((s->ofldFlags & ST_OFLD_HW_UDP_CKSUM) != ST_OFLD_HW_UDP_CKSUM))
    {
        udp->dgram_cksum = rte_ipv4_udptcp_cksum(ip, (const void *)udp);
        if (udp->dgram_cksum == 0)
            udp->dgram_cksum = 0xFFFF;
    }

    //iterate to next packet
    s->ctx.line1Offset += s->fmt.pixelsInPkt;
    s->ctx.seqNumber.sequence++;
    if ((rtp->marker == 0) && (s->ctx.line1Offset >= s->fmt.width))
    {
        s->ctx.line1Offset = 0;
        s->ctx.line1Number++;
		s->ctx.alignTmstamp = (s->ctx.line1Number & 0x1);
        s->ctx.sliceOffset = byteLn1Offset + s->ctx.line1Length;

        uint32_t currentOffset = __sync_fetch_and_or(&s->sliceOffset, 0);

        if (s->ctx.sliceOffset >= currentOffset)
        {
            RvRtpSessionLock(s);

            uint32_t sliceOffset = s->prod.St21GetNextSliceOffset(s->prod.appHandle, s->prodBuf, currentOffset, s->ctx.fieldId);
            if (sliceOffset == currentOffset)
            {
                RTE_LOG(INFO, USER2, "St21GetNextSliceOffset logical error of offset %u == %u\n", s->ctx.sliceOffset, currentOffset);
                s->state = ST_SN_STATE_NO_NEXT_SLICE;
            }
            s->sliceOffset = sliceOffset;

            RvRtpSessionUnlock(s);
        }
    }
    s->sn.pktsSend++;
    if (unlikely(rtp->marker == 1))
    {
        s->ctx.tmstamp = 0;//renew tmstamp at the next round
        s->ctx.line1Offset = 0;
        s->ctx.line1Number = 0;
        s->sn.frmsSend++;
        s->ctx.sliceOffset = 0;
		s->ctx.fieldId ^= 0x1;

        //critical section
        RvRtpSessionLock(s);
        s->sliceOffset = 0x0;
        s->prodBuf = s->prod.St21GetNextFrameBuf(s->prod.appHandle, s->prodBuf, s->sn.frameSize, s->ctx.fieldId);
        if (s->prodBuf == NULL)
        {
            s->state = ST_SN_STATE_NO_NEXT_FRAME;
        }
        else
        {
            uint32_t nextOffset = s->prod.St21GetNextSliceOffset(s->prod.appHandle, s->prodBuf, 0, s->ctx.fieldId);
            s->sliceOffset += nextOffset;
            if (nextOffset == 0)
            {
                RTE_LOG(INFO, USER2, "St21GetNextSliceOffset logical error of offset %u\n", nextOffset);
                s->state = ST_SN_STATE_NO_NEXT_SLICE;
            }
        }
        //unlock session and sliceOffset
        RvRtpSessionUnlock(s);
    }
    /* Return the original pointer for IP hdr */
    return hdr;
}
