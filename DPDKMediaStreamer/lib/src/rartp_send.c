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
#include "st_rtp.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

void *RaRtpUpdateAudioPacket(st_session_impl_t *s, void *hdr, struct rte_mbuf *m);

static inline void *RaRtpBuildAudioPacket(st_session_impl_t *s, void *hdr);

//#define TX_RINGS_DEBUG 1
//#define ST_MULTICAST_TEST

extern st_device_impl_t stSendDevice;
extern st_enqueue_stats_t enqStats[RTE_MAX_LCORE];

uint64_t audioCount[6] = { 0, 0, 0, 0, 0, 0 };

/**
 * Get the ST2110-30 timestamp of 48k aligned to the 1ms epoch
 */
uint32_t
RaRtpGetFrameTmstamp(st_session_impl_t *s, uint32_t firstWaits, U64 *roundTime, struct rte_mbuf *m)
{
	U64 ntime;
	U64 ntimeCpu, ntimeCpuLast;

	if (unlikely(!roundTime))
		ST_ASSERT;
	if (*roundTime == 0)
	{
		*roundTime = StPtpGetTime();
	}
	ntime = *roundTime;
	U64 epochs = ntime / s->fmt.a.epochTime;

	int areSameEpochs = 0, isOneLate = 0;

	if (s->actx.epochs == 0)
	{
		s->actx.epochs = epochs;
	}
	else if ((int64_t)epochs - s->actx.epochs > 1)
	{
		s->actx.epochs = epochs;
		__sync_fetch_and_add(&audioCount[0], 1);
	}
	else if ((int64_t)epochs - s->actx.epochs == 0)
	{
		areSameEpochs++;
		__sync_fetch_and_add(&audioCount[1], 1);
	}
	else if ((int64_t)epochs - s->actx.epochs == 1)
	{
		isOneLate++;
		s->actx.epochs++;
		__sync_fetch_and_add(&audioCount[2], 1);
	}
	else
	{
		//? case
		s->actx.epochs = epochs;
		__sync_fetch_and_add(&audioCount[5], 1);
	}

	U64 toEpoch;
	int64_t toElapse;
	U64 st30Tmstamp48k;
	U64 advance = s->nicTxTime;
	ntime = StPtpGetTime();
	ntimeCpu = StGetCpuTimeNano();
	//U64 remaind = ntime % s->fmt.a.epochTime;

	if (isOneLate || !areSameEpochs)
	{
		toElapse = 0;
		__sync_fetch_and_add(&audioCount[3], 1);

		// set 48k timestamp aligned to epoch
		st30Tmstamp48k = s->actx.epochs * s->fmt.a.sampleGrpCount;
#if (RTE_VER_YEAR < 21)
		m->timestamp = ((U64)s->actx.epochs * s->fmt.a.epochTime) - advance;
#else
		/* No access to portid, hence we have rely on pktpriv_data */
		pktpriv_data_t *ptr = rte_mbuf_to_priv(m);
		ptr->timestamp = ((U64)s->actx.epochs * s->fmt.a.epochTime) - advance;
#endif
	}
	else
	{
		s->actx.epochs++;
		toEpoch = s->actx.epochs * s->fmt.a.epochTime - ntime;
		toElapse = toEpoch - advance;
		// set 90k timestamp aligned to epoch
		st30Tmstamp48k = s->actx.epochs * s->fmt.a.sampleGrpCount;
		__sync_fetch_and_add(&audioCount[4], 1);

		// set mbuf timestmap to expected nanoseconds on a wire
#if (RTE_VER_YEAR < 21)
		m->timestamp = ((U64)s->actx.epochs * s->fmt.a.epochTime) - advance;
#else
		/* No access to portid, hence we have rely on pktpriv_data */
		pktpriv_data_t *ptr = rte_mbuf_to_priv(m);
		ptr->timestamp = ((U64)s->actx.epochs * s->fmt.a.epochTime) - advance;
#endif
	}

	if ((toElapse > (int64_t)(2 * ST_CLOCK_PRECISION_TIME)) && firstWaits)
	{
		toElapse -= ST_CLOCK_PRECISION_TIME;

		uint32_t repeats = 0;
		uint32_t repeatCountMax = 2 * (uint32_t)(toElapse / ST_CLOCK_PRECISION_TIME);
		U64 elapsed;
		struct timespec req, rem;

		req.tv_sec = 0;
		if (toElapse > ST_CLOCK_PRECISION_TIME * 10)
		{
			req.tv_nsec = 2 * ST_CLOCK_PRECISION_TIME;
		}
		else
		{
			req.tv_nsec = ST_CLOCK_PRECISION_TIME / 2;
		}

		for (; repeats < repeatCountMax; repeats++)
		{
			clock_nanosleep(CLOCK_REALTIME, 0, &req, &rem);
			ntimeCpuLast = StGetCpuTimeNano();
			elapsed = ntimeCpuLast - ntimeCpu;
			if (elapsed + ST_CLOCK_PRECISION_TIME > toElapse)
				break;
		}
#ifdef TX_TIMER_DEBUG
		if (s->sn.timeslot == 0)
		{
			uint64_t tmstamp64_ = (U64)ntimeLast / s->tmstampTime;
			uint32_t tmstamp32_ = (uint32_t)tmstamp64_;
			uint32_t tmstamp32 = (uint32_t)st30Tmstamp48k;
#if (RTE_VER_YEAR < 21)

			uint64_t mtmtstamp = m->timestamp;
#else
			/* No access to portid, hence we have rely on pktpriv_data */
			pktpriv_data_t *ptr = rte_mbuf_to_priv(m);
			uint64_t mtmtstamp = ptr->timestamp;
#endif

			RTE_LOG(INFO, USER2, "RaRtpGetFrameTmstamp: elapsed %llu diff %lld tmdelta %d\n",
					elapsed, (long long int)toElapse - elapsed, tmstamp32 - s->lastTmstamp);
			RTE_LOG(INFO, USER2, "RaRtpGetFrameTmstamp: waiting %llu tmstamp %u delta %d\n",
					toElapse, tmstamp32, (int32_t)(tmstamp32_ - tmstamp32));
			RTE_LOG(INFO, USER2, "RaRtpGetFrameTmstamp: mbuf->tmstamp %lu time %lu timeDiff %lu\n",
					mtmtstamp, (uint64_t)ntimeLast, mtmtstamp - ntimeLast);
		}
#endif
	}
	s->lastTmstamp = st30Tmstamp48k;
	return (uint32_t)st30Tmstamp48k;
}

st_status_t
RaRtpDummyRecvPacket(st_session_impl_t *s, struct rte_mbuf *m)
{
	return ST_OK;
}

extern st_main_params_t stMainParams;

int32_t
RaRtpGetTimeslot(st_device_impl_t *dev)
{
	if (dev->sn30Count >= dev->dev.maxSt30Sessions)
		return -1;

	for (uint32_t i = 0; i < dev->dev.maxSt30Sessions; i++)
	{
		if (dev->sn30Table[i] == NULL)
		{
			return i;
		}
	}
	return -1;
}

void
RaRtpSetTimeslot(st_device_impl_t *dev, int32_t timeslot, st_session_impl_t *s)
{
	dev->sn30Table[timeslot] = s;
	return;
}

void
RaRtpInitPacketCtx(st_session_impl_t *s, uint32_t ring)
{
	struct rte_ipv4_hdr *ip;
	struct rte_udp_hdr *udp;

	s->actx.payloadSize = s->fmt.a.pktSize - ST_PKT_AUDIO_HDR_LEN;

	ip = (struct rte_ipv4_hdr *)StRtpBuildL2Packet(s, &s->hdrPrint[ST_PPORT].audioHdr.eth, 0);
	udp = (struct rte_udp_hdr *)StRtpBuildIpHeader(s, ip, 0);
	st_rfc3550_audio_hdr_t *rtp = (st_rfc3550_audio_hdr_t *)StRtpBuildUdpHeader(s, udp);
	RaRtpBuildAudioPacket(s, rtp);
	if (s->sn.caps & ST_SN_DUAL_PATH && stMainParams.numPorts > 1)
	{
		ip = (struct rte_ipv4_hdr *)StRtpBuildL2Packet(s, &s->hdrPrint[ST_RPORT].audioHdr.eth, 1);
		udp = (struct rte_udp_hdr *)StRtpBuildIpHeader(s, ip, 1);
	}

	RTE_LOG(DEBUG, USER2, "RaRtpInitPacketCtx payloadLength %u\n", s->actx.payloadSize);

	s->sn.pktsRecv = 0;
	s->sn.pktsSend = 0;
	memset(s->sn.pktsDrop, 0, sizeof(s->sn.pktsDrop));

	s->actx.bufOffset = 0;
}

st_status_t
RaRtpGetTmstampTime(st30_format_t *fmt, double *tmstampTime)
{
	if ((!tmstampTime) || (!fmt))
		return ST_INVALID_PARAM;

	//several format checks for sanity
	if ((fmt->chanCount == 0)
		|| ((fmt->chanCount > 8) && (fmt->chanCount != 24)
			&& (fmt->chanOrder[0] != ST30_SURROUND_222)))
		return ST_FMT_ERR_BAD_CHANNEL_COUNT;
	if ((fmt->chanCount == 24) && (fmt->chanOrder[0] == ST30_SURROUND_222))
		return ST_NOT_SUPPORTED;
	if ((fmt->chanOrder[0] == ST30_SURROUND_71) && (fmt->chanCount != 8))
		return ST_FMT_ERR_BAD_CHANNEL_COUNT;

	uint32_t chanCount = 0;
	for (uint32_t i = 0; i < 8 && chanCount < 8; i++)
	{
		switch (fmt->chanOrder[i])
		{
		case ST30_UNDEFINED:  //1 channel of undefined audio
		case ST30_STD_MONO:	  //1 channel of standard mono
			chanCount++;
			break;
		case ST30_DUAL_MONO:   //2 channels of dual mono
		case ST30_STD_STEREO:  //2 channels of standard stereo: Left, Right
		case ST30_MAX_STEREO:  //2 channels of matrix stereo - LeftTotal, Righ Total
			chanCount += 2;
			break;
		case ST30_SURROUND_51:	//6 channles of doubly 5.1 surround
			chanCount += 6;
			break;
		case ST30_SURROUND_71:	//8 channels of doubly surround 7.1
			if (i != 0)
				return ST_FMT_ERR_BAD_CHANNEL_ORDER;
			chanCount = 8;
			break;
		case ST30_SURROUND_222:	 //24 channels of doubly surround 22.2
			if (i != 0)
				return ST_FMT_ERR_BAD_CHANNEL_ORDER;
			chanCount = 24;
			break;
		case ST30_SGRP_SDI:	 //4 channels of SDI audio group
			chanCount += 4;
			break;
		case ST30_UNUSED:
			if (chanCount == 0)
				return ST_FMT_ERR_BAD_CHANNEL_ORDER;
			break;
		default:
			return ST_FMT_ERR_BAD_CHANNEL_ORDER;
		}
	}
	if (!chanCount || (chanCount != 24 && chanCount > 8))
		return ST_FMT_ERR_BAD_CHANNEL_ORDER;

	if (chanCount != fmt->chanCount)
		return ST_FMT_ERR_BAD_CHANNEL_COUNT;

	uint32_t sampleGrpSize = 0;
	switch (fmt->sampleFmt)
	{
	case ST30_PCM8_SAMPLING:				 //8bits 1B per channel
		sampleGrpSize = fmt->chanCount * 1;	 //*1B
		break;
	case ST30_PCM16_SAMPLING:				 //16bits 2B per channel
		sampleGrpSize = fmt->chanCount * 2;	 //*2B
		break;
	case ST30_PCM24_SAMPLING:				 //24bits 3B per channel
		sampleGrpSize = fmt->chanCount * 3;	 //*3B
		break;
	default:
		return ST_FMT_ERR_BAD_PCM_SAMPLING;
	}
	if (sampleGrpSize != fmt->sampleGrpSize)
		return ST_FMT_ERR_BAD_SAMPLE_GRP_SIZE;

	double tmTime = 0.0f;

	uint32_t sampleGrpCount = 0;
	if (fmt->epochTime == 1000000 /*1ms*/)
	{
		switch (fmt->sampleClkRate)
		{
		case ST30_SAMPLE_CLK_RATE_48KHZ:
			tmTime = (double)MEGA / 48.0f;
			sampleGrpCount = 48;
			break;
		case ST30_SAMPLE_CLK_RATE_96KHZ:
			tmTime = (double)MEGA / 96.0f;
			sampleGrpCount = 96;
		default:
			return ST_FMT_ERR_BAD_SAMPLE_CLK_RATE;
		}
	}
	else if (fmt->epochTime == 125000 /*125us*/)
	{
		return ST_NOT_SUPPORTED;
	}
	else
	{
		return ST_FMT_ERR_BAD_AUDIO_EPOCH_TIME;
	}
	if (sampleGrpCount != fmt->sampleGrpCount)
		return ST_FMT_ERR_BAD_SAMPLE_GRP_COUNT;
	if (fmt->pktSize < ST_MIN_AUDIO_PKT_SIZE)
		return ST_FMT_ERR_BAD_PKT_SZ;

	//update tmstampTime upon successful completion
	*tmstampTime = tmTime;
	return ST_OK;
}

st_status_t
RaRtpCreateTxSession(st_device_impl_t *dev, st_session_t *sin, st_format_t *fmt,
					 st_session_impl_t **sout)
{
	st_essence_type_t mtype;
	st30_format_t *afmt;

	if ((!dev) || (!sin) || (!fmt))
		return ST_INVALID_PARAM;

	mtype = fmt->mtype;

	/* This method just handle audio */
	if (mtype != ST_ESSENCE_AUDIO)
		return ST_INVALID_PARAM;

	st_session_t sn = *sin;
	afmt = &fmt->a;

	double tmstampTime = 0.0f;
	st_status_t res = RaRtpGetTmstampTime(afmt, &tmstampTime);
	if (res != ST_OK)
		return res;

	int timeslot = RaRtpGetTimeslot(dev);
	if (timeslot < 0)
		return ST_SN_ERR_NO_TIMESLOT;

	sn.timeslot = timeslot;

	sn.frameSize = 1024 * fmt->a.pktSize;  // Need to fix this.

	st_session_impl_t *s = malloc(sizeof(st_session_impl_t));
	if (s)
	{
		memset(s, 0x0, sizeof(st_session_impl_t));

		s->fmt = *fmt;
		s->dev = dev;
		s->sn = sn;
		s->tmstampTime = tmstampTime;

		s->UpdateRtpPkt = RaRtpUpdateAudioPacket;
		s->RecvRtpPkt = RaRtpDummyRecvPacket;

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
RaRtpDestroyTxSession(st_session_impl_t *s)
{
	return ST_OK;
}

/*****************************************************************************
 *
 * RaRtpSessionCheckRunState - check if session is in run state that permits
 * packets to be trasnmitted
 *
 * Returns: nothing but session state is internally updated
 *
 * SEE ALSO:
 */
int
RaRtpSessionCheckRunState(st_session_impl_t *s)
{
	uint8_t *newbuf;
	// lock session
	StSessionLock(s);

	if (s->state != ST_SN_STATE_RUN)  // state is protected by the lock as well as prodBuf
	{
		if (s->state == ST_SN_STATE_NO_NEXT_FRAME)
		{
			newbuf = s->aprod.St30GetNextAudioBuf(s->aprod.appHandle, s->prodBuf, s->aprod.bufSize);
			if (newbuf != NULL)
			{
				s->prodBuf = newbuf;
				s->state = ST_SN_STATE_RUN;
				s->actx.bufOffset = 0;
				s->bufOffset = s->aprod.St30GetNextSampleOffset(s->prod.appHandle, s->prodBuf, 0,
																&s->actx.tmstamp);
			}
			else
			{
				RTE_LOG(INFO, USER2, "ST_SN_STATE_NO_NEXT_FRAME: for session %u prodBuf %p\n",
						s->sn.timeslot, s->prodBuf);
			}
		}
		else if (s->state == ST_SN_STATE_NO_NEXT_SLICE)
		{
			uint32_t nextOffset = s->aprod.St30GetNextSampleOffset(s->prod.appHandle, s->prodBuf,
																   s->bufOffset, &s->actx.tmstamp);
			if (nextOffset > s->bufOffset)
			{
				s->bufOffset = nextOffset;
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
	StSessionUnlock(s);

	return (s->state == ST_SN_STATE_RUN);
}

/*****************************************************************************
 *
 * RaRtpBuildAudioPacket - UDP RAW Video RTP header constructor routine.
 *
 * DESCRIPTION
 * Constructs the UDP RFC 3550 packet with audio
 * and headers in a packer buffer.
 *
 * RETURNS: IP header location
 *
 * SEE ALSO:
 */
static inline void *
RaRtpBuildAudioPacket(st_session_impl_t *s, void *hdr)
{
	/* Create the RTP header */
	st_rfc3550_audio_hdr_t *rtp = (st_rfc3550_audio_hdr_t *)hdr;

	rtp->version = RVRTP_VERSION_2;
	rtp->padding = 0;
	rtp->marker = 0;
	rtp->csrcCount = 0;
	rtp->payloadType = RARTP_PAYLOAD_TYPE_PCM_AUDIO;

	rtp->ssrc = htonl(s->sn.ssid);

	/* Return the original pointer for IP hdr */
	return hdr;
}

/*****************************************************************************
 *
 * RaRtpUpdateAudioPacket - UDP RAW Video RTP header constructor routine.
 *
 * DESCRIPTION
 * Constructs the UDP RFC 3550 packet with audio
 * and headers in a packer buffer.
 *
 * RETURNS: IP header location
 *
 * SEE ALSO:
 */
void *
RaRtpUpdateAudioPacket(st_session_impl_t *s, void *hdr, struct rte_mbuf *m)
{
	/* Create the IP & UDP header */
	struct rte_ipv4_hdr *ip = hdr;
	struct rte_udp_hdr *udp = (struct rte_udp_hdr *)StRtpUpdateIpHeader(s, ip);

	st_rfc3550_audio_hdr_t *rtp = (st_rfc3550_audio_hdr_t *)&udp[1];

	udp->dgram_len = htons(m->pkt_len - m->l2_len - m->l3_len);
	rtp->seqNumber = htons(s->actx.seqNumber);
	rtp->tmstamp = htonl(s->actx.tmstamp);

	/* copy payload */
	uint8_t *payload = (uint8_t *)&rtp[1];

	rte_memcpy(payload, &s->prodBuf[s->actx.bufOffset], s->actx.payloadSize);
	//memset(payload, cnt++, s->actx.payloadSize);
	// UDP checksum
	udp->dgram_cksum = 0;
	if (unlikely((s->ofldFlags & ST_OFLD_HW_UDP_CKSUM) != ST_OFLD_HW_UDP_CKSUM))
	{
		udp->dgram_cksum = rte_ipv4_udptcp_cksum(ip, (const void *)udp);
		if (udp->dgram_cksum == 0)
			udp->dgram_cksum = 0xFFFF;
	}

	//iterate to next packet offset in buffer
	s->actx.seqNumber++;

	s->actx.bufOffset += s->actx.payloadSize;

	if (s->actx.bufOffset + s->actx.payloadSize >= s->aprod.bufSize)  //end of buffer
	{
		//critical section
		StSessionLock(s);

		s->aprod.St30NotifyBufferDone(s->aprod.appHandle, s->prodBuf);
		s->actx.bufOffset = 0;

		s->prodBuf = s->aprod.St30GetNextAudioBuf(s->aprod.appHandle, s->prodBuf, s->aprod.bufSize);
		if (s->prodBuf)
		{
			uint32_t nextOffset = s->aprod.St30GetNextSampleOffset(s->aprod.appHandle, s->prodBuf,
																   0, &s->actx.tmstamp);
			s->bufOffset = nextOffset;
			if (nextOffset == 0)
			{
				RTE_LOG(INFO, USER2, "St30GetNextBufferOffset logical error of offset %u\n",
						nextOffset);
				s->state = ST_SN_STATE_NO_NEXT_OFFSET;
			}
		}
		s->state = (s->prodBuf == NULL) ? ST_SN_STATE_NO_NEXT_BUFFER : s->state;
		//unlock session and sliceOffset
		StSessionUnlock(s);
	}
	s->sn.pktsSend++;

	/* Return the original pointer for IP hdr */
	return hdr;
}

void
RaRtpCopyPacket(struct rte_mbuf *dst, struct rte_mbuf *src)
{
	struct rte_udp_hdr *udp_dst
		= rte_pktmbuf_mtod_offset(dst, struct rte_udp_hdr *, dst->l2_len + dst->l3_len);
	st_rfc3550_audio_hdr_t *rtp_dst = (st_rfc3550_audio_hdr_t *)&udp_dst[1];
	struct rte_udp_hdr *udp_src
		= rte_pktmbuf_mtod_offset(src, struct rte_udp_hdr *, src->l2_len + src->l3_len);
	st_rfc3550_audio_hdr_t *rtp_src = (st_rfc3550_audio_hdr_t *)&udp_src[1];
	/* copy header */
	rte_memcpy(rtp_dst, rtp_src, sizeof(st_rfc3550_audio_hdr_t));
	/* copy payload */
	rte_memcpy((uint8_t *)&rtp_dst[1], (uint8_t *)&rtp_src[1],
			   src->pkt_len - sizeof(st_rfc3550_pkt_audio_t));
}

/* Audio packet creator thread runned on master lcore for St2110-30 */
int
LcoreMainAudioRingEnqueue(void *args)
{
	unsigned int coreId = rte_lcore_index(rte_lcore_id());
	uint32_t threadId = (uint32_t)((uint64_t)args);
	st_main_params_t *mp = &stMainParams;
	bool redRing = (mp->numPorts > 1) ? 1 : 0;

	RTE_LOG(DEBUG, USER1, "PKT AUDIO ENQUEUE RUNNING ON LCORE %d SOCKET %d THREAD %d\n",
			rte_lcore_id(), rte_lcore_to_socket_id(rte_lcore_id()), threadId);

	/* TODO
     * need to consider if we'd like to merge audio with video*/
	printf("launching audio enqueue thread on thread id %d\n", threadId);

	st_device_impl_t *dev = &stSendDevice;

	// wait for scheduler threads to be ready
	RVRTP_SEMAPHORE_WAIT(mp->ringStart, mp->maxSchThrds);

	st_session_impl_t *s;

	uint32_t pktsCount = dev->dev.maxSt30Sessions;

	struct rte_mbuf *pktVect[pktsCount];
	struct rte_mbuf *pktVectR[pktsCount];

	struct rte_mempool *pool = dev->mbufPool;
	if (!pool)
		rte_exit(ST_GENERAL_ERR, "Packets mbufPool is invalid\n");

#ifdef TX_RINGS_DEBUG
	RTE_LOG(DEBUG, USER2, "Audio thread params: %u %u %u %u\n", 0, dev->dev.maxSt30Sessions,
			pktsCount, threadId);
#endif

	do
	{
		rte_delay_us_block(1);
	} while (!mp->schedStart);

	RTE_LOG(INFO, USER2, "Audio transmitter ready - sending packet STARTED\n");

	while (rte_atomic32_read(&isTxDevToDestroy) == 0)
	{
		/* allocate pkts */
		if (rte_pktmbuf_alloc_bulk(pool, pktVect, pktsCount) < 0)
		{
			enqStats[coreId].pktsPriAllocFail += 1;
			RTE_LOG(INFO, USER2, "Packets allocation problem after: %lu for %u\n",
					(uint64_t)enqStats[coreId].pktsBuild, pktsCount);
			continue;
		}
		if (redRing && rte_pktmbuf_alloc_bulk(pool, pktVectR, pktsCount) < 0)
		{
			enqStats[coreId].pktsRedAllocFail += 1;
			rte_pktmbuf_free_bulk(pktVect, pktsCount);
			RTE_LOG(INFO, USER2, "Packets allocation problem after: %u for %u\n",
					(uint32_t)enqStats[coreId].pktsBuild, pktsCount);
			continue;
		}
		U64 roundTime = 0;
		uint32_t firstSnInRound = 1;

		for (uint32_t i = 0; i < dev->dev.maxSt30Sessions; i++)
		{
			bool sendR = 0;
			/* TODO: need to re-work base on this version*/
			s = dev->sn30Table[i];

			struct rte_mbuf *m = pktVect[i];
			if (__sync_fetch_and_add(&dev->sn30Table[i], 0) == 0)
			{
				rte_pktmbuf_free(m);
				pktVect[i] = NULL;
				if (redRing)
				{
					rte_pktmbuf_free(pktVectR[i]);
					pktVectR[i] = NULL;
				}
				continue;
			}
			sendR = (redRing && (s->sn.caps & ST_SN_DUAL_PATH)) ? 1 : 0;

#if (RTE_VER_YEAR < 21)
			pktVect[i]->timestamp = 0;
#else
			pktpriv_data_t *ptr = rte_mbuf_to_priv(pktVect[i]);
			ptr->timestamp = 0;
#endif
			do
			{
				s->actx.tmstamp = RaRtpGetFrameTmstamp(s, firstSnInRound, &roundTime, m);
				firstSnInRound = 0;
			} while (RaRtpSessionCheckRunState(s) == 0);

			struct rte_ether_hdr *l2 = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
			struct rte_ipv4_hdr *ip = StRtpFillHeader(s, l2);

			m->data_len = StSessionGetPktsize(s);
			m->pkt_len = StSessionGetPktsize(s);
			m->l2_len = 14;
			m->l3_len = 20;
			// assemble the RTP packet accordingly to the format
			s->UpdateRtpPkt(s, ip, m);

			m->ol_flags = PKT_TX_IPV4;

			// UDP & IP checksum offload
			m->ol_flags |= PKT_TX_IP_CKSUM | PKT_TX_UDP_CKSUM;
			if (sendR)
			{
				pktVectR[i]->data_len = pktVect[i]->data_len;
				pktVectR[i]->pkt_len = pktVect[i]->pkt_len;
				pktVectR[i]->l2_len = pktVect[i]->l2_len;
				pktVectR[i]->l3_len = pktVect[i]->l3_len;
				pktVectR[i]->ol_flags = pktVect[i]->ol_flags;
				RaRtpCopyPacket(pktVectR[i], pktVect[i]);
				uint8_t *l2R = rte_pktmbuf_mtod(pktVectR[i], uint8_t *);
				StRtpFillHeaderR(s, l2R, rte_pktmbuf_mtod(pktVect[i], uint8_t *));
			}
			else if (redRing)
			{
				rte_pktmbuf_free(pktVectR[i]);
				pktVectR[i] = NULL;
			}

			enqStats[coreId].pktsBuild += 1;
		}
		for (uint32_t i = 0; i < dev->dev.maxSt30Sessions; i++)
		{
			if (!pktVect[i])
				continue;

			uint32_t noFails = 0;
			uint32_t ring = dev->dev.maxSt21Sessions;  //audio ring is after video sessions
			while (rte_ring_mp_enqueue(dev->txRing[ST_PPORT][ring], (void *)pktVect[i]) != 0)
			{
				noFails++;
#ifdef _TX_RINGS_DEBUG_
				if ((noFails % 1000) == 0)
					RTE_LOG(INFO, USER2, "Packets enqueue ring %u after: %llu times %u\n", ring,
							(U64)pktsQueued, noFails);
#endif
				__sync_synchronize();
			}
			while (redRing && pktVectR[i]
				   && rte_ring_mp_enqueue(dev->txRing[ST_RPORT][ring], (void *)pktVectR[i]) != 0)
			{
				noFails++;
#ifdef _TX_RINGS_DEBUG_
				if ((noFails % 1000) == 0)
					RTE_LOG(INFO, USER2, "Packets enqueue ring %u after: %llu times %u\n", ring,
							(U64)pktsQueued, noFails);
#endif
				__sync_synchronize();
			}
			enqStats[coreId].pktsQueued += 1;
		}
	}
	RTE_LOG(INFO, USER2, "Audio transmitter closed - sending packet STOPPED\n");
	return 0;
}

static st_session_method_t rartp_method = {
	.create_tx_session = RaRtpCreateTxSession,
	.create_rx_session = RaRtpCreateRxSession,
	.destroy_tx_session = RaRtpDestroyTxSession,
	.destroy_rx_session = RaRtpDestroyRxSession,
	.init_packet_ctx = RaRtpInitPacketCtx,
};

void
rartp_method_init()
{
	st_init_session_method(&rartp_method, ST_ESSENCE_AUDIO);
}
