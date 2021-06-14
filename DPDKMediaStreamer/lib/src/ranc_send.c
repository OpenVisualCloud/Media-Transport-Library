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

#include "dpdk_common.h"
#include "rvrtp_main.h"
#include "st_rtp.h"

#include <unistd.h>

//#define _ANC_DEBUG_

static inline void *RancRtpBuildAncillaryPacket(st_session_impl_t *s, void *hdr);

extern st_main_params_t stMainParams;
extern uint16_t evenParrity[];
extern st_enqueue_stats_t enqStats[RTE_MAX_LCORE] __rte_cache_aligned;

st_status_t
RancRtpDummyRecvPacket(st_session_impl_t *s, struct rte_mbuf *m)
{
	return ST_OK;
}

int32_t
RancRtpGetTimeslot(st_device_impl_t *dev)
{
	if (dev->sn40Count >= dev->dev.maxSt40Sessions)
		return -1;

	for (uint32_t i = 0; i < dev->dev.maxSt40Sessions; i++)
	{
		if (dev->sn40Table[i] == NULL)
		{
			return i;
		}
	}
	return -1;
}

void
RancRtpCopyPacket(struct rte_mbuf *dst, struct rte_mbuf *src)
{
	struct rte_udp_hdr *udp_dst
		= rte_pktmbuf_mtod_offset(dst, struct rte_udp_hdr *, dst->l2_len + dst->l3_len);
	st_rfc8331_anc_rtp_hdr_t *rtp_dst = (st_rfc8331_anc_rtp_hdr_t *)&udp_dst[1];
	struct rte_udp_hdr *udp_src
		= rte_pktmbuf_mtod_offset(src, struct rte_udp_hdr *, src->l2_len + src->l3_len);
	st_rfc8331_anc_rtp_hdr_t *rtp_src = (st_rfc8331_anc_rtp_hdr_t *)&udp_src[1];
	/* copy header */
	rte_memcpy(rtp_dst, rtp_src, sizeof(st_rfc8331_anc_rtp_hdr_t));
	/* copy payload */
	rte_memcpy((uint8_t *)&rtp_dst[1], (uint8_t *)&rtp_src[1],
			   src->pkt_len - sizeof(st_rfc8331_pkt_anc_t));
}

void
RancRtpSetTimeslot(st_device_impl_t *dev, int32_t timeslot, st_session_impl_t *s)
{
	dev->sn40Table[timeslot] = s;
	return;
}

uint16_t
RancRtpUpdatePktSize(uint32_t payloadSize)
{
	uint16_t pktSize = sizeof(st_rfc8331_pkt_anc_t) + payloadSize;
	return pktSize;
}

void
RancRtpInitPacketCtx(st_session_impl_t *s, uint32_t ring)
{
	struct rte_ipv4_hdr *ip;
	struct rte_udp_hdr *udp;

	s->ancctx.payloadSize = ST_ANC_UDW_MAX_SIZE;
	s->sn.pktsRecv = 0;
	s->sn.pktsSend = 0;
	memset(s->sn.pktsDrop, 0, sizeof(s->sn.pktsDrop));
	s->ancctx.pktSize = RancRtpUpdatePktSize(s->ancctx.payloadSize);
	s->ancctx.bufOffset = 0;
	s->ancctx.seqNumber = 0;
	s->ancctx.extSeqNumber = 0;

	ip = (struct rte_ipv4_hdr *)StRtpBuildL2Packet(s, &s->hdrPrint[ST_PPORT].ancillaryHdr.eth, 0);
	udp = (struct rte_udp_hdr *)StRtpBuildIpHeader(s, ip, 0);
	st_rfc8331_anc_rtp_hdr_t *rtp = (st_rfc8331_anc_rtp_hdr_t *)StRtpBuildUdpHeader(s, udp);
	RancRtpBuildAncillaryPacket(s, rtp);
	if (s->sn.caps & ST_SN_DUAL_PATH && stMainParams.numPorts > 1)
	{
		ip = (struct rte_ipv4_hdr *)StRtpBuildL2Packet(s, &s->hdrPrint[ST_RPORT].ancillaryHdr.eth,
													   1);
		udp = (struct rte_udp_hdr *)StRtpBuildIpHeader(s, ip, 1);
	}

#ifdef TX_RINGS_DEBUG
	RTE_LOG(DEBUG, USER2, "RancRtpInitPacketCtx payloadLength %u\n", s->ancctx.payloadSize);
#endif
}

static inline void *
RancRtpBuildAncillaryPacket(st_session_impl_t *s, void *hdr)
{
	/* Create the RTP header */
	st_rfc8331_anc_rtp_hdr_t *rtp = (st_rfc8331_anc_rtp_hdr_t *)hdr;

	rtp->version = RVRTP_VERSION_2;
	rtp->padding = 0;
	rtp->marker = 0;
	rtp->csrcCount = 0;
	rtp->payloadType = RANCRTP_PAYLOAD_TYPE_ANCILLARY;	//TODO: Get this value from the parameter

	rtp->ssrc = htonl(s->sn.ssid);

	/* Return the original pointer for IP hdr */
	return hdr;
}

st_status_t
RancRtpCreateTxSession(st_device_impl_t *dev, st_session_t *sin, st_format_t *fmt,
					   st_session_impl_t **sout)
{
	st_essence_type_t mtype;
	st40_format_t *ancFmt;

	if ((!dev) || (!sin) || (!fmt))
		return ST_INVALID_PARAM;

	mtype = fmt->mtype;

	/* This method just handle ancillary data */
	if (mtype != ST_ESSENCE_ANC)
		return ST_INVALID_PARAM;

	st_session_t sn = *sin;
	ancFmt = &fmt->anc;

	double tmstampTime = 0.0f;

	switch (ancFmt->clockRate)
	{
	case 90000:	 //90kHz
		tmstampTime = 11111;
		break;
	default:
		return ST_FMT_ERR_BAD_CLK_RATE;
	}

	int timeslot = RancRtpGetTimeslot(dev);
	if (timeslot < 0)
		return ST_SN_ERR_NO_TIMESLOT;

	sn.timeslot = timeslot;

	st_session_impl_t *s = rte_malloc_socket("SessionAnc", sizeof(st_session_impl_t),
											 RTE_CACHE_LINE_SIZE, rte_socket_id());
	if (s)
	{
		memset(s, 0x0, sizeof(st_session_impl_t));
		RancRtpSetTimeslot(dev, timeslot, s);

		s->fmt = *fmt;
		s->dev = dev;
		s->sn = sn;
		s->tmstampTime = tmstampTime;

		s->UpdateRtpPkt = RancRtpUpdateAncillaryPacket;
		s->RecvRtpPkt = RancRtpDummyRecvPacket;

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
RancRtpDestroyTxSession(st_session_impl_t *s)
{
	return ST_OK;
}

static uint16_t parityTab[] = {
	//              0       1       2       3       4       5       6       7       8       9       A       B       C       D       E       F
	/* 0 */ 0x0200, 0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
	0x0100,			0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
	/* 1 */ 0x0100, 0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
	0x0200,			0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
	/* 2 */ 0x0100, 0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
	0x0200,			0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
	/* 3 */ 0x0200, 0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
	0x0100,			0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
	/* 4 */ 0x0100, 0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
	0x0200,			0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
	/* 5 */ 0x0200, 0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
	0x0100,			0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
	/* 6 */ 0x0200, 0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
	0x0100,			0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
	/* 7 */ 0x0100, 0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
	0x0200,			0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
	/* 8 */ 0x0100, 0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
	0x0200,			0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
	/* 9 */ 0x0200, 0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
	0x0100,			0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
	/* A */ 0x0200, 0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
	0x0100,			0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
	/* B */ 0x0100, 0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
	0x0200,			0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
	/* C */ 0x0200, 0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
	0x0100,			0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
	/* D */ 0x0100, 0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
	0x0200,			0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
	/* E */ 0x0100, 0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
	0x0200,			0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
	/* F */ 0x0200, 0x0100, 0x0100, 0x0200, 0x0100, 0x0200, 0x0200, 0x0100,
	0x0100,			0x0200, 0x0200, 0x0100, 0x0200, 0x0100, 0x0100, 0x0200,
};

static inline uint16_t
St40GetParityBits(uint16_t val)
{
	return parityTab[val & 0xFF];
}

uint16_t
St40AddParityBits(uint16_t val)
{
	return St40GetParityBits(val) | (val & 0xFF);
}

int
St40CheckParityBits(uint16_t val)
{
	return val == St40AddParityBits(val & 0xFF);
}

static st_status_t
St40GetStrNbAndPointer(int idx, int *off, int *strNb)
{
	int cntBit = idx * 10;
	*off = cntBit / 8;	//in bytes
	*strNb = idx % 4;

	return ST_OK;
}

static uint16_t
StNtohs(uint8_t *data)
{
	assert(data != NULL);
	return ((data[0] << 8) | (data[1]));
}

st_status_t
St40Get10bWord(int idx, uint16_t *udw, uint8_t *data)
{
	int off, strNb;
	St40GetStrNbAndPointer(idx, &off, &strNb);
	data += off;
	//  host to network conversion without load from misalligned address
	uint16_t val = StNtohs(data);

	switch (strNb)
	{
	case 0:
	{
		anc_udw_10_6e_t val10;
		val10.val = val;
		*udw = val10.udw;
		break;
	}
	case 1:
	{
		anc_udw_2e_10_4e_t val10;
		val10.val = val;
		*udw = val10.udw;
		break;
	}
	case 2:
	{
		anc_udw_4e_10_2e_t val10;
		val10.val = val;
		*udw = val10.udw;
		break;
	}
	case 3:
	{
		anc_udw_6e_10_t val10;
		val10.val = val;
		*udw = val10.udw;
		break;
	}
	}
	return ST_OK;
}

st_status_t
St40Set10bWord(int idx, uint16_t udw, uint8_t *data)
{
	int off, strNb;
	St40GetStrNbAndPointer(idx, &off, &strNb);
	data += off;
	//  host to network conversion without load from misalligned address
	uint16_t val = StNtohs(data);
	switch (strNb)
	{
	case 0:
	{
		anc_udw_10_6e_t val10;
		val10.val = val;
		val10.udw = udw;
		val = val10.val;
		break;
	}
	case 1:
	{
		anc_udw_2e_10_4e_t val10;
		val10.val = val;
		val10.udw = udw;
		val = val10.val;
		break;
	}
	case 2:
	{
		anc_udw_4e_10_2e_t val10;
		val10.val = val;
		val10.udw = udw;
		val = val10.val;
		break;
	}
	case 3:
	{
		anc_udw_6e_10_t val10;
		val10.val = val;
		val10.udw = udw;
		val = val10.val;
		break;
	}
	}
	//  host to network conversion without store to misalligned address
	data[0] = (uint8_t)((val & 0xFF00) >> 8);
	data[1] = (uint8_t)(val & 0xFF);

	return ST_OK;
}

st_status_t
St40GetUDW(int idx, uint16_t *udw, uint8_t *data)
{
	return St40Get10bWord(idx + 3, udw, data);
}

st_status_t
St40SetUDW(int idx, uint16_t udw, uint8_t *data)
{
	return St40Set10bWord(idx + 3, udw, data);
}

uint16_t
St40CalcChecksum(int howData, uint8_t *data)
{
	uint16_t chks = 0, udw;
	for (int i = 0; i < howData; i++)
	{

		St40Get10bWord(i, &udw, data);
		chks += udw;
	}
	chks &= 0x1ff;
	chks = (~((chks << 1)) & 0x200) | chks;

	return chks;
}

#define ST_TPRS_SLOTS_ADVANCE 8
uint64_t ac_adjustCount[6] = { 0, 0, 0, 0, 0, 0 };
uint32_t
RancRtpGetFrameTmstamp(st_session_impl_t *s, uint32_t firstWaits, U64 *roundTime,
					   struct rte_mbuf *m)
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
	U64 epochs = (U64)(ntime / s->fmt.anc.frameTime);

	int areSameEpochs = 0, isOneLate = 0;

	if (s->ancctx.epochs == 0)
	{
		s->ancctx.epochs = epochs;
	}
	else if ((int64_t)epochs - s->ancctx.epochs > 1)
	{
		s->ancctx.epochs = epochs;
		__sync_fetch_and_add(&ac_adjustCount[0], 1);
	}
	else if ((int64_t)epochs - s->ancctx.epochs == 0)
	{
		areSameEpochs++;
		__sync_fetch_and_add(&ac_adjustCount[1], 1);
	}
	else if ((int64_t)epochs - s->ancctx.epochs == 1)
	{
		isOneLate++;
		s->ancctx.epochs++;
		__sync_fetch_and_add(&ac_adjustCount[2], 1);
	}
	else
	{
		//? case
		s->ancctx.epochs = epochs;
		__sync_fetch_and_add(&ac_adjustCount[0], 1);
	}

	U64 toEpoch;
	int64_t toElapse;
	U64 st40Tmstamp90k;
	U64 advance = s->nicTxTime + ST_TPRS_SLOTS_ADVANCE * s->sn.tprs;
	long double frmTime90k = 1.0L * s->fmt.anc.clockRate * 1001 / 60000;  //todo real fps?
	ntime = StPtpGetTime();
	ntimeCpu = StGetCpuTimeNano();
	U64 epochs_r = (U64)(ntime / s->fmt.anc.frameTime);
	U64 remaind = ntime - (U64)(epochs_r * s->fmt.anc.frameTime);

	if ((isOneLate || (!areSameEpochs)) && (remaind < s->sn.trOffset - advance))
	{
		if (remaind > s->sn.trOffset / 2)
		{
			toElapse = 0;
			__sync_fetch_and_add(&ac_adjustCount[3], 1);
		}
		else
		{
			toElapse = s->sn.trOffset - advance - remaind;
			__sync_fetch_and_add(&ac_adjustCount[4], 1);
		}

		// set 90k timestamp aligned to epoch
		st40Tmstamp90k = s->ancctx.epochs * frmTime90k;
#if (RTE_VER_YEAR < 21)
		m->timestamp = (U64)(s->ancctx.epochs * s->fmt.anc.frameTime) + s->sn.trOffset - advance;
#else
		/* No access to portid, hence we have rely on pktpriv_data */
		pktpriv_data_t *ptr = rte_mbuf_to_priv(m);
		ptr->timestamp = (U64)(s->ancctx.epochs * s->fmt.anc.frameTime) + s->sn.trOffset - advance;
#endif
	}
	else
	{
		s->ancctx.epochs++;
		toEpoch = (U64)(s->ancctx.epochs * s->fmt.anc.frameTime) - ntime;
		toElapse = toEpoch + s->sn.trOffset - advance;
		// set 90k timestamp aligned to epoch
		st40Tmstamp90k = s->ancctx.epochs * frmTime90k;
		__sync_fetch_and_add(&ac_adjustCount[5], 1);

#if (RTE_VER_YEAR < 21)
		m->timestamp = (U64)(s->ancctx.epochs * s->fmt.anc.frameTime) + s->sn.trOffset - advance;
#else
		/* No access to portid, hence we have rely on pktpriv_data */
		pktpriv_data_t *ptr = rte_mbuf_to_priv(m);
		ptr->timestamp = (U64)(s->ancctx.epochs * s->fmt.anc.frameTime) + s->sn.trOffset - advance;
#endif
	}
	if (toElapse < 0)
		toElapse = 0;
	//leave only complete 128us steps so that waiting will be done with 128us accuracy
	if ((toElapse > 2 * ST_CLOCK_PRECISION_TIME) && firstWaits)
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
			if (elapsed + MAX(req.tv_nsec, ST_CLOCK_PRECISION_TIME) > toElapse)
				break;
			repeats++;
		}
	}
	s->lastTmstamp = st40Tmstamp90k;

	return (uint32_t)st40Tmstamp90k;
}

/*****************************************************************************
 *
 * RaRtpUpdateAncillaryPacket - Ancillary packet constructor routine.
 *
 * DESCRIPTION
 * Constructs the UDP RFC 3550 packet with ancillary data (RFC 8331)
 * and headers in a packer buffer.
 *
 * RETURNS: IP header location
 *
 */
void *
RancRtpUpdateAncillaryPacket(st_session_impl_t *s, void *hdr, struct rte_mbuf *m)
{
#ifdef _ANC_DEBUG_
	RTE_LOG(DEBUG, USER2, "RancRtpUpdateAncillaryPacket start\n");
#endif
	/* Create the IP & UDP header */
	struct rte_ipv4_hdr *ip = hdr;
	struct rte_udp_hdr *udp = (struct rte_udp_hdr *)StRtpUpdateIpHeader(s, ip);
	// UDP checksum
	udp->dgram_cksum = 0;
	if (unlikely((s->ofldFlags & ST_OFLD_HW_UDP_CKSUM) != ST_OFLD_HW_UDP_CKSUM))
	{
		udp->dgram_cksum = rte_ipv4_udptcp_cksum(ip, (const void *)udp);
		if (udp->dgram_cksum == 0)
			udp->dgram_cksum = 0xFFFF;
	}
	udp->dgram_len = htons(m->pkt_len - m->l2_len - m->l3_len);
	st_rfc8331_anc_rtp_hdr_t *rtp = (st_rfc8331_anc_rtp_hdr_t *)StRtpBuildUdpHeader(s, udp);

	rtp->seqNumber = htons(s->ancctx.seqNumber);
	rtp->tmstamp = htonl(s->ancctx.tmstamp);
	rtp->seqNumberExt = htons(s->ancctx.extSeqNumber);

	if (s->ancctx.seqNumber == 0xFFFF)
		s->ancctx.extSeqNumber++;

	s->ancctx.seqNumber++;
	/* Set place for payload just behind rtp header */
	uint8_t *payload = (uint8_t *)&rtp[1];
	s->prodBuf = s->ancprod.St40GetNextAncFrame(s->ancprod.appHandle);
	strtp_ancFrame_t *ptr = (strtp_ancFrame_t *)s->prodBuf;
	int ancCount = ptr->metaSize;
	for (int idx = 0; idx < ancCount; idx++)
	{
		uint16_t udwSize = ptr->meta[idx].udwSize;
		st_anc_pkt_payload_hdr_t *pktBuff = (st_anc_pkt_payload_hdr_t *)(payload);
		pktBuff->first_hdr_chunk.c = ptr->meta[idx].c;
		pktBuff->first_hdr_chunk.lineNumber = ptr->meta[idx].lineNumber;
		pktBuff->first_hdr_chunk.horizontalOffset = ptr->meta[idx].horiOffset;
		pktBuff->first_hdr_chunk.s = ptr->meta[idx].s;
		pktBuff->first_hdr_chunk.streamNum = ptr->meta[idx].streamNum;
		pktBuff->second_hdr_chunk.did = St40AddParityBits(ptr->meta[idx].did);
		pktBuff->second_hdr_chunk.sdid = St40AddParityBits(ptr->meta[idx].sdid);
		pktBuff->second_hdr_chunk.dataCount = St40AddParityBits(udwSize);

		pktBuff->swaped_first_hdr_chunk = htonl(pktBuff->swaped_first_hdr_chunk);
		pktBuff->swaped_second_hdr_chunk = htonl(pktBuff->swaped_second_hdr_chunk);
		int i = 0;
		int offset = ptr->meta[idx].udwOffset;
		for (; i < udwSize; i++)
		{
			St40SetUDW(i, (uint16_t)ptr->data[offset++], (uint8_t *)&pktBuff->second_hdr_chunk);
		}
		uint16_t checksum = 0;
		checksum = St40CalcChecksum(3 + udwSize, (uint8_t *)&pktBuff->second_hdr_chunk);
		St40SetUDW(i, checksum, (uint8_t *)&pktBuff->second_hdr_chunk);

		uint16_t sidDidUdwChsum = ((3 + udwSize + 1) * 10) / 8;	 // Calculate size of the
			// 10-bit words: DID, SDID, DATA_COUNT
			// + size of buffer with data + checksum
		sidDidUdwChsum
			= (4 - sidDidUdwChsum % 4) + sidDidUdwChsum;  // Calculate word align to the 32-bit word
														  // of ANC data packet
		uint16_t sizeToSend
			= sizeof(st_anc_pkt_payload_hdr_t) - 4 + sidDidUdwChsum;  // Full size of one ANC
		payload = payload + sizeToSend;
	}
	int payloadSize = payload - (uint8_t *)&rtp[1];
	s->ancctx.pktSize = payloadSize + sizeof(st_rfc8331_pkt_anc_t);
	rtp->length = htons(payloadSize);
	rtp->ancCount = ptr->metaSize;
	rtp->f = 0b00;	//TODO: Need to be changed to be determined by interlaced info

	//iterate to next packet offset in buffer
	s->ancctx.bufOffset += payloadSize;

	s->sn.pktsSend++;
	s->ancprod.St40NotifyFrameDone(s->ancprod.appHandle, s->prodBuf);
#ifdef _ANC_DEBUG_
	RTE_LOG(DEBUG, USER2, "RancRtpUpdateAAncillaryPacket return\n");
#endif
	/* Return the original pointer for IP hdr */
	return hdr;
}

int
LcoreMainAncillaryRingEnqueue(void *args)
{
	st_main_params_t *mp = &stMainParams;
	unsigned int coreId = rte_lcore_index(rte_lcore_id());

#ifdef TX_RINGS_DEBUG
	uint32_t threadId = (uint32_t)((uint64_t)args);
	RTE_LOG(DEBUG, USER1, "PKT ANC ENQUEUE RUNNING ON LCORE %d SOCKET %d THREAD %d\n",
			rte_lcore_id(), rte_lcore_to_socket_id(rte_lcore_id()), threadId);
#endif

	st_device_impl_t *dev = &stSendDevice;

	// wait for scheduler threads to be ready
	RVRTP_SEMAPHORE_WAIT(mp->ringStart, mp->maxSchThrds);
	st_session_impl_t *s;

	uint32_t pktsCount = dev->dev.maxSt40Sessions;
	struct rte_mbuf *pktVect[pktsCount];
	struct rte_mbuf *pktVectR[pktsCount];
	bool redRing = (mp->numPorts > 1) ? 1 : 0;

	struct rte_mempool *pool = dev->mbufPool;
	if (!pool)
		rte_exit(ST_GENERAL_ERR, "Packets mbufPool is invalid\n");

	do
	{
		rte_delay_us_block(1);
	} while (!mp->schedStart);

	RTE_LOG(INFO, USER2, "Anc transmitter ready - sending packet STARTED\n");
	while (rte_atomic32_read(&isTxDevToDestroy) == 0)
	{
		/* allocate pkts */
		if (rte_pktmbuf_alloc_bulk(pool, pktVect, pktsCount) < 0)
		{
			enqStats[coreId].pktsPriAllocFail += 1;
			RTE_LOG(INFO, USER2, "Packets allocation problem after: %u for %u\n",
					(uint32_t)enqStats[coreId].pktsBuild, pktsCount);
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
		for (uint32_t i = 0; i < pktsCount; i++)
		{
			bool sendR = 0;
			/* TODO
				 * need to re-work base on this version*/
			s = dev->sn40Table[i];

			if (__sync_fetch_and_add(&dev->sn40Table[i], 0) == 0)
			{
				if (pktVect[i])
					rte_pktmbuf_free(pktVect[i]);
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

			s->ancctx.tmstamp = RancRtpGetFrameTmstamp(s, firstSnInRound, &roundTime, pktVect[i]);
			firstSnInRound = 0;

			struct rte_ether_hdr *l2 = rte_pktmbuf_mtod(pktVect[i], struct rte_ether_hdr *);
			struct rte_ipv4_hdr *ip = StRtpFillHeader(s, l2);

			// assemble the RTP packet accordingly to the format
			s->UpdateRtpPkt(s, ip, pktVect[i]);

			pktVect[i]->data_len = StSessionGetPktsize(s);
			pktVect[i]->pkt_len = StSessionGetPktsize(s);
			pktVect[i]->l2_len = sizeof(struct rte_ether_hdr);
			pktVect[i]->l3_len = sizeof(struct rte_ipv4_hdr);
			pktVect[i]->l4_len = sizeof(struct rte_udp_hdr);
			pktVect[i]->ol_flags = PKT_TX_IPV4;

			// UDP & IP checksum offload
			pktVect[i]->ol_flags |= PKT_TX_IP_CKSUM | PKT_TX_UDP_CKSUM;
			if (sendR)
			{
				pktVectR[i]->data_len = pktVect[i]->data_len;
				pktVectR[i]->pkt_len = pktVect[i]->pkt_len;
				pktVectR[i]->l2_len = pktVect[i]->l2_len;
				pktVectR[i]->l3_len = pktVect[i]->l3_len;
				pktVectR[i]->l4_len = pktVect[i]->l4_len;
				pktVectR[i]->ol_flags = pktVect[i]->ol_flags;
				RancRtpCopyPacket(pktVectR[i], pktVect[i]);
				uint8_t *l2R = rte_pktmbuf_mtod(pktVectR[i], uint8_t *);
				StRtpFillHeaderR(s, l2R, rte_pktmbuf_mtod(pktVect[i], uint8_t *));
			}
			else if (redRing)
			{
				rte_pktmbuf_free(pktVectR[i]);
				pktVectR[i] = NULL;
			}
			enqStats[coreId].pktsBuild += 1;
#ifdef _ANC_DEBUG_
			RTE_LOG(INFO, USER2,
					"LcoreMainAncRingEnqueue updated packet for i:%u, pktVect[i]:%u, pktsBuild:%u, "
					"pktVect[i]->data_len: %u\n",
					i, pktVect[i], enqStats[coreId].pktsBuild, pktVect[i]->data_len);
#endif
		}
		for (uint32_t i = 0; i < pktsCount; i++)
		{
			if (!pktVect[i])
				continue;

			uint32_t noFails = 0;
			uint32_t ring
				= dev->dev.maxSt21Sessions;	 //ancillary data and audio use the next ring after
											 //is after video sessions
#ifdef _ANC_DEBUG_
			RTE_LOG(DEBUG, USER2, "LcoreMainAncRingEnqueue enqueing i:%u, pktVect[i]:%u\n", i,
					pktVect[i]);
#endif
			while (rte_ring_mp_enqueue(dev->txRing[ST_PPORT][ring], (void *)pktVect[i]) != 0)
			{
				noFails++;
#ifdef _TX_RINGS_DEBUG_
				if ((noFails % 1000) == 0)
					RTE_LOG(DEBUG, USER2, "Packets enqueue ring %u times %u\n", ring, , noFails);
#endif
				__sync_synchronize();
			}
			while (redRing && pktVectR[i]
				   && rte_ring_mp_enqueue(dev->txRing[ST_RPORT][ring], (void *)pktVectR[i]) != 0)
			{
				noFails++;
#ifdef _TX_RINGS_DEBUG_
				if ((noFails % 1000) == 0)
					RTE_LOG(DEBUG, USER2, "Packets enqueue ring %u times %u\n", ring, , noFails);
#endif
				__sync_synchronize();
			}
			enqStats[coreId].pktsQueued += 1;
#ifdef _ANC_DEBUG_
			RTE_LOG(DEBUG, USER2, "LcoreMainAncRingEnqueue enque DONE for i:%u, pktVect[i]:%u, \n",
					i, pktVect[i]);
#endif
		}
	}
	RTE_LOG(INFO, USER2, "ANC transmitter closed - sending packet STOPPED\n");
	return 0;
}

static st_session_method_t ranc_method = {
	.create_tx_session = RancRtpCreateTxSession,
	.create_rx_session = RancRtpCreateRxSession,
	.destroy_tx_session = RancRtpDestroyTxSession,
	.destroy_rx_session = RancRtpDestroyRxSession,
	.init_packet_ctx = RancRtpInitPacketCtx,
};

void
ranc_method_init()
{
	st_init_session_method(&ranc_method, ST_ESSENCE_ANC);
}
