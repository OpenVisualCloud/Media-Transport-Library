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
#include "st_api_internal.h"
#include "st_flw_cls.h"

//#define ST_RECV_TIME_PRINT
//#define ST_MEMCPY_TEST
//#define ST_MULTICAST_TEST

rvrtp_device_t stRecvDevice;

void *
RvRtpDummyBuildPacket(rvrtp_session_t *s, void *hdr)
{
	return NULL;
}

st_status_t
RvRtpFreeRxSession(rvrtp_session_t *s)
{
	if (s)
	{
		rte_free(s->ctx.lineHistogram);
		rte_free(s->ctx.fragHistogram);
		rte_free(s->cons.appHandle);
		rte_free(s);
	}
	return ST_OK;
}

static inline void
RvRtpClearPacketEbu(rvrtp_session_t *s)
{
	s->ebu.vrxMax = 0;
	s->ebu.vrxSum = 0;
	s->ebu.vrxMin = 10e12;
	s->ebu.vrxCnt = 0;
	s->ebu.vrxAvg = 0.0f;
	s->ebu.cinTmstamp = 0;
	s->ebu.cinMax = 0;
	s->ebu.cinSum = 0;
	s->ebu.cinMin = 10e12;
	s->ebu.cinCnt = 0;
	s->ebu.cinAvg = 0.0f;
}

static inline void
RvRtpClearFrameEbu(rvrtp_session_t *s)
{
	s->ebu.fptSum = 0;
	s->ebu.fptMax = 0;
	s->ebu.fptMin = 10e12;
	s->ebu.fptCnt = 0;
	s->ebu.fptAvg = 0.0f;
	s->ebu.tmdSum = 0;
	s->ebu.tmdMax = 0;
	s->ebu.tmdMin = 0xffffffff;
	s->ebu.tmdCnt = 0;
	s->ebu.tmdAvg = 0.0f;
	s->ebu.tmiSum = 0;
	s->ebu.tmiMax = 0;
	s->ebu.tmiMin = 0xffffffff;
	s->ebu.tmiCnt = 0;
	s->ebu.tmiAvg = 0.0f;
	s->ebu.latSum = 0;
	s->ebu.latMax = 0;
	s->ebu.latMin = 10e12;
	s->ebu.latCnt = 0;
	s->ebu.latAvg = 0.0f;
}

st_status_t
RvRtpCreateRxSession(
    rvrtp_device_t *dev, 
	st21_session_t *sin, 
	st21_format_t *fmt,
    rvrtp_session_t **sout)
{
	if ((!dev) || (!sin) || (!fmt))
		return ST_INVALID_PARAM;

	rvrtp_session_t *s = 
		rte_malloc_socket("Session", sizeof(rvrtp_session_t), RTE_CACHE_LINE_SIZE, rte_socket_id());
	if (s)
	{
		memset(s, 0x0, sizeof(struct rvrtp_session));

		s->fmt = *fmt;
		s->dev = dev;
		s->sn = *sin;

		for (uint32_t i = 0; i < stMainParams.maxRcvThrds; i++)
		{
			if ((stMainParams.rcvThrds[i].thrdSnFirst <= sin->timeslot)
				&& (sin->timeslot < stMainParams.rcvThrds[i].thrdSnLast))
			{
				s->tid = i;
				break;
			}
		}

		switch (fmt->clockRate)
		{
		case 90000:	 //90kHz
			s->tmstampTime = 11111;
			break;
		default:
			return ST_FMT_ERR_BAD_CLK_RATE;
		}

		switch (dev->dev.pacerType)
		{
		case ST_2110_21_TPN:
			s->sn.tprs = (fmt->frameTime - s->sn.trOffset) / fmt->pktsInFrame;
			break;
		case ST_2110_21_TPNL:
		case ST_2110_21_TPW:
			s->sn.tprs = (fmt->frameTime) / fmt->pktsInFrame;
			break;
		default:
			ST_ASSERT;
			break;
		}
		s->sn.frameSize
			= (uint32_t)((uint64_t)s->fmt.height * s->fmt.width * s->fmt.pixelGrpSize) / s->fmt.pixelsInGrp;
		s->sn.trOffset = s->sn.tprs * fmt->pktsInLine * fmt->trOffsetLines;
		s->pktTime = ((fmt->pktSize + 24) * 8) / dev->dev.rateGbps;
		uint32_t remaind = ((fmt->pktSize + 24) * 8) % dev->dev.rateGbps;
		if (remaind >= (dev->dev.rateGbps / 2)) s->pktTime++;

		s->UpdateRtpPkt = RvRtpDummyBuildPacket;

		//good for single line formats
		uint32_t lineHistogramSize = s->fmt.height * sizeof(*s->ctx.lineHistogram);
		//default
		uint32_t fragHistogramSize = s->fmt.height * sizeof(*s->ctx.fragHistogram);
		switch (s->fmt.vscan)
		{
		case ST21_720P:
			switch (s->fmt.pktFmt)
			{
			case ST_INTEL_DLN_RFC4175_PKT:
				s->RecvRtpPkt = RvRtpReceiveFirstPacketsDln720p;
				fragHistogramSize = ST_FRAG_HISTOGRAM_720P_DLN_SZ;
				s->fragPattern = 0x1f1f1f1f1f1f1f1fllu;	 //5 bit version
				lineHistogramSize = s->fmt.height / 2 * sizeof(*s->ctx.lineHistogram);
				break;
			case ST_INTEL_SLN_RFC4175_PKT:	//single line fmt
				s->RecvRtpPkt = RvRtpReceiveFirstPacketsSln720p;
				fragHistogramSize = ST_FRAG_HISTOGRAM_720P_SLN_SZ;
				s->fragPattern = 0x3f3f3f3f3f3f3f3fllu;	 //6 bit version
				break;
			case ST_OTHER_SLN_RFC4175_PKT:	//other vendors
				s->RecvRtpPkt = RvRtpReceiveFirstPackets720p;
				fragHistogramSize = ST_FRAG_HISTOGRAM_720P_SLN_SZ;
				s->fragPattern = 0x3f3f3f3f3f3f3f3fllu;	 //6 bit version
				break;
			default:
				ST_ASSERT;
				break;
			}
			break;
		case ST21_1080P:
			switch (s->fmt.pktFmt)
			{
			case ST_INTEL_DLN_RFC4175_PKT:
				s->RecvRtpPkt = RvRtpReceiveFirstPacketsDln1080p;
				fragHistogramSize = ST_FRAG_HISTOGRAM_1080P_DLN_SZ;
				s->fragPattern = 0xffffffffffffffffllu;
				lineHistogramSize = s->fmt.height / 2 * sizeof(*s->ctx.lineHistogram);
				break;
			case ST_INTEL_SLN_RFC4175_PKT:	//single line fmt
				s->RecvRtpPkt = RvRtpReceiveFirstPacketsSln1080p;
				fragHistogramSize = ST_FRAG_HISTOGRAM_1080P_SLN_SZ;
				s->fragPattern = 0xffffffffffffffffllu;
				break;
			case ST_OTHER_SLN_RFC4175_PKT:	//other vendors
				s->RecvRtpPkt = RvRtpReceiveFirstPackets1080p;
				fragHistogramSize = ST_FRAG_HISTOGRAM_1080P_SLN_SZ;
				s->fragPattern = 0xffffffffffffffffllu;
				break;
			default:
				ST_ASSERT;
				break;
			}
			break;
		case ST21_2160P:
			switch (s->fmt.pktFmt)
			{
			case ST_INTEL_SLN_RFC4175_PKT:
				s->RecvRtpPkt = RvRtpReceiveFirstPacketsSln2160p;
				fragHistogramSize = ST_FRAG_HISTOGRAM_2160P_SLN_SZ;
				s->fragPattern = 0xffffffffffffffffllu;
				break;
			case ST_OTHER_SLN_RFC4175_PKT:	//other vendors
				s->RecvRtpPkt = RvRtpReceiveFirstPackets2160p;
				fragHistogramSize = ST_FRAG_HISTOGRAM_2160P_SLN_SZ;
				s->fragPattern = 0xffffffffffffffffllu;
				break;
			default:
				ST_ASSERT;
				break;
			}
			break;
		case ST21_720I:
			s->RecvRtpPkt = RvRtpReceiveFirstPackets720i;
			fragHistogramSize = ST_FRAG_HISTOGRAM_720I_SLN_SZ;
			s->fragPattern = 0x3f3f3f3fllu;	 //6 bit version, uses uint32_t cursor
			lineHistogramSize = s->fmt.height * sizeof(*s->ctx.lineHistogram);
			s->sn.frameSize /= 2;
			break;
		case ST21_1080I:
			s->RecvRtpPkt = RvRtpReceiveFirstPackets1080i;
			fragHistogramSize = ST_FRAG_HISTOGRAM_1080I_SLN_SZ;
			s->fragPattern = 0xffffffffffffffffllu;
			lineHistogramSize = s->fmt.height * sizeof(*s->ctx.lineHistogram);
			s->sn.frameSize /= 2;
			break;
		case ST21_2160I:
			s->RecvRtpPkt = RvRtpReceiveFirstPackets2160i;
			fragHistogramSize = ST_FRAG_HISTOGRAM_2160I_SLN_SZ;
			s->fragPattern = 0xffffffffffffffffllu;
			lineHistogramSize = s->fmt.height * sizeof(*s->ctx.lineHistogram);
			s->sn.frameSize /= 2;
			break;
		default:
			ST_ASSERT;
			break;
		}
		s->ctx.lineHistogram =
			rte_malloc_socket("Line", lineHistogramSize, RTE_CACHE_LINE_SIZE, rte_socket_id());

		s->ctx.fragHistogram = 
			rte_malloc_socket("Frag", fragHistogramSize, RTE_CACHE_LINE_SIZE, rte_socket_id());

		if ((!s->ctx.lineHistogram) || (!s->ctx.fragHistogram))
		{
			rte_free(s);
			if (s->ctx.lineHistogram) free(s->ctx.lineHistogram);
			if (s->ctx.fragHistogram) free(s->ctx.fragHistogram);
			return ST_NO_MEMORY;
		}

		memset(s->ctx.lineHistogram, 0x0, lineHistogramSize);
		memset(s->ctx.fragHistogram, 0x0, fragHistogramSize);

		s->state = ST_SN_STATE_ON;

		RvRtpClearFrameEbu(s);
		RvRtpClearPacketEbu(s);

		*sout = s;
		return ST_OK;
	}
	return ST_NO_MEMORY;
}

/*****************************************************************************************
 *
 * RvRtpCalculatePacketEbu
 *
 * DESCRIPTION
 * Calculates Cinst and VRX values per each packet
 *
 * RETURNS: epochTmstamp for further use
 */
static inline uint64_t
RvRtpCalculatePacketEbu(rvrtp_session_t *s, uint64_t pktTmstamp, uint64_t pktCnt)
{
	uint64_t epochTmstamp = s->ctx.epochs * s->fmt.frameTime;

	uint64_t tVd = epochTmstamp + s->sn.trOffset;

	if (pktTmstamp > tVd)
	{
		int expectedPkts = (pktTmstamp - tVd) / s->sn.tprs;
		int diffPkts = MAX(0, (int)pktCnt - expectedPkts);
		s->ebu.vrxSum += diffPkts;
		if (s->ebu.vrxMin > diffPkts)
		{
			s->ebu.vrxMin = diffPkts;
		}
		else if (s->ebu.vrxMax < diffPkts)
		{
			s->ebu.vrxMax = diffPkts;
		}
		s->ebu.vrxCnt++;
	}

	if ((pktCnt == 1) || (s->ebu.cinTmstamp == 0))	//store initial tmstamp
	{
		s->ebu.cinTmstamp = pktTmstamp;
	}
	else  //calculate Cinst otherwise
	{
		int64_t diffTime = pktTmstamp - s->ebu.cinTmstamp;
		int expCinPkts = (diffTime / s->sn.tprs) * 1.1f;
		int cin = MAX(0, (int)pktCnt - expCinPkts);
		s->ebu.cinSum += cin;
		if (s->ebu.cinMin > cin)
		{
			s->ebu.cinMin = cin;
		}
		else if (s->ebu.cinMax < cin)
		{
			s->ebu.cinMax = cin;
		}
		s->ebu.cinCnt++;
	}
	return epochTmstamp;
}

/*****************************************************************************************
 *
 * RvRtpCalculateFpo
 *
 * DESCRIPTION
 * Calculates FPO and Lat values per each 1st packet of the frame
 *
 * RETURNS: epochTmstamp for further use
 */
static inline void
RvRtpCalculateFrameEbu(rvrtp_session_t *s, uint32_t rtpTmstamp, uint64_t pktTmstamp)
{
	uint64_t epochs = pktTmstamp / s->fmt.frameTime;
	s->ctx.epochs = epochs;
	uint64_t epochTmstamp = epochs * s->fmt.frameTime;

	{
		uint64_t diffLat = pktTmstamp - epochTmstamp;
		s->ebu.latSum += diffLat;
		if (s->ebu.latMin > diffLat)
		{
			s->ebu.latMin = diffLat;
		}
		else if (s->ebu.latMax < diffLat)
		{
			s->ebu.latMax = diffLat;
		}
		s->ebu.latCnt++;
	}
#ifdef ST_RECV_TIME_PRINT
	else
	{
		RTE_LOG(WARNING, USER3, "Packet bad tmstamp to RTP: of %x when rtp is %x\n", refTmstamp,
				rtpTmstamp);
	}
#endif	
	uint64_t diffTime = pktTmstamp - epochTmstamp;
	s->ebu.fptSum += diffTime;
	if (s->ebu.fptMin > diffTime)
	{
		s->ebu.fptMin = diffTime;
	}
	else if (s->ebu.fptMax < diffTime)
	{
		s->ebu.fptMax = diffTime;
	}
	s->ebu.fptCnt++;
#ifdef ST_RECV_TIME_PRINT
	else
	{
		RTE_LOG(WARNING, USER3, "Packet bad tmstamp to epoch: of %lx when rtp is %lx\n", pktTmstamp,
				epochTmstamp);
	}
#endif
	double frmTime90k = s->fmt.clockRate * s->fmt.frmRateDen / s->fmt.frmRateMul;
	uint64_t tmstamp64 = epochs * frmTime90k;
	uint32_t tmstamp32 = tmstamp64;

	int64_t diffRtp = (tmstamp32 > rtpTmstamp) ? tmstamp32 - rtpTmstamp : rtpTmstamp - tmstamp32;
	s->ebu.tmdSum += diffRtp;
	if (s->ebu.tmdMin > diffRtp)
	{
		s->ebu.tmdMin = diffRtp;
	}
	else if (s->ebu.tmdMax < diffRtp)
	{
		s->ebu.tmdMax = diffRtp;
	}
	s->ebu.tmdCnt++;
	s->ebu.prevPktTmstamp = tmstamp32;
	s->ebu.prevEpochTime = epochTmstamp;
	s->ebu.prevTime = pktTmstamp;

	if (s->ebu.prevRtpTmstamp)
	{
		int diffInc = rtpTmstamp - s->ebu.prevRtpTmstamp;
		s->ebu.tmiSum += diffInc;
		if (s->ebu.tmiMin > diffInc)
		{
			s->ebu.tmiMin = diffInc;
		}
		else if (s->ebu.tmiMax < diffInc)
		{
			s->ebu.tmiMax = diffInc;
		}
		s->ebu.tmiCnt++;
	}
	s->ebu.prevRtpTmstamp = rtpTmstamp;
}

/*****************************************************************************************
 *
 * RvRtpCalculatePacketEbuAvg
 *
 * DESCRIPTION
 * Calculates VRX average values after the frame
 * Calculates Cinst average values after the frame
 *
 * RETURNS: none
 */
static inline void
RvRtpCalculatePacketEbuAvg(rvrtp_session_t *s)
{
	if (s->ebu.vrxCnt)
	{
		s->ebu.vrxAvg = s->ebu.vrxSum / s->ebu.vrxCnt;
	}
	else
	{
		s->ebu.vrxAvg = -1.0f;
	}
	if (s->ebu.cinCnt)
	{
		s->ebu.cinAvg = s->ebu.cinSum / s->ebu.cinCnt;
	}
	else
	{
		s->ebu.cinAvg = -1.0f;
	}
}

/*****************************************************************************************
 *
 * RvRtpCalculateFrameEbuAvg
 *
 * DESCRIPTION
 * Calculates FPO average values after the frame
 * Calculates latency average values after the frame
 * Calculates Timestamp difference average values after the frame 
 * (assesses timestamp accouracy of other vendor)
 * Calculates Timestamp increment average values between 2 frames 
 *
 * RETURNS: none
 */
static inline void
RvRtpCalculateFrameEbuAvg(rvrtp_session_t *s)
{
	if (s->ebu.fptCnt)
	{
		s->ebu.fptAvg = s->ebu.fptSum / s->ebu.fptCnt;
	}
	else
	{
		s->ebu.fptAvg = -1.0f;
	}
	if (s->ebu.tmdCnt)
	{
		s->ebu.tmdAvg = s->ebu.tmdSum / s->ebu.tmdCnt;
	}
	else
	{
		s->ebu.tmdAvg = -1.0f;
	}
	if (s->ebu.tmiCnt)
	{
		s->ebu.tmiAvg = s->ebu.tmiSum / s->ebu.tmiCnt;
	}
	else
	{
		s->ebu.tmiAvg = -1.0f;
	}
	if (s->ebu.latCnt)
	{
		s->ebu.latAvg = s->ebu.latSum / s->ebu.latCnt;
	}
	else
	{
		s->ebu.latAvg = -1.0f;
	}
}

/*****************************************************************************************
 *
 * RvRtpCalculateEbuAvg
 *
 * DESCRIPTION
 * Calculates latency average values after the frame
 *
 * RETURNS: average value
 */
static inline void
RvRtpCalculateEbuAvg(rvrtp_session_t *s)
{
	if (s->sn.frmsRecv % 100 == 0)
	{
		RvRtpCalculatePacketEbuAvg(s);
		RvRtpCalculateFrameEbuAvg(s);

		RTE_LOG(INFO, USER3, "Session %d Cinst AVG %.2f MIN %lu MAX %lu test %s!\n", s->sn.timeslot,
				s->ebu.cinAvg, s->ebu.cinMin, s->ebu.cinMax,
				(s->ebu.cinMax <= 5)	? "PASSED NARROW"
				: (s->ebu.cinMax <= 16) ? "PASSED WIDE"
										: "FAILED");
		RTE_LOG(INFO, USER3, "Session %d VRX AVG %.2f MIN %lu MAX %lu test %s!\n", s->sn.timeslot,
				s->ebu.vrxAvg, s->ebu.vrxMin, s->ebu.vrxMax,
				(s->ebu.vrxMax <= 9)	 ? "PASSED NARROW"
				: (s->ebu.vrxMax <= 720) ? "PASSED WIDE"
										 : "FAILED");
		RTE_LOG(INFO, USER3, "Session %d TRO %u FPT AVG %.2f MIN %lu MAX %lu test %s!\n",
				s->sn.timeslot, s->sn.trOffset, s->ebu.fptAvg, s->ebu.fptMin, s->ebu.fptMax,
				(s->ebu.fptMax < 2*s->sn.trOffset) ? "PASSED" : "FAILED");
		RTE_LOG(INFO, USER3, "Session %d TM inc AVG %.2f MIN %u MAX %u test %s!\n", s->sn.timeslot,
				s->ebu.tmiAvg, s->ebu.tmiMin, s->ebu.tmiMax,
				(s->ebu.tmiMax == s->ebu.tmiMin)				   ? "PASSED"
				: (s->ebu.tmiMax == 1502 && s->ebu.tmiMin == 1501) ? "PASSED"
																   : "FAILED");
		RTE_LOG(INFO, USER3, "Session %u TMD last diff %d Rtp %x Pkt %x MIN %ld MAX %ld test %s!\n", s->sn.timeslot,
				s->ebu.prevRtpTmstamp - s->ebu.prevPktTmstamp, s->ebu.prevRtpTmstamp, 
				s->ebu.prevPktTmstamp, s->ebu.tmdMin, s->ebu.tmdMax,
				(s->ebu.tmdMax < 129) ? "PASSED" : "FAILED");
#ifdef ST_EXTENDED_EBU_LOGS	
		RTE_LOG(INFO, USER3, "Session %u PrevEpochTime %lu frameTime %u EpochCount %u\n", s->sn.timeslot, s->ebu.prevEpochTime, s->fmt.frameTime, (uint32_t)(s->ebu.prevTime / s->fmt.frameTime));
#endif
		RTE_LOG(INFO, USER3, "Session %d LAT AVG %.2f MIN %lu MAX %lu test %s!\n", s->sn.timeslot,
				s->ebu.latAvg, s->ebu.latMin, s->ebu.latMax,
				(s->ebu.latMax < 1000000) ? "PASSED" : "FAILED");

		RvRtpClearPacketEbu(s);
		RvRtpClearFrameEbu(s);
	}
}

/*****************************************************************************************
 *
 * RvRtpIpUdpHdrCheck
  *
 * DESCRIPTION
 * Check IP and UDP hdrs in packet within session context
 *
 * RETURNS: st_status_t
 */
static inline st_status_t
RvRtpIpUdpHdrCheck(rvrtp_session_t *s, struct rte_ipv4_hdr *ip)
{
#ifdef ST_DONT_IGNORE_PKT_CHECK
	struct rte_udp_hdr *udp = (struct rte_udp_hdr *)&ip[1];

	/* Validate the IP & UDP header */
	uint16_t tlen = ntohs(ip->total_length);
	if (tlen != s->fmt.pktSize - s->etherSize)
	{
		s->pktsDrop++;
		s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_IP_LEN)]++;

		RTE_LOG(INFO, USER3, "Packet bad IPLEN: of %u\n", tlen);
		RTE_LOG(INFO, USER3, "Packet bad IPLEN: expected %u\n", s->fmt.pktSize - s->etherSize);
		RTE_LOG(INFO, USER3, "Packet bad IPLEN: pktsDrop %llu\n", (U64)s->pktsDrop);

		return ST_PKT_DROP_BAD_IP_LEN;
	}
	tlen = ntohs(udp->dgram_len);

	if (tlen != s->fmt.pktSize - s->etherSize - sizeof(struct rte_ipv4_hdr))
	{
		s->pktsDrop++;
		s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_UDP_LEN)]++;
		RTE_LOG(INFO, USER3, "Packet bad UDPLEN: pktsDrop %llu\n", (U64)s->pktsDrop);
		return ST_PKT_DROP_BAD_UDP_LEN;
	}
#endif
	return ST_OK;
}

/*****************************************************************************************
 *
 * RvRtpHdrCheck
  *
 * DESCRIPTION
 * Check RFC 4175 RTP hdr in packet within session context
 *
 * RETURNS: st_status_t
 */
static inline st_status_t
RvRtpHdrCheck(rvrtp_session_t *s, struct st_rfc4175_rtp_dual_hdr *rtp, st21_pkt_fmt_t pktFmt, st21_vscan_t vscan)
{
	if ((rtp->version != RVRTP_VERSION_2) || (rtp->csrcCount != 0))
	{
		s->pktsDrop++;
		s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_HDR)]++;
		RTE_LOG(INFO, USER3, "Packet bad RTP HDR: pktsDrop %llu\n", (U64)s->pktsDrop);
		return ST_PKT_DROP_BAD_RTP_HDR;
	}

	if (pktFmt == ST_INTEL_DLN_RFC4175_PKT)
	{
		if (NFIELD_TEST_16_BIT(rtp->line1Offset) == 0)	//shall be 1
		{
			s->pktsDrop++;
			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_LN_CONT)]++;
			RTE_LOG(INFO, USER3, "Packet bad LNCONT of %u pktsDrop %llu\n", 0, (U64)s->pktsDrop);
			return ST_PKT_DROP_BAD_RTP_LN_CONT;
		}
#ifdef ST_DONT_IGNORE_PKT_CHECK
		uint16_t line1LenRtp = ntohs(rtp->line1Length);
		uint16_t line2LenRtp = ntohs(rtp->line2Length);

		if ((line1LenRtp > s->ctx.line1Length) || (line2LenRtp > s->ctx.line2Length))
		{
			s->pktsDrop++;
			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_LN_LEN)]++;
			RTE_LOG(INFO, USER3, "Packet bad RTPLEN of %u pktsDrop %llu\n", line1LenRtp,
					(U64)s->pktsDrop);
			RTE_LOG(INFO, USER3, "Packet bad RTPLEN of %u pktsDrop %llu\n", line2LenRtp,
					(U64)s->pktsDrop);
			return ST_PKT_DROP_BAD_RTP_LN_LEN;
		}
#endif
		s->ctx.line1Number = ntohs(NFIELD_MASK_15_BITS(rtp->line1Number));
		s->ctx.line2Number = ntohs(NFIELD_MASK_15_BITS(rtp->line2Number));

		s->ctx.line1Offset = ntohs(NFIELD_MASK_15_BITS(rtp->line1Offset));
		s->ctx.line2Offset = ntohs(NFIELD_MASK_15_BITS(rtp->line2Offset));

		if ((vscan == ST21_2160I) || (vscan == ST21_1080I) || (vscan == ST21_720I))
		{
			s->ctx.fieldId = NFIELD_GET_16_BIT(rtp->line1Number);
		}
		else
		{
			s->ctx.fieldId = 2;
		}

#ifdef ST_DONT_IGNORE_PKT_CHECK
		if ((s->ctx.line1Number >= s->fmt.height) || (s->ctx.line2Number >= s->fmt.height))
		{
			s->pktsDrop++;
			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_LN_NUM)]++;
			RTE_LOG(INFO, USER3, "Packet bad RTPLN NUMBER of %u pktsDrop %llu\n",
					s->ctx.line1Number, (U64)s->pktsDrop);
			RTE_LOG(INFO, USER3, "Packet bad RTPLN NUMBER of %u pktsDrop %llu\n",
					s->ctx.line2Number, (U64)s->pktsDrop);
			return ST_PKT_DROP_BAD_RTP_LN_NUM;
		}
		if ((s->ctx.line1Offset + s->fmt.pixelsInPkt > s->fmt.width)
			|| (s->ctx.line2Offset + s->fmt.pixelsInPkt > s->fmt.width))
		{
			s->pktsDrop++;
			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_OFFSET)]++;
			RTE_LOG(INFO, USER3, "Packet bad LN OFFSET of %u pktsDrop %llu\n", s->ctx.line1Offset,
					(U64)s->pktsDrop);
			RTE_LOG(INFO, USER3, "Packet bad LN OFFSET of %u pktsDrop %llu\n", s->ctx.line2Offset,
					(U64)s->pktsDrop);
			return ST_PKT_DROP_BAD_RTP_OFFSET;
		}
#endif
	}
	else
	{
		if (NFIELD_TEST_16_BIT(rtp->line1Offset))  //here shall be 0 if single line packet
		{
			s->pktsDrop++;
			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_LN_CONT)]++;
			RTE_LOG(INFO, USER3, "Packet bad LNCONT of %u pktsDrop %llu\n", 1, (U64)s->pktsDrop);
			return ST_PKT_DROP_BAD_RTP_LN_CONT;
		}
#ifdef ST_DONT_IGNORE_PKT_CHECK
		uint16_t line1LenRtp = ntohs(rtp->line1Length);

		if (line1LenRtp > s->ctx.line1Length)
		{
			s->pktsDrop++;
			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_LN_LEN)]++;
			RTE_LOG(INFO, USER3, "Packet bad RTPLEN of %u pktsDrop %llu\n", line1LenRtp,
					(U64)s->pktsDrop);
			return ST_PKT_DROP_BAD_RTP_LN_LEN;
		}
#endif

		s->ctx.line1Number = ntohs(NFIELD_MASK_15_BITS(rtp->line1Number));
		s->ctx.line1Offset = ntohs(NFIELD_MASK_15_BITS(rtp->line1Offset));

		if ((vscan == ST21_2160I) || (vscan == ST21_1080I) || (vscan == ST21_720I))
		{
			s->ctx.fieldId = NFIELD_GET_16_BIT(rtp->line1Number);
		}
		else
		{
			s->ctx.fieldId = 2;
		}

#ifdef ST_DONT_IGNORE_PKT_CHECK
		if (s->ctx.line1Number >= s->fmt.height)
		{
			s->pktsDrop++;
			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_LN_NUM)]++;
			RTE_LOG(INFO, USER3, "Packet bad RTPLN NUMBER of %u pktsDrop %llu\n",
					s->ctx.line1Number, (U64)s->pktsDrop);
			return ST_PKT_DROP_BAD_RTP_LN_NUM;
		}
		if (s->ctx.line1Offset + s->fmt.pixelsInPkt > s->fmt.width)
		{
			s->pktsDrop++;
			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_OFFSET)]++;
			RTE_LOG(INFO, USER3, "Packet bad LN OFFSET of %u pktsDrop %llu\n", s->ctx.line1Offset,
					(U64)s->pktsDrop);
			return ST_PKT_DROP_BAD_RTP_OFFSET;
		}
#endif
	}
	return ST_OK;
}

/*****************************************************************************************
 *
 * RvRtpDropTmstampPush inline - manages stack of timestamps to drop
 */
static inline void
RvRtpDropTmstampPush(rvrtp_session_t *s, uint32_t rtpTmstamp)
{
	s->tmstampToDrop[1] = s->tmstampToDrop[0];
	s->tmstampToDrop[0] = rtpTmstamp;
	s->ctx.tmstamp = 0;
}

/*****************************************************************************************
 *
 * RvRtpDropFrameAtTmstamp template inline
 */
static inline st_status_t
RvRtpDropFrameAtTmstamp(rvrtp_session_t *s, uint32_t rtpTmstamp, st_status_t status)
{
	RvRtpDropTmstampPush(s, rtpTmstamp);
	s->pktsDrop++;
	s->frmsDrop++;

	assert(0 <= ST_PKT_DROP(status));
	assert(ST_PKT_DROP(status) < (sizeof(s->sn.pktsDrop) / sizeof(*s->sn.pktsDrop)));

	s->sn.pktsDrop[ST_PKT_DROP(status)]++;

	assert(0 <= ST_FRM_DROP(status));
	assert(ST_FRM_DROP(status) < (sizeof(s->sn.frmsDrop) / sizeof(*s->sn.frmsDrop)));

	s->sn.frmsDrop[ST_FRM_DROP(status)]++;
	return status;
}

/*****************************************************************************************
 *
 * RvRtpClearFragHistInline template resolving inline
 */
static inline void
RvRtpClearFragHistInline(rvrtp_session_t *s, st21_vscan_t vscan, st21_pkt_fmt_t pktFmt)
{
	if (vscan == ST21_2160P)
	{
		memset(s->ctx.fragHistogram, 0, ST_FRAG_HISTOGRAM_2160P_SLN_SZ);
	}
	else if ((vscan == ST21_1080P) && (pktFmt == ST_INTEL_DLN_RFC4175_PKT))
	{
		memset(s->ctx.fragHistogram, 0, ST_FRAG_HISTOGRAM_1080P_DLN_SZ);
	}
	else if ((vscan == ST21_1080P) && (pktFmt == ST_INTEL_SLN_RFC4175_PKT))
	{
		memset(s->ctx.fragHistogram, 0, ST_FRAG_HISTOGRAM_1080P_SLN_SZ);
	}
	else if ((vscan == ST21_1080P) && (pktFmt == ST_OTHER_SLN_RFC4175_PKT))
	{
		memset(s->ctx.fragHistogram, 0, ST_FRAG_HISTOGRAM_1080P_SLN_SZ);
	}
	else if ((vscan == ST21_720P) && (pktFmt == ST_INTEL_DLN_RFC4175_PKT))
	{
		memset(s->ctx.fragHistogram, 0, ST_FRAG_HISTOGRAM_720P_DLN_SZ);
	}
	else if ((vscan == ST21_720P) && (pktFmt == ST_INTEL_SLN_RFC4175_PKT))
	{
		memset(s->ctx.fragHistogram, 0, ST_FRAG_HISTOGRAM_720P_SLN_SZ);
	}
	else if ((vscan == ST21_720P) && (pktFmt == ST_OTHER_SLN_RFC4175_PKT))
	{
		memset(s->ctx.fragHistogram, 0, ST_FRAG_HISTOGRAM_720P_SLN_SZ);
	}
	else if (vscan == ST21_2160I)
	{
		memset(s->ctx.fragHistogram, 0, ST_FRAG_HISTOGRAM_2160I_SLN_SZ);
	}
	else if (vscan == ST21_1080I)
	{
		memset(s->ctx.fragHistogram, 0, ST_FRAG_HISTOGRAM_1080I_SLN_SZ);
	}
	else if (vscan == ST21_720I)
	{
		memset(s->ctx.fragHistogram, 0, ST_FRAG_HISTOGRAM_720I_SLN_SZ);
	}
	else
	{
		ST_ASSERT;
	}
}

/*****************************************************************************************
 *
 * RvRtpFixVideoFrameDln720p inline
 * Intel standard packets
 */
static inline void
RvRtpFixVideoFrameDln720p(rvrtp_session_t *s)
{
	uint64_t maxLine = s->sn.frmsRecv * s->fmt.pktsInLine;

	for (uint32_t i = 0; i < 720; i += 2)  // now fix frame 720p
	{
		uint32_t idx = i / 2;
		if (s->ctx.lineHistogram[idx] != (uint32_t)maxLine)
		{
			for (uint32_t j = 0; j < ST_FMT_HD720_PKTS_IN_DLN; j++)
			{
				if (((uint8_t)s->ctx.fragHistogram[idx] & (1 << j)) == 0)
				{
					uint32_t offsetLn1 = i * s->ctx.line1Size + j * s->ctx.line1Length;
					uint32_t offsetLn2 = offsetLn1 + s->ctx.line1Size;
					memcpy(&s->consBufs[FRAME_CURR].buf[offsetLn1],
						   &s->consBufs[FRAME_PREV].buf[offsetLn1], ST_FMT_HD720_PKT_DLN_SZ);
					memcpy(&s->consBufs[FRAME_CURR].buf[offsetLn2],
						   &s->consBufs[FRAME_PREV].buf[offsetLn2], ST_FMT_HD720_PKT_DLN_SZ);
				}
			}
			s->ctx.lineHistogram[idx] = (uint32_t)maxLine;
		}
	}
}

/*****************************************************************************************
 *
 * RvRtpFixVideoFrameSln720p inline
 * Intel standard packets
 */
static inline void
RvRtpFixVideoFrameSln720p(rvrtp_session_t *s)
{
	uint64_t maxLine = s->sn.frmsRecv * s->fmt.pktsInLine;

	for (uint32_t i = 0; i < 720; i++)	// now fix frame 720p, 2 pkts of 1200 and then 800
	{
		if (s->ctx.lineHistogram[i] != (uint32_t)maxLine)
		{
			for (uint32_t j = 0; j < ST_FMT_HD720_PKTS_IN_SLN; j++)
			{
				uint8_t bitToTest = (1 << j) << (3 * (i & 0x1));
				if (((uint8_t)s->ctx.fragHistogram[i / 2] & bitToTest) == 0)
				{
					if ((1 << j) & 0x1b)  //1200, 1260 payload case
					{
						uint32_t offsetLn1 = i * s->ctx.line1Size + j * s->ctx.line1Length;
						memcpy(&s->consBufs[FRAME_CURR].buf[offsetLn1],
							   &s->consBufs[FRAME_PREV].buf[offsetLn1], s->ctx.line1Length);
					}
					else  //800, 680 payload part
					{
						uint32_t offsetLn1 = i * s->ctx.line1Size + j * s->ctx.line1Length;
						uint32_t remaind = s->ctx.line1Size - j * s->ctx.line1Length;
						memcpy(&s->consBufs[FRAME_CURR].buf[offsetLn1],
							   &s->consBufs[FRAME_PREV].buf[offsetLn1], remaind);
					}
				}
			}
			s->ctx.lineHistogram[i] = (uint32_t)maxLine;
		}
	}
}

/*****************************************************************************************
 *
 * RvRtpFixVideoFrame720p inline
 * Other vendors packets with not all equal lengths
 */
static inline void
RvRtpFixVideoFrame720p(rvrtp_session_t *s)
{
	uint64_t maxLine = s->sn.frmsRecv * s->fmt.pktsInLine;

	for (uint32_t i = 0; i < 720; i++)	// now fix frame 720p
	{
		if (s->ctx.lineHistogram[i] != (uint32_t)maxLine)
		{
			for (uint32_t j = 0; j < ST_FMT_HD720_PKTS_IN_SLN; j++)
			{
				uint8_t bitToTest = (1 << j) << (3 * (i & 0x1));
				if (((uint8_t)s->ctx.fragHistogram[i / 2] & bitToTest) == 0)
				{
					if ((1 << j) & 0x1b)  //1200, 1260 payload case
					{
						uint32_t offsetLn1 = i * s->ctx.line1Size + j * s->ctx.line1Length;
						memcpy(&s->consBufs[FRAME_CURR].buf[offsetLn1],
							   &s->consBufs[FRAME_PREV].buf[offsetLn1], s->ctx.line1Length);
					}
					else  //800, 680 payload part
					{
						uint32_t offsetLn1 = i * s->ctx.line1Size + j * s->ctx.line1Length;
						uint32_t remaind = s->ctx.line1Size - j * s->ctx.line1Length;
						memcpy(&s->consBufs[FRAME_CURR].buf[offsetLn1],
							   &s->consBufs[FRAME_PREV].buf[offsetLn1], remaind);
					}
				}
			}
			s->ctx.lineHistogram[i] = (uint32_t)maxLine;
		}
	}
}

/*****************************************************************************************
 *
 * RvRtpFixVideoFrame720i inline
 * Other vendors packets with not all equal lengths
 */
static inline void
RvRtpFixVideoFrame720i(rvrtp_session_t *s)
{
	uint64_t maxLine = s->sn.frmsRecv * s->fmt.pktsInLine;

	for (uint32_t i = 0; i < 360; i++) // now fix frame 720i, 2 pkts of some length equal then remaind completing the line
	{
		if (s->ctx.lineHistogram[i] != (uint32_t)maxLine)
		{
			for (uint32_t j = 0; j < ST_FMT_HD720_PKTS_IN_SLN; j++)
			{
				uint8_t bitToTest = (1 << j) << (3 * (i & 0x1));
				if (((uint8_t)s->ctx.fragHistogram[i / 2] & bitToTest) == 0)
				{
					if ((1 << j) & 0x1b)  //1200, 1260 payload case
					{
						uint32_t offsetLn1 = i * s->ctx.line1Size + j * s->ctx.line1Length;
						memcpy(&s->consBufs[FRAME_CURR].buf[offsetLn1],
							   &s->consBufs[FRAME_PREV].buf[offsetLn1], s->ctx.line1Length);
					}
					else  //800, 680 payload part
					{
						uint32_t offsetLn1 = i * s->ctx.line1Size + j * s->ctx.line1Length;
						uint32_t remaind = s->ctx.line1Size - j * s->ctx.line1Length;
						memcpy(&s->consBufs[FRAME_CURR].buf[offsetLn1],
							   &s->consBufs[FRAME_PREV].buf[offsetLn1], remaind);
					}
				}
			}
			s->ctx.lineHistogram[i] = (uint32_t)maxLine;
		}
	}
}

/*****************************************************************************************
 *
 * RvRtpFixVideoFrameDln1080p inline
 * Intel standard packets
 */
static inline void
RvRtpFixVideoFrameDln1080p(rvrtp_session_t *s)
{
	uint64_t maxLine = s->sn.frmsRecv * s->fmt.pktsInLine;

	for (uint32_t i = 0; i < 1080; i += 2)	// now fix frame
	{
		if ((s->ctx.lineHistogram[i / 2] != (uint32_t)maxLine)
			&& ((uint8_t)s->ctx.fragHistogram[i / 2] != 0xff))
		{
			for (uint32_t j = 0; j < ST_FMT_HD1080_PKTS_IN_DLN; j++)
			{
				if (((uint8_t)s->ctx.fragHistogram[i / 2] & (1 << j)) == 0)
				{
					uint32_t offsetLn1 = i * s->ctx.line1Size + j * s->ctx.line1Length;
					uint32_t offsetLn2 = offsetLn1 + s->ctx.line1Size;
					memcpy(&s->consBufs[FRAME_CURR].buf[offsetLn1],
						   &s->consBufs[FRAME_PREV].buf[offsetLn1], ST_FMT_HD1080_PKT_DLN_SZ);
					memcpy(&s->consBufs[FRAME_CURR].buf[offsetLn2],
						   &s->consBufs[FRAME_PREV].buf[offsetLn2], ST_FMT_HD1080_PKT_DLN_SZ);
				}
			}
			s->ctx.lineHistogram[i / 2] = (uint32_t)maxLine;
		}
	}
}

/*****************************************************************************************
 *
 * RvRtpFixVideoFrameSln1080p inline
 * Intel standard packets
 */
static inline void
RvRtpFixVideoFrameSln1080p(rvrtp_session_t *s)
{
	uint64_t maxLine = s->sn.frmsRecv * s->fmt.pktsInLine;

	for (uint32_t l = 0; l < 1080; l += 8)	// now fix frame
	{
		uint64_t *p = (uint64_t *)&s->ctx.fragHistogram[l];
		if (*p == s->fragPattern) continue;

		for (uint32_t k = 0; k < 8; k++)
		{
			uint32_t i = l + k;
			for (uint32_t j = 0; j < ST_FMT_HD1080_PKTS_IN_SLN; j++)
			{
				uint8_t bitToTest = (1 << j) << (4 * (i & 0x1));
				if (((uint8_t)s->ctx.fragHistogram[i / 2] & bitToTest) == 0)
				{
					uint32_t offset = i * s->ctx.line1Size + j * s->ctx.line1Length;
					memcpy(&s->consBufs[FRAME_CURR].buf[offset],
						   &s->consBufs[FRAME_PREV].buf[offset], ST_FMT_HD1080_PKT_SLN_SZ);
				}
			}
			s->ctx.lineHistogram[i] = (uint32_t)maxLine;
		}
	}
}

/*****************************************************************************************
 *
 * RvRtpFixVideoFrame1080p inline
 * Other vendors packets with not all equal lengths
 */
static inline void
RvRtpFixVideoFrame1080p(rvrtp_session_t *s)
{
	uint64_t maxLine = s->sn.frmsRecv * s->fmt.pktsInLine;

	for (uint32_t i = 0; i < 1080; i++)	 // now fix 1080p frame
	{
		if (s->ctx.lineHistogram[i] != (uint32_t)maxLine)
		{
			for (uint32_t j = 0; j < ST_FMT_HD1080_PKTS_IN_SLN; j++)
			{
				uint8_t bitToTest = (1 << j) << (4 * (i & 0x1));
				if (((uint8_t)s->ctx.fragHistogram[i / 2] & bitToTest) == 0)
				{
					uint32_t offset = i * s->ctx.line1Size + j * s->ctx.line1Length;
					if ((1 << j) & 0x77)  //1200, 1260 payload case
					{
						memcpy(&s->consBufs[FRAME_CURR].buf[offset],
							   &s->consBufs[FRAME_PREV].buf[offset], s->ctx.line1Length);
					}
					else  //800, 680 payload part
					{
						uint32_t remaind = s->ctx.line1Size - j * s->ctx.line1Length;
						memcpy(&s->consBufs[FRAME_CURR].buf[offset],
							   &s->consBufs[FRAME_PREV].buf[offset], remaind);
					}
				}
			}
			s->ctx.lineHistogram[i] = (uint32_t)maxLine;
		}
	}
}

/*****************************************************************************************
 *
 * RvRtpFixVideoFrame1080i inline
 * Other vendors packets with not all equal lengths
 */
static inline void
RvRtpFixVideoFrame1080i(rvrtp_session_t *s)
{
	uint64_t maxLine = s->sn.frmsRecv * s->fmt.pktsInLine;

	for (uint32_t i = 0; i < 540; i++)	// now fix interlaced field
	{
		if (s->ctx.lineHistogram[i] != (uint32_t)maxLine)
		{
			for (uint32_t j = 0; j < ST_FMT_HD1080_PKTS_IN_SLN; j++)
			{
				uint8_t bitToTest = (1 << j) << (4 * (i & 0x1));
				if (((uint8_t)s->ctx.fragHistogram[i / 2] & bitToTest) == 0)
				{
					uint32_t offset = i * s->ctx.line1Size + j * s->ctx.line1Length;
					if ((1 << j) & 0x77)  //1200, 1260 payload case
					{
						memcpy(&s->consBufs[FRAME_CURR].buf[offset],
							   &s->consBufs[FRAME_PREV].buf[offset], s->ctx.line1Length);
					}
					else  //800, 680 payload part
					{
						uint32_t remaind = s->ctx.line1Size - j * s->ctx.line1Length;
						memcpy(&s->consBufs[FRAME_CURR].buf[offset],
							   &s->consBufs[FRAME_PREV].buf[offset], remaind);
					}
				}
			}
			s->ctx.lineHistogram[i] = (uint32_t)maxLine;
		}
	}
}

/*****************************************************************************************
 *
 * RvRtpFixVideoFrameSln2160p inline, 
 * Intel standard packet with equal sizes
 */
static inline void
RvRtpFixVideoFrameSln2160p(rvrtp_session_t *s)
{
	uint32_t maxLine = s->sn.frmsRecv * s->fmt.pktsInLine;

	for (uint32_t l = 0; l < 2160; l += 8)	// now fix frame 2160p Intel standard
	{
		uint64_t *p = (uint64_t *)&s->ctx.fragHistogram[l];
		if (*p == s->fragPattern) continue;

		for (uint32_t k = 0; k < 8; k++)
		{
			uint32_t i = l + k;
			for (uint32_t j = 0; j < ST_FMT_UHD2160_PKTS_IN_SLN; j++)
			{
				if (((uint8_t)s->ctx.fragHistogram[i] & (1 << j)) == 0)
				{
					uint32_t offset = i * s->ctx.line1Size + j * s->ctx.line1Length;
					memcpy(&s->consBufs[FRAME_CURR].buf[offset],
						   &s->consBufs[FRAME_PREV].buf[offset], ST_FMT_UHD2160_PKT_SLN_SZ);
				}
			}
			s->ctx.lineHistogram[i] = (uint32_t)maxLine;
		}
	}
}

/*****************************************************************************************
 *
 * RvRtpFixVideoFrame2160p inline, 
 * Other vendors packets with not all equal lengths
 */
static inline void
RvRtpFixVideoFrame2160p(rvrtp_session_t *s)
{
	uint32_t maxLine = s->sn.frmsRecv * s->fmt.pktsInLine;

	for (uint32_t l = 0; l < 2160; l += 8)	// now fix frame 2160p other vendors
	{
		uint64_t *p = (uint64_t *)&s->ctx.fragHistogram[l];
		if (*p == s->fragPattern) continue;

		for (uint32_t k = 0; k < 8; k++)
		{
			uint32_t i = l + k;
			for (uint32_t j = 0; j < ST_FMT_UHD2160_PKTS_IN_SLN; j++)
			{
				if (((uint8_t)s->ctx.fragHistogram[i] & (1 << j)) == 0)
				{
					uint32_t offset = i * s->ctx.line1Size + j * s->ctx.line1Length;
					if ((1 << j) < 0x80)
					{
						rte_memcpy(&s->consBufs[FRAME_CURR].buf[offset],
								   &s->consBufs[FRAME_PREV].buf[offset], s->ctx.line1Length);
					}
					else
					{
						uint32_t remaind = s->ctx.line1Size - j * s->ctx.line1Length;
						rte_memcpy(&s->consBufs[FRAME_CURR].buf[offset],
								   &s->consBufs[FRAME_PREV].buf[offset], remaind);
					}
				}
			}
			s->ctx.lineHistogram[i] = (uint32_t)maxLine;
		}
	}
}

/*****************************************************************************************
 *
 * RvRtpFixVideoFrame2160i inline, 
 * Other vendors packets with not all equal lengths
 */
static inline void
RvRtpFixVideoFrame2160i(rvrtp_session_t *s)
{
	uint32_t maxLine = s->sn.frmsRecv * s->fmt.pktsInLine;

	for (uint32_t l = 0; l < 1080; l += 8)	// now fix frame 2160i other vendors
	{
		uint64_t *p = (uint64_t *)&s->ctx.fragHistogram[l];
		if (*p == s->fragPattern) continue;

		for (uint32_t k = 0; k < 8; k++)
		{
			uint32_t i = l + k;
			for (uint32_t j = 0; j < ST_FMT_UHD2160_PKTS_IN_SLN; j++)
			{
				if (((uint8_t)s->ctx.fragHistogram[i] & (1 << j)) == 0)
				{
					uint32_t offset = i * s->ctx.line1Size + j * s->ctx.line1Length;
					if ((1 << j) < 0x80)
					{
						rte_memcpy(&s->consBufs[FRAME_CURR].buf[offset],
								   &s->consBufs[FRAME_PREV].buf[offset], s->ctx.line1Length);
					}
					else
					{
						uint32_t remaind = s->ctx.line1Size - j * s->ctx.line1Length;
						rte_memcpy(&s->consBufs[FRAME_CURR].buf[offset],
								   &s->consBufs[FRAME_PREV].buf[offset], remaind);
					}
				}
			}
			s->ctx.lineHistogram[i] = (uint32_t)maxLine;
		}
	}
}

/*****************************************************************************************
 *
 * RvRtpFixVideoFrameInline template resolving inline
 */
static inline void
RvRtpFixVideoFrameInline(rvrtp_session_t *s, st21_vscan_t vscan, st21_pkt_fmt_t pktFmt)
{
	if ((vscan == ST21_2160P) && (pktFmt == ST_INTEL_SLN_RFC4175_PKT))
	{
		RvRtpFixVideoFrameSln2160p(s);
	}
	else if ((vscan == ST21_1080P) && (pktFmt == ST_INTEL_DLN_RFC4175_PKT))
	{
		RvRtpFixVideoFrameDln1080p(s);
	}
	else if ((vscan == ST21_1080P) && (pktFmt == ST_INTEL_SLN_RFC4175_PKT))
	{
		RvRtpFixVideoFrameSln1080p(s);
	}
	else if ((vscan == ST21_720P) && (pktFmt == ST_INTEL_DLN_RFC4175_PKT))
	{
		RvRtpFixVideoFrameDln720p(s);
	}
	else if ((vscan == ST21_720P) && (pktFmt == ST_INTEL_SLN_RFC4175_PKT))
	{
		RvRtpFixVideoFrameSln720p(s);
	}
	else if ((vscan == ST21_2160P) && (pktFmt == ST_OTHER_SLN_RFC4175_PKT))
	{
		RvRtpFixVideoFrame2160p(s);
	}
	else if ((vscan == ST21_1080P) && (pktFmt == ST_OTHER_SLN_RFC4175_PKT))
	{
		RvRtpFixVideoFrame1080p(s);
	}
	else if ((vscan == ST21_720P) && (pktFmt == ST_OTHER_SLN_RFC4175_PKT))
	{
		RvRtpFixVideoFrame720p(s);
	}
	else if (vscan == ST21_2160I)
	{
		RvRtpFixVideoFrame2160i(s);
	}
	else if (vscan == ST21_1080I)
	{
		RvRtpFixVideoFrame1080i(s);
	}
	else if (vscan == ST21_720I)
	{
		RvRtpFixVideoFrame720i(s);
	}
	else
	{
		ST_ASSERT;
	}
}

/*****************************************************************************************
 *
 * RvRtpIncompleteDropNCont template inline
 */
static inline st_status_t
RvRtpIncompleteDropNCont(
	rvrtp_session_t *s, 
	uint32_t rtpTmstamp, 
	uint32_t frameId, 
	uint32_t cont, 
	st21_vscan_t vscan, 
	st21_pkt_fmt_t pktFmt)
{
	if (cont)
		RvRtpDropTmstampPush(s, s->consBufs[frameId].tmstamp);
	else
		RvRtpDropTmstampPush(s, rtpTmstamp);

	s->sn.pktsLost[ST_PKT_LOST(ST_PKT_LOST_TIMEDOUT)] += s->fmt.pktsInFrame - s->consBufs[frameId].pkts;
	s->frmsDrop++;
	s->sn.frmsDrop[ST_FRM_DROP(ST_PKT_DROP_INCOMPL_FRAME)]++;

	if (frameId == FRAME_CURR)
	{
		RvRtpClearFragHistInline(s, vscan, pktFmt);
	}
	if (cont)
	{
		s->consBufs[frameId].pkts = 1;
		s->consBufs[frameId].tmstamp = rtpTmstamp;
		s->ctx.data = s->consBufs[frameId].buf;
		s->sn.pktsRecv++;
	}
	else
	{
		s->consBufs[frameId].pkts = 0;
		s->consBufs[frameId].tmstamp = 0;
	}
	return ST_OK;
}

/*****************************************************************************************
 *
 * RvRtpFixCurrentFrame template inline
 */
static inline void
RvRtpFixCurrentFrame(rvrtp_session_t *s, uint32_t rtpTmstamp, st21_vscan_t vscan,
					 st21_pkt_fmt_t pktFmt)
{
#ifdef RX_RECV_DEBUG
	RTE_LOG(INFO, USER3, "Incomplete frame fixed of %lu received pkts %u, shall be %u\n",
			s->sn.frmsRecv + 1, s->consBufs[FRAME_CURR].pkts, s->fmt.pktsInFrame);
#endif

	RvRtpFixVideoFrameInline(s, vscan, pktFmt);

	s->sn.frmsRecv++;
	s->frmsFixed++;

	if (stMainParams.isEbuCheck)
	{
		RvRtpCalculateEbuAvg(s);
	}

	s->cons.St21NotifyFrameRecv(s->cons.appHandle, s->consBufs[FRAME_CURR].buf, rtpTmstamp,
								s->ctx.fieldId);
	s->cons.St21NotifyFrameDone(s->cons.appHandle, s->consBufs[FRAME_PREV].buf, s->ctx.fieldId);
	s->sn.pktsLost[ST_PKT_LOST(ST_PKT_LOST_TIMEDOUT)]
		+= s->fmt.pktsInFrame - s->consBufs[FRAME_CURR].pkts;
	//move active to prev position
	s->consBufs[FRAME_PREV] = s->consBufs[FRAME_CURR];
	s->consBufs[FRAME_CURR].buf = s->cons.St21GetNextFrameBuf(
		s->cons.appHandle, s->consBufs[FRAME_PREV].buf, s->cons.frameSize, s->ctx.fieldId);

	RvRtpClearFragHistInline(s, vscan, pktFmt);
}

/*****************************************************************************************
 *
 * RvRtpReceiveFastCopyInline
 *
 * DESCRIPTION
 * Main function to copy packet's payload and update histogram, solves patterns
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
static inline st_status_t
RvRtpReceiveFastCopyInline(rvrtp_session_t *s, void *rtp, st21_vscan_t vscan, st21_pkt_fmt_t pktFmt,
						   uint32_t whichFrm)
{
	if (pktFmt == ST_INTEL_SLN_RFC4175_PKT)
	{
		/* Single line copy payload */
		st_rfc4175_rtp_single_hdr_t *rtpSingle = (st_rfc4175_rtp_single_hdr_t *)rtp;
		uint8_t *payload = (uint8_t *)&rtpSingle[1];

		uint32_t byteLn1Offset = s->ctx.line1Number * s->ctx.line1Size + 
			(uint32_t)s->ctx.line1Offset / s->fmt.pixelsInGrp * s->ctx.line1PixelGrpSize;

		// solve patterns of vscan
		if (vscan == ST21_2160P)
		{
			if (whichFrm == FRAME_CURR)
			{
				s->ctx.lineHistogram[s->ctx.line1Number]++;
			}
			s->ctx.fragHistogram[s->ctx.line1Number] |= (1 << (s->ctx.line1Offset / s->fmt.pixelsInPkt));

#ifndef ST_MEMCPY_TEST
			/* line 1 payload copy */
			memcpy(&s->ctx.data[byteLn1Offset], payload, ST_FMT_UHD2160_PKT_SLN_SZ);
#endif
		}
		else if (vscan == ST21_1080P)
		{
			if (whichFrm == FRAME_CURR)
			{
				s->ctx.lineHistogram[s->ctx.line1Number]++;
				uint8_t bitToSet = (1 << (s->ctx.line1Offset / s->fmt.pixelsInPkt))
								   << (4 * (s->ctx.line1Number & 0x1));
				s->ctx.fragHistogram[s->ctx.line1Number / 2] |= (1 << bitToSet);
			}
			else
			{
				uint8_t bitToSet = (1 << (s->ctx.line1Offset / s->fmt.pixelsInPkt))
								   << (4 * (s->ctx.line1Number & 0x1));
				s->ctx.fragHistogram[s->ctx.line1Number / 2] |= (1 << bitToSet);
			}
#ifndef ST_MEMCPY_TEST
			/* line 1 payload copy */
			memcpy(&s->ctx.data[byteLn1Offset], payload, ST_FMT_HD1080_PKT_SLN_SZ);
#endif
		}
		else if (vscan == ST21_720P)
		{
			uint32_t lnOffset = s->ctx.line1Offset / s->fmt.pixelsInGrp * s->ctx.line1PixelGrpSize;
			if (whichFrm == FRAME_CURR)
			{
				s->ctx.lineHistogram[s->ctx.line1Number]++;
				uint8_t bitToSet = (1 << (s->ctx.line1Offset / s->fmt.pixelsInPkt))
								   << (3 * (s->ctx.line1Number & 0x1));
				s->ctx.fragHistogram[s->ctx.line1Number / 2] |= (1 << bitToSet);
			}
			else
			{
				uint8_t bitToSet = (1 << (s->ctx.line1Offset / s->fmt.pixelsInPkt))
								   << (3 * (s->ctx.line1Number & 0x1));
				s->ctx.fragHistogram[s->ctx.line1Number / 2] |= (1 << bitToSet);
			}

			uint16_t line1LenRtp = ntohs(rtpSingle->line1Length);
			if (line1LenRtp + lnOffset > s->ctx.line1Size)
			{
				s->pktsDrop++;
				s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_LN_LEN)]++;
				RTE_LOG(INFO, USER3, "Packet bad RTPLEN of %u pktsDrop %llu\n", line1LenRtp,
						(U64)s->pktsDrop);
				return ST_PKT_DROP_BAD_RTP_LN_LEN;
			}
#ifndef ST_MEMCPY_TEST
			/* line 1 payload copy */
			memcpy(&s->ctx.data[byteLn1Offset], payload, line1LenRtp);
#endif
		}
	}
	else if (pktFmt == ST_INTEL_DLN_RFC4175_PKT)  //dual line packets here
	{
		st_rfc4175_rtp_dual_hdr_t *rtpDual = (st_rfc4175_rtp_dual_hdr_t *)rtp;
		/* Dual line copy payload */
		uint8_t *payload = (uint8_t *)&rtpDual[1];
		uint32_t byteLn1Offset =
			s->ctx.line1Number * s->ctx.line1Size + (uint32_t)s->ctx.line1Offset / s->fmt.pixelsInGrp * s->ctx.line1PixelGrpSize;
		uint32_t byteLn2Offset =
			s->ctx.line2Number * s->ctx.line2Size + (uint32_t)s->ctx.line2Offset / s->fmt.pixelsInGrp * s->ctx.line2PixelGrpSize;

		if (whichFrm == FRAME_CURR)
		{
			s->ctx.lineHistogram[s->ctx.line1Number/2]++;
		}
		s->ctx.fragHistogram[s->ctx.line1Number/2] |= (1 << (s->ctx.line1Offset / s->fmt.pixelsInPkt));

		if (vscan == ST21_1080P)
		{
#ifndef ST_MEMCPY_TEST
			/* line 1 */
			memcpy(&s->ctx.data[byteLn1Offset], payload, ST_FMT_HD1080_PKT_DLN_SZ);
			/* line 2 */
			memcpy(&s->ctx.data[byteLn2Offset], &payload[s->ctx.line1Length],
				   ST_FMT_HD1080_PKT_DLN_SZ);
#endif
		}
		else if (vscan == ST21_720P)
		{
#ifndef ST_MEMCPY_TEST
			/* line 1 */
			memcpy(&s->ctx.data[byteLn1Offset], payload, ST_FMT_HD720_PKT_DLN_SZ);
			/* line 2 */
			memcpy(&s->ctx.data[byteLn2Offset], &payload[s->ctx.line1Length],
				   ST_FMT_HD720_PKT_DLN_SZ);
#endif
		}
		else
		{
			ST_ASSERT;
		}
	}
	else if (pktFmt == ST_OTHER_SLN_RFC4175_PKT)  //open standard packets here
	{
		/* Single line copy payload */
		st_rfc4175_rtp_single_hdr_t *rtpSingle = (st_rfc4175_rtp_single_hdr_t *)rtp;
		uint8_t *payload = (uint8_t *)&rtpSingle[1];
		uint32_t byteLn1Offset = (uint32_t)s->ctx.line1Offset / s->fmt.pixelsInGrp * s->ctx.line1PixelGrpSize;
		uint16_t line1LenRtp = ntohs(rtpSingle->line1Length);

		if (line1LenRtp + byteLn1Offset > s->ctx.line1Size)
		{
			s->pktsDrop++;
			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_LN_LEN)]++;
			RTE_LOG(INFO, USER3, "Packet bad RTPLEN of %u pktsDrop %llu\n", line1LenRtp,
					(U64)s->pktsDrop);
			return ST_PKT_DROP_BAD_RTP_LN_LEN;
		}
		byteLn1Offset += s->ctx.line1Number * s->ctx.line1Size;
#ifndef ST_MEMCPY_TEST
		/* line 1 payload copy */
		rte_memcpy(&s->ctx.data[byteLn1Offset], payload, line1LenRtp);
#endif
		// solve patterns of vscan
		if ((vscan == ST21_2160P) || (vscan == ST21_2160I))
		{
			if (whichFrm == FRAME_CURR)
			{
				s->ctx.lineHistogram[s->ctx.line1Number]++;
			}
			s->ctx.fragHistogram[s->ctx.line1Number] |= (1 << (s->ctx.line1Offset / s->fmt.pixelsInPkt));
		}
		else if ((vscan == ST21_1080P) || (vscan == ST21_1080I))  //1080p and 1080i cases
		{
			if (whichFrm == FRAME_CURR)
			{
				s->ctx.lineHistogram[s->ctx.line1Number]++;
				uint8_t bitToSet = (1 << (s->ctx.line1Offset / s->fmt.pixelsInPkt))
								   << (4 * (s->ctx.line1Number & 0x1));
				s->ctx.fragHistogram[s->ctx.line1Number / 2] |= (1 << bitToSet);
			}
			else
			{
				uint8_t bitToSet = (1 << (s->ctx.line1Offset / s->fmt.pixelsInPkt))
								   << (4 * (s->ctx.line1Number & 0x1));
				s->ctx.fragHistogram[s->ctx.line1Number / 2] |= (1 << bitToSet);
			}
		}
		else if ((vscan == ST21_720P) || (vscan == ST21_720I))
		{
			if (whichFrm == FRAME_CURR)
			{
				s->ctx.lineHistogram[s->ctx.line1Number]++;
				uint8_t bitToSet = (1 << (s->ctx.line1Offset / s->fmt.pixelsInPkt))
								   << (3 * (s->ctx.line1Number & 0x1));
				s->ctx.fragHistogram[s->ctx.line1Number / 2] |= (1 << bitToSet);
			}
			else
			{
				uint8_t bitToSet = (1 << (s->ctx.line1Offset / s->fmt.pixelsInPkt))
								   << (3 * (s->ctx.line1Number & 0x1));
				s->ctx.fragHistogram[s->ctx.line1Number / 2] |= (1 << bitToSet);
			}
		}
	}
	return ST_OK;
}

/*****************************************************************************************
 *
 * RvRtpReceiveFastFragCheckInline
 *
 * DESCRIPTION
 * Main function to processes packet within session context
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
static inline st_status_t
RvRtpReceiveFastFragCheckInline(rvrtp_session_t *s, st21_vscan_t vscan, st21_pkt_fmt_t pktFmt)
{
	// solve patterns of vscan
	if (vscan == ST21_2160P)
	{
		for (uint32_t i = 0; i < 2160; i += 8)
		{
			uint64_t *p = (uint64_t *)&s->ctx.fragHistogram[i];
			if (*p == s->fragPattern) continue;
			//incomplete yet
			return ST_PKT_DROP_INCOMPL_FRAME;
		}
	}
	else if (vscan == ST21_1080P)
	{
		for (uint32_t i = 0; i < 1080 / 2; i += 8)
		{
			uint64_t *p = (uint64_t *)&s->ctx.fragHistogram[i];
			if (*p == s->fragPattern) continue;
			//incomplete yet
			return ST_PKT_DROP_INCOMPL_FRAME;
		}
	}
	else if ((vscan == ST21_720P) && (pktFmt == ST_INTEL_DLN_RFC4175_PKT))
	{
		for (uint32_t i = 0; i < 720 / 2; i += 8)
		{
			uint64_t *p = (uint64_t *)&s->ctx.fragHistogram[i];
			if (*p == s->fragPattern) continue;
			//incomplete yet
			return ST_PKT_DROP_INCOMPL_FRAME;
		}
	}
	else if (vscan == ST21_720P) 
	{
		for (uint32_t i = 0; i < 720; i += 8)
		{
			uint64_t *p = (uint64_t *)&s->ctx.fragHistogram[i];
			if (*p == s->fragPattern) continue;
			//incomplete yet
			return ST_PKT_DROP_INCOMPL_FRAME;
		}
	}
	else if (vscan == ST21_2160I)
	{
		for (uint32_t i = 0; i < 1080; i += 8)
		{
			uint64_t *p = (uint64_t *)&s->ctx.fragHistogram[i];
			if (*p == s->fragPattern) continue;
			return ST_PKT_DROP_INCOMPL_FRAME;
		}
	}
	else if (vscan == ST21_1080I)
	{
		for (uint32_t i = 0; i < 264; i += 8)
		{
			uint64_t *p = (uint64_t *)&s->ctx.fragHistogram[i];
			if (*p == s->fragPattern) continue;
			return ST_PKT_DROP_INCOMPL_FRAME;
		}
		for (uint32_t i = 264; i < 270; i++)
		{
			if (s->ctx.fragHistogram[i] == 0xff) continue;
			return ST_PKT_DROP_INCOMPL_FRAME;
		}
	}
	else if (vscan == ST21_720I) 
	{
		for (uint32_t i = 0; i < 180; i += 4)
		{
			uint32_t *p = (uint32_t *)&s->ctx.fragHistogram[i];
			if (*p == (uint32_t)s->fragPattern) continue;
			return ST_PKT_DROP_INCOMPL_FRAME;
		}
	}
	return ST_OK;
}

/*****************************************************************************************
 *
 * RvRtpReceiveNextPacketsInline
 *
 * DESCRIPTION
 * Main function to processes packets within session context
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveNextPacketsInline(
    rvrtp_session_t *s, 
	struct rte_mbuf *m, 
	st21_vscan_t vscan, 
	st21_pkt_fmt_t pktFmt)
{
	struct rte_ipv4_hdr *ip = rte_pktmbuf_mtod_offset(m, struct rte_ipv4_hdr *, 14);
	struct rte_udp_hdr *udp = rte_pktmbuf_mtod_offset(m, struct rte_udp_hdr *, 34);
	struct st_rfc4175_rtp_dual_hdr *rtp = (struct st_rfc4175_rtp_dual_hdr *)&udp[1];

	s->ctx.data = NULL;	 //pointer to right frameBuffer

	/* Validate the IP & UDP & RTP header */
	st_status_t res;
	if (((res = RvRtpIpUdpHdrCheck(s, ip)) != ST_OK) ||
		((res = RvRtpHdrCheck(s, rtp, pktFmt, vscan)) != ST_OK)
		)
	{
		return res;
	}

	uint32_t rtpTmstamp = ntohl(rtp->tmstamp);

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

	if (rtpTmstamp == s->ctx.tmstamp)  //tmstamp match most cases here
	{
#ifdef ST_DONT_IGNORE_PKT_CHECK
		if (s->consBufs[FRAME_CURR].tmstamp == rtpTmstamp)
#endif
		{
			s->ctx.data = s->consBufs[FRAME_CURR].buf;
			s->consBufs[FRAME_CURR].pkts++;
			s->sn.pktsRecv++;
		}
#ifdef ST_DONT_IGNORE_PKT_CHECK
		else
		{
			RTE_LOG(INFO, USER3, "Packet tmstamp of %u while expected %u matched GEN_ERR 2\n",
					rtpTmstamp, s->consBufs[FRAME_CURR].tmstamp);
			s->pktsDrop++;
			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_TMSTAMP)]++;
			return ST_PKT_DROP_BAD_RTP_TMSTAMP;
		}
#endif
		if (stMainParams.isEbuCheck)
		{
			RvRtpCalculatePacketEbu(s, m->timestamp, s->consBufs[FRAME_CURR].pkts);
		}
	}
	else if ((rtpTmstamp > s->ctx.tmstamp) ||
		((rtpTmstamp & (0x1 << 31)) < (s->ctx.tmstamp & (0x1 << 31))))//new frame condition
	{
		if (s->consBufs[FRAME_CURR].tmstamp == 0)
		{
			//First time 2nd frame
			if (unlikely(s->consBufs[FRAME_CURR].buf == NULL))
			{
				s->consBufs[FRAME_CURR].buf =
					s->cons.St21GetNextFrameBuf(s->cons.appHandle, s->consBufs[FRAME_PREV].buf, s->cons.frameSize, s->ctx.fieldId);
				if (unlikely(s->consBufs[FRAME_CURR].buf == NULL))
				{
					return RvRtpDropFrameAtTmstamp(s, rtpTmstamp, ST_PKT_DROP_NO_FRAME_BUF);
				}
			}
			s->consBufs[FRAME_CURR].pkts = 1;
			s->ctx.data = s->consBufs[FRAME_CURR].buf;
			s->consBufs[FRAME_CURR].tmstamp = rtpTmstamp;
			s->sn.pktsRecv++;
		}
		else
		{
			//current frame is incomplete
			if (s->consBufs[FRAME_CURR].pkts + ST_PKTS_LOSS_ALLOWED >= s->fmt.pktsInFrame)
			{
				RvRtpFixCurrentFrame(s, rtpTmstamp, vscan, pktFmt);
				//here receive the frame to current buffer then
				if (unlikely(s->consBufs[FRAME_CURR].buf == NULL))
				{
					return RvRtpDropFrameAtTmstamp(s, rtpTmstamp, ST_PKT_DROP_NO_FRAME_BUF);
				}
				s->consBufs[FRAME_CURR].pkts = 1;
				s->consBufs[FRAME_CURR].tmstamp = rtpTmstamp;
				s->ctx.data = s->consBufs[FRAME_CURR].buf;
				s->sn.pktsRecv++;
			}
			else
			{
#ifdef RX_RECV_DEBUG
				RTE_LOG(INFO, USER3, "Incomplete next frame of %lu received pkts %u, shall be %u\n",
						s->sn.frmsRecv + 1, s->consBufs[FRAME_CURR].pkts, s->fmt.pktsInFrame);
#endif
				RvRtpIncompleteDropNCont(s, rtpTmstamp, FRAME_CURR, TRUE, vscan, pktFmt);
			}
		}
		if (stMainParams.isEbuCheck)
		{
			RvRtpCalculateFrameEbu(s, rtpTmstamp, m->timestamp);
		}
	}
	else if ((rtpTmstamp == s->tmstampToDrop[0]) || (rtpTmstamp == s->tmstampToDrop[1]))
	{
		s->pktsDrop++;
		s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_NO_FRAME_BUF)]++;
		return ST_PKT_DROP_NO_FRAME_BUF;
	}
	//out of order packet arrived - drop it silently
	else if (s->ctx.tmstamp > rtpTmstamp)
	{
		s->pktsDrop++;
		s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_TMSTAMP)]++;
		return ST_PKT_DROP_BAD_RTP_TMSTAMP;
	}

	s->ctx.tmstamp = rtpTmstamp;

	res = RvRtpReceiveFastCopyInline(s, rtp, vscan, pktFmt, FRAME_CURR);
	if (unlikely(res != ST_OK))
	{
		return res;
	}

	/* is frame complete ?*/
	if (rtp->marker)
	{
		if (s->consBufs[FRAME_CURR].tmstamp == rtpTmstamp)
		{
			if (s->consBufs[FRAME_CURR].pkts == s->fmt.pktsInFrame)
			{
				s->sn.frmsRecv++;
				s->cons.St21NotifyFrameRecv(s->cons.appHandle, s->ctx.data, rtpTmstamp,
											s->ctx.fieldId);
				if (stMainParams.isEbuCheck)
				{
					RvRtpCalculateEbuAvg(s);
				}
				if (s->consBufs[FRAME_PREV].buf)
				{
					s->cons.St21NotifyFrameDone(s->cons.appHandle, s->consBufs[FRAME_PREV].buf,
												s->ctx.fieldId);
				}
				memcpy(&s->consBufs[FRAME_PREV], &s->consBufs[FRAME_CURR], sizeof(rvrtp_bufs_t));
				s->consBufs[FRAME_CURR].pkts = 0;
				s->consBufs[FRAME_CURR].tmstamp = 0;
				s->consBufs[FRAME_CURR].buf
					= s->cons.St21GetNextFrameBuf(s->cons.appHandle, s->consBufs[FRAME_PREV].buf,
												  s->cons.frameSize, s->ctx.fieldId);
				RvRtpClearFragHistInline(s, vscan, pktFmt);
			}
			else if (s->consBufs[FRAME_CURR].pkts + ST_PKTS_LOSS_ALLOWED >= s->fmt.pktsInFrame)
			{
				RvRtpFixCurrentFrame(s, rtpTmstamp, vscan, pktFmt);
				s->consBufs[FRAME_CURR].pkts = 0;
				s->consBufs[FRAME_CURR].tmstamp = 0;
			}
			else
			{
#ifdef RX_RECV_DEBUG
				RTE_LOG(INFO, USER3,
						"Incomplete frame dropped of %lu received pkts %u, shall be %u\n",
						s->sn.frmsRecv + 1, s->consBufs[FRAME_CURR].pkts, s->fmt.pktsInFrame);
#endif
				return RvRtpIncompleteDropNCont(s, rtpTmstamp, FRAME_CURR, FALSE, vscan, pktFmt);
			}
		}
	}

	return ST_OK;
}

/*****************************************************************************************
 *
 * RvRtpReceiveNextPackets720p
 *
 * DESCRIPTION
 * Main function to processes single line 720p Next packets within session context
 * encapsulation is general and can be anyu permutation of fields allowed by the standard
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveNextPackets720p(rvrtp_session_t *s, struct rte_mbuf *mbuf)
{
	return RvRtpReceiveNextPacketsInline(s, mbuf, ST21_720P, ST_OTHER_SLN_RFC4175_PKT);
}

/*****************************************************************************************
 *
 * RvRtpReceiveNextPackets1080p
 *
 * DESCRIPTION
 * Main function to processes single line 1080p Next packets within session context
 * encapsulation is general and can be anyu permutation of fields allowed by the standard
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveNextPackets1080p(rvrtp_session_t *s, struct rte_mbuf *mbuf)
{
	return RvRtpReceiveNextPacketsInline(s, mbuf, ST21_1080P, ST_OTHER_SLN_RFC4175_PKT);
}

/*****************************************************************************************
 *
 * RvRtpReceiveNextPackets2160p
 *
 * DESCRIPTION
 * Main function to processes single line 1080p Next packets within session context
 * encapsulation is general and can be any permutation of fields allowed by the standard
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveNextPackets2160p(rvrtp_session_t *s, struct rte_mbuf *mbuf)
{
	return RvRtpReceiveNextPacketsInline(s, mbuf, ST21_2160P, ST_OTHER_SLN_RFC4175_PKT);
}

/*****************************************************************************************
 *
 * RvRtpReceiveNextPackets720i
 *
 * DESCRIPTION
 * Main function to processes single line 720i Next packets within session context
 * encapsulation is general and can be any permutation of fields allowed by the standard
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveNextPackets720i(rvrtp_session_t *s, struct rte_mbuf *mbuf)
{
	return RvRtpReceiveNextPacketsInline(s, mbuf, ST21_720I, ST_OTHER_SLN_RFC4175_PKT);
}

/*****************************************************************************************
 *
 * RvRtpReceiveNextPackets1080i
 *
 * DESCRIPTION
 * Main function to processes single line 1080i Next packets within session context
 * encapsulation is general and can be anyu permutation of fields allowed by the standard
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveNextPackets1080i(rvrtp_session_t *s, struct rte_mbuf *mbuf)
{
	return RvRtpReceiveNextPacketsInline(s, mbuf, ST21_1080I, ST_OTHER_SLN_RFC4175_PKT);
}

/*****************************************************************************************
 *
 * RvRtpReceiveNextPackets2160i
 *
 * DESCRIPTION
 * Main function to processes single line 1080p Next packets within session context
 * encapsulation is general and can be anyu permutation of fields allowed by the standard
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveNextPackets2160i(rvrtp_session_t *s, struct rte_mbuf *mbuf)
{
	return RvRtpReceiveNextPacketsInline(s, mbuf, ST21_2160I, ST_OTHER_SLN_RFC4175_PKT);
}

/*****************************************************************************************
 *
 * RvRtpReceiveNextPacketsSln720p
 *
 * DESCRIPTION
 * Main function to processes single line 720p Next packets within session context
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveNextPacketsSln720p(rvrtp_session_t *s, struct rte_mbuf *mbuf)
{
	return RvRtpReceiveNextPacketsInline(s, mbuf, ST21_720P, ST_INTEL_SLN_RFC4175_PKT);
}

/*****************************************************************************************
 *
 * RvRtpReceiveNextPacketsSln1080p
 *
 * DESCRIPTION
 * Main function to processes single line 1080p Next packets within session context
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveNextPacketsSln1080p(rvrtp_session_t *s, struct rte_mbuf *mbuf)
{
	return RvRtpReceiveNextPacketsInline(s, mbuf, ST21_1080P, ST_INTEL_SLN_RFC4175_PKT);
}

/*****************************************************************************************
 *
 * RvRtpReceiveNextPacketsSln2160p
 *
 * DESCRIPTION
 * Main function to processes single line 1080p Next packets within session context
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveNextPacketsSln2160p(rvrtp_session_t *s, struct rte_mbuf *mbuf)
{
	return RvRtpReceiveNextPacketsInline(s, mbuf, ST21_2160P, ST_INTEL_SLN_RFC4175_PKT);
}

/*****************************************************************************************
 *
 * RvRtpReceiveNextPacketsDln720p
 *
 * DESCRIPTION
 * Main function to processes dual line 720p Next packets within session context
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveNextPacketsDln720p(rvrtp_session_t *s, struct rte_mbuf *mbuf)
{
	return RvRtpReceiveNextPacketsInline(s, mbuf, ST21_720P, ST_INTEL_DLN_RFC4175_PKT);
}

/*****************************************************************************************
 *
 * RvRtpReceiveNextPacketsDln1080p
 *
 * DESCRIPTION
 * Main function to processes dual line 1080p Next packets within session context
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveNextPacketsDln1080p(rvrtp_session_t *s, struct rte_mbuf *mbuf)
{
	return RvRtpReceiveNextPacketsInline(s, mbuf, ST21_1080P, ST_INTEL_DLN_RFC4175_PKT);
}

/*****************************************************************************************
 *
 * RvRtpGetReceiveFunction
 *
 * DESCRIPTION
 * Dispatch which function to use within the format and vscan
 *
 * RETURNS: RvRtpRecvPacket_f function
 *
 * SEE ALSO:
 */
static inline RvRtpRecvPacket_f
RvRtpGetReceiveFunction(st21_vscan_t vscan, st21_pkt_fmt_t pktFmt)
{
	if ((vscan == ST21_2160P) && (pktFmt == ST_INTEL_SLN_RFC4175_PKT))
	{
		return RvRtpReceiveNextPacketsSln2160p;
	}
	else if ((vscan == ST21_1080P) && (pktFmt == ST_INTEL_SLN_RFC4175_PKT))
	{
		return RvRtpReceiveNextPacketsSln1080p;
	}
	else if ((vscan == ST21_720P) && (pktFmt == ST_INTEL_SLN_RFC4175_PKT))
	{
		return RvRtpReceiveNextPacketsSln720p;
	}
	else if ((vscan == ST21_2160P) && (pktFmt == ST_OTHER_SLN_RFC4175_PKT))
	{
		return RvRtpReceiveNextPackets2160p;
	}
	else if ((vscan == ST21_1080P) && (pktFmt == ST_OTHER_SLN_RFC4175_PKT))
	{
		return RvRtpReceiveNextPackets1080p;
	}
	else if ((vscan == ST21_720P) && (pktFmt == ST_OTHER_SLN_RFC4175_PKT))
	{
		return RvRtpReceiveNextPackets720p;
	}
	else if ((vscan == ST21_2160I) && (pktFmt == ST_OTHER_SLN_RFC4175_PKT))
	{
		return RvRtpReceiveNextPackets2160i;
	}
	else if ((vscan == ST21_1080I) && (pktFmt == ST_OTHER_SLN_RFC4175_PKT))
	{
		return RvRtpReceiveNextPackets1080i;
	}
	else if ((vscan == ST21_720I) && (pktFmt == ST_OTHER_SLN_RFC4175_PKT))
	{
		return RvRtpReceiveNextPackets720i;
	}
	else if ((vscan == ST21_1080P) && (pktFmt == ST_INTEL_DLN_RFC4175_PKT))
	{
		return RvRtpReceiveNextPacketsDln1080p;
	}
	else if ((vscan == ST21_720P) && (pktFmt == ST_INTEL_DLN_RFC4175_PKT))
	{
		return RvRtpReceiveNextPacketsDln720p;
	}
	ST_ASSERT;
	return NULL;
}


/*****************************************************************************************
 *
 * RvRtpReceivePacketCallback
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
RvRtpReceivePacketCallback(
    rvrtp_session_t *s, 
	struct rte_mbuf *m) 
{
	struct rte_ipv4_hdr *ip = rte_pktmbuf_mtod_offset(m, struct rte_ipv4_hdr *, 14);
	struct rte_udp_hdr *udp = rte_pktmbuf_mtod_offset(m, struct rte_udp_hdr *, 34);
	st_rfc4175_rtp_single_hdr_t *rtp = (st_rfc4175_rtp_single_hdr_t *)&udp[1];

	if (!(s->cons.St21RecvRtpPkt)) ST_ASSERT;

	if (m->pkt_len < MIN_PKT_SIZE) return ST_PKT_DROP_BAD_PKT_LEN;

	/* Validate the IP & UDP header */
	st_status_t res;
	if ((res = RvRtpIpUdpHdrCheck(s, ip)) != ST_OK)
	{
		return res;
	}

	uint32_t hdrSize = 34 + sizeof(struct rte_udp_hdr) + sizeof(st_rfc4175_rtp_single_hdr_t);
	uint8_t *rtpPayload = (uint8_t *)&rtp[1];
	uint32_t payloadSize = m->pkt_len - hdrSize;
	uint8_t *pktHdr = rte_pktmbuf_mtod_offset(m, uint8_t *, 0);
	
	switch (s->cons.consType)
	{
	case ST21_CONS_RAW_L2_PKT:
		return s->cons.St21RecvRtpPkt(s->cons.appHandle, pktHdr, hdrSize, rtpPayload, payloadSize, m->timestamp);

	case ST21_CONS_RAW_RTP:
		return s->cons.St21RecvRtpPkt(s->cons.appHandle, (uint8_t *)rtp, sizeof(st_rfc4175_rtp_single_hdr_t), 
                                      rtpPayload, payloadSize, m->timestamp);
	default:
		ST_ASSERT;
	}		
	return ST_GENERAL_ERR;
}

/*****************************************************************************************
 *
 * RvRtpReceiveFirstPacketsInline
 *
 * DESCRIPTION
 * Main function to processes packet within session context
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
static inline st_status_t
RvRtpReceiveFirstPacketsInline(
    rvrtp_session_t *s, 
	struct rte_mbuf *m, 
	st21_vscan_t vscan,
    st21_pkt_fmt_t pktFmt)
{
	struct rte_ipv4_hdr *ip = rte_pktmbuf_mtod_offset(m, struct rte_ipv4_hdr *, 14);
	struct rte_udp_hdr *udp = rte_pktmbuf_mtod_offset(m, struct rte_udp_hdr *, 34);
	struct st_rfc4175_rtp_dual_hdr *rtp = (struct st_rfc4175_rtp_dual_hdr *)&udp[1];

	s->ctx.data = NULL;	 //pointer to right frameBuffer

	/* Validate the IP & UDP & RTP header */
	st_status_t res;
	if (((res = RvRtpIpUdpHdrCheck(s, ip)) != ST_OK) ||
		((res = RvRtpHdrCheck(s, rtp, pktFmt, vscan)) != ST_OK))
	{
		return res;
	}

	uint32_t rtpTmstamp = ntohl(rtp->tmstamp);

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

	if (rtpTmstamp == s->ctx.tmstamp)  //tmstamp match most cases here
	{
#ifdef ST_DONT_IGNORE_PKT_CHECK
		if (s->consBufs[FRAME_PREV].tmstamp == rtpTmstamp)
#endif
		{
			s->ctx.data = s->consBufs[FRAME_PREV].buf;
			s->consBufs[FRAME_PREV].pkts++;
			s->sn.pktsRecv++;
		}
#ifdef ST_DONT_IGNORE_PKT_CHECK
		else
		{
			RTE_LOG(INFO, USER3, "Packet tmstamp of %u while expcetd %u matched GEN_ERR 0\n",
					rtpTmstamp, s->consBufs[FRAME_PREV].tmstamp);
			s->pktsDrop++;
			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_TMSTAMP)]++;
			return ST_PKT_DROP_BAD_RTP_TMSTAMP;
		}
#endif
#ifdef ST_EBU_IN_1ST_PACKET
		if (stMainParams.isEbuCheck)
		{
			RvRtpCalculatePacketEbu(s, m->timestamp, s->consBufs[FRAME_PREV].pkts);
		}
#endif		
	}
	else if ((rtpTmstamp > s->ctx.tmstamp) ||
		((rtpTmstamp & (0x1 << 31)) < (s->ctx.tmstamp & (0x1 << 31))))//new frame condition
	{
		if (unlikely(s->ctx.tmstamp == 0))
		{
			//1st time packet arrived on the sessions
			s->consBufs[FRAME_PREV].pkts = 1;
			s->ctx.data = s->consBufs[FRAME_PREV].buf;
			s->consBufs[FRAME_PREV].tmstamp = rtpTmstamp;
			s->sn.pktsRecv++;
			s->lastTmstamp = rtpTmstamp - s->ctx.tmstampEvenInc;
		}
		else
		{
			//new frame condition when yet receiving previous
			//so previous is incomplete -> drop it and keep previous as state
#ifdef RX_RECV_DEBUG
			RTE_LOG(INFO, USER3, "Incomplete 1st frame tmstamp of %u received pkts %u, shall be %u\n",
				s->sn.timeslot, s->consBufs[FRAME_PREV].pkts, s->fmt.pktsInFrame);
#endif
			uint32_t complete = 1;
			if (RvRtpReceiveFastFragCheckInline(s, vscan, pktFmt) == ST_PKT_DROP_INCOMPL_FRAME)
			{
				RvRtpIncompleteDropNCont(s, rtpTmstamp, FRAME_PREV, TRUE, vscan, pktFmt);
				complete = 0;
			}
			if (complete)
			{
				s->sn.frmsRecv++;
				s->cons.St21NotifyFrameRecv(s->cons.appHandle, s->ctx.data, s->ctx.tmstamp,
											s->ctx.fieldId);
				s->cons.St21NotifyFrameDone(s->cons.appHandle, s->consBufs[FRAME_PREV].buf,
											s->ctx.fieldId);
				s->consState = FRAME_CURR;
				s->consBufs[FRAME_CURR].pkts = 1;
				s->consBufs[FRAME_CURR].tmstamp = rtpTmstamp;

				s->RecvRtpPkt = RvRtpGetReceiveFunction(vscan, pktFmt);
				s->consBufs[FRAME_CURR].buf =
					s->cons.St21GetNextFrameBuf(s->cons.appHandle, s->consBufs[FRAME_PREV].buf,
                                                s->cons.frameSize, s->ctx.fieldId);
				if (unlikely(s->consBufs[FRAME_CURR].buf == NULL))
				{
					return RvRtpDropFrameAtTmstamp(s, rtpTmstamp, ST_PKT_DROP_NO_FRAME_BUF);
				}
				s->ctx.data = s->consBufs[FRAME_PREV].buf;
				RvRtpClearFragHistInline(s, vscan, pktFmt);
			}
		}
#ifdef ST_EBU_IN_1ST_PACKET
		if (stMainParams.isEbuCheck)
		{
			RvRtpCalculateFrameEbu(s, rtpTmstamp, m->timestamp);
		}
#endif
	}
	else if ((rtpTmstamp == s->tmstampToDrop[0]) || (rtpTmstamp == s->tmstampToDrop[1]))
	{
		s->pktsDrop++;
		s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_NO_FRAME_BUF)]++;
		return ST_PKT_DROP_NO_FRAME_BUF;
	}
	//out of order packet arrived - drop it silently
	else if (s->ctx.tmstamp > rtpTmstamp)
	{
		s->pktsDrop++;
		s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_TMSTAMP)]++;
		return ST_PKT_DROP_BAD_RTP_TMSTAMP;
	}

	s->ctx.tmstamp = rtpTmstamp;

	res = RvRtpReceiveFastCopyInline(s, rtp, vscan, pktFmt, FRAME_PREV);
	if (unlikely(res != ST_OK))
	{
		return res;
	}

	/* is frame complete ?*/
	if (rtp->marker)
	{
		if (s->consBufs[FRAME_PREV].pkts == s->fmt.pktsInFrame)
		{
			s->sn.frmsRecv++;
#ifdef ST_EBU_IN_1ST_PACKET			
			if (stMainParams.isEbuCheck)
			{
                RvRtpCalculateEbuAvg(s);
            }
#endif
			s->cons.St21NotifyFrameRecv(s->cons.appHandle, s->ctx.data, rtpTmstamp, s->ctx.fieldId);
			s->cons.St21NotifyFrameDone(s->cons.appHandle, s->consBufs[FRAME_PREV].buf,
										s->ctx.fieldId);
			s->consState = FRAME_CURR;
			s->consBufs[FRAME_CURR].pkts = 0;
			s->consBufs[FRAME_CURR].tmstamp = 0;

			s->RecvRtpPkt = RvRtpGetReceiveFunction(vscan, pktFmt);

			s->consBufs[FRAME_CURR].buf = 
				s->cons.St21GetNextFrameBuf(s->cons.appHandle, s->consBufs[FRAME_PREV].buf, s->cons.frameSize, 
				                            s->ctx.fieldId);
			RvRtpClearFragHistInline(s, vscan, pktFmt);
		}
		else
		{
#ifdef RX_RECV_DEBUG
			RTE_LOG(INFO, USER3, "Incomplete 1st frame of %u received pkts %u, shall be %u\n",
					s->sn.timeslot, s->consBufs[FRAME_PREV].pkts, s->fmt.pktsInFrame);
#endif
			//check frag histogram if it is complete i.e. we have 1st frame completed from several
			if (RvRtpReceiveFastFragCheckInline(s, vscan, pktFmt) == ST_PKT_DROP_INCOMPL_FRAME)
			{
				return RvRtpIncompleteDropNCont(s, rtpTmstamp, FRAME_PREV, FALSE, vscan, pktFmt);
			}

			//here frame completed as we have full histogram
			s->sn.frmsRecv++;
#ifdef ST_EBU_IN_1ST_PACKET
			if (stMainParams.isEbuCheck)
			{
                RvRtpCalculateEbuAvg(s);
            }
#endif
			s->cons.St21NotifyFrameRecv(s->cons.appHandle, s->ctx.data, rtpTmstamp, s->ctx.fieldId);
			s->cons.St21NotifyFrameDone(s->cons.appHandle, s->consBufs[FRAME_PREV].buf,
										s->ctx.fieldId);
			s->consState = FRAME_CURR;
			s->consBufs[FRAME_CURR].pkts = 0;
			s->consBufs[FRAME_CURR].tmstamp = 0;
			s->RecvRtpPkt = RvRtpGetReceiveFunction(vscan, pktFmt);
			s->consBufs[FRAME_CURR].buf = s->cons.St21GetNextFrameBuf(
				s->cons.appHandle, s->consBufs[FRAME_PREV].buf, s->cons.frameSize, s->ctx.fieldId);
			RvRtpClearFragHistInline(s, vscan, pktFmt);
		}
	}
	return ST_OK;
}

/*****************************************************************************************
 *
 * RvRtpReceiveFirstPackets720p
 *
 * DESCRIPTION
 * Main function to processes single line 720p first packets within session context
 * of the other vendors that can have any permutation of the size of packets
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveFirstPackets720p(rvrtp_session_t *s, struct rte_mbuf *mbuf)
{
	return RvRtpReceiveFirstPacketsInline(s, mbuf, ST21_720P, ST_OTHER_SLN_RFC4175_PKT);
}

/*****************************************************************************************
 *
 * RvRtpReceiveFirstPackets720i
 *
 * DESCRIPTION
 * Main function to processes single line 720i first packets within session context
 * of the other vendors that can have any permutation of the size of packets
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveFirstPackets720i(rvrtp_session_t *s, struct rte_mbuf *mbuf)
{
	return RvRtpReceiveFirstPacketsInline(s, mbuf, ST21_720I, ST_OTHER_SLN_RFC4175_PKT);
}

/*****************************************************************************************
 *
 * RvRtpReceiveFirstPacketsSln720p
 *
 * DESCRIPTION
 * Main function to processes single line 720p first packets within session context
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveFirstPacketsSln720p(rvrtp_session_t *s, struct rte_mbuf *mbuf)
{
	return RvRtpReceiveFirstPacketsInline(s, mbuf, ST21_720P, ST_INTEL_SLN_RFC4175_PKT);
}

/*****************************************************************************************
 *
 * RvRtpReceiveFirstPackets1080i
 *
 * DESCRIPTION
 * Main function to processes single line 1080i first packets within session context
 * of the other vendors that can have any permutation of the size of packets
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveFirstPackets1080i(rvrtp_session_t *s, struct rte_mbuf *mbuf)
{
	return RvRtpReceiveFirstPacketsInline(s, mbuf, ST21_1080I, ST_OTHER_SLN_RFC4175_PKT);
}

/*****************************************************************************************
 *
 * RvRtpReceiveFirstPackets1080p
 *
 * DESCRIPTION
 * Main function to processes single line 1080p first packets within session context
 * of the other vendors that can have any permutation of the size of packets
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveFirstPackets1080p(rvrtp_session_t *s, struct rte_mbuf *mbuf)
{
	return RvRtpReceiveFirstPacketsInline(s, mbuf, ST21_1080P, ST_OTHER_SLN_RFC4175_PKT);
}

/*****************************************************************************************
 *
 * RvRtpReceiveFirstPacketsSln1080p
 *
 * DESCRIPTION
 * Main function to processes single line 1080p first packets within session context
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveFirstPacketsSln1080p(rvrtp_session_t *s, struct rte_mbuf *mbuf)
{
	return RvRtpReceiveFirstPacketsInline(s, mbuf, ST21_1080P, ST_INTEL_SLN_RFC4175_PKT);
}

/*****************************************************************************************
 *
 * RvRtpReceiveFirstPackets2160p
 *
 * DESCRIPTION
 * Main function to processes single line 2160p first packets within session context
 * of the other vendors that can have any permutation of the size of packets
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveFirstPackets2160p(rvrtp_session_t *s, struct rte_mbuf *mbuf)
{
	return RvRtpReceiveFirstPacketsInline(s, mbuf, ST21_2160P, ST_OTHER_SLN_RFC4175_PKT);
}

/*****************************************************************************************
 *
 * RvRtpReceiveFirstPackets2160i
 *
 * DESCRIPTION
 * Main function to processes single line 2160p first packets within session context
 * of the other vendors that can have any permutation of the size of packets
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveFirstPackets2160i(rvrtp_session_t *s, struct rte_mbuf *mbuf)
{
	return RvRtpReceiveFirstPacketsInline(s, mbuf, ST21_2160I, ST_OTHER_SLN_RFC4175_PKT);
}

/*****************************************************************************************
 *
 * RvRtpReceiveFirstPacketsSln2160p
 *
 * DESCRIPTION
 * Main function to processes single line 2160p first packets within session context
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveFirstPacketsSln2160p(rvrtp_session_t *s, struct rte_mbuf *mbuf)
{
	return RvRtpReceiveFirstPacketsInline(s, mbuf, ST21_2160P, ST_INTEL_SLN_RFC4175_PKT);
}

/*****************************************************************************************
 *
 * RvRtpReceiveFirstPacketsSln2160i
 *
 * DESCRIPTION
 * Main function to processes single line 2160i first packets within session context
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveFirstPacketsSln2160i(rvrtp_session_t *s, struct rte_mbuf *mbuf)
{
	return RvRtpReceiveFirstPacketsInline(s, mbuf, ST21_2160I, ST_OTHER_SLN_RFC4175_PKT);
}

/*****************************************************************************************
 *
 * RvRtpReceiveFirstPacketsDln720p
 *
 * DESCRIPTION
 * Main function to processes dual line 720p first packets within session context
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveFirstPacketsDln720p(rvrtp_session_t *s, struct rte_mbuf *mbuf)
{
	return RvRtpReceiveFirstPacketsInline(s, mbuf, ST21_720P, ST_INTEL_DLN_RFC4175_PKT);
}

/*****************************************************************************************
 *
 * RvRtpReceiveFirstPacketsDln1080p
 *
 * DESCRIPTION
 * Main function to processes dual line 1080p first packets within session context
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveFirstPacketsDln1080p(rvrtp_session_t *s, struct rte_mbuf *mbuf)
{
	return RvRtpReceiveFirstPacketsInline(s, mbuf, ST21_720P, ST_INTEL_DLN_RFC4175_PKT);
}

/*****************************************************************************************
 *
 * RvRtpDispatchPacket
 *
 * DESCRIPTION
 * Find right session for the packet
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
#ifndef ST_FLOW_CLASS_IN_HW
static inline st_status_t
RvRtpDispatchPacket(
    rvrtp_device_t *dev, 
	st_flow_t *flow, 
	struct rte_mbuf *m, 
	uint32_t firstSn,
    uint32_t lastSn)
#else
static inline st_status_t
RvRtpDispatchPacket(
    rvrtp_device_t *dev, 
	uint16_t dstPort, 
	struct rte_mbuf *m, 
	uint32_t firstSn,
    uint32_t lastSn)
#endif
{
	st_status_t status = ST_OK;
#ifdef ST_DONT_IGNORE_PKT_CHECK
	//sanity checks
	if (unlikely(m->pkt_len < ST_MIN_VALID_PKT_SIZE))
	{
		RTE_LOG(INFO, USER3, "Packet received pkt: %p of weird len: %u\n", m, m->pkt_len);

		rte_pktmbuf_free(m);
		return ST_PKT_DROP_BAD_PKT_LEN;
	}
#endif
#ifndef ST_FLOW_CLASS_IN_HW
	uint16_t port = flow->dst.addr4.sin_port;
#else
	uint16_t port = dstPort;
#endif

	for (uint32_t i = firstSn; i < lastSn; i++)
	{
		rvrtp_session_t *s = dev->snTable[i];
		if (unlikely(!s)) continue;
		if ((port == s->fl[0].dst.addr4.sin_port) || (port == s->fl[1].dst.addr4.sin_port))
		{
			if (s->state == ST_SN_STATE_RUN)
			{
				status = s->RecvRtpPkt(s, m);
			}
			break;
		}
	}
	rte_pktmbuf_free(m);
	return status;
}

/*****************************************************************************************
 *
 * LcoreMainReceiver
 *
 * DESCRIPTION
 * Main thread loop for receiving packets
 *
 * SEE ALSO:
 */
int
LcoreMainReceiver(void *args)
{
	uint32_t threadId = (uint32_t)((uint64_t)args);

	if (threadId > stDevParams->maxRcvThrds)
		rte_exit(127, "Receiver threadId is invalid\n");
#ifdef DEBUG
	RTE_LOG(INFO, USER3, "RECEIVER ON %d LCORE THREAD %u\n", rte_lcore_id(), threadId);
#endif
	st_main_params_t *mp = &stMainParams;
	rvrtp_device_t *dev = &stRecvDevice;

	int rv = 0;
	int vlanOffload = rte_eth_dev_get_vlan_offload(mp->rxPortId);
	vlanOffload |= ETH_VLAN_STRIP_OFFLOAD;
	rte_eth_dev_set_vlan_offload(mp->rxPortId, vlanOffload);
	RTE_LOG(INFO, USER1, "Receiver ready - receiving packets STARTED\n");

	while (rte_atomic32_read(&isRxDevToDestroy) == 0)
	{
		struct rte_mbuf *rxVect[RX_BURTS_SIZE];

#ifdef ST_RECV_TIME_PRINT
		uint64_t cycles0 = StPtpGetTime();
#endif
		rv = rte_eth_rx_burst(mp->rxPortId, 1 + threadId, rxVect, 8);

		uint64_t ptpTime = StPtpGetTime();

		for (int i = 0; i < rv; i++)
		{
			rxVect[i]->timestamp = ptpTime;

#ifndef ST_FLOW_CLASS_IN_HW
			if (rte_pktmbuf_mtod(rxVect[i], struct ethhdr *)->h_proto != 0x0008)
			{
				rte_pktmbuf_free(rxVect[i]);
				continue;
			}
			if (rte_pktmbuf_mtod_offset(rxVect[i], struct rte_ipv4_hdr *, 14)->next_proto_id != 17)
			{
				rte_pktmbuf_free(rxVect[i]);
				continue;
			}
#endif

#ifndef ST_FLOW_CLASS_IN_HW
			st_flow_t flow;
			flow.dst.addr4.sin_port = rte_pktmbuf_mtod_offset(rxVect[i], struct udphdr *, 34)->uh_sport;
			flow.src.addr4.sin_port	= rte_pktmbuf_mtod_offset(rxVect[i], struct udphdr *, 34)->uh_dport;
			flow.src.addr4.sin_addr.s_addr = rte_pktmbuf_mtod_offset(rxVect[i], struct rte_ipv4_hdr *, 14)->src_addr;
			flow.dst.addr4.sin_addr.s_addr = rte_pktmbuf_mtod_offset(rxVect[i], struct rte_ipv4_hdr *, 14)->dst_addr;
			RvRtpDispatchPacket(dev, &flow, rxVect[i], mp->rcvThrds[threadId].thrdSnFirst,
								mp->rcvThrds[threadId].thrdSnLast);
#else
			uint16_t dstPort = rte_pktmbuf_mtod_offset(rxVect[i], struct udphdr *, 34)->uh_dport;
			RvRtpDispatchPacket(dev, dstPort, rxVect[i], mp->rcvThrds[threadId].thrdSnFirst,
								mp->rcvThrds[threadId].thrdSnLast);
#endif
		}
#ifdef ST_RECV_TIME_PRINT
		uint64_t cycles1 = StPtpGetTime();
		if (rv > 0)
		{
			RTE_LOG(INFO, USER1, "Time elapsed %llu pktTime %llu burstSize %u\n",
					(U64)(cycles1 - cycles0), (U64)(cycles1 - cycles0) / rv, rv);
		}
#endif
	}
	RTE_LOG(INFO, USER1, "Receiver closed - receiving packets STOPPED\n");
	return 0;
}
