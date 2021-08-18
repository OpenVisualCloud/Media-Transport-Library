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
  *	Implementation of the Ancillary Data transmittion
  *	compatible with SMPTE ST 2110-40, SMPTE ST 291-1 and RFC 8331
  */

#include "rvrtp_main.h"
#include "st_rtp.h"

st_rcv_stats_t rxThreadAncilStats[RTE_MAX_LCORE];

void *
RancRtpDummyBuildPacket(st_session_impl_t *s, void *hdr, struct rte_mbuf *m)
{
	return NULL;
}

st_status_t
RancRtpCreateRxSession(st_device_impl_t *dev, st_session_t *sin, st_format_t *fmt,
					   st_session_impl_t **sout)
{
	st_essence_type_t mtype;
	st40_format_t *ancfmt;

	if ((!dev) || (!sin) || (!fmt))
		return ST_INVALID_PARAM;

	mtype = fmt->mtype;
	if (mtype != ST_ESSENCE_ANC)
		return ST_INVALID_PARAM;

	ancfmt = &fmt->anc;

	int timeslot = RancRtpGetTimeslot(dev);
	if (timeslot < 0)
		return ST_SN_ERR_NO_TIMESLOT;

	st_session_impl_t *s = rte_malloc_socket("SessionAnc", sizeof(st_session_impl_t),
											 RTE_CACHE_LINE_SIZE, rte_socket_id());

	if (s)
	{
		memset(s, 0x0, sizeof(st_session_impl_t));
		RancRtpSetTimeslot(dev, timeslot, s);

		s->fmt = *fmt;
		s->dev = dev;
		s->sn = *sin;
		s->sn.timeslot = timeslot;
		s->sn.frameSize = s->fmt.anc.pktSize;
		s->sn.rtpProfile = RANCRTP_PAYLOAD_TYPE_ANCILLARY;

		switch (ancfmt->clockRate)
		{
		case 90000:	 //90kHz
			s->tmstampTime = 11111;
			break;
		default:
			return ST_FMT_ERR_BAD_CLK_RATE;
		}
		int maxAudioRcvThrds = (stMainParams.sn30Count == 0) ? 0 : stMainParams.maxAudioRcvThrds;
		for (uint32_t i = 0; i < stMainParams.maxAncRcvThrds; i++)
		{
			if ((stMainParams.ancRcvThrds[i].thrdSnFirst <= timeslot)
				&& (timeslot < stMainParams.ancRcvThrds[i].thrdSnLast))
			{
				s->tid = i + stMainParams.maxRcvThrds + maxAudioRcvThrds;
				break;
			}
		}

		s->UpdateRtpPkt = RancRtpDummyBuildPacket;
		s->RecvRtpPkt = RancRtpReceivePacketsRegular;

		s->state = ST_SN_STATE_ON;

		s->ancctx.payloadSize = s->fmt.anc.pktSize;
		s->anccons.bufSize = s->sn.frameSize;
		*sout = s;
		return ST_OK;
	}
	return ST_NO_MEMORY;
}

st_status_t
RancRtpDestroyRxSession(st_session_impl_t *s)
{
	if (!s)
		return ST_INVALID_PARAM;

	if(s->consBuf)
	{
		s->anccons.St40NotifyFrameDone(s->cons.appHandle, s->prodBuf);
	}
	s->consBufs[FRAME_PREV].buf = NULL;

	if(s->anccons.appHandle)
		RTE_LOG(WARNING, USER1, "App handler is not cleared!\n");

	rte_free(s);
	s = NULL;

	return ST_OK;
}

/*****************************************************************************************
 *
 * RancRtpHdrCheck
  *
 * DESCRIPTION
 * Check RFC 8331 RTP hdr in packet within session context
 *
 * RETURNS: st_status_t
 */
static inline st_status_t
RancRtpHdrCheck(st_session_impl_t *s, const st_rfc8331_anc_rtp_hdr_t *rtp)
{
	if ((rtp->version != RVRTP_VERSION_2) || (rtp->csrcCount != 0))
	{
		s->pktsDrop++;
		s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_HDR)]++;
		RTE_LOG(ERR, USER3, "Packet bad RTP HDR: pktsDrop %llu\n", (U64)s->pktsDrop);
		return ST_PKT_DROP_BAD_RTP_HDR;
	}

	if (rtp->payloadType != s->sn.rtpProfile)
	{
		s->pktsDrop++;
		s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_HDR)]++;
		RTE_LOG(ERR, USER3, "Packet bad profileType of %u pktsDrop %lu\n", 0, s->pktsDrop);
		return ST_PKT_DROP_BAD_RTP_HDR;
	}

	return ST_OK;
}

/*****************************************************************************************
 *
 * RancRtpReceiveFastCopyInline
 *
 * DESCRIPTION
 * Main function to copy packet's payload
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
static inline st_status_t
RancRtpReceiveFastCopyInline(st_session_impl_t *s, void *rtp)
{
	st_rfc8331_anc_rtp_hdr_t *rtpAnc = (st_rfc8331_anc_rtp_hdr_t *)rtp;
	uint8_t *payload = (uint8_t *)&rtpAnc[1];
	strtp_ancFrame_t *frame = (strtp_ancFrame_t *)(s->ancctx.data);
	st_anc_pkt_payload_hdr_t *pktBuff = (st_anc_pkt_payload_hdr_t *)(payload);
	pktBuff->swaped_first_hdr_chunk = ntohl(pktBuff->swaped_first_hdr_chunk);
	pktBuff->swaped_second_hdr_chunk = ntohl(pktBuff->swaped_second_hdr_chunk);
	int idx = frame->metaSize;
	frame->meta[idx].c = pktBuff->first_hdr_chunk.c;
	frame->meta[idx].lineNumber = pktBuff->first_hdr_chunk.lineNumber;
	frame->meta[idx].horiOffset = pktBuff->first_hdr_chunk.horizontalOffset;
	frame->meta[idx].s = pktBuff->first_hdr_chunk.s;
	frame->meta[idx].streamNum = pktBuff->first_hdr_chunk.streamNum;
	if (!St40CheckParityBits(pktBuff->second_hdr_chunk.did)
		|| !St40CheckParityBits(pktBuff->second_hdr_chunk.sdid)
		|| !St40CheckParityBits(pktBuff->second_hdr_chunk.dataCount))
	{
		RTE_LOG(ERR, USER3, "anc RTP checkParityBits error\n");
		return ST_PKT_DROP_BAD_RTP_HDR;
	}
	frame->meta[idx].did = pktBuff->second_hdr_chunk.did & 0xff;
	frame->meta[idx].sdid = pktBuff->second_hdr_chunk.sdid & 0xff;
	frame->meta[idx].udwSize = pktBuff->second_hdr_chunk.dataCount & 0xff;
	frame->meta[idx].udwOffset = 0;
	// verify checksum
	uint16_t checksum = 0;
	St40GetUDW(frame->meta[idx].udwSize, &checksum, (uint8_t *)&pktBuff->second_hdr_chunk);
	pktBuff->swaped_second_hdr_chunk = htonl(pktBuff->swaped_second_hdr_chunk);
	if (checksum
		!= St40CalcChecksum(3 + frame->meta[idx].udwSize, (uint8_t *)&pktBuff->second_hdr_chunk))
	{
		RTE_LOG(ERR, USER3, "anc frame checksum error\n");
		return ST_PKT_DROP_INCOMPL_FRAME;
	}
	// get payload
	pktBuff->swaped_second_hdr_chunk = ntohl(pktBuff->swaped_second_hdr_chunk);
	int i = 0;
	for (; i < idx; i++)
	{
		frame->meta[idx].udwOffset += frame->meta[i].udwSize;
	}
	i = 0;
	int offset = frame->meta[idx].udwOffset;
	if (frame->meta[idx].udwOffset + frame->meta[idx].udwSize > s->sn.frameSize)
	{
		return ST_NO_MEMORY;
	}
	frame->metaSize++;
	for (; i < frame->meta[idx].udwSize; i++)
	{
		uint16_t data;
		St40GetUDW(i, &data, (uint8_t *)&pktBuff->second_hdr_chunk);
		frame->data[offset++] = data & 0xff;
	}
	return ST_OK;
}

/*****************************************************************************************
 *
 * RancRtpReceivePacketsRegular
 *
 * DESCRIPTION
 * Main function to processes packets within session context
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RancRtpReceivePacketsRegular(st_session_impl_t *s, struct rte_mbuf *m)
{
	const struct rte_ether_hdr *ethHdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
	const struct rte_ipv4_hdr *ip
		= (struct rte_ipv4_hdr *)((char *)ethHdr + sizeof(struct rte_ether_hdr));
	const struct rte_udp_hdr *udp
		= (struct rte_udp_hdr *)((char *)ethHdr + sizeof(struct rte_ether_hdr)
								 + sizeof(struct rte_ipv4_hdr));
	const st_rfc8331_anc_rtp_hdr_t *rtp = (st_rfc8331_anc_rtp_hdr_t *)&udp[1];

	s->ancctx.data = NULL;	//pointer to right frameBuffer

	/* Validate the IP & UDP & RTP header */
	st_status_t res;
	if (((res = StRtpIpUdpHdrCheck(s, ip)) != ST_OK) || ((res = RancRtpHdrCheck(s, rtp)) != ST_OK))
	{
		return res;
	}
	if (!rtp->ancCount)
		return ST_OK;

	uint32_t rtpTmstamp = ntohl(rtp->tmstamp);

#ifdef ST_DONT_IGNORE_PKT_CHECK
	if (unlikely(rtpTmstamp == 0))
	{
		s->pktsDrop++;
		s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_TMSTAMP)]++;
		RTE_LOG(ERR, USER3, "Packet bad tmstamp of %u pktsDrop %llu\n", rtpTmstamp,
				(U64)s->pktsDrop);
		return ST_PKT_DROP_BAD_RTP_TMSTAMP;
	}
#endif

	if (rtpTmstamp == s->ancctx.tmstamp)  //tmstamp match most cases here
	{
		s->ancctx.data = s->consBuf;
		s->sn.pktsRecv++;
	}
	else if ((rtpTmstamp > s->ancctx.tmstamp)
			 || ((rtpTmstamp & (0x1 << 31))
				 < (s->ancctx.tmstamp & (0x1 << 31))))	// new frame comming
	{
		if (s->consBuf)
		{
			s->anccons.St40NotifyFrameDone(s->anccons.appHandle, s->consBuf);
		}
		s->consBuf = s->anccons.St40GetNextAncFrame(s->anccons.appHandle);
		if (unlikely(s->consBuf == NULL))
		{
			s->pktsDrop++;
			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_NO_FRAME_BUF)]++;
			return ST_PKT_DROP_NO_FRAME_BUF;
		}
		s->ancctx.data = s->consBuf;
		s->sn.pktsRecv++;
	}
	//out of order packet arrived - drop it silently, likely redundant path packet
	else if (s->ancctx.tmstamp >= rtpTmstamp)  //redundant path
	{
		s->pktsDrop++;
		s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_REDUNDANT_PATH)]++;
		return ST_PKT_DROP_REDUNDANT_PATH;
	}
	res = RancRtpReceiveFastCopyInline(s, (void *)rtp);
	if (unlikely(res != ST_OK))
	{
		if (res == ST_PKT_DROP_REDUNDANT_PATH)
		{
			s->pktsDrop++;
			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_REDUNDANT_PATH)]++;
		}
		return res;
	}
	s->ancctx.tmstamp = rtpTmstamp;
	return ST_OK;
}
