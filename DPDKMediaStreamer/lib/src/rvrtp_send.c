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

#include <rte_malloc.h>

#include "rvrtp_main.h"
#include "st_rtp.h"

#include <stdio.h>
#include <string.h>

void *StRtpBuildIpHeader(st_session_impl_t *s, struct rte_ipv4_hdr *ip, uint32_t portId);
void *StRtpUpdateIpHeader(st_session_impl_t *s, struct rte_ipv4_hdr *ip);
void *StRtpBuildUdpHeader(st_session_impl_t *s, struct rte_udp_hdr *udp, uint32_t portId);
static inline void *RvRtpBuildDualLinePacket(st_session_impl_t *s, void *hdr);
static inline void *RvRtpBuildSingleLinePacket(st_session_impl_t *s, void *hdr);

extern int StGetExtIndex(st_session_t *sn, uint8_t *addr);
//#define TX_RINGS_DEBUG
//#define ST_MULTICAST_TEST

st_device_impl_t stSendDevice;

st_status_t
RvRtpDummyRecvPacket(st_session_impl_t *s, struct rte_mbuf *rxbuf)
{
	return ST_OK;
}

st_status_t
RvRtpValidateFormat(st21_format_t *fmt)
{
	switch (fmt->pixelFmt)
	{
	case ST21_PIX_FMT_RGB_8BIT:
	case ST21_PIX_FMT_RGB_10BIT_BE:
	case ST21_PIX_FMT_RGB_10BIT_LE:
	case ST21_PIX_FMT_RGB_12BIT_BE:
	case ST21_PIX_FMT_RGB_12BIT_LE:
	case ST21_PIX_FMT_BGR_8BIT:
	case ST21_PIX_FMT_BGR_10BIT_BE:
	case ST21_PIX_FMT_BGR_10BIT_LE:
	case ST21_PIX_FMT_BGR_12BIT_BE:
	case ST21_PIX_FMT_BGR_12BIT_LE:
	case ST21_PIX_FMT_YCBCR_420_8BIT:
	case ST21_PIX_FMT_YCBCR_420_10BIT_BE:
	case ST21_PIX_FMT_YCBCR_420_10BIT_LE:
	case ST21_PIX_FMT_YCBCR_420_12BIT_BE:
	case ST21_PIX_FMT_YCBCR_420_12BIT_LE:
	case ST21_PIX_FMT_YCBCR_422_8BIT:
	case ST21_PIX_FMT_YCBCR_422_10BIT_BE:
	case ST21_PIX_FMT_YCBCR_422_10BIT_LE:
	case ST21_PIX_FMT_YCBCR_422_12BIT_BE:
	case ST21_PIX_FMT_YCBCR_422_12BIT_LE:
		break;
	default:
		return ST_INVALID_PARAM;
	}
	return ST_OK;
}

int32_t
RvRtpGetTrOffsetTimeslot(st_device_impl_t *dev, uint32_t pktTime, uint32_t tprs, uint32_t *timeslot)
{
	if ((tprs < pktTime) || (dev->snCount == dev->dev.maxSt21Sessions))
		return -1;

	if (dev->timeQuot == 0)
	{
		//initialize with the 1st TPRS
		dev->timeQuot = tprs;  // *dev->dev.rateGbps;
		dev->timeTable[0] = pktTime;
		*timeslot = 0;

		RTE_LOG(DEBUG, USER2, "RvRtpGetTimeslot devQuot %u tprs %u\n", dev->timeQuot, tprs);

		dev->lastAllocSn = 0;
		return 0;
	}

	uint32_t usedTimeQuot = 0;

	for (uint32_t i = 0; i < dev->snCount; i++)
	{
		usedTimeQuot += dev->timeTable[i];
	}
	RTE_LOG(DEBUG, USER2, "RvRtpGetTimeslot usedTimeQuot %u\n", usedTimeQuot);

	if (dev->timeQuot >= usedTimeQuot + pktTime)
	{
		dev->timeTable[dev->snCount] = pktTime;
		RTE_LOG(DEBUG, USER2, "RvRtpGetTimeslot pktTime %u usedTimeQuot %u\n", pktTime,
				usedTimeQuot);
		*timeslot = dev->snCount;
		return usedTimeQuot;
	}

	RTE_LOG(ERR, USER2, "RvRtpGetTimeslot failed since pktTime %u + usedTimeQuot %u > quot of %u\n",
			pktTime, usedTimeQuot, dev->timeQuot);
	return (-1);
}

extern st_main_params_t stMainParams;

void
RvRtpInitPacketCtx(st_session_impl_t *s, uint32_t ring)
{
	s->vctx.tmstampOddInc
		= (uint32_t)(((uint64_t)s->fmt.v.clockRate * (uint64_t)s->fmt.v.frmRateDen)
					 / (uint64_t)s->fmt.v.frmRateMul);
	s->vctx.tmstampEvenInc = s->vctx.tmstampOddInc;
	if ((s->vctx.tmstampOddInc & 0x3) == 1)
	{
		s->vctx.tmstampEvenInc++;
	}
	s->vctx.alignTmstamp = 0;
	s->vctx.line1PixelGrpSize = s->fmt.v.pixelGrpSize;
	s->vctx.line1Offset = 0;
	s->vctx.line1Number = 0;
	s->vctx.line1Length = s->fmt.v.pixelsInPkt / s->fmt.v.pixelsInGrp * s->vctx.line1PixelGrpSize;
	s->vctx.line1Size = s->fmt.v.width / s->fmt.v.pixelsInGrp * s->vctx.line1PixelGrpSize;

	struct rte_ipv4_hdr *ip;
	struct rte_udp_hdr *udp;

	switch (s->fmt.v.pktFmt)
	{
	case ST_INTEL_DLN_RFC4175_PKT:
		s->vctx.line2Offset = 0;
		s->vctx.line2Number = 1;
		s->vctx.line2PixelGrpSize = s->fmt.v.pixelGrpSize;
		s->vctx.line2Size = s->fmt.v.width / s->fmt.v.pixelsInGrp * s->vctx.line2PixelGrpSize;
		s->vctx.line2Length
			= s->fmt.v.pixelsInPkt / s->fmt.v.pixelsInGrp * s->vctx.line2PixelGrpSize;
		ip = (struct rte_ipv4_hdr *)StRtpBuildL2Packet(s, &s->hdrPrint[ST_PPORT].dualHdr.eth, ST_PPORT);
		udp = (struct rte_udp_hdr *)StRtpBuildIpHeader(s, ip, ST_PPORT);
		st_rfc4175_rtp_dual_hdr_t *rtp_dual;
		{
			rtp_dual = (st_rfc4175_rtp_dual_hdr_t *)StRtpBuildUdpHeader(s, udp, ST_PPORT);
			RvRtpBuildDualLinePacket(s, rtp_dual);
		}
		if (s->sn.caps & ST_SN_DUAL_PATH && stMainParams.numPorts > 1)
		{
			ip = (struct rte_ipv4_hdr *)StRtpBuildL2Packet(s, &s->hdrPrint[ST_RPORT].dualHdr.eth,
														   ST_RPORT);
			udp = (struct rte_udp_hdr *)StRtpBuildIpHeader(s, ip, ST_RPORT);
			rtp_dual = (st_rfc4175_rtp_dual_hdr_t *)StRtpBuildUdpHeader(s, udp, ST_RPORT);

		}
		break;

	case ST_INTEL_SLN_RFC4175_PKT:
		ip = (struct rte_ipv4_hdr *)StRtpBuildL2Packet(s, &s->hdrPrint[ST_PPORT].singleHdr.eth, ST_PPORT);
		udp = (struct rte_udp_hdr *)StRtpBuildIpHeader(s, ip, ST_PPORT);
		st_rfc4175_rtp_single_hdr_t *rtp_sln;
		{
			rtp_sln = (st_rfc4175_rtp_single_hdr_t *)StRtpBuildUdpHeader(s, udp, ST_PPORT);
			RvRtpBuildSingleLinePacket(s, rtp_sln);
		}
		if (s->sn.caps & ST_SN_DUAL_PATH && stMainParams.numPorts > 1)
		{
			ip = (struct rte_ipv4_hdr *)StRtpBuildL2Packet(s, &s->hdrPrint[ST_RPORT].singleHdr.eth,
														   ST_RPORT);
			udp = (struct rte_udp_hdr *)StRtpBuildIpHeader(s, ip, ST_RPORT);
			rtp_sln = (st_rfc4175_rtp_single_hdr_t *)StRtpBuildUdpHeader(s, udp, ST_RPORT);
		}
		break;
	default:
		break;
	}

	RTE_LOG(DEBUG, USER2, "RvRtpInitPacketCtx line1Length %u line2Length %u\n", s->vctx.line1Length,
			s->vctx.line2Length);

	s->sn.pktsRecv = 0;
	s->sn.pktsSend = 0;
	s->sn.frmsRecv = 0;
	s->sn.frmsSend = 0;
	memset(s->sn.frmsDrop, 0, sizeof(s->sn.frmsDrop));
	memset(s->sn.pktsDrop, 0, sizeof(s->sn.pktsDrop));
}

st_status_t
RvRtpCreateTxSession(st_device_impl_t *dev, st_session_t *sin, st_format_t *fmt,
					 st_session_impl_t **sout)
{
	st_essence_type_t mtype;
	uint32_t tmstampTime;
	st21_format_t *vfmt;
	st_status_t status;

	if ((!dev) || (!sin) || (!fmt))
		return ST_INVALID_PARAM;

	mtype = fmt->mtype;

	/* This method just handle video */
	if (mtype != ST_ESSENCE_VIDEO)
		return ST_INVALID_PARAM;

	vfmt = &fmt->v;

	status = RvRtpValidateFormat(vfmt);
	if (status != ST_OK)
	{
		return status;
	}

	st_session_t sn = *sin;

	sn.trOffset = (uint32_t)((uint64_t)(vfmt->frameTime * vfmt->trOffsetLines)
							 / (uint64_t)vfmt->totalLines);
	sn.frameSize = ((uint64_t)vfmt->width * vfmt->height * vfmt->pixelGrpSize) / vfmt->pixelsInGrp;

	uint32_t pktsInGappedMode = vfmt->pktsInLine * vfmt->totalLines;

	if (vfmt->pktFmt == ST_INTEL_DLN_RFC4175_PKT)
	{
		pktsInGappedMode /= 2;	//have 8 pkts in line so need to adjust
	}
	if ((vfmt->vscan == ST21_2160I) || (vfmt->vscan == ST21_1080I) || (vfmt->vscan == ST21_720I))
	{
		pktsInGappedMode /= 2;	//have a half packets to send
	}
	else if ((vfmt->vscan != ST21_2160P) && (vfmt->vscan != ST21_1080P)
			 && (vfmt->vscan != ST21_720P))
	{
		return ST_FMT_ERR_BAD_VSCAN;
	}

	switch (dev->dev.pacerType)
	{
	case ST_2110_21_TPN:
		sn.tprs = (uint32_t)(vfmt->frameTime / pktsInGappedMode);
		break;
	case ST_2110_21_TPNL:
	case ST_2110_21_TPW:
		sn.tprs = (uint32_t)(vfmt->frameTime / vfmt->pktsInFrame);
		break;
	default:
		return ST_DEV_BAD_PACING;
	}

	switch (vfmt->clockRate)
	{
	case 90000:	 //90kHz
		tmstampTime = 11111;
		break;
	default:
		return ST_FMT_ERR_BAD_CLK_RATE;
	}

	sn.pktTime = ((vfmt->pktSize + ST_PHYS_PKT_ADD) * 8) / dev->dev.rateGbps;
	uint32_t remaind = ((vfmt->pktSize + ST_PHYS_PKT_ADD) * 8) % dev->dev.rateGbps;
	if (remaind >= (dev->dev.rateGbps / 2))
		sn.pktTime++;

	int32_t trOffset = RvRtpGetTrOffsetTimeslot(dev, sn.pktTime, sn.tprs, &sn.timeslot);
	if (trOffset < 0)
	{
		RTE_LOG(INFO, USER1, "failed RvRtpGetTrOffsetTimeslot %u %u\n", sn.pktTime, sn.tprs);

		return ST_SN_ERR_NO_TIMESLOT;  //impossible to find timeslot for the producer
	}
	sn.trOffset += trOffset;

	RTE_LOG(DEBUG, USER1, "RvRtpGetTrOffsetTimeslot troffste %u timeslot %u\n", sn.trOffset,
			sn.timeslot);

	st_session_impl_t *s = rte_malloc_socket("Session", sizeof(st_session_impl_t),
											 RTE_CACHE_LINE_SIZE, rte_socket_id());
	if (s)
	{
		memset(s, 0x0, sizeof(st_session_impl_t));

		s->fmt = *fmt;
		s->dev = dev;
		s->sn = sn;
		s->tmstampTime = tmstampTime;

		//have a half lines to send in interlaced cases
		switch (vfmt->vscan)
		{
		case ST21_2160I:
		case ST21_1080I:
		case ST21_720I:
			s->vctx.fieldId = 0;
			s->UpdateRtpPkt = RvRtpUpdateInterlacedPacket;
			break;
		default:
			switch (s->fmt.v.pktFmt)
			{
			case ST_INTEL_DLN_RFC4175_PKT:
				s->UpdateRtpPkt = RvRtpUpdateDualLinePacket;
				break;
			case ST_INTEL_SLN_RFC4175_PKT:
				s->UpdateRtpPkt = RvRtpUpdateSingleLinePacket;
				break;
			default:
				RTE_LOG(ERR, USER2, "Not supported format on transmitter\n");
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

st_status_t
RvRtpDestroyTxSession(st_session_impl_t *s)
{
	if (!s)
		return ST_INVALID_PARAM;

	if(s->cons.appHandle)
		RTE_LOG(WARNING, USER1, "App handler is not cleared!\n");

	rte_free(s);
	s = NULL;

	return ST_OK;
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
RvRtpSessionCheckRunState(st_session_impl_t *s)
{
	if(s == NULL)
		return ST_SN_STATE_STOP_PENDING;
	uint32_t tmstamp = 0;

	// lock session
	StSessionLock(s);

	if (s->state != ST_SN_STATE_RUN)  // state is protected by the lock as well as prodBuf
	{
		if (s->state == ST_SN_STATE_NO_NEXT_FRAME)
		{
			uint8_t *new_prod_buf = s->prod.St21GetNextFrameBuf(s->prod.appHandle, s->prodBuf,
																s->prod.frameSize, &tmstamp, s->vctx.fieldId);
			if (new_prod_buf != NULL)
			{
				s->prodBuf = new_prod_buf;
				s->state = ST_SN_STATE_RUN;
				s->vctx.sliceOffset = 0;
				if (stMainParams.userTmstamp)
				{
					s->vctx.tmstamp = tmstamp;
				}
				s->sliceOffset = s->prod.St21GetNextSliceOffset(s->prod.appHandle, s->prodBuf, 0,
																s->vctx.fieldId);
			}
		}
		else if (s->state == ST_SN_STATE_NO_NEXT_SLICE)
		{
			uint32_t nextOffset = s->prod.St21GetNextSliceOffset(s->prod.appHandle, s->prodBuf,
																 s->sliceOffset, s->vctx.fieldId);
			if (nextOffset > s->sliceOffset)
			{
				s->sliceOffset = nextOffset;
				s->state = ST_SN_STATE_RUN;
			}
		}
	}

	//leave critical section
	StSessionUnlock(s);

	return (s->state == ST_SN_STATE_RUN);
}

/*****************************************************************************
 *
 * StRtpBuildIpHeader -IP header constructor routine.
 *
 * DESCRIPTION
 * Constructs the IP header
 *
 * RETURNS: UDP header location
 */
void *
StRtpBuildIpHeader(st_session_impl_t *s, struct rte_ipv4_hdr *ip, uint32_t portId)
{
	uint16_t tlen;

	/* Zero out the header space */
	memset((char *)ip, 0, sizeof(struct rte_ipv4_hdr));

	ip->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);

	ip->time_to_live = 64;
	ip->type_of_service = s->fl[portId].tos;

#define IP_DONT_FRAGMENT_FLAG 0x0040
	ip->fragment_offset = IP_DONT_FRAGMENT_FLAG;

	tlen = StSessionGetPktsize(s) - s->etherSize;

	ip->total_length = htons(tlen);
	ip->next_proto_id = 17;
	ip->src_addr = s->fl[portId].src.addr4.sin_addr.s_addr;
	ip->dst_addr = s->fl[portId].dst.addr4.sin_addr.s_addr;

	return &ip[1];
}

/*****************************************************************************
 *
 * StRtpUpdateIpHeader -IP header constructor routine.
 *
 * DESCRIPTION
 * Constructs the IP header
 *
 * RETURNS: UDP header location
 */
void *
StRtpUpdateIpHeader(st_session_impl_t *s, struct rte_ipv4_hdr *ip)
{
#ifdef ST_LATE_SN_CONNECT
	ip->src_addr = s->fl[0].src.addr4.sin_addr.s_addr;
	ip->dst_addr = s->fl[0].dst.addr4.sin_addr.s_addr;
#endif

	if (s->sn.type == ST_ESSENCE_VIDEO || s->sn.type == ST_ESSENCE_AUDIO)
	{
		ip->packet_id = htons(s->vctx.ipPacketId++);
	}
	else if (s->sn.type == ST_ESSENCE_ANC)
	{
		ip->packet_id = htons(s->vctx.ipPacketId++);
		uint16_t tlen = StSessionGetPktsize(s) - s->etherSize;
		ip->total_length = htons(tlen);
	}

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
 * StRtpBuildUdpHeader -UDP header constructor routine.
 *
 * DESCRIPTION
 * Constructs the UDP header
 *
 * RETURNS: RTP header location
 */
void *
StRtpBuildUdpHeader(st_session_impl_t *s, struct rte_udp_hdr *udp, uint32_t portid)
{
	uint16_t tlen;
	tlen = StSessionGetPktsize(s) - (s->etherSize + sizeof(struct rte_ipv4_hdr));
	udp->dgram_len = htons(tlen);
	udp->src_port = s->fl[portid].src.addr4.sin_port;
	udp->dst_port = s->fl[portid].dst.addr4.sin_port;
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
RvRtpBuildDualLinePacket(st_session_impl_t *s, void *hdr)
{
	/* Create the IP & UDP header */
	struct st_rfc4175_rtp_dual_hdr *rtp = (struct st_rfc4175_rtp_dual_hdr *)hdr;

	rtp->version = RVRTP_VERSION_2;
	rtp->padding = 0;
	rtp->marker = 0;
	rtp->csrcCount = 0;
	rtp->payloadType = RVRTP_PAYLOAD_TYPE_RAW_VIDEO;

	rtp->ssrc = htonl(s->sn.ssid);

	rtp->line1Length = htons(s->vctx.line1Length);
	rtp->line2Length = htons(s->vctx.line2Length);
	rtp->line1Number = htons(s->vctx.line1Number);
	rtp->line2Number = htons(s->vctx.line2Number);
	rtp->line1Offset = htons(s->vctx.line1Offset);
	rtp->line2Offset = htons(s->vctx.line2Offset);

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
RvRtpBuildSingleLinePacket(st_session_impl_t *s, void *hdr)
{
	/* Create the IP & UDP header */
	struct st_rfc4175_rtp_single_hdr *rtp = (struct st_rfc4175_rtp_single_hdr *)hdr;

	rtp->version = RVRTP_VERSION_2;
	rtp->padding = 0;
	rtp->marker = 0;
	rtp->csrcCount = 0;
	rtp->payloadType = RVRTP_PAYLOAD_TYPE_RAW_VIDEO;
	rtp->ssrc = htonl(s->sn.ssid);
	rtp->line1Length = htons(s->vctx.line1Length);
	rtp->line1Number = htons(s->vctx.line1Number);
	rtp->line1Offset = htons(s->vctx.line1Offset);
	return hdr;
}

/*****************************************************************************
 *
 * StRtpBuildL2Packet - L2 header constructor routine.
 *
 * DESCRIPTION
 * Constructs the l2 packet
 *
 * RETURNS: IP header location
 *
 * SEE ALSO:
 */
void *
StRtpBuildL2Packet(st_session_impl_t *s, struct rte_ether_hdr *l2, uint32_t portId)
{
	struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)&l2[1];

	l2->ether_type = htons(0x0800);
	memcpy(&l2->d_addr, s->fl[portId].dstMac, ETH_ADDR_LEN);
	memcpy(&l2->s_addr, s->fl[portId].srcMac, ETH_ADDR_LEN);
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
RvRtpUpdateDualLinePacket(st_session_impl_t *s, void *hdr, struct rte_mbuf *extMbuf)
{
	/* Create the IP & UDP header */
	struct rte_ipv4_hdr *ip = hdr;
	struct rte_udp_hdr *udp = (struct rte_udp_hdr *)StRtpUpdateIpHeader(s, ip);
	uint32_t tmstamp = 0;

	struct st_rfc4175_rtp_dual_hdr *rtp = (struct st_rfc4175_rtp_dual_hdr *)&udp[1];

	/* is frame complete ?*/
	if (unlikely(((s->vctx.line2Number + 1) == s->fmt.v.height)
				 && (s->vctx.line2Offset >= s->fmt.v.width - s->fmt.v.pixelsInPkt)))
	{
		rtp->marker = 1;
	}
	else
	{
		rtp->marker = 0;
	}
	rtp->seqNumber = htons((uint16_t)s->vctx.seqNumber.lohi.seqLo);
	rtp->seqNumberExt = htons((uint16_t)s->vctx.seqNumber.lohi.seqHi);
	rtp->tmstamp = htonl(s->vctx.tmstamp);
	rtp->line1Number = htons(s->vctx.line1Number);
	rtp->line2Number = htons(s->vctx.line2Number);
	rtp->line1Offset = htons(s->vctx.line1Offset | 0x8000);	 //set line1Continue bit
	rtp->line2Offset = htons(s->vctx.line2Offset);

	/* copy payload */
	uint8_t *payload = (uint8_t *)&rtp[1];

	uint32_t byteLn1Offset
		= s->vctx.line1Number * s->vctx.line1Size
		  + (uint32_t)s->vctx.line1Offset / s->fmt.v.pixelsInGrp * s->vctx.line1PixelGrpSize;
	uint32_t byteLn2Offset
		= s->vctx.line2Number * s->vctx.line2Size
		  + (uint32_t)s->vctx.line2Offset / s->fmt.v.pixelsInGrp * s->vctx.line2PixelGrpSize;

	/* line 0 */
	memcpy(payload, &s->prodBuf[byteLn1Offset], s->vctx.line1Length);
	/* line 1 */
	memcpy(&payload[s->vctx.line1Length], &s->prodBuf[byteLn2Offset], s->vctx.line2Length);

	extMbuf->data_len = extMbuf->pkt_len = 0;  // Not used
	// UDP checksum
	udp->dgram_cksum = 0;
	if (unlikely((s->ofldFlags & ST_OFLD_HW_UDP_CKSUM) != ST_OFLD_HW_UDP_CKSUM))
	{
		udp->dgram_cksum = rte_ipv4_udptcp_cksum(ip, (const void *)udp);
		if (udp->dgram_cksum == 0)
			udp->dgram_cksum = 0xFFFF;
	}

	//iterate to next packet
	s->vctx.line1Offset += s->fmt.v.pixelsInPkt;
	s->vctx.line2Offset += s->fmt.v.pixelsInPkt;
	s->vctx.seqNumber.sequence++;
	if ((rtp->marker == 0) && (s->vctx.line2Offset >= s->fmt.v.width))
	{
		s->vctx.line1Offset = 0;
		s->vctx.line2Offset = 0;
		s->vctx.line1Number += 2;
		s->vctx.line2Number += 2;
		s->vctx.sliceOffset = byteLn2Offset + s->vctx.line2Length;
		s->vctx.alignTmstamp = 1;

		uint32_t currentOffset = __sync_fetch_and_or(&s->sliceOffset, 0);

		if (s->vctx.sliceOffset >= currentOffset)
		{
			StSessionLock(s);

			uint32_t sliceOffset = s->prod.St21GetNextSliceOffset(s->prod.appHandle, s->prodBuf,
																  currentOffset, s->vctx.fieldId);
			if (sliceOffset == currentOffset)
			{
				RTE_LOG(ERR, USER2, "St21GetNextSliceOffset logical error of offset %u == %u\n",
						s->vctx.sliceOffset, currentOffset);
				s->state = ST_SN_STATE_NO_NEXT_SLICE;
			}
			s->sliceOffset = sliceOffset;

			StSessionUnlock(s);
		}
	}
	s->sn.pktsSend++;
	if (unlikely(rtp->marker == 1))
	{
		s->sn.frmsSend++;
		s->vctx.tmstamp = 0;  //renew tmstamp at the next round
		s->vctx.line1Offset = 0;
		s->vctx.line2Offset = 0;
		s->vctx.line1Number = 0;
		s->vctx.line2Number = 1;

		s->vctx.sliceOffset = 0;

		//critical section
		StSessionLock(s);

		s->sliceOffset = 0x0;

		uint8_t *new_prod_buf = s->prod.St21GetNextFrameBuf(s->prod.appHandle, s->prodBuf,
															s->sn.frameSize, &tmstamp, s->vctx.fieldId);
		if (new_prod_buf == NULL)
		{
			s->state = ST_SN_STATE_NO_NEXT_FRAME;
		}
		else
		{
			uint32_t nextOffset;
			s->prodBuf = new_prod_buf;

			nextOffset
				= s->prod.St21GetNextSliceOffset(s->prod.appHandle, s->prodBuf, 0, s->vctx.fieldId);
			s->sliceOffset += nextOffset;
			if (nextOffset == 0)
			{
				RTE_LOG(ERR, USER2, "St21GetNextSliceOffset logical error of offset %u\n",
						nextOffset);
				s->state = ST_SN_STATE_NO_NEXT_SLICE;
			}
			if (stMainParams.userTmstamp)
				s->vctx.usertmstamp = tmstamp;
		}
		//unlock session and sliceOffset
		StSessionUnlock(s);
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
RvRtpUpdateSingleLinePacket(st_session_impl_t *s, void *hdr, struct rte_mbuf *extMbuf)
{
	/* Create the IP & UDP header */
	struct rte_ipv4_hdr *ip = hdr;
	struct rte_udp_hdr *udp = (struct rte_udp_hdr *)StRtpUpdateIpHeader(s, ip);
	uint32_t tmstamp = 0;

	struct st_rfc4175_rtp_single_hdr *rtp = (struct st_rfc4175_rtp_single_hdr *)&udp[1];

	/* is frame complete ?*/
	if (unlikely(((s->vctx.line1Number + 1) == s->fmt.v.height)
				 && (s->vctx.line1Offset >= s->fmt.v.width - s->fmt.v.pixelsInPkt)))
	{
		rtp->marker = 1;
	}
	else
	{
		rtp->marker = 0;
	}
	rtp->seqNumber = htons((uint16_t)s->vctx.seqNumber.lohi.seqLo);
	rtp->seqNumberExt = htons((uint16_t)s->vctx.seqNumber.lohi.seqHi);

	rtp->tmstamp = htonl(s->vctx.tmstamp);
	rtp->line1Number = htons(s->vctx.line1Number);
	rtp->line1Offset = htons(s->vctx.line1Offset);
	uint32_t lengthLeft
		= MIN(s->vctx.line1Length, s->vctx.line1Size
									   - (uint32_t)s->vctx.line1Offset / s->fmt.v.pixelsInGrp
											 * s->vctx.line1PixelGrpSize);
	rtp->line1Length = htons(lengthLeft);

	/* copy payload */
	uint32_t byteLn1Offset
		= s->vctx.line1Number * s->vctx.line1Size
		  + (uint32_t)s->vctx.line1Offset / s->fmt.v.pixelsInGrp * s->vctx.line1PixelGrpSize;

	/* line 1 */
	int idx = StGetExtIndex(&s->sn, s->prodBuf);
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
	s->vctx.line1Offset += s->fmt.v.pixelsInPkt;
	s->vctx.seqNumber.sequence++;
	if ((rtp->marker == 0) && (s->vctx.line1Offset >= s->fmt.v.width))
	{
		s->vctx.line1Offset = 0;
		s->vctx.line1Number++;
		s->vctx.alignTmstamp = (s->vctx.line1Number & 0x1);
		s->vctx.sliceOffset = byteLn1Offset + s->vctx.line1Length;

		uint32_t currentOffset = __sync_fetch_and_or(&s->sliceOffset, 0);

		if (s->vctx.sliceOffset >= currentOffset)
		{
			StSessionLock(s);

			uint32_t sliceOffset = s->prod.St21GetNextSliceOffset(s->prod.appHandle, s->prodBuf,
																  currentOffset, s->vctx.fieldId);
			if (sliceOffset == currentOffset)
			{
				RTE_LOG(ERR, USER2, "St21GetNextSliceOffset logical error of offset %u == %u\n",
						s->vctx.sliceOffset, currentOffset);
				s->state = ST_SN_STATE_NO_NEXT_SLICE;
			}
			s->sliceOffset = sliceOffset;

			StSessionUnlock(s);
		}
	}
	s->sn.pktsSend++;
	if (unlikely(rtp->marker == 1))
	{
		s->sn.frmsSend++;
		s->vctx.tmstamp = 0;  //renew tmstamp at the next round
		s->vctx.line1Offset = 0;
		s->vctx.line1Number = 0;
		s->vctx.sliceOffset = 0;

		//critical section
		StSessionLock(s);

		s->sliceOffset = 0x0;

		uint8_t *new_prod_buf = s->prod.St21GetNextFrameBuf(s->prod.appHandle, s->prodBuf,
															s->sn.frameSize, &tmstamp, s->vctx.fieldId);
		if (new_prod_buf == NULL)
		{
			s->state = ST_SN_STATE_NO_NEXT_FRAME;
		}
		else
		{
			uint32_t nextOffset;
			s->prodBuf = new_prod_buf;

			nextOffset
				= s->prod.St21GetNextSliceOffset(s->prod.appHandle, s->prodBuf, 0, s->vctx.fieldId);
			s->sliceOffset += nextOffset;
			if (nextOffset == 0)
			{
				RTE_LOG(ERR, USER2, "St21GetNextSliceOffset logical error of offset %u\n",
						nextOffset);
				s->state = ST_SN_STATE_NO_NEXT_SLICE;
			}
			if (stMainParams.userTmstamp)
				s->vctx.usertmstamp = tmstamp;
		}
		
		//unlock session and sliceOffset
		StSessionUnlock(s);
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
RvRtpUpdateInterlacedPacket(st_session_impl_t *s, void *hdr, struct rte_mbuf *extMbuf)
{
	/* Create the IP & UDP header */
	struct rte_ipv4_hdr *ip = hdr;
	struct rte_udp_hdr *udp = (struct rte_udp_hdr *)StRtpUpdateIpHeader(s, ip);
	uint32_t tmstamp = 0;


	struct st_rfc4175_rtp_single_hdr *rtp = (struct st_rfc4175_rtp_single_hdr *)&udp[1];

	/* is frame complete ?*/
	if (unlikely(((s->vctx.line1Number + 1) == s->fmt.v.height / 2)
				 && (s->vctx.line1Offset >= s->fmt.v.width - s->fmt.v.pixelsInPkt)))
	{
		rtp->marker = 1;
	}
	else
	{
		rtp->marker = 0;
	}
	rtp->seqNumber = htons((uint16_t)s->vctx.seqNumber.lohi.seqLo);
	rtp->seqNumberExt = htons((uint16_t)s->vctx.seqNumber.lohi.seqHi);

	rtp->tmstamp = htonl(s->vctx.tmstamp);
	rtp->line1Number
		= htons(s->vctx.line1Number | (s->vctx.fieldId << 15));	 //set Field value and line number;
	rtp->line1Offset = htons(s->vctx.line1Offset);
	uint32_t lengthLeft
		= MIN(s->vctx.line1Length, s->vctx.line1Size
									   - (uint32_t)s->vctx.line1Offset / s->fmt.v.pixelsInGrp
											 * s->vctx.line1PixelGrpSize);

	/* copy payload */
	uint32_t byteLn1Offset
		= (s->vctx.line1Number * 2 + s->vctx.fieldId) * s->vctx.line1Size
		  + (uint32_t)s->vctx.line1Offset / s->fmt.v.pixelsInGrp * s->vctx.line1PixelGrpSize;

	/* line 1 */
	int idx = StGetExtIndex(&s->sn, s->prodBuf);
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
	s->vctx.line1Offset += s->fmt.v.pixelsInPkt;
	s->vctx.seqNumber.sequence++;
	if ((rtp->marker == 0) && (s->vctx.line1Offset >= s->fmt.v.width))
	{
		s->vctx.line1Offset = 0;
		s->vctx.line1Number++;
		s->vctx.alignTmstamp = (s->vctx.line1Number & 0x1);
		s->vctx.sliceOffset = byteLn1Offset + s->vctx.line1Length;

		uint32_t currentOffset = __sync_fetch_and_or(&s->sliceOffset, 0);

		if (s->vctx.sliceOffset >= currentOffset)
		{
			StSessionLock(s);

			uint32_t sliceOffset = s->prod.St21GetNextSliceOffset(s->prod.appHandle, s->prodBuf,
																  currentOffset, s->vctx.fieldId);
			if (sliceOffset == currentOffset)
			{
				RTE_LOG(ERR, USER2, "St21GetNextSliceOffset logical error of offset %u == %u\n",
						s->vctx.sliceOffset, currentOffset);
				s->state = ST_SN_STATE_NO_NEXT_SLICE;
			}
			s->sliceOffset = sliceOffset;

			StSessionUnlock(s);
		}
	}
	s->sn.pktsSend++;
	if (unlikely(rtp->marker == 1))
	{
		s->vctx.tmstamp = 0;  //renew tmstamp at the next round
		s->vctx.line1Offset = 0;
		s->vctx.line1Number = 0;
		s->sn.frmsSend++;
		s->vctx.sliceOffset = 0;
		s->vctx.fieldId ^= 0x1;

		//critical section
		StSessionLock(s);
		s->sliceOffset = 0x0;
		uint8_t *temp = s->prod.St21GetNextFrameBuf(s->prod.appHandle, s->prodBuf, s->sn.frameSize,
													&tmstamp, s->vctx.fieldId);
		if (temp == NULL)
		{
			s->state = ST_SN_STATE_NO_NEXT_FRAME;
		}
		else
		{
			uint32_t nextOffset;
			s->prodBuf = temp;
			nextOffset
				= s->prod.St21GetNextSliceOffset(s->prod.appHandle, s->prodBuf, 0, s->vctx.fieldId);
			s->sliceOffset += nextOffset;
			if (nextOffset == 0)
			{
				RTE_LOG(ERR, USER2, "St21GetNextSliceOffset logical error of offset %u\n",
						nextOffset);
				s->state = ST_SN_STATE_NO_NEXT_SLICE;
			}
			if (stMainParams.userTmstamp)
				s->vctx.usertmstamp = tmstamp;
		}

		//unlock session and sliceOffset
		StSessionUnlock(s);
	}
	/* Return the original pointer for IP hdr */
	return hdr;
}

static st_session_method_t rvrtp_method = {
	.create_tx_session = RvRtpCreateTxSession,
	.create_rx_session = RvRtpCreateRxSession,
	.destroy_tx_session = RvRtpDestroyTxSession,
	.destroy_rx_session = RvRtpDestroyRxSession,
	.init_packet_ctx = RvRtpInitPacketCtx,
};

void
rvrtp_method_init()
{
	st_init_session_method(&rvrtp_method, ST_ESSENCE_VIDEO);
}
