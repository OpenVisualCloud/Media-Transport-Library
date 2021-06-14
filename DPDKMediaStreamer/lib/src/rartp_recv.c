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

#include "dpdk_common.h"
#include "rvrtp_main.h"
#include "st_flw_cls.h"
#include "st_rtp.h"

//#define ST_RECV_TIME_PRINT
//#define ST_MEMCPY_TEST
//#define ST_MULTICAST_TEST

// #define RARTP_RCV_DEBUG

extern st_device_impl_t stRecvDevice;
volatile uint32_t prevSeqNumber = 0;

void *
RaRtpDummyBuildPacket(st_session_impl_t *s, void *hdr, struct rte_mbuf *m)
{
	return NULL;
}

st_status_t
RaRtpFreeRxSession(st_session_impl_t *s)
{
	if (s)
	{
		rte_free(s);
	}
	return ST_OK;
}

st_status_t
RaRtpCreateRxSession(st_device_impl_t *dev, st_session_t *sin, st_format_t *fmt,
					 st_session_impl_t **sout)
{
	st_essence_type_t mtype;
	st30_format_t *afmt;

	if ((!dev) || (!sin) || (!fmt))
		return ST_INVALID_PARAM;

	mtype = fmt->mtype;
	if (mtype != ST_ESSENCE_AUDIO)
		return ST_INVALID_PARAM;

	afmt = &fmt->a;

	double tmstampTime = 0.0f;
	st_status_t res = RaRtpGetTmstampTime(afmt, &tmstampTime);
	if (res != ST_OK)
		return res;

	int timeslot = RaRtpGetTimeslot(dev);
	if (timeslot < 0)
		return ST_SN_ERR_NO_TIMESLOT;

	st_session_impl_t *s = rte_malloc_socket("SessionAudio", sizeof(st_session_impl_t),
											 RTE_CACHE_LINE_SIZE, rte_socket_id());

	if (s)
	{
		memset(s, 0x0, sizeof(st_session_impl_t));
		/* TBD - TX function called below calculates wrongly for RX queue Audio
		RaRtpSetTimeslot(dev, timeslot, s);
		*/

		s->fmt = *fmt;
		s->dev = dev;
		s->sn = *sin;
		s->sn.timeslot = timeslot;
		s->sn.frameSize = s->fmt.a.pktSize;
		s->sn.rtpProfile = RARTP_PAYLOAD_TYPE_PCM_AUDIO;

		for (uint32_t i = 0; i < stMainParams.maxAudioRcvThrds; i++)
		{
			if ((stMainParams.audioRcvThrds[i].thrdSnFirst <= timeslot)
				&& (timeslot < stMainParams.audioRcvThrds[i].thrdSnLast))
			{
				s->tid = i + stMainParams.maxRcvThrds;
				break;
			}
		}

		s->tmstampTime = tmstampTime;

		s->UpdateRtpPkt = RaRtpDummyBuildPacket;
		s->RecvRtpPkt = RaRtpReceivePacketsRegular;

		s->state = ST_SN_STATE_ON;

		s->actx.payloadSize = s->fmt.a.pktSize;
		s->acons.bufSize = s->sn.frameSize;
		s->actx.histogramSize = 2 * s->acons.bufSize / s->actx.payloadSize;
		s->actx.histogram = rte_malloc_socket("Audio", s->actx.histogramSize, RTE_CACHE_LINE_SIZE,
											  rte_socket_id());
		if (!s->actx.histogram)
		{
			rte_free(s);
			free(s->actx.histogram);
			return ST_NO_MEMORY;
		}

		*sout = s;
		return ST_OK;
	}
	return ST_NO_MEMORY;
}

st_status_t
RaRtpDestroyRxSession(st_session_impl_t *s)
{
	return ST_OK;
}

/*****************************************************************************************
 *
 * RaRtpHdrCheck
  *
 * DESCRIPTION
 * Check RFC 3550 RTP hdr in packet within session context
 *
 * RETURNS: st_status_t
 */
static inline st_status_t
RaRtpHdrCheck(st_session_impl_t *s, const st_rfc3550_audio_hdr_t *rtp)
{
	if (unlikely((rtp->version != RVRTP_VERSION_2) || (rtp->csrcCount != 0)))
	{
		s->pktsDrop++;
		s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_HDR)]++;
		RTE_LOG(INFO, USER3, "Packet bad RTP HDR: pktsDrop %llu\n", (U64)s->pktsDrop);
		return ST_PKT_DROP_BAD_RTP_HDR;
	}

	if (unlikely(rtp->payloadType != s->sn.rtpProfile))
	{
		s->pktsDrop++;
		s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_HDR)]++;
		RTE_LOG(INFO, USER3, "Packet bad profileType of %u pktsDrop %lu\n", 0, s->pktsDrop);
		return ST_PKT_DROP_BAD_RTP_HDR;
	}

	return ST_OK;
}

/*****************************************************************************************
 *
 * RaRtpReceiveFastCopyInline
 *
 * DESCRIPTION
 * Main function to copy packet's payload and update histogram, solves patterns
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
static inline st_status_t
RaRtpReceiveFastCopyInline(st_session_impl_t *s, void *rtp)
{
	st_rfc3550_audio_hdr_t *rtpAudio = (st_rfc3550_audio_hdr_t *)rtp;
	uint8_t *payload = (uint8_t *)&rtpAudio[1];

	uint32_t byteOffset = s->actx.bufOffset;
	uint32_t histIndex = s->actx.bufOffset / s->actx.payloadSize;
	uint16_t seqNumber = ntohs(rtpAudio->seqNumber);
	if (s->actx.histogram[histIndex] == seqNumber && seqNumber != 0)
		return ST_PKT_DROP_REDUNDANT_PATH;
	s->actx.histogram[histIndex] = seqNumber;
	// Debug code to make sure audio stream is sequential
#ifdef DEBUG
	prevSeqNumber = s->actx.seqNumber;
	if ((seqNumber != 0) && (seqNumber != (prevSeqNumber + 1)))
	{
		RTE_LOG(INFO, USER1, " session: %d prevSeqNumber:%u while current: %u\n", s->sn.ssid,
				prevSeqNumber, seqNumber);
		struct rte_eth_stats stats;
		rte_eth_stats_get(0, &stats);
		printf("Rx:%ld Missed %ld error %ld nombuf %ld\n", stats.ipackets, stats.imissed,
			   stats.ierrors, stats.rx_nombuf);
	}
	s->actx.seqNumber = seqNumber;
#endif /* DEBUG code */

#ifdef DEBUG
	RTE_LOG(INFO, USER2, "Audio_packet rtp->seqNumber: %u \npayload: ", seqNumber);
#endif

	/* payload copy */
	rte_memcpy(&s->actx.data[byteOffset], payload, s->actx.payloadSize);

	return ST_OK;
}

/*****************************************************************************************
 *
 * RaRtpReceivePacketsRegular
 *
 * DESCRIPTION
 * Main function to processes packets within session context
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RaRtpReceivePacketsRegular(st_session_impl_t *s, struct rte_mbuf *m)
{
	const struct rte_ether_hdr *ethHdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
	const struct rte_ipv4_hdr *ip
		= (struct rte_ipv4_hdr *)((char *)ethHdr + sizeof(struct rte_ether_hdr));
	const struct rte_udp_hdr *udp
		= (struct rte_udp_hdr *)((char *)ethHdr + sizeof(struct rte_ether_hdr)
								 + sizeof(struct rte_ipv4_hdr));
	const struct st_rfc3550_audio_hdr *rtp = (struct st_rfc3550_audio_hdr *)&udp[1];

	s->actx.data = NULL;  //pointer to right frameBuffer

	/* Validate the IP & UDP & RTP header */
	st_status_t res = StRtpIpUdpHdrCheck(s, ip) || RaRtpHdrCheck(s, rtp);
	if (unlikely(res != ST_OK))
	{
		return res;
	}

	const uint32_t rtpTmstamp = ntohl(rtp->tmstamp);

#ifdef ST_DONT_IGNORE_PKT_CHECK
	if (unlikely(rtpTmstamp == 0))
	{
		s->pktsDrop++;
		s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_TMSTAMP)]++;
		RTE_LOG(INFO, USER3, "Packet bad tmstamp of %u pktsDrop %llu\n", rtpTmstamp,
				(U64)s->pktsDrop);
		return ST_PKT_DROP_BAD_RTP_TMSTAMP;
	}
#endif

	if (rtpTmstamp == (s->actx.tmstamp + s->fmt.a.sampleGrpCount))	//tmstamp match most cases here
	{
		s->actx.data = s->consBuf;
		s->sn.pktsRecv++;
	}
	else if ((rtpTmstamp > s->actx.tmstamp + s->fmt.a.sampleGrpCount)
			 || ((rtpTmstamp & (0x1 << 31))
				 < (s->actx.tmstamp & (0x1 << 31))))  //at least 1 sample lost
	{
		if (!s->consBuf)
		{
			s->consBuf
				= s->acons.St30GetNextAudioBuf(s->acons.appHandle, s->consBuf, s->acons.bufSize);
			if (unlikely(s->consBuf == NULL))
			{
				s->pktsDrop++;
				s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_NO_FRAME_BUF)]++;
				return ST_PKT_DROP_NO_FRAME_BUF;
			}
		}

		s->actx.data = s->consBuf;
		s->sn.pktsRecv++;
		/* TODO need to unify video and audio below */
		//s->sn.pktsLost++;
		// TO BE DONE - produce silent since packet is lost
	}
	//out of order packet arrived - drop it silently, likely redundant path packet
	else if (s->actx.tmstamp >= rtpTmstamp)	 //redundant path
	{
		s->pktsDrop++;
		s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_REDUNDANT_PATH)]++;
		return ST_PKT_DROP_REDUNDANT_PATH;
	}

	s->actx.tmstamp = rtpTmstamp;

	res = RaRtpReceiveFastCopyInline(s, (void *)rtp);
	if (unlikely(res != ST_OK))
	{
		if (res == ST_PKT_DROP_REDUNDANT_PATH)
		{
			s->pktsDrop++;
			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_REDUNDANT_PATH)]++;
		}
		return res;
	}

	s->acons.St30NotifySampleRecv(s->acons.appHandle, s->actx.data, s->actx.bufOffset, rtpTmstamp);
	s->actx.bufOffset += s->actx.payloadSize;
	if (s->actx.bufOffset >= s->acons.bufSize)
	{

		s->acons.St30NotifyBufferDone(s->acons.appHandle, s->consBuf);
		s->consBuf = s->acons.St30GetNextAudioBuf(s->acons.appHandle, s->consBuf, s->acons.bufSize);
		s->actx.bufOffset = 0;
	}

	return ST_OK;
}

/*****************************************************************************************
 *
 * RaRtpReceivePacketCallback
 *
 * DESCRIPTION
 * Main function to processes packet within session context when callback is called on 
 * receive
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RaRtpReceivePacketCallback(st_session_impl_t *s, struct rte_mbuf *m)
{
	struct rte_ipv4_hdr *ip = rte_pktmbuf_mtod_offset(m, struct rte_ipv4_hdr *, 14);
	struct rte_udp_hdr *udp = rte_pktmbuf_mtod_offset(m, struct rte_udp_hdr *, 34);
	st_rfc3550_audio_hdr_t *rtp = (st_rfc3550_audio_hdr_t *)&udp[1];

	if (unlikely(!(s->acons.St30RecvRtpPkt)))
		ST_ASSERT;

	if (unlikely(m->pkt_len < MIN_PKT_SIZE))
		return ST_PKT_DROP_BAD_PKT_LEN;

	/* Validate the IP & UDP header */
	st_status_t res = StRtpIpUdpHdrCheck(s, ip);
	if (unlikely(res != ST_OK))
	{
		return res;
	}

	uint32_t hdrSize = 34 + sizeof(struct rte_udp_hdr) + sizeof(st_rfc3550_audio_hdr_t);
	uint8_t *rtpPayload = (uint8_t *)&rtp[1];
	uint32_t payloadSize = m->pkt_len - hdrSize;
	uint8_t *pktHdr = rte_pktmbuf_mtod_offset(m, uint8_t *, 0);

	switch (s->acons.consType)
	{
	case ST21_CONS_RAW_L2_PKT:
#if (RTE_VER_YEAR < 21)
		return s->acons.St30RecvRtpPkt(s->acons.appHandle, pktHdr, hdrSize, rtpPayload, payloadSize,
									   m->timestamp);
#else
	{
		pktpriv_data_t *ptr = rte_mbuf_to_priv(m);
		return s->acons.St30RecvRtpPkt(s->acons.appHandle, pktHdr, hdrSize, rtpPayload, payloadSize,
									   ptr->timestamp);
	}
#endif

	case ST21_CONS_RAW_RTP:
#if (RTE_VER_YEAR < 21)
		return s->acons.St30RecvRtpPkt(s->acons.appHandle, (uint8_t *)rtp,
									   sizeof(st_rfc3550_audio_hdr_t), rtpPayload, payloadSize,
									   m->timestamp);
#else
	{
		pktpriv_data_t *ptr = rte_mbuf_to_priv(m);
		return s->acons.St30RecvRtpPkt(s->acons.appHandle, (uint8_t *)rtp,
									   sizeof(st_rfc3550_audio_hdr_t), rtpPayload, payloadSize,
									   ptr->timestamp);
	}
#endif

	default:
		ST_ASSERT;
	}
	return ST_GENERAL_ERR;
}
