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

#include <rte_hexdump.h>
#include <rte_malloc.h>

#include "dpdk_common.h"
#include "rvrtp_main.h"
#include "st_flw_cls.h"
#include "st_igmp.h"

// #define THREAD_DEBUG
//#define ST_RECV_TIME_PRINT
//#define ST_MEMCPY_TEST
//#define ST_MULTICAST_TEST
//
/* TODO SW Timestamp is not accurate
 * Use an acceptable adjustment. Will remove that in HW timestamp */
#define ST_SW_TIMESTAMP_ADJUSTMENT 1000000
#define ST_HUGE_DELAY 0xfffffff

//#define RX_RECV_DEBUG
#define MAX_PENDING_CNT 512

st_rcv_stats_t rxThreadStats[RTE_MAX_LCORE];

extern int hwts_dynfield_offset[RTE_MAX_ETHPORTS];

static __thread st_session_impl_t *sn[ST_MAX_SESSIONS_MAX];
static __thread int sn_count;
st_device_impl_t stRecvDevice;
static inline void RvRtpCopyFragHistInline(st_session_impl_t *s, st21_vscan_t vscan,
										   st21_pkt_fmt_t pktFmt);

void *
RvRtpDummyBuildPacket(st_session_impl_t *s, void *hdr, struct rte_mbuf *m)
{
	return NULL;
}

st_status_t
RvRtpFreeRxSession(st_session_impl_t *s)
{
	if (s)
	{
		rte_free(s->vctx.lineHistogram);
		rte_free(s->vctx.fragHistogram[CURR_HIST]);
		rte_free(s->vctx.fragHistogram[PEND_HIST]);
		rte_free(s->cons.appHandle);
		rte_free(s);
	}
	return ST_OK;
}

static inline void
RvRtpClearPacketEbu(st_session_impl_t *s)
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
RvRtpClearFrameEbu(st_session_impl_t *s)
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
RvRtpCreateRxSession(st_device_impl_t *dev, st_session_t *sin, st_format_t *fmt,
					 st_session_impl_t **sout)
{
	st_essence_type_t mtype;
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

	st_session_impl_t *s = rte_malloc_socket("Session", sizeof(st_session_impl_t),
											 RTE_CACHE_LINE_SIZE, rte_socket_id());

	if (s)
	{
		memset(s, 0x0, sizeof(st_session_impl_t));

		s->fmt = *fmt;
		s->dev = dev;
		s->sn = *sin;

		for (uint32_t i = 0; i < stMainParams.maxRcvThrds; i++)
		{
			if ((stMainParams.rcvThrds[i].thrdSnFirst <= sin->timeslot)
				&& (sin->timeslot < stMainParams.rcvThrds[i].thrdSnLast))
			{
				printf("ts:%d tid=%d\n", sin->timeslot, i);
				s->tid = i;
				break;
			}
		}

		switch (vfmt->clockRate)
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
			s->sn.tprs = (uint32_t)((vfmt->frameTime - s->sn.trOffset) / vfmt->pktsInFrame);
			break;
		case ST_2110_21_TPNL:
		case ST_2110_21_TPW:
			s->sn.tprs = (uint32_t)(vfmt->frameTime / vfmt->pktsInFrame);
			break;
		default:
			ST_ASSERT;
			break;
		}
		s->sn.frameSize
			= (uint32_t)((uint64_t)s->fmt.v.height * s->fmt.v.width * s->fmt.v.pixelGrpSize)
			  / s->fmt.v.pixelsInGrp;
		s->sn.trOffset = s->sn.tprs * vfmt->pktsInLine * vfmt->trOffsetLines;
		s->pktTime = ((vfmt->pktSize + 24) * 8) / dev->dev.rateGbps;
		uint32_t remaind = ((vfmt->pktSize + 24) * 8) % dev->dev.rateGbps;
		if (remaind >= (dev->dev.rateGbps / 2))
			s->pktTime++;

		s->UpdateRtpPkt = RvRtpDummyBuildPacket;

		//good for single line formats
		uint32_t lineHistogramSize = s->fmt.v.height * sizeof(*s->vctx.lineHistogram);
		//default
		uint32_t fragHistogramSize = s->fmt.v.height * sizeof(*s->vctx.fragHistogram);
		switch (s->fmt.v.vscan)
		{
		case ST21_720P:
			switch (s->fmt.v.pktFmt)
			{
			case ST_INTEL_DLN_RFC4175_PKT:
				s->RecvRtpPkt = RvRtpReceiveFirstPacketsDln720p;
				fragHistogramSize = ST_FRAG_HISTOGRAM_720P_DLN_SZ;
				s->fragPattern = 0x1f1f1f1f1f1f1f1fllu;	 //5 bit version
				lineHistogramSize = s->fmt.v.height / 2 * sizeof(*s->vctx.lineHistogram);
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
			switch (s->fmt.v.pktFmt)
			{
			case ST_INTEL_DLN_RFC4175_PKT:
				s->RecvRtpPkt = RvRtpReceiveFirstPacketsDln1080p;
				fragHistogramSize = ST_FRAG_HISTOGRAM_1080P_DLN_SZ;
				s->fragPattern = 0xffffffffffffffffllu;
				lineHistogramSize = s->fmt.v.height / 2 * sizeof(*s->vctx.lineHistogram);
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
			switch (s->fmt.v.pktFmt)
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
			lineHistogramSize = s->fmt.v.height * sizeof(*s->vctx.lineHistogram);
			s->sn.frameSize /= 2;
			break;
		case ST21_1080I:
			s->RecvRtpPkt = RvRtpReceiveFirstPackets1080i;
			fragHistogramSize = ST_FRAG_HISTOGRAM_1080I_SLN_SZ;
			s->fragPattern = 0xffffffffffffffffllu;
			lineHistogramSize = s->fmt.v.height * sizeof(*s->vctx.lineHistogram);
			s->sn.frameSize /= 2;
			break;
		case ST21_2160I:
			s->RecvRtpPkt = RvRtpReceiveFirstPackets2160i;
			fragHistogramSize = ST_FRAG_HISTOGRAM_2160I_SLN_SZ;
			s->fragPattern = 0xffffffffffffffffllu;
			lineHistogramSize = s->fmt.v.height * sizeof(*s->vctx.lineHistogram);
			s->sn.frameSize /= 2;
			break;
		default:
			ST_ASSERT;
			break;
		}
		s->vctx.lineHistogram
			= rte_malloc_socket("Line", lineHistogramSize, RTE_CACHE_LINE_SIZE, rte_socket_id());

		s->vctx.fragHistogram[ST_PPORT]
			= rte_malloc_socket("Frag", fragHistogramSize, RTE_CACHE_LINE_SIZE, rte_socket_id());
		s->vctx.fragHistogram[PEND_HIST] = rte_malloc_socket("FragPend", fragHistogramSize,
															 RTE_CACHE_LINE_SIZE, rte_socket_id());

		if ((!s->vctx.lineHistogram) || (!s->vctx.fragHistogram[ST_PPORT])
			|| (!s->vctx.fragHistogram[PEND_HIST]))
		{
			rte_free(s);
			if (s->vctx.lineHistogram)
				free(s->vctx.lineHistogram);
			if (s->vctx.fragHistogram[ST_PPORT])
				free(s->vctx.fragHistogram[CURR_HIST]);
			if (s->vctx.fragHistogram[PEND_HIST])
				free(s->vctx.fragHistogram[PEND_HIST]);
			return ST_NO_MEMORY;
		}

		memset(s->vctx.lineHistogram, 0x0, lineHistogramSize);
		memset(s->vctx.fragHistogram[ST_PPORT], 0x0, fragHistogramSize);
		memset(s->vctx.fragHistogram[PEND_HIST], 0x0, fragHistogramSize);

		s->state = ST_SN_STATE_ON;

		RvRtpClearFrameEbu(s);
		RvRtpClearPacketEbu(s);

		*sout = s;
		return ST_OK;
	}
	return ST_NO_MEMORY;
}

st_status_t
RvRtpDestroyRxSession(st_session_impl_t *s)
{

	return ST_OK;
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
RvRtpCalculatePacketEbu(st_session_impl_t *s, uint64_t pktTmstamp, uint64_t pktCnt)
{
	uint64_t epochTmstamp = (uint64_t)(s->vctx.epochs * s->fmt.v.frameTime);

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
		/* TODO Will remove it when using HW timestamp */
		s->ebu.cinTmstamp = pktTmstamp - ST_SW_TIMESTAMP_ADJUSTMENT;
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
RvRtpCalculateFrameEbu(st_session_impl_t *s, uint32_t rtpTmstamp, uint64_t pktTmstamp)
{
	uint64_t epochs = (uint64_t)(pktTmstamp / s->fmt.v.frameTime);
	s->vctx.epochs = epochs;
	uint64_t epochTmstamp = (uint64_t)(epochs * s->fmt.v.frameTime);

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
	long double frmTime90k = 1.0L * s->fmt.v.clockRate * s->fmt.v.frmRateDen / s->fmt.v.frmRateMul;
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
RvRtpCalculatePacketEbuAvg(st_session_impl_t *s)
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
RvRtpCalculateFrameEbuAvg(st_session_impl_t *s)
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
RvRtpCalculateEbuAvg(st_session_impl_t *s)
{
	if (s->sn.frmsRecv % 100 == 0)
	{
		RvRtpCalculatePacketEbuAvg(s);
		RvRtpCalculateFrameEbuAvg(s);

		RTE_LOG(INFO, USER3, "Session %d Cinst AVG %.2f MIN %lu MAX %lu test %s!\n", s->sn.timeslot,
				s->ebu.cinAvg, s->ebu.cinMin, s->ebu.cinMax,
				(s->ebu.cinMax <= 5) ? "PASSED NARROW"
									 : (s->ebu.cinMax <= 16) ? "PASSED WIDE" : "FAILED");
		RTE_LOG(INFO, USER3, "Session %d VRX AVG %.2f MIN %lu MAX %lu test %s!\n", s->sn.timeslot,
				s->ebu.vrxAvg, s->ebu.vrxMin, s->ebu.vrxMax,
				(s->ebu.vrxMax <= 9) ? "PASSED NARROW"
									 : (s->ebu.vrxMax <= 720) ? "PASSED WIDE" : "FAILED");
		RTE_LOG(INFO, USER3, "Session %d TRO %u FPT AVG %.2f MIN %lu MAX %lu test %s!\n",
				s->sn.timeslot, s->sn.trOffset, s->ebu.fptAvg, s->ebu.fptMin, s->ebu.fptMax,
				(s->ebu.fptMax < 2 * s->sn.trOffset) ? "PASSED" : "FAILED");
		RTE_LOG(INFO, USER3, "Session %d TM inc AVG %.2f MIN %u MAX %u test %s!\n", s->sn.timeslot,
				s->ebu.tmiAvg, s->ebu.tmiMin, s->ebu.tmiMax,
				(s->ebu.tmiMax == s->ebu.tmiMin)
					? "PASSED"
					: (s->ebu.tmiMax == 1502 && s->ebu.tmiMin == 1501) ? "PASSED" : "FAILED");
		RTE_LOG(INFO, USER3, "Session %u TMD last diff %d Rtp %x Pkt %x MIN %ld MAX %ld test %s!\n",
				s->sn.timeslot, s->ebu.prevRtpTmstamp - s->ebu.prevPktTmstamp,
				s->ebu.prevRtpTmstamp, s->ebu.prevPktTmstamp, s->ebu.tmdMin, s->ebu.tmdMax,
				(s->ebu.tmdMax < 129) ? "PASSED" : "FAILED");
#ifdef ST_EXTENDED_EBU_LOGS
		RTE_LOG(INFO, USER3, "Session %u PrevEpochTime %llu frameTime %u EpochCount %u\n",
				s->sn.timeslot, s->ebu.prevEpochTime, (uint32_t)s->fmt.v.frameTime,
				(uint32_t)(s->ebu.prevTime / s->fmt.v.frameTime));
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
 * StRtpIpUdpHdrCheck
  *
 * DESCRIPTION
 * Check IP and UDP hdrs in packet within session context
 *
 * RETURNS: st_status_t
 */
st_status_t
StRtpIpUdpHdrCheck(st_session_impl_t *s, const struct rte_ipv4_hdr *ip)
{
#ifdef ST_DONT_IGNORE_PKT_CHECK
	struct rte_udp_hdr *udp = (struct rte_udp_hdr *)&ip[1];

	const uint16_t tIpLen = ntohs(ip->total_length);
	const uint16_t tUdpLen = ntohs(udp->dgram_len);

	/* Validate the IP & UDP header */
	const bool invalidIpLen = (tIpLen != (s->fmt.v.pktSize - s->etherSize));
	const bool invalidUdpLen = (tUdpLen != (s->fmt.v.pktSize - s->etherSize - sizeof(struct rte_ipv4_hdr)));

	if (unlikely(invalidIpLen || invalidUdpLen))
	{
		s->pktsDrop++;
		s->sn.pktsDrop[ST_PKT_DROP((invalidIpLen) ? ST_PKT_DROP_BAD_IP_LEN : ST_PKT_DROP_BAD_UDP_LEN)]++;

#ifdef RX_RECV_DEBUG
		if (invalidIpLen)
		{
			RTE_LOG(INFO, USER3, "Packet bad IPLEN: of %u\n", tIpLen);
			RTE_LOG(INFO, USER3, "Packet bad IPLEN: expected %u\n", s->fmt.v.pktSize - s->etherSize);
		}

		RTE_LOG(INFO, USER3, "Packet bad %s-LEN: pktsDrop %llu\n", (invalidIpLen) ? "IP" : "UDP", (U64)s->pktsDrop);
#endif
		return (invalidIpLen) ? ST_PKT_DROP_BAD_IP_LEN : ST_PKT_DROP_BAD_UDP_LEN;
	}
#endif
	return ST_OK;
}

/*****************************************************************************************
 *
 * StRtpHdrCheck
  *
 * DESCRIPTION
 * Check RFC 4175 RTP hdr in packet within session context
 *
 * RETURNS: st_status_t
 */
static inline st_status_t
StRtpHdrCheck(st_session_impl_t *s, const struct st_rfc4175_rtp_dual_hdr *rtp, st21_pkt_fmt_t pktFmt,
			  st21_vscan_t vscan)
{
	if (unlikely((rtp->version != RVRTP_VERSION_2) || (rtp->csrcCount != 0)))
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

		if ((line1LenRtp > s->vctx.line1Length) || (line2LenRtp > s->vctx.line2Length))
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
		s->vctx.line1Number = ntohs(NFIELD_MASK_15_BITS(rtp->line1Number));
		s->vctx.line2Number = ntohs(NFIELD_MASK_15_BITS(rtp->line2Number));

		s->vctx.line1Offset = ntohs(NFIELD_MASK_15_BITS(rtp->line1Offset));
		s->vctx.line2Offset = ntohs(NFIELD_MASK_15_BITS(rtp->line2Offset));

		if ((vscan == ST21_2160I) || (vscan == ST21_1080I) || (vscan == ST21_720I))
		{
#ifdef ST_DONT_IGNORE_PKT_CHECK
			if ((s->vctx.line1Number >= s->fmt.v.height / 2)
				|| (s->vctx.line2Number >= s->fmt.v.height / 2))
			{
				s->pktsDrop++;
				s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_LN_NUM)]++;
				RTE_LOG(INFO, USER3, "Packet bad RTPLN NUMBER of %u pktsDrop %llu\n",
						s->vctx.line1Number, (U64)s->pktsDrop);
				RTE_LOG(INFO, USER3, "Packet bad RTPLN NUMBER of %u pktsDrop %llu\n",
						s->vctx.line2Number, (U64)s->pktsDrop);
				return ST_PKT_DROP_BAD_RTP_LN_NUM;
			}
#endif
			s->vctx.fieldId = NFIELD_GET_16_BIT(rtp->line1Number);
		}
		else
		{
#ifdef ST_DONT_IGNORE_PKT_CHECK
			if ((s->vctx.line1Number >= s->fmt.v.height)
				|| (s->vctx.line2Number >= s->fmt.v.height))
			{
				s->pktsDrop++;
				s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_LN_NUM)]++;
				RTE_LOG(INFO, USER3, "Packet bad RTPLN NUMBER of %u pktsDrop %llu\n",
						s->vctx.line1Number, (U64)s->pktsDrop);
				RTE_LOG(INFO, USER3, "Packet bad RTPLN NUMBER of %u pktsDrop %llu\n",
						s->vctx.line2Number, (U64)s->pktsDrop);
				return ST_PKT_DROP_BAD_RTP_LN_NUM;
			}
#endif
			s->vctx.fieldId = 2;
		}

#ifdef ST_DONT_IGNORE_PKT_CHECK
		if ((s->vctx.line1Offset + s->fmt.v.pixelsInPkt > s->fmt.v.width)
			|| (s->vctx.line2Offset + s->fmt.v.pixelsInPkt > s->fmt.v.width))
		{
			s->pktsDrop++;
			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_OFFSET)]++;
			RTE_LOG(INFO, USER3, "Packet bad LN OFFSET of %u pktsDrop %llu\n", s->vctx.line1Offset,
					(U64)s->pktsDrop);
			RTE_LOG(INFO, USER3, "Packet bad LN OFFSET of %u pktsDrop %llu\n", s->vctx.line2Offset,
					(U64)s->pktsDrop);
			return ST_PKT_DROP_BAD_RTP_OFFSET;
		}
#endif
		return ST_OK;
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

		if (line1LenRtp > s->vctx.line1Length)
		{
			s->pktsDrop++;
			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_LN_LEN)]++;
			RTE_LOG(INFO, USER3, "Packet bad RTPLEN of %u pktsDrop %llu\n", line1LenRtp,
					(U64)s->pktsDrop);
			return ST_PKT_DROP_BAD_RTP_LN_LEN;
		}
#endif

		s->vctx.line1Number = ntohs(NFIELD_MASK_15_BITS(rtp->line1Number));
		s->vctx.line1Offset = ntohs(NFIELD_MASK_15_BITS(rtp->line1Offset));

		if ((vscan == ST21_2160I) || (vscan == ST21_1080I) || (vscan == ST21_720I))
		{
#ifdef ST_DONT_IGNORE_PKT_CHECK
			if (s->vctx.line1Number >= s->fmt.v.height / 2)
			{
				s->pktsDrop++;
				s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_LN_NUM)]++;
				RTE_LOG(INFO, USER3, "Packet bad RTPLN NUMBER of %u pktsDrop %llu\n",
						s->vctx.line1Number, (U64)s->pktsDrop);
				return ST_PKT_DROP_BAD_RTP_LN_NUM;
			}
#endif
			s->vctx.fieldId = NFIELD_GET_16_BIT(rtp->line1Number);
		}
		else
		{
#ifdef ST_DONT_IGNORE_PKT_CHECK
			if (s->vctx.line1Number >= s->fmt.v.height)
			{
				s->pktsDrop++;
				s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_LN_NUM)]++;
				RTE_LOG(INFO, USER3, "Packet bad RTPLN NUMBER of %u pktsDrop %llu\n",
						s->vctx.line1Number, (U64)s->pktsDrop);
				return ST_PKT_DROP_BAD_RTP_LN_NUM;
			}
#endif
			s->vctx.fieldId = 2;
		}

#ifdef ST_DONT_IGNORE_PKT_CHECK
		if (s->vctx.line1Offset + s->fmt.v.pixelsInPkt > s->fmt.v.width)
		{
			s->pktsDrop++;
			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_OFFSET)]++;
			RTE_LOG(INFO, USER3, "Packet bad LN OFFSET of %u pktsDrop %llu\n", s->vctx.line1Offset,
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
RvRtpDropTmstampPush(st_session_impl_t *s, uint32_t rtpTmstamp)
{
	s->tmstampToDrop[1] = s->tmstampToDrop[0];
	s->tmstampToDrop[0] = rtpTmstamp;
	s->vctx.tmstamp = 0;
}

/*****************************************************************************************
 *
 * RvRtpDropFrameAtTmstamp template inline
 */
static inline st_status_t
RvRtpDropFrameAtTmstamp(st_session_impl_t *s, uint32_t rtpTmstamp, st_status_t status)
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
RvRtpClearFragHistInline(st_session_impl_t *s, st21_vscan_t vscan, st21_pkt_fmt_t pktFmt)
{
	if (vscan == ST21_2160P)
	{
		memset(s->vctx.fragHistogram[CURR_HIST], 0, ST_FRAG_HISTOGRAM_2160P_SLN_SZ);
	}
	else if ((vscan == ST21_1080P) && (pktFmt == ST_INTEL_DLN_RFC4175_PKT))
	{
		memset(s->vctx.fragHistogram[CURR_HIST], 0, ST_FRAG_HISTOGRAM_1080P_DLN_SZ);
	}
	else if ((vscan == ST21_1080P) && (pktFmt == ST_INTEL_SLN_RFC4175_PKT))
	{
		memset(s->vctx.fragHistogram[CURR_HIST], 0, ST_FRAG_HISTOGRAM_1080P_SLN_SZ);
	}
	else if ((vscan == ST21_1080P) && (pktFmt == ST_OTHER_SLN_RFC4175_PKT))
	{
		memset(s->vctx.fragHistogram[CURR_HIST], 0, ST_FRAG_HISTOGRAM_1080P_SLN_SZ);
	}
	else if ((vscan == ST21_720P) && (pktFmt == ST_INTEL_DLN_RFC4175_PKT))
	{
		memset(s->vctx.fragHistogram[CURR_HIST], 0, ST_FRAG_HISTOGRAM_720P_DLN_SZ);
	}
	else if ((vscan == ST21_720P) && (pktFmt == ST_INTEL_SLN_RFC4175_PKT))
	{
		memset(s->vctx.fragHistogram[CURR_HIST], 0, ST_FRAG_HISTOGRAM_720P_SLN_SZ);
	}
	else if ((vscan == ST21_720P) && (pktFmt == ST_OTHER_SLN_RFC4175_PKT))
	{
		memset(s->vctx.fragHistogram[CURR_HIST], 0, ST_FRAG_HISTOGRAM_720P_SLN_SZ);
	}
	else if (vscan == ST21_2160I)
	{
		memset(s->vctx.fragHistogram[CURR_HIST], 0, ST_FRAG_HISTOGRAM_2160I_SLN_SZ);
	}
	else if (vscan == ST21_1080I)
	{
		memset(s->vctx.fragHistogram[CURR_HIST], 0, ST_FRAG_HISTOGRAM_1080I_SLN_SZ);
	}
	else if (vscan == ST21_720I)
	{
		memset(s->vctx.fragHistogram[CURR_HIST], 0, ST_FRAG_HISTOGRAM_720I_SLN_SZ);
	}
	else
	{
		ST_ASSERT;
	}
}

/*****************************************************************************************
 *
 * RvRtpCopytemplate resolving inline
 */
static inline void
RvRtpCopyFragHistInline(st_session_impl_t *s, st21_vscan_t vscan, st21_pkt_fmt_t pktFmt)
{
	if (vscan == ST21_2160P)
	{
		memcpy(s->vctx.fragHistogram[PEND_HIST], s->vctx.fragHistogram[CURR_HIST],
			   ST_FRAG_HISTOGRAM_2160P_SLN_SZ);
	}
	else if ((vscan == ST21_1080P) && (pktFmt == ST_INTEL_DLN_RFC4175_PKT))
	{
		memcpy(s->vctx.fragHistogram[PEND_HIST], s->vctx.fragHistogram[CURR_HIST],
			   ST_FRAG_HISTOGRAM_1080P_DLN_SZ);
	}
	else if ((vscan == ST21_1080P) && (pktFmt == ST_INTEL_SLN_RFC4175_PKT))
	{
		memcpy(s->vctx.fragHistogram[PEND_HIST], s->vctx.fragHistogram[CURR_HIST],
			   ST_FRAG_HISTOGRAM_1080P_SLN_SZ);
	}
	else if ((vscan == ST21_1080P) && (pktFmt == ST_OTHER_SLN_RFC4175_PKT))
	{
		memcpy(s->vctx.fragHistogram[PEND_HIST], s->vctx.fragHistogram[CURR_HIST],
			   ST_FRAG_HISTOGRAM_1080P_SLN_SZ);
	}
	else if ((vscan == ST21_720P) && (pktFmt == ST_INTEL_DLN_RFC4175_PKT))
	{
		memcpy(s->vctx.fragHistogram[PEND_HIST], s->vctx.fragHistogram[CURR_HIST],
			   ST_FRAG_HISTOGRAM_720P_DLN_SZ);
	}
	else if ((vscan == ST21_720P) && (pktFmt == ST_INTEL_SLN_RFC4175_PKT))
	{
		memcpy(s->vctx.fragHistogram[PEND_HIST], s->vctx.fragHistogram[CURR_HIST],
			   ST_FRAG_HISTOGRAM_720P_SLN_SZ);
	}
	else if ((vscan == ST21_720P) && (pktFmt == ST_OTHER_SLN_RFC4175_PKT))
	{
		memcpy(s->vctx.fragHistogram[PEND_HIST], s->vctx.fragHistogram[CURR_HIST],
			   ST_FRAG_HISTOGRAM_720P_SLN_SZ);
	}
	else if (vscan == ST21_2160I)
	{
		memcpy(s->vctx.fragHistogram[PEND_HIST], s->vctx.fragHistogram[CURR_HIST],
			   ST_FRAG_HISTOGRAM_2160I_SLN_SZ);
	}
	else if (vscan == ST21_1080I)
	{
		memcpy(s->vctx.fragHistogram[PEND_HIST], s->vctx.fragHistogram[CURR_HIST],
			   ST_FRAG_HISTOGRAM_1080I_SLN_SZ);
	}
	else if (vscan == ST21_720I)
	{
		memcpy(s->vctx.fragHistogram[PEND_HIST], s->vctx.fragHistogram[CURR_HIST],
			   ST_FRAG_HISTOGRAM_720I_SLN_SZ);
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
RvRtpFixVideoFrameDln720p(st_session_impl_t *s)
{
	uint64_t maxLine = s->sn.frmsRecv * s->fmt.v.pktsInLine;
	uint32_t histIdx = s->consState / NUM_HISTOGRAMS;

	for (uint32_t i = 0; i < 720; i += 2)  // now fix frame 720p
	{
		uint32_t idx = i / 2;
		if (s->vctx.lineHistogram[idx] != (uint32_t)maxLine)
		{
			for (uint32_t j = 0; j < ST_FMT_HD720_PKTS_IN_DLN; j++)
			{
				if (((uint8_t)s->vctx.fragHistogram[histIdx][idx] & (1 << j)) == 0)
				{
					uint32_t offsetLn1 = i * s->vctx.line1Size + j * s->vctx.line1Length;
					uint32_t offsetLn2 = offsetLn1 + s->vctx.line1Size;
					memcpy(&s->consBufs[s->consState].buf[offsetLn1],
						   &s->consBufs[FRAME_PREV].buf[offsetLn1], ST_FMT_HD720_PKT_DLN_SZ);
					memcpy(&s->consBufs[s->consState].buf[offsetLn2],
						   &s->consBufs[FRAME_PREV].buf[offsetLn2], ST_FMT_HD720_PKT_DLN_SZ);
				}
			}
			s->vctx.lineHistogram[idx] = (uint32_t)maxLine;
		}
	}
}

/*****************************************************************************************
 *
 * RvRtpFixVideoFrameSln720p inline
 * Intel standard packets
 */
static inline void
RvRtpFixVideoFrameSln720p(st_session_impl_t *s)
{
	uint64_t maxLine = s->sn.frmsRecv * s->fmt.v.pktsInLine;
	uint32_t histIdx = s->consState / NUM_HISTOGRAMS;

	for (uint32_t i = 0; i < 720; i++)	// now fix frame 720p, 2 pkts of 1200 and then 800
	{
		if (s->vctx.lineHistogram[i] != (uint32_t)maxLine)
		{
			for (uint32_t j = 0; j < ST_FMT_HD720_PKTS_IN_SLN; j++)
			{
				uint8_t bitToTest = (1 << j) << (3 * (i & 0x1));
				if (((uint8_t)s->vctx.fragHistogram[histIdx][i / 2] & bitToTest) == 0)
				{
					if ((1 << j) & 0x1b)  //1200, 1260 payload case
					{
						uint32_t offsetLn1 = i * s->vctx.line1Size + j * s->vctx.line1Length;
						memcpy(&s->consBufs[s->consState].buf[offsetLn1],
							   &s->consBufs[FRAME_PREV].buf[offsetLn1], s->vctx.line1Length);
					}
					else  //800, 680 payload part
					{
						uint32_t offsetLn1 = i * s->vctx.line1Size + j * s->vctx.line1Length;
						uint32_t remaind = s->vctx.line1Size - j * s->vctx.line1Length;
						memcpy(&s->consBufs[s->consState].buf[offsetLn1],
							   &s->consBufs[FRAME_PREV].buf[offsetLn1], remaind);
					}
				}
			}
			s->vctx.lineHistogram[i] = (uint32_t)maxLine;
		}
	}
}

/*****************************************************************************************
 *
 * RvRtpFixVideoFrame720p inline
 * Other vendors packets with not all equal lengths
 */
static inline void
RvRtpFixVideoFrame720p(st_session_impl_t *s)
{
	uint64_t maxLine = s->sn.frmsRecv * s->fmt.v.pktsInLine;
	uint32_t histIdx = s->consState / NUM_HISTOGRAMS;

	for (uint32_t i = 0; i < 720; i++)	// now fix frame 720p
	{
		if (s->vctx.lineHistogram[i] != (uint32_t)maxLine)
		{
			for (uint32_t j = 0; j < ST_FMT_HD720_PKTS_IN_SLN; j++)
			{
				uint8_t bitToTest = (1 << j) << (3 * (i & 0x1));
				if (((uint8_t)s->vctx.fragHistogram[histIdx][i / 2] & bitToTest) == 0)
				{
					if ((1 << j) & 0x1b)  //1200, 1260 payload case
					{
						uint32_t offsetLn1 = i * s->vctx.line1Size + j * s->vctx.line1Length;
						memcpy(&s->consBufs[s->consState].buf[offsetLn1],
							   &s->consBufs[FRAME_PREV].buf[offsetLn1], s->vctx.line1Length);
					}
					else  //800, 680 payload part
					{
						uint32_t offsetLn1 = i * s->vctx.line1Size + j * s->vctx.line1Length;
						uint32_t remaind = s->vctx.line1Size - j * s->vctx.line1Length;
						memcpy(&s->consBufs[s->consState].buf[offsetLn1],
							   &s->consBufs[FRAME_PREV].buf[offsetLn1], remaind);
					}
				}
			}
			s->vctx.lineHistogram[i] = (uint32_t)maxLine;
		}
	}
}

/*****************************************************************************************
 *
 * RvRtpFixVideoFrame720i inline
 * Other vendors packets with not all equal lengths
 */
static inline void
RvRtpFixVideoFrame720i(st_session_impl_t *s)
{
	uint64_t maxLine = s->sn.frmsRecv * s->fmt.v.pktsInLine;
	uint32_t histIdx = s->consState / NUM_HISTOGRAMS;

	for (uint32_t i = 0; i < 360;
		 i++)  // now fix frame 720i, 2 pkts of some length equal then remaind completing the line
	{
		if (s->vctx.lineHistogram[i] != (uint32_t)maxLine)
		{
			for (uint32_t j = 0; j < ST_FMT_HD720_PKTS_IN_SLN; j++)
			{
				uint8_t bitToTest = (1 << j) << (3 * (i & 0x1));
				if (((uint8_t)s->vctx.fragHistogram[histIdx][i / 2] & bitToTest) == 0)
				{
					if ((1 << j) & 0x1b)  //1200, 1260 payload case
					{
						uint32_t offsetLn1 = i * s->vctx.line1Size + j * s->vctx.line1Length;
						memcpy(&s->consBufs[s->consState].buf[offsetLn1],
							   &s->consBufs[FRAME_PREV].buf[offsetLn1], s->vctx.line1Length);
					}
					else  //800, 680 payload part
					{
						uint32_t offsetLn1 = i * s->vctx.line1Size + j * s->vctx.line1Length;
						uint32_t remaind = s->vctx.line1Size - j * s->vctx.line1Length;
						memcpy(&s->consBufs[s->consState].buf[offsetLn1],
							   &s->consBufs[FRAME_PREV].buf[offsetLn1], remaind);
					}
				}
			}
			s->vctx.lineHistogram[i] = (uint32_t)maxLine;
		}
	}
}

/*****************************************************************************************
 *
 * RvRtpFixVideoFrameDln1080p inline
 * Intel standard packets
 */
static inline void
RvRtpFixVideoFrameDln1080p(st_session_impl_t *s)
{
	uint64_t maxLine = s->sn.frmsRecv * s->fmt.v.pktsInLine;
	uint32_t histIdx = s->consState / NUM_HISTOGRAMS;

	for (uint32_t i = 0; i < 1080; i += 2)	// now fix frame
	{
		if ((s->vctx.lineHistogram[i / 2] != (uint32_t)maxLine)
			&& ((uint8_t)s->vctx.fragHistogram[histIdx][i / 2] != 0xff))
		{
			for (uint32_t j = 0; j < ST_FMT_HD1080_PKTS_IN_DLN; j++)
			{
				if (((uint8_t)s->vctx.fragHistogram[histIdx][i / 2] & (1 << j)) == 0)
				{
					uint32_t offsetLn1 = i * s->vctx.line1Size + j * s->vctx.line1Length;
					uint32_t offsetLn2 = offsetLn1 + s->vctx.line1Size;
					memcpy(&s->consBufs[s->consState].buf[offsetLn1],
						   &s->consBufs[FRAME_PREV].buf[offsetLn1], ST_FMT_HD1080_PKT_DLN_SZ);
					memcpy(&s->consBufs[s->consState].buf[offsetLn2],
						   &s->consBufs[FRAME_PREV].buf[offsetLn2], ST_FMT_HD1080_PKT_DLN_SZ);
				}
			}
			s->vctx.lineHistogram[i / 2] = (uint32_t)maxLine;
		}
	}
}

/*****************************************************************************************
 *
 * RvRtpFixVideoFrameSln1080p inline
 * Intel standard packets
 */
static inline void
RvRtpFixVideoFrameSln1080p(st_session_impl_t *s)
{
	uint64_t maxLine = s->sn.frmsRecv * s->fmt.v.pktsInLine;
	uint32_t histIdx = s->consState / NUM_HISTOGRAMS;

	for (uint32_t l = 0; l < 1080; l += 8)	// now fix frame
	{
		uint64_t *p = (uint64_t *)&s->vctx.fragHistogram[histIdx][l];
		if (*p == s->fragPattern)
			continue;

		for (uint32_t k = 0; k < 8; k++)
		{
			uint32_t i = l + k;
			for (uint32_t j = 0; j < ST_FMT_HD1080_PKTS_IN_SLN; j++)
			{
				uint8_t bitToTest = (1 << j) << (4 * (i & 0x1));
				if (((uint8_t)s->vctx.fragHistogram[histIdx][i / 2] & bitToTest) == 0)
				{
					uint32_t offset = i * s->vctx.line1Size + j * s->vctx.line1Length;
					memcpy(&s->consBufs[s->consState].buf[offset],
						   &s->consBufs[FRAME_PREV].buf[offset], ST_FMT_HD1080_PKT_SLN_SZ);
				}
			}
			s->vctx.lineHistogram[i] = (uint32_t)maxLine;
		}
	}
}

/*****************************************************************************************
 *
 * RvRtpFixVideoFrame1080p inline
 * Other vendors packets with not all equal lengths
 */
static inline void
RvRtpFixVideoFrame1080p(st_session_impl_t *s)
{
	uint64_t maxLine = s->sn.frmsRecv * s->fmt.v.pktsInLine;
	uint32_t histIdx = s->consState / NUM_HISTOGRAMS;

	for (uint32_t i = 0; i < 1080; i++)	 // now fix 1080p frame
	{
		if (s->vctx.lineHistogram[i] != (uint32_t)maxLine)
		{
			for (uint32_t j = 0; j < ST_FMT_HD1080_PKTS_IN_SLN; j++)
			{
				uint8_t bitToTest = (1 << j) << (4 * (i & 0x1));
				if (((uint8_t)s->vctx.fragHistogram[histIdx][i / 2] & bitToTest) == 0)
				{
					uint32_t offset = i * s->vctx.line1Size + j * s->vctx.line1Length;
					if ((1 << j) & 0x77)  //1200, 1260 payload case
					{
						memcpy(&s->consBufs[s->consState].buf[offset],
							   &s->consBufs[FRAME_PREV].buf[offset], s->vctx.line1Length);
					}
					else  //800, 680 payload part
					{
						uint32_t remaind = s->vctx.line1Size - j * s->vctx.line1Length;
						memcpy(&s->consBufs[s->consState].buf[offset],
							   &s->consBufs[FRAME_PREV].buf[offset], remaind);
					}
				}
			}
			s->vctx.lineHistogram[i] = (uint32_t)maxLine;
		}
	}
}

/*****************************************************************************************
 *
 * RvRtpFixVideoFrame1080i inline
 * Other vendors packets with not all equal lengths
 */
static inline void
RvRtpFixVideoFrame1080i(st_session_impl_t *s)
{
	uint64_t maxLine = s->sn.frmsRecv * s->fmt.v.pktsInLine;
	uint32_t histIdx = s->consState / NUM_HISTOGRAMS;

	for (uint32_t i = 0; i < 540; i++)	// now fix interlaced field
	{
		if (s->vctx.lineHistogram[i] != (uint32_t)maxLine)
		{
			for (uint32_t j = 0; j < ST_FMT_HD1080_PKTS_IN_SLN; j++)
			{
				uint8_t bitToTest = (1 << j) << (4 * (i & 0x1));
				if (((uint8_t)s->vctx.fragHistogram[histIdx][i / 2] & bitToTest) == 0)
				{
					uint32_t offset = i * s->vctx.line1Size + j * s->vctx.line1Length;
					if ((1 << j) & 0x77)  //1200, 1260 payload case
					{
						memcpy(&s->consBufs[s->consState].buf[offset],
							   &s->consBufs[FRAME_PREV].buf[offset], s->vctx.line1Length);
					}
					else  //800, 680 payload part
					{
						uint32_t remaind = s->vctx.line1Size - j * s->vctx.line1Length;
						memcpy(&s->consBufs[s->consState].buf[offset],
							   &s->consBufs[FRAME_PREV].buf[offset], remaind);
					}
				}
			}
			s->vctx.lineHistogram[i] = (uint32_t)maxLine;
		}
	}
}

/*****************************************************************************************
 *
 * RvRtpFixVideoFrameSln2160p inline, 
 * Intel standard packet with equal sizes
 */
static inline void
RvRtpFixVideoFrameSln2160p(st_session_impl_t *s)
{
	uint32_t maxLine = s->sn.frmsRecv * s->fmt.v.pktsInLine;
	uint32_t histIdx = s->consState / NUM_HISTOGRAMS;

	for (uint32_t l = 0; l < 2160; l += 8)	// now fix frame 2160p Intel standard
	{
		uint64_t *p = (uint64_t *)&s->vctx.fragHistogram[histIdx][l];
		if (*p == s->fragPattern)
			continue;

		for (uint32_t k = 0; k < 8; k++)
		{
			uint32_t i = l + k;
			for (uint32_t j = 0; j < ST_FMT_UHD2160_PKTS_IN_SLN; j++)
			{
				if (((uint8_t)s->vctx.fragHistogram[histIdx][i] & (1 << j)) == 0)
				{
					uint32_t offset = i * s->vctx.line1Size + j * s->vctx.line1Length;
					memcpy(&s->consBufs[s->consState].buf[offset],
						   &s->consBufs[FRAME_PREV].buf[offset], ST_FMT_UHD2160_PKT_SLN_SZ);
				}
			}
			s->vctx.lineHistogram[i] = (uint32_t)maxLine;
		}
	}
}

/*****************************************************************************************
 *
 * RvRtpFixVideoFrame2160p inline, 
 * Other vendors packets with not all equal lengths
 */
static inline void
RvRtpFixVideoFrame2160p(st_session_impl_t *s)
{
	uint32_t maxLine = s->sn.frmsRecv * s->fmt.v.pktsInLine;
	uint32_t histIdx = s->consState / NUM_HISTOGRAMS;

	for (uint32_t l = 0; l < 2160; l += 8)	// now fix frame 2160p other vendors
	{
		uint64_t *p = (uint64_t *)&s->vctx.fragHistogram[histIdx][l];
		if (*p == s->fragPattern)
			continue;

		for (uint32_t k = 0; k < 8; k++)
		{
			uint32_t i = l + k;
			for (uint32_t j = 0; j < ST_FMT_UHD2160_PKTS_IN_SLN; j++)
			{
				if (((uint8_t)s->vctx.fragHistogram[histIdx][i] & (1 << j)) == 0)
				{
					uint32_t offset = i * s->vctx.line1Size + j * s->vctx.line1Length;
					if ((1 << j) < 0x80)
					{
						rte_memcpy(&s->consBufs[s->consState].buf[offset],
								   &s->consBufs[FRAME_PREV].buf[offset], s->vctx.line1Length);
					}
					else
					{
						uint32_t remaind = s->vctx.line1Size - j * s->vctx.line1Length;
						rte_memcpy(&s->consBufs[s->consState].buf[offset],
								   &s->consBufs[FRAME_PREV].buf[offset], remaind);
					}
				}
			}
			s->vctx.lineHistogram[i] = (uint32_t)maxLine;
		}
	}
}

/*****************************************************************************************
 *
 * RvRtpFixVideoFrame2160i inline, 
 * Other vendors packets with not all equal lengths
 */
static inline void
RvRtpFixVideoFrame2160i(st_session_impl_t *s)
{
	uint32_t maxLine = s->sn.frmsRecv * s->fmt.v.pktsInLine;
	uint32_t histIdx = s->consState / NUM_HISTOGRAMS;

	for (uint32_t l = 0; l < 1080; l += 8)	// now fix frame 2160i other vendors
	{
		uint64_t *p = (uint64_t *)&s->vctx.fragHistogram[histIdx][l];
		if (*p == s->fragPattern)
			continue;

		for (uint32_t k = 0; k < 8; k++)
		{
			uint32_t i = l + k;
			for (uint32_t j = 0; j < ST_FMT_UHD2160_PKTS_IN_SLN; j++)
			{
				if (((uint8_t)s->vctx.fragHistogram[histIdx][i] & (1 << j)) == 0)
				{
					uint32_t offset = i * s->vctx.line1Size + j * s->vctx.line1Length;
					if ((1 << j) < 0x80)
					{
						rte_memcpy(&s->consBufs[s->consState].buf[offset],
								   &s->consBufs[FRAME_PREV].buf[offset], s->vctx.line1Length);
					}
					else
					{
						uint32_t remaind = s->vctx.line1Size - j * s->vctx.line1Length;
						rte_memcpy(&s->consBufs[s->consState].buf[offset],
								   &s->consBufs[FRAME_PREV].buf[offset], remaind);
					}
				}
			}
			s->vctx.lineHistogram[i] = (uint32_t)maxLine;
		}
	}
}

/*****************************************************************************************
 *
 * RvRtpFixVideoFrameInline template resolving inline
 */
static inline void
RvRtpFixVideoFrameInline(st_session_impl_t *s, st21_vscan_t vscan, st21_pkt_fmt_t pktFmt)
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
RvRtpIncompleteDropNCont(st_session_impl_t *s, uint32_t rtpTmstamp, uint32_t frameId, uint32_t cont,
						 st21_vscan_t vscan, st21_pkt_fmt_t pktFmt)
{
	if (cont)
		RvRtpDropTmstampPush(s, s->consBufs[frameId].tmstamp);
	else
		RvRtpDropTmstampPush(s, rtpTmstamp);

	s->sn.pktsLost[ST_PKT_LOST(ST_PKT_LOST_TIMEDOUT)]
		+= s->fmt.v.pktsInFrame - s->consBufs[frameId].pkts;
	s->frmsDrop++;
	s->sn.frmsDrop[ST_FRM_DROP(ST_PKT_DROP_INCOMPL_FRAME)]++;

	if (frameId == FRAME_CURR)
	{
		RvRtpClearFragHistInline(s, vscan, pktFmt);
	}
	if (cont)
	{
		s->consBufs[frameId].pkts = 0;
		s->consBufs[frameId].tmstamp = rtpTmstamp;
		s->vctx.data = s->consBufs[frameId].buf;
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
RvRtpFixCurrentFrame(st_session_impl_t *s, uint32_t rtpTmstamp, st21_vscan_t vscan,
					 st21_pkt_fmt_t pktFmt)
{
	uint32_t frameId = s->consState;
#ifdef RX_RECV_DEBUG
	RTE_LOG(INFO, USER3, "Incomplete frame fixed of %lu received pkts %u, shall be %u\n",
			s->sn.frmsRecv + 1, s->consBufs[FRAME_CURR].pkts, s->fmt.v.pktsInFrame);
#endif

	RvRtpFixVideoFrameInline(s, vscan, pktFmt);

	s->sn.frmsRecv++;
	s->frmsFixed++;

	if (stMainParams.isEbuCheck)
	{
		RvRtpCalculateEbuAvg(s);
	}

	s->cons.St21NotifyFrameRecv(s->cons.appHandle, s->consBufs[frameId].buf, rtpTmstamp,
								s->vctx.fieldId);
	s->cons.St21NotifyFrameDone(s->cons.appHandle, s->consBufs[FRAME_PREV].buf, s->vctx.fieldId);
	s->sn.pktsLost[ST_PKT_LOST(ST_PKT_LOST_TIMEDOUT)]
		+= s->fmt.v.pktsInFrame - s->consBufs[frameId].pkts;
	//move active to prev position
	s->consBufs[FRAME_PREV] = s->consBufs[frameId];
	if (frameId != FRAME_PEND)
	{
		s->consBufs[FRAME_CURR].buf = s->cons.St21GetNextFrameBuf(
			s->cons.appHandle, s->consBufs[FRAME_PREV].buf, s->cons.frameSize, s->vctx.fieldId);

		RvRtpClearFragHistInline(s, vscan, pktFmt);
	}
	s->tmstampDone = rtpTmstamp;
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
RvRtpReceiveFastCopyInline(st_session_impl_t *s, const void *rtp, st21_vscan_t vscan,
						   st21_pkt_fmt_t pktFmt, uint32_t whichFrm)
{
	uint32_t histIdx = whichFrm / NUM_HISTOGRAMS;
	if (pktFmt == ST_INTEL_SLN_RFC4175_PKT)
	{
		/* Single line copy payload */
		st_rfc4175_rtp_single_hdr_t *rtpSingle = (st_rfc4175_rtp_single_hdr_t *)rtp;
		uint8_t *payload = (uint8_t *)&rtpSingle[1];

		uint32_t byteLn1Offset
			= s->vctx.line1Number * s->vctx.line1Size
			  + (uint32_t)s->vctx.line1Offset / s->fmt.v.pixelsInGrp * s->vctx.line1PixelGrpSize;

		// solve patterns of vscan
		if (vscan == ST21_2160P)
		{
			if (s->vctx.fragHistogram[histIdx][s->vctx.line1Number])
				return ST_PKT_DROP_REDUNDANT_PATH;

			if (whichFrm == FRAME_CURR)
			{
				s->vctx.lineHistogram[s->vctx.line1Number]++;
			}
			s->vctx.fragHistogram[histIdx][s->vctx.line1Number]
				|= (1 << (s->vctx.line1Offset / s->fmt.v.pixelsInPkt));

#ifndef ST_MEMCPY_TEST
			/* line 1 payload copy */
			memcpy(&s->vctx.data[byteLn1Offset], payload, ST_FMT_UHD2160_PKT_SLN_SZ);
#endif
		}
		else if (vscan == ST21_1080P)
		{
			uint8_t bitToSet = (1 << (s->vctx.line1Offset / s->fmt.v.pixelsInPkt))
							   << (4 * (s->vctx.line1Number & 0x1));
			if (s->vctx.fragHistogram[histIdx][s->vctx.line1Number / 2] & bitToSet)
			{
				return ST_PKT_DROP_REDUNDANT_PATH;
			}
			s->vctx.fragHistogram[histIdx][s->vctx.line1Number / 2] |= bitToSet;

			if (whichFrm == FRAME_CURR)
			{
				s->vctx.lineHistogram[s->vctx.line1Number]++;
			}
#ifndef ST_MEMCPY_TEST
			/* line 1 payload copy */
			memcpy(&s->vctx.data[byteLn1Offset], payload, ST_FMT_HD1080_PKT_SLN_SZ);
#endif
		}
		else if (vscan == ST21_720P)
		{
			uint32_t lnOffset
				= s->vctx.line1Offset / s->fmt.v.pixelsInGrp * s->vctx.line1PixelGrpSize;
			uint8_t bitToSet = (1 << (s->vctx.line1Offset / s->fmt.v.pixelsInPkt))
							   << (3 * (s->vctx.line1Number & 0x1));
			if (s->vctx.fragHistogram[histIdx][s->vctx.line1Number / 2] & bitToSet)
			{
				return ST_PKT_DROP_REDUNDANT_PATH;
			}
			if (whichFrm == FRAME_CURR)
			{
				s->vctx.lineHistogram[s->vctx.line1Number]++;
			}
			s->vctx.fragHistogram[histIdx][s->vctx.line1Number / 2] |= (1 << bitToSet);

			uint16_t line1LenRtp = ntohs(rtpSingle->line1Length);
			if (line1LenRtp + lnOffset > s->vctx.line1Size)
			{
				s->pktsDrop++;
				s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_LN_LEN)]++;
				RTE_LOG(INFO, USER3, "Packet bad RTPLEN of %u pktsDrop %llu\n", line1LenRtp,
						(U64)s->pktsDrop);
				return ST_PKT_DROP_BAD_RTP_LN_LEN;
			}
#ifndef ST_MEMCPY_TEST
			/* line 1 payload copy */
			memcpy(&s->vctx.data[byteLn1Offset], payload, line1LenRtp);
#endif
		}
	}
	else if (pktFmt == ST_INTEL_DLN_RFC4175_PKT)  //dual line packets here
	{
		st_rfc4175_rtp_dual_hdr_t *rtpDual = (st_rfc4175_rtp_dual_hdr_t *)rtp;
		/* Dual line copy payload */
		uint8_t *payload = (uint8_t *)&rtpDual[1];
		uint32_t byteLn1Offset
			= s->vctx.line1Number * s->vctx.line1Size
			  + (uint32_t)s->vctx.line1Offset / s->fmt.v.pixelsInGrp * s->vctx.line1PixelGrpSize;
		uint32_t byteLn2Offset
			= s->vctx.line2Number * s->vctx.line2Size
			  + (uint32_t)s->vctx.line2Offset / s->fmt.v.pixelsInGrp * s->vctx.line2PixelGrpSize;

		if (s->vctx.fragHistogram[histIdx][s->vctx.line1Number])
			return ST_PKT_DROP_REDUNDANT_PATH;
		if (whichFrm == FRAME_CURR)
		{
			s->vctx.lineHistogram[s->vctx.line1Number / 2]++;
		}
		s->vctx.fragHistogram[histIdx][s->vctx.line1Number / 2]
			|= (1 << (s->vctx.line1Offset / s->fmt.v.pixelsInPkt));

		if (vscan == ST21_1080P)
		{
#ifndef ST_MEMCPY_TEST
			/* line 1 */
			memcpy(&s->vctx.data[byteLn1Offset], payload, ST_FMT_HD1080_PKT_DLN_SZ);
			/* line 2 */
			memcpy(&s->vctx.data[byteLn2Offset], &payload[s->vctx.line1Length],
				   ST_FMT_HD1080_PKT_DLN_SZ);
#endif
		}
		else if (vscan == ST21_720P)
		{
#ifndef ST_MEMCPY_TEST
			/* line 1 */
			memcpy(&s->vctx.data[byteLn1Offset], payload, ST_FMT_HD720_PKT_DLN_SZ);
			/* line 2 */
			memcpy(&s->vctx.data[byteLn2Offset], &payload[s->vctx.line1Length],
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
		uint32_t byteLn1Offset
			= (uint32_t)s->vctx.line1Offset / s->fmt.v.pixelsInGrp * s->vctx.line1PixelGrpSize;
		uint16_t line1LenRtp = ntohs(rtpSingle->line1Length);

		if (line1LenRtp + byteLn1Offset > s->vctx.line1Size)
		{
			s->pktsDrop++;
			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_LN_LEN)]++;
			RTE_LOG(INFO, USER3, "Packet bad RTPLEN of %u pktsDrop %llu\n", line1LenRtp,
					(U64)s->pktsDrop);
			return ST_PKT_DROP_BAD_RTP_LN_LEN;
		}
		byteLn1Offset += s->vctx.line1Number * s->vctx.line1Size;
#ifndef ST_MEMCPY_TEST
		/* line 1 payload copy */
		rte_memcpy(&s->vctx.data[byteLn1Offset], payload, line1LenRtp);
#endif
		// solve patterns of vscan
		if ((vscan == ST21_2160P) || (vscan == ST21_2160I))
		{
			if (s->vctx.fragHistogram[histIdx][s->vctx.line1Number])
				return ST_PKT_DROP_REDUNDANT_PATH;
			if (whichFrm == FRAME_CURR)
			{
				s->vctx.lineHistogram[s->vctx.line1Number]++;
			}
			s->vctx.fragHistogram[histIdx][s->vctx.line1Number]
				|= (1 << (s->vctx.line1Offset / s->fmt.v.pixelsInPkt));
		}
		else if ((vscan == ST21_1080P) || (vscan == ST21_1080I))  //1080p and 1080i cases
		{
			uint8_t bitToSet = (1 << (s->vctx.line1Offset / s->fmt.v.pixelsInPkt))
							   << (4 * (s->vctx.line1Number & 0x1));
			if (s->vctx.fragHistogram[histIdx][s->vctx.line1Number / 2] & bitToSet)
			{
				return ST_PKT_DROP_REDUNDANT_PATH;
			}
			if (whichFrm == FRAME_CURR)
			{
				s->vctx.lineHistogram[s->vctx.line1Number]++;
			}
			s->vctx.fragHistogram[histIdx][s->vctx.line1Number / 2] |= bitToSet;
		}
		else if ((vscan == ST21_720P) || (vscan == ST21_720I))
		{
			uint8_t bitToSet = (1 << (s->vctx.line1Offset / s->fmt.v.pixelsInPkt))
							   << (3 * (s->vctx.line1Number & 0x1));
			if (s->vctx.fragHistogram[histIdx][s->vctx.line1Number / 2] & bitToSet)
			{
				return ST_PKT_DROP_REDUNDANT_PATH;
			}
			if (whichFrm == FRAME_CURR)
			{
				s->vctx.lineHistogram[s->vctx.line1Number]++;
			}
			s->vctx.fragHistogram[histIdx][s->vctx.line1Number / 2] |= bitToSet;
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
RvRtpReceiveFastFragCheckInline(st_session_impl_t *s, st21_vscan_t vscan, st21_pkt_fmt_t pktFmt,
								uint32_t frameId)
{
	uint32_t histIdx = frameId / NUM_HISTOGRAMS;
	// solve patterns of vscan
	if (vscan == ST21_2160P)
	{
		for (uint32_t i = 0; i < 2160; i += 8)
		{
			uint64_t *p = (uint64_t *)&s->vctx.fragHistogram[histIdx][i];
			if (*p == s->fragPattern)
				continue;
			//incomplete yet
			return ST_PKT_DROP_INCOMPL_FRAME;
		}
	}
	else if (vscan == ST21_1080P)
	{
		for (uint32_t i = 0; i < 1080 / 2; i += 8)
		{
			uint64_t *p = (uint64_t *)&s->vctx.fragHistogram[histIdx][i];
			if (*p == s->fragPattern)
				continue;
			//incomplete yet
			return ST_PKT_DROP_INCOMPL_FRAME;
		}
	}
	else if ((vscan == ST21_720P) && (pktFmt == ST_INTEL_DLN_RFC4175_PKT))
	{
		for (uint32_t i = 0; i < 720 / 2; i += 8)
		{
			uint64_t *p = (uint64_t *)&s->vctx.fragHistogram[histIdx][i];
			if (*p == s->fragPattern)
				continue;
			//incomplete yet
			return ST_PKT_DROP_INCOMPL_FRAME;
		}
	}
	else if (vscan == ST21_720P)
	{
		for (uint32_t i = 0; i < 720; i += 8)
		{
			uint64_t *p = (uint64_t *)&s->vctx.fragHistogram[histIdx][i];
			if (*p == s->fragPattern)
				continue;
			//incomplete yet
			return ST_PKT_DROP_INCOMPL_FRAME;
		}
	}
	else if (vscan == ST21_2160I)
	{
		for (uint32_t i = 0; i < 1080; i += 8)
		{
			uint64_t *p = (uint64_t *)&s->vctx.fragHistogram[histIdx][i];
			if (*p == s->fragPattern)
				continue;
			return ST_PKT_DROP_INCOMPL_FRAME;
		}
	}
	else if (vscan == ST21_1080I)
	{
		for (uint32_t i = 0; i < 264; i += 8)
		{
			uint64_t *p = (uint64_t *)&s->vctx.fragHistogram[histIdx][i];
			if (*p == s->fragPattern)
				continue;
			return ST_PKT_DROP_INCOMPL_FRAME;
		}
		for (uint32_t i = 264; i < 270; i++)
		{
			if (s->vctx.fragHistogram[histIdx][i] == 0xff)
				continue;
			return ST_PKT_DROP_INCOMPL_FRAME;
		}
	}
	else if (vscan == ST21_720I)
	{
		for (uint32_t i = 0; i < 180; i += 4)
		{
			uint32_t *p = (uint32_t *)&s->vctx.fragHistogram[i];
			if (*p == (uint32_t)s->fragPattern)
				continue;
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
 * assumption:
 * 	- we are not expecting out of order or previous frames
 *      - any packet recieved with marker for current frame is treated as end of frame
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveNextPacketsInline(st_session_impl_t *s, struct rte_mbuf *m, st21_vscan_t vscan,
							  st21_pkt_fmt_t pktFmt)
{
	uint32_t frameId = FRAME_CURR;
	st_status_t res = ST_OK;

	const struct rte_ipv4_hdr *ip = rte_pktmbuf_mtod_offset(m, struct rte_ipv4_hdr *, m->l2_len);
	const struct st_rfc4175_rtp_dual_hdr *rtp = rte_pktmbuf_mtod_offset(
		m, struct st_rfc4175_rtp_dual_hdr *, m->l2_len + m->l3_len + m->l4_len);
	uint32_t rtpTmstamp = ntohl(rtp->tmstamp);
#if (RTE_VER_YEAR < 21)
	const uint64_t dpdk_timestamp = m->timestamp;
#else
	pktpriv_data_t *ptr = rte_mbuf_to_priv(m);
	const uint64_t dpdk_timestamp = ptr->timestamp;
#endif

	s->vctx.data = NULL;  //pointer to right frameBuffer

	/* Validate the IP & UDP & RTP header */
	res = StRtpIpUdpHdrCheck(s, ip);
	if (unlikely(res != ST_OK))
	{
		return res;
	}

	res = StRtpHdrCheck(s, rtp, pktFmt, vscan);
	if (unlikely(res != ST_OK))
	{
		return res;
	}

	if (rtpTmstamp == s->vctx.tmstamp)	//tmstamp match most cases here
	{
		if (unlikely(rtpTmstamp == s->tmstampDone))
		{
			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_TMSTAMP)]++;
			return ST_PKT_DROP_BAD_RTP_TMSTAMP;
		}
		else
		{
			if (unlikely(s->consBufs[FRAME_PEND].tmstamp == rtpTmstamp))
			{
			}

			if (unlikely(s->consBufs[FRAME_CURR].tmstamp > rtpTmstamp))
			{
				RTE_LOG(INFO, USER3, "Packet tmstamp of %u while expected %u matched GEN_ERR 2\n",
						rtpTmstamp, s->consBufs[FRAME_CURR].tmstamp);
				s->pktsDrop++;
				s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_TMSTAMP)]++;
				return ST_PKT_DROP_BAD_RTP_TMSTAMP;
			}

			s->vctx.data = s->consBufs[FRAME_CURR].buf;
			s->sn.pktsRecv++;

			if (stMainParams.isEbuCheck)
			{
				RvRtpCalculatePacketEbu(s, dpdk_timestamp, s->consBufs[FRAME_CURR].pkts);
			}
		}
	}
	else if ((rtpTmstamp > s->vctx.tmstamp)
			 || ((rtpTmstamp & (0x1 << 31))
				 < (s->vctx.tmstamp & (0x1 << 31))))  //new frame condition
	{
		if (stMainParams.isEbuCheck)
		{
#if (RTE_VER_YEAR < 21)
			RvRtpCalculateFrameEbu(s, rtpTmstamp, m->timestamp);
#else
			pktpriv_data_t *ptr = rte_mbuf_to_priv(m);
			RvRtpCalculateFrameEbu(s, rtpTmstamp, ptr->timestamp);
#endif
		}
		if (s->consBufs[FRAME_CURR].tmstamp == 0)
		{
			//First time 2nd frame
			if (unlikely(s->consBufs[FRAME_CURR].buf == NULL))
			{
				s->consBufs[FRAME_CURR].buf
					= s->cons.St21GetNextFrameBuf(s->cons.appHandle, s->consBufs[FRAME_PREV].buf,
												  s->cons.frameSize, s->vctx.fieldId);
				if (unlikely(s->consBufs[FRAME_CURR].buf == NULL))
				{
					return RvRtpDropFrameAtTmstamp(s, rtpTmstamp, ST_PKT_DROP_NO_FRAME_BUF);
				}
			}
		}
		else
		{
			s->consBufs[FRAME_PEND].pkts = s->consBufs[FRAME_CURR].pkts;
			s->consBufs[FRAME_PEND].tmstamp = s->vctx.tmstamp;
			RvRtpCopyFragHistInline(s, vscan, pktFmt);
			s->consBufs[FRAME_PEND].buf = s->consBufs[FRAME_CURR].buf;
			s->pendCnt = 0;
			s->consBufs[FRAME_CURR].buf = s->cons.St21GetNextFrameBuf(
				s->cons.appHandle, s->consBufs[FRAME_CURR].buf, s->cons.frameSize, s->vctx.fieldId);
			if (unlikely(s->consBufs[FRAME_CURR].buf == NULL))
			{
				return RvRtpDropFrameAtTmstamp(s, rtpTmstamp, ST_PKT_DROP_NO_FRAME_BUF);
			}
			RvRtpClearFragHistInline(s, vscan, pktFmt);
		}
		s->consBufs[FRAME_CURR].pkts = 0;
		s->vctx.data = s->consBufs[FRAME_CURR].buf;
		s->consBufs[FRAME_CURR].tmstamp = rtpTmstamp;
		s->sn.pktsRecv++;
	}
	else if ((rtpTmstamp == s->tmstampToDrop[0]) || (rtpTmstamp == s->tmstampToDrop[1]))
	{
		s->pktsDrop++;
		s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_NO_FRAME_BUF)]++;
		return ST_PKT_DROP_NO_FRAME_BUF;
	}
	//out of order packet arrived - drop it silently
	else if (s->vctx.tmstamp > rtpTmstamp)
	{
		if (s->consBufs[FRAME_PEND].buf && s->consBufs[FRAME_PEND].tmstamp == rtpTmstamp)
		{
			frameId = FRAME_PEND;
			s->vctx.data = s->consBufs[FRAME_PEND].buf;
		}
		else if (s->vctx.tmstamp - rtpTmstamp > ST_HUGE_DELAY)
		{
			RvRtpDropFrameAtTmstamp(s, rtpTmstamp, ST_PKT_DROP_NO_FRAME_BUF);
			s->consBufs[FRAME_CURR].pkts = 0;
			s->vctx.data = s->consBufs[FRAME_CURR].buf;
			s->consBufs[FRAME_CURR].tmstamp = rtpTmstamp;
			s->sn.pktsRecv++;
			RvRtpClearFragHistInline(s, vscan, pktFmt);
		}
		else
		{
			s->pktsDrop++;
			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_TMSTAMP)]++;
			return ST_PKT_DROP_BAD_RTP_TMSTAMP;
		}
	}

	if (frameId != FRAME_PEND)
		s->vctx.tmstamp = rtpTmstamp;

	res = RvRtpReceiveFastCopyInline(s, rtp, vscan, pktFmt, frameId);
	if (unlikely(res != ST_OK))
	{
		if (res == ST_PKT_DROP_REDUNDANT_PATH)
			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_REDUNDANT_PATH)]++;
		return res;
	}
	s->consBufs[frameId].pkts++;

	/* is frame complete ?*/
	if (rtp->marker || s->consBufs[frameId].pkts == s->fmt.v.pktsInFrame)
	{
		if (s->consBufs[frameId].tmstamp == rtpTmstamp)
		{
			if (s->consBufs[frameId].pkts == s->fmt.v.pktsInFrame)
			{
				s->sn.frmsRecv++;
				s->cons.St21NotifyFrameRecv(s->cons.appHandle, s->vctx.data, rtpTmstamp,
											s->vctx.fieldId);
				if (stMainParams.isEbuCheck)
				{
					RvRtpCalculateEbuAvg(s);
				}
				if (s->consBufs[FRAME_PREV].buf)
				{
					s->cons.St21NotifyFrameDone(s->cons.appHandle, s->consBufs[FRAME_PREV].buf,
												s->vctx.fieldId);
				}
				memcpy(&s->consBufs[FRAME_PREV], &s->consBufs[frameId], sizeof(rvrtp_bufs_t));
				if (frameId == FRAME_PEND)
				{
					s->consBufs[FRAME_PEND].buf = NULL;
					s->consBufs[FRAME_PEND].pkts = 0;
					s->consBufs[FRAME_PEND].tmstamp = 0;
				}
				else
				{
					s->consBufs[FRAME_CURR].pkts = 0;
					s->consBufs[FRAME_CURR].tmstamp = 0;
					s->consBufs[FRAME_CURR].buf = s->cons.St21GetNextFrameBuf(
						s->cons.appHandle, s->consBufs[FRAME_PREV].buf, s->cons.frameSize,
						s->vctx.fieldId);
					RvRtpClearFragHistInline(s, vscan, pktFmt);
				}
				s->tmstampDone = rtpTmstamp;
			}
		}
	}
	else if (s->consBufs[FRAME_PEND].buf && s->pendCnt++ >= MAX_PENDING_CNT)  //Maybe use Timer?
	{
		s->consState = FRAME_PEND;
		rtpTmstamp = s->consBufs[FRAME_PEND].tmstamp;
		if (s->consBufs[FRAME_PEND].pkts + ST_PKTS_LOSS_ALLOWED >= s->fmt.v.pktsInFrame)
		{
			RvRtpFixCurrentFrame(s, rtpTmstamp, vscan, pktFmt);
			memcpy(&s->consBufs[FRAME_PREV], &s->consBufs[FRAME_PEND], sizeof(rvrtp_bufs_t));
			s->consBufs[FRAME_PEND].pkts = 0;
			s->consBufs[FRAME_PEND].tmstamp = 0;
		}
		else
		{
#ifdef RX_RECV_DEBUG
			RTE_LOG(INFO, USER3, "Incomplete frame dropped of %lu received pkts %u, shall be %u\n",
					s->sn.frmsRecv + 1, s->consBufs[FRAME_PEND].pkts, s->fmt.v.pktsInFrame);
#endif
			RvRtpIncompleteDropNCont(s, rtpTmstamp, FRAME_PEND, FALSE, vscan, pktFmt);
		}
		s->consState = FRAME_CURR;
		s->pendCnt = 0;
		s->consBufs[FRAME_PEND].buf = NULL;
	}

	return ST_OK;
}

/*****************************************************************************************
 *
 * RvRtpReceiveNextPacketsRedundantInline
 *
 * DESCRIPTION: Main function to processes packets within session context
 *
 * assumption:
 *      - we are not expecting out of order or previous frames from the same port - DROP
 *      - any packet recieved with marker for current|Pendig frame is treated as end of frame
 *			1) if the packet is from current - check any previous frame and send it out
 *			2) if the packet is from prending frame - check for complete packet and sending pending frame notification
 *      - we will use prending|prev frame as reference for one instance before drop or send partial packets
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 *
 */
st_status_t
RvRtpReceiveNextPacketsRedundantInline(st_session_impl_t *s, struct rte_mbuf *m, st21_vscan_t vscan,
                                                          st21_pkt_fmt_t pktFmt)
{
	uint32_t frameId = FRAME_CURR;
	st_status_t res = ST_OK;

	const struct rte_ipv4_hdr *ip = rte_pktmbuf_mtod_offset(m, struct rte_ipv4_hdr *, m->l2_len);
	const struct st_rfc4175_rtp_dual_hdr *rtp = rte_pktmbuf_mtod_offset(m, struct st_rfc4175_rtp_dual_hdr *, m->l2_len + m->l3_len + m->l4_len);

	const uint32_t rtpTmstamp = ntohl(rtp->tmstamp);
#if (RTE_VER_YEAR < 21)
	const uint64_t dpdk_timestamp = m->timestamp;
#else
	pktpriv_data_t *ptr = rte_mbuf_to_priv(m);
	const uint64_t dpdk_timestamp = ptr->timestamp;
#endif
	const uint16_t coreId = rte_lcore_id();
	s->vctx.data = NULL;  //pointer to right frameBuffer

	/* scenario case checkers */
	const bool rtpMarkerPacket = (rtp->marker) ? true : false;
	const bool isRedundantPort = (m->port == 1) ? true : false;
	bool isMiddlePacket = false;
	bool isLastPacket = false;
	bool isMiddlePendPacket = false;
	bool isLastPendPacket = false;
	bool pendFramePresent = (s->consBufs[FRAME_PEND].buf) ? true : false;
	bool userNotifyLine = false;

	/* Validate the IP & UDP & RTP header */
	res = StRtpIpUdpHdrCheck(s, ip);
	if (unlikely(res != ST_OK))
	{
		rxThreadStats[coreId].badIpUdp += isRedundantPort ? 0 : 1;
		rxThreadStats[coreId].badIpUdpR += isRedundantPort ? 1 : 0;
		s->pendCnt += 1;
		return res;
	}

	res = StRtpHdrCheck(s, rtp, pktFmt, vscan);
	if (unlikely(res != ST_OK))
	{
		rxThreadStats[coreId].badRtp += isRedundantPort ? 0 : 1;
		rxThreadStats[coreId].badRtpR += isRedundantPort ? 1 : 0;
		s->pendCnt += 1;
		return res;
	}

	if (rtpTmstamp == s->tmstampDone)
	{
		rxThreadStats[coreId].tmpstampDone += isRedundantPort ? 0 : 1;
		rxThreadStats[coreId].tmpstampDoneR += isRedundantPort ? 1 : 0;

		s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_TMSTAMP)]++;
		return ST_PKT_DROP_BAD_RTP_TMSTAMP;
	}

	/* check if rcvd pkt time is expected currFrame */
	if (rtpTmstamp == s->vctx.tmstamp)	/* expected current packet */
	{
		isMiddlePacket = (!rtpMarkerPacket) ? true : false;
		isLastPacket = (rtpMarkerPacket) ? true : false;

		s->vctx.data = s->consBufs[FRAME_CURR].buf;
		s->sn.pktsRecv++;
	}
	/* check if rcvd pkt time is expected pendFrame */
	else if (pendFramePresent && (rtpTmstamp == s->consBufs[FRAME_PEND].tmstamp)) /* expected prev packet */
	{	
		isMiddlePendPacket = (!rtpMarkerPacket) ? true : false;
		isLastPendPacket = (rtpMarkerPacket) ? true : false;

		frameId = FRAME_PEND;
                s->vctx.data = s->consBufs[FRAME_PEND].buf;
                s->sn.pktsRecv++;
	}
	/* pkt can be the first of the frame or any packet which does not match curr|pend frame */
	else
	{
		uint32_t currFrameTmstamp = s->vctx.tmstamp;
		uint32_t pendFrameTmstamp = pendFramePresent ? s->consBufs[FRAME_PEND].tmstamp : s->vctx.tmstamp;
		bool checkRtpTmstampOverflow = (currFrameTmstamp > pendFrameTmstamp) && (rtpTmstamp < currFrameTmstamp);

		bool dropOutofOrderPkts = false;
		bool forceNotifyPend = false;
		bool forceNotifyCurr = false;
		bool restartAsNewFrame = true;

		if (checkRtpTmstampOverflow)
		{
			/* ToDo: 0xFFFFF447 - 30fps & 0xFFFFFA23 - 60fps*/
			if (currFrameTmstamp >= 0xFFFFFA23)
			{
				/* overflow scenario */
				s->pendCnt = 0;
				rxThreadStats[coreId].rtpTmstampOverflow += isRedundantPort ? 0 : 1;
				rxThreadStats[coreId].rtpTmstampOverflowR += isRedundantPort ? 1 : 0;
			}
			else
			{
				/* nvalid scenario */
				rxThreadStats[coreId].rtpTmstampLess += isRedundantPort ? 0 : 1;
				rxThreadStats[coreId].rtpTmstampLessR += isRedundantPort ? 1 : 0;
				s->pendCnt += 1;

				restartAsNewFrame = false;
				/*
				 * send cufrrent frame done notify to user
 				 * reset current expected timestamp to current rtpTmstamp
				 */
				dropOutofOrderPkts = true;
				/* wait for 1 complete frame s->pendCnt == expected then drop*/
				forceNotifyPend = ((s->pendCnt + s->consBufs[FRAME_PEND].pkts) == s->fmt.v.pktsInFrame) ? true : false;
				forceNotifyCurr = ((s->pendCnt + s->consBufs[FRAME_CURR].pkts) == s->fmt.v.pktsInFrame) ? true : false;
			}
		}

		/* 
		 * we have 3 scenarios
		 * 	- there is pend buff to send notification
		 * 	- if there is current frame try moving to pending
		 * 	- if there is no buff try allocating buffer
		 */

		if ((stMainParams.isEbuCheck) && (restartAsNewFrame))
		{
			RvRtpCalculateFrameEbu(s, rtpTmstamp, dpdk_timestamp);
		}

		if ((forceNotifyPend && (s->consBufs[FRAME_PEND].pkts > 0)) && ((forceNotifyCurr && pendFramePresent) || (pendFramePresent && restartAsNewFrame)))
		{
			s->cons.St21NotifyFrameRecv(s->cons.appHandle, s->vctx.data, s->consBufs[FRAME_PEND].tmstamp, s->vctx.fieldId);
			s->cons.St21NotifyFrameDone(s->cons.appHandle, s->consBufs[FRAME_PEND].buf, s->vctx.fieldId);
			s->tmstampDone = s->consBufs[FRAME_PEND].tmstamp;

			rxThreadStats[coreId].incompletePendFrameDone += (s->consBufs[FRAME_PEND].pkts != s->fmt.v.pktsInFrame) ? 1 : 0;
			rxThreadStats[coreId].completePendFrames += (s->consBufs[FRAME_PEND].pkts == s->fmt.v.pktsInFrame) ? 1 : 0;
			rxThreadStats[coreId].userNotifyPendFrame += 1;

			s->tmstampDone = s->consBufs[FRAME_PEND].tmstamp;
			s->consBufs[FRAME_PEND].pkts = 0;
			s->consBufs[FRAME_PEND].tmstamp = 0;
			s->consBufs[FRAME_PEND].buf = NULL;

			if (forceNotifyPend || forceNotifyCurr)
			{
				rxThreadStats[coreId].forcePendBuffOut += isRedundantPort ? 0 : 1;
				rxThreadStats[coreId].forcePendBuffOutR += isRedundantPort ? 1 : 0;
			}
		}

		if (forceNotifyCurr)
		{
			s->cons.St21NotifyFrameRecv(s->cons.appHandle, s->vctx.data, s->consBufs[FRAME_CURR].tmstamp, s->vctx.fieldId);
			s->cons.St21NotifyFrameDone(s->cons.appHandle, s->consBufs[FRAME_CURR].buf, s->vctx.fieldId);

			s->tmstampDone = s->consBufs[FRAME_CURR].tmstamp;
			RvRtpClearFragHistInline(s, vscan, pktFmt);
		
			rxThreadStats[coreId].incompleteFrameDone += (s->consBufs[FRAME_CURR].pkts != s->fmt.v.pktsInFrame) ? 1 : 0;;
			rxThreadStats[coreId].completeFrames += (s->consBufs[FRAME_CURR].pkts == s->fmt.v.pktsInFrame) ? 1 : 0;
			rxThreadStats[coreId].userNotifyFrame += 1;
			s->pendCnt = 0;

			rxThreadStats[coreId].forceCurrBuffOut += isRedundantPort ? 0 : 1;
			rxThreadStats[coreId].forceCurrBuffOutR += isRedundantPort ? 1 : 0;
		}

		/* drop out of order packets */
		if (dropOutofOrderPkts && (restartAsNewFrame == false))
		{
			rxThreadStats[coreId].outOfOrder += isRedundantPort ? 0 : 1;
			rxThreadStats[coreId].outOfOrderR += isRedundantPort ? 1 : 0;

			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_TMSTAMP)]++;
			return ST_PKT_DROP_BAD_RTP_TMSTAMP;
		}

		/* it is a new frame */
		s->sn.frmsRecv++;
		if (stMainParams.isEbuCheck)
		{
			RvRtpCalculateEbuAvg(s);
		}

		/* make current buffer as pend buffer */
		s->consBufs[FRAME_PEND].pkts = s->consBufs[FRAME_CURR].pkts;
		s->consBufs[FRAME_PEND].tmstamp = s->consBufs[FRAME_CURR].tmstamp;
		RvRtpCopyFragHistInline(s, vscan, pktFmt);
		s->consBufs[FRAME_PEND].buf = s->consBufs[FRAME_CURR].buf;

		s->consBufs[FRAME_CURR].buf = s->cons.St21GetNextFrameBuf(s->cons.appHandle, s->consBufs[FRAME_PREV].buf, s->cons.frameSize, s->vctx.fieldId);
		if (unlikely(s->consBufs[FRAME_CURR].buf == NULL))
        	{
                	return RvRtpDropFrameAtTmstamp(s, rtpTmstamp, ST_PKT_DROP_NO_FRAME_BUF);
		}
		s->pendCnt = 0;
		s->consBufs[FRAME_CURR].pkts = 0;
		s->consBufs[FRAME_CURR].tmstamp = 0;
		RvRtpClearFragHistInline(s, vscan, pktFmt);

		s->consBufs[FRAME_CURR].tmstamp = rtpTmstamp;
		s->vctx.tmstamp = rtpTmstamp;
		s->vctx.data = s->consBufs[FRAME_CURR].buf;
		s->sn.pktsRecv++;

		rxThreadStats[coreId].restartAsNewFrame += (restartAsNewFrame && isRedundantPort) ? 0 : 1;
		rxThreadStats[coreId].restartAsNewFrameR += (restartAsNewFrame && isRedundantPort) ? 1 : 0;

		rxThreadStats[coreId].firstPacketGood += isRedundantPort ? 0 : 1;
		rxThreadStats[coreId].firstPacketGoodR += isRedundantPort ? 1 : 0;
	}

	s->consBufs[frameId].lastGoodPacketPort = m->port;
	s->pendCnt += pendFramePresent ? 1 : 0;

	/* counter update */
	rxThreadStats[coreId].nonFirstPacketGood += (!isRedundantPort && isMiddlePacket) ? 1 : 0;
	rxThreadStats[coreId].nonFirstPacketGoodR += (isRedundantPort && isMiddlePacket)? 1 : 0;
	rxThreadStats[coreId].lastPacketGood += (!isRedundantPort && isLastPacket)? 1 : 0;
	rxThreadStats[coreId].lastPacketGoodR += (isRedundantPort && isLastPacket)? 1 : 0;
	rxThreadStats[coreId].nonFirstPacketPendGood += (!isRedundantPort && isMiddlePendPacket) ? 1 : 0;
	rxThreadStats[coreId].nonFirstPacketPendGoodR += (isRedundantPort && isMiddlePendPacket)? 1 : 0;
	rxThreadStats[coreId].lastPacketPendGood += (!isRedundantPort && isLastPendPacket) ? 1 : 0;
	rxThreadStats[coreId].lastPacketPendGoodR += (isRedundantPort && isLastPendPacket)? 1 : 0;

	res = RvRtpReceiveFastCopyInline(s, rtp, vscan, pktFmt, frameId);
	if (unlikely(res != ST_OK))
	{
		if (res == ST_PKT_DROP_REDUNDANT_PATH)
		{
			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_REDUNDANT_PATH)]++;
			rxThreadStats[coreId].fastCopyFail += isRedundantPort ? 0 : 1;
			rxThreadStats[coreId].fastCopyFailR += isRedundantPort ? 1 : 0;
		}
		else
		{
			rxThreadStats[coreId].fastCopyFailErr += isRedundantPort ? 0 : 1;
			rxThreadStats[coreId].fastCopyFailErrR += isRedundantPort ? 1 : 0;
		}

		return res;
	}
	s->consBufs[frameId].pkts++;

	if (userNotifyLine)
	{
		rxThreadStats[coreId].userNotifyLine += 1;
		//s->sn.linesRecv++;
		//s->cons.St21NotifyFrameRecv(s->cons.appHandle, s->vctx.data, rtpTmstamp, s->vctx.fieldId);
	}

	bool sendPendFrame = (s->consBufs[FRAME_PEND].buf) &&
				((s->consBufs[FRAME_PEND].pkts == s->fmt.v.pktsInFrame) ||
				(((s->consBufs[FRAME_CURR].pkts == s->fmt.v.pktsInFrame) || ((s->consBufs[FRAME_PEND].pkts + s->pendCnt) > s->fmt.v.pktsInFrame)) && 
					(m->port) != s->consBufs[frameId].lastGoodPacketPort)) ? true : false;

	if (sendPendFrame)
	//  (s->consBufs[FRAME_CURR].pkts == s->fmt.v.pktsInFrame) && ((s->consBufs[FRAME_PEND].pkts + s->pendCnt) > s->fmt.v.pktsInFrame) - 
	{
		s->sn.frmsRecv++;
		s->cons.St21NotifyFrameRecv(s->cons.appHandle, s->vctx.data, s->consBufs[FRAME_PEND].tmstamp, s->vctx.fieldId);

		if (stMainParams.isEbuCheck)
		{
			RvRtpCalculateEbuAvg(s);
		}

		s->cons.St21NotifyFrameDone(s->cons.appHandle, s->consBufs[FRAME_PEND].buf, s->vctx.fieldId);

		rxThreadStats[coreId].completePendFrames += (s->consBufs[FRAME_PEND].pkts == s->fmt.v.pktsInFrame) ? 1 : 0;
		rxThreadStats[coreId].incompletePendFrameDone += (s->consBufs[FRAME_PEND].pkts != s->fmt.v.pktsInFrame) ? 1 : 0;
		rxThreadStats[coreId].userNotifyPendFrame += 1;

		s->tmstampDone = s->consBufs[FRAME_PEND].tmstamp;
		s->consBufs[FRAME_PEND].pkts = 0;
		s->consBufs[FRAME_PEND].tmstamp = 0;
		s->consBufs[FRAME_PEND].buf = NULL;
		s->pendCnt = 0;

		pendFramePresent = false;
	}

	if ((!pendFramePresent) && (s->consBufs[FRAME_CURR].pkts == s->fmt.v.pktsInFrame))
	{
		s->sn.frmsRecv++;
		s->cons.St21NotifyFrameRecv(s->cons.appHandle, s->vctx.data, s->consBufs[FRAME_CURR].tmstamp, s->vctx.fieldId);

		if (stMainParams.isEbuCheck)
		{
			RvRtpCalculateEbuAvg(s);
		}

		s->cons.St21NotifyFrameDone(s->cons.appHandle, s->consBufs[FRAME_CURR].buf, s->vctx.fieldId);

		s->tmstampDone = s->consBufs[FRAME_CURR].tmstamp;
		s->consBufs[FRAME_CURR].pkts = 0;
		s->consBufs[FRAME_CURR].tmstamp = 0;
		s->consBufs[FRAME_CURR].buf = s->cons.St21GetNextFrameBuf(s->cons.appHandle, s->consBufs[FRAME_PREV].buf, s->cons.frameSize, s->vctx.fieldId);
		RvRtpClearFragHistInline(s, vscan, pktFmt);

		rxThreadStats[coreId].completeFrames += 1;
		rxThreadStats[coreId].userNotifyFrame += 1;
	}

	return ST_OK;
}

/*****************************************************************************************
 *
 * RvRtpReceiveNextPacketsPrimaryInline
 *
 * DESCRIPTION
 * Main function to processes packets within session context
 *
 * assumption:
 * 	- we are not expecting out of order or previous frames
 *      - any packet recieved with marker for current frame is treated as end of frame
 *
 * RETURNS: st_status_t
 *
 * SEE ALSO:
 */
st_status_t
RvRtpReceiveNextPacketsPrimaryInline(st_session_impl_t *s, struct rte_mbuf *m, st21_vscan_t vscan,
							  st21_pkt_fmt_t pktFmt)
{
	uint32_t frameId = FRAME_CURR;
	st_status_t res = ST_OK;

	const struct rte_ipv4_hdr *ip = rte_pktmbuf_mtod_offset(m, struct rte_ipv4_hdr *, m->l2_len);
	const struct st_rfc4175_rtp_dual_hdr *rtp = rte_pktmbuf_mtod_offset(
		m, struct st_rfc4175_rtp_dual_hdr *, m->l2_len + m->l3_len + m->l4_len);
	const uint32_t rtpTmstamp = ntohl(rtp->tmstamp);
#if (RTE_VER_YEAR < 21)
	const uint64_t dpdk_timestamp = m->timestamp;
#else
	pktpriv_data_t *ptr = rte_mbuf_to_priv(m);
	const uint64_t dpdk_timestamp = ptr->timestamp;
#endif

	const uint16_t coreId = rte_lcore_id();

	s->vctx.data = NULL;  //pointer to right frameBuffer

	/* Validate the IP & UDP header */
	res = StRtpIpUdpHdrCheck(s, ip);
	if (unlikely(res != ST_OK))
	{
		rxThreadStats[coreId].badIpUdp += 1;
		return res;
	}

	/* Validate the RTP header */
	res = StRtpHdrCheck(s, rtp, pktFmt, vscan);
	if (unlikely(res != ST_OK))
	{
		rxThreadStats[coreId].badRtp += 1;
		return res;
	}

	//	bool overflowRtpNewPacket = false;
	const bool rtpMarkerPacket = (rtp->marker) ? true : false;
	const bool userNotifyLine = false;
	bool userNotifyFrame = false;
	bool isMiddlePendPacket = false;
	bool isLastPendPacket = false;
	uint32_t frameDoneTotalPkts = 0;

	if (unlikely(rtpTmstamp == s->tmstampDone))
	{
		rxThreadStats[coreId].tmpstampDone += 1;
		s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_TMSTAMP)]++;
		return ST_PKT_DROP_BAD_RTP_TMSTAMP;
	}

	if (likely(rtpTmstamp == s->vctx.tmstamp))	//tmstamp match most cases here
	{
		isMiddlePendPacket = (!rtpMarkerPacket) ? true : false;
		isLastPendPacket = (rtpMarkerPacket) ? true : false;

		rxThreadStats[coreId].nonFirstPacketGood += (!rtpMarkerPacket) ? 0 : 1;
		/* except first packet in the frame all subsequent packet of the frame */
		s->vctx.data = s->consBufs[FRAME_CURR].buf;
		s->sn.pktsRecv++;

		/* ToDo: user notify for n packets */
		//s->cons.St21NotifyFrameRecv(s->cons.appHandle, s->vctx.data + n * nCount, rtpTmstamp, s->vctx.fieldId);
		//userNotifyLine = true;

		if (stMainParams.isEbuCheck)
		{
			RvRtpCalculatePacketEbu(s, dpdk_timestamp, s->consBufs[FRAME_CURR].pkts);
		}
	}
	else
	{
		const uint32_t currFrameTmstamp = s->vctx.tmstamp;
		const bool checkRtpTmstampOverflow = (rtpTmstamp < currFrameTmstamp);
		bool dropOutofOrderPkts = false;
		bool restartAsNewFrame = true;

		if (checkRtpTmstampOverflow)
		{
			/* ToDo: 0xFFFFF447 - 30fps & 0xFFFFFA23 - 60fps*/
			if (currFrameTmstamp >= 0xFFFFFA23)
			{
				s->pendCnt = 0;
				rxThreadStats[coreId].rtpTmstampOverflow += 1;
			}
			else
			{
				/*
				 * send current frame done notify to user
				 */
				s->pendCnt += 1;
				rxThreadStats[coreId].rtpTmstampLess += 1;
				dropOutofOrderPkts = true;
				restartAsNewFrame= ((s->pendCnt + s->consBufs[FRAME_CURR].pkts) == s->fmt.v.pktsInFrame) ? true : false;
			}
		}
	
		/* new frame scenario */
		if (restartAsNewFrame)
		{

			if (stMainParams.isEbuCheck)
			{
				RvRtpCalculateEbuAvg(s);
			}
			rxThreadStats[coreId].restartAsNewFrame += 1;

			s->consBufs[FRAME_CURR].tmstamp = s->vctx.tmstamp;

			s->cons.St21NotifyFrameRecv(s->cons.appHandle, s->vctx.data, rtpTmstamp, s->vctx.fieldId);
			s->cons.St21NotifyFrameDone(s->cons.appHandle, s->consBufs[FRAME_CURR].buf, s->vctx.fieldId);
			s->sn.frmsRecv++;

			s->tmstampDone = s->consBufs[FRAME_CURR].tmstamp;

			rxThreadStats[coreId].incompleteFrameDone += (s->consBufs[FRAME_CURR].pkts < s->fmt.v.pktsInFrame) ? 0 : 1;

			s->consBufs[FRAME_CURR].buf = s->cons.St21GetNextFrameBuf(s->cons.appHandle, s->consBufs[FRAME_CURR].buf, s->cons.frameSize, s->vctx.fieldId);
			if (unlikely(s->consBufs[FRAME_CURR].buf == NULL))
			{
				return RvRtpDropFrameAtTmstamp(s, rtpTmstamp, ST_PKT_DROP_NO_FRAME_BUF);
			}
			RvRtpClearFragHistInline(s, vscan, pktFmt);

			/* making current packet as new frame */
			s->consBufs[FRAME_CURR].pkts = 0;
			s->vctx.data = s->consBufs[FRAME_CURR].buf;
			s->consBufs[FRAME_CURR].tmstamp = rtpTmstamp;
		}

		if (dropOutofOrderPkts)
		{
			rxThreadStats[coreId].forceCurrBuffOut += 1;
			/* if no overflow drop packets */
			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_TMSTAMP)]++;
			return ST_PKT_DROP_BAD_RTP_TMSTAMP;
		}
		if (stMainParams.isEbuCheck)
		{
			RvRtpCalculatePacketEbu(s, dpdk_timestamp, 1);
			RvRtpCalculateFrameEbu(s, rtpTmstamp, dpdk_timestamp);
		}

		s->sn.pktsRecv++;
		rxThreadStats[coreId].firstPacketGood += 1;
	}

	rxThreadStats[coreId].nonFirstPacketPendGood += isMiddlePendPacket ? 1 : 0;
	rxThreadStats[coreId].lastPacketPendGood += isLastPendPacket ? 1 : 0;

	s->vctx.tmstamp = rtpTmstamp;
	res = RvRtpReceiveFastCopyInline(s, rtp, vscan, pktFmt, frameId);
	if (unlikely(res != ST_OK))
	{
		if (res == ST_PKT_DROP_REDUNDANT_PATH)
			rxThreadStats[coreId].fastCopyFail += 1;
		else
			rxThreadStats[coreId].fastCopyFailErr += 1;
		return res;
	}
	s->consBufs[frameId].pkts++;

	if ((rtpMarkerPacket) && (s->consBufs[frameId].pkts >= 1))
	{
		userNotifyFrame = true;
	}

	if (userNotifyLine)
	{
		rxThreadStats[coreId].userNotifyLine += 1;
	}

	if (userNotifyFrame)
	{
		s->sn.frmsRecv++;
		s->cons.St21NotifyFrameRecv(s->cons.appHandle, s->vctx.data, rtpTmstamp, s->vctx.fieldId);
		rxThreadStats[coreId].userNotifyFrame += 1;
		s->cons.St21NotifyFrameDone(s->cons.appHandle, s->consBufs[FRAME_CURR].buf, s->vctx.fieldId);
		s->tmstampDone = rtpTmstamp;
	}

	if (rtpMarkerPacket)
	{
		rxThreadStats[coreId].lastPacketGood += 1;
		frameDoneTotalPkts = s->consBufs[FRAME_CURR].pkts;

		s->consBufs[FRAME_CURR].pkts = 0;
		s->consBufs[FRAME_CURR].tmstamp = 0;
		s->tmstampDone = rtpTmstamp;
		s->consBufs[FRAME_CURR].buf = s->cons.St21GetNextFrameBuf(
			s->cons.appHandle, s->consBufs[FRAME_PREV].buf, s->cons.frameSize, s->vctx.fieldId);
		RvRtpClearFragHistInline(s, vscan, pktFmt);
	}

	if ((rtpMarkerPacket) && (frameDoneTotalPkts == s->fmt.v.pktsInFrame))
	{
		rxThreadStats[coreId].completeFrames += 1;
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
RvRtpReceiveNextPackets720p(st_session_impl_t *s, struct rte_mbuf *mbuf)
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
RvRtpReceiveNextPackets1080p(st_session_impl_t *s, struct rte_mbuf *mbuf)
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
RvRtpReceiveNextPackets2160p(st_session_impl_t *s, struct rte_mbuf *mbuf)
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
RvRtpReceiveNextPackets720i(st_session_impl_t *s, struct rte_mbuf *mbuf)
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
RvRtpReceiveNextPackets1080i(st_session_impl_t *s, struct rte_mbuf *mbuf)
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
RvRtpReceiveNextPackets2160i(st_session_impl_t *s, struct rte_mbuf *mbuf)
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
RvRtpReceiveNextPacketsSln720p(st_session_impl_t *s, struct rte_mbuf *mbuf)
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
RvRtpReceiveNextPacketsSln1080p(st_session_impl_t *s, struct rte_mbuf *mbuf)
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
RvRtpReceiveNextPacketsSln2160p(st_session_impl_t *s, struct rte_mbuf *mbuf)
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
RvRtpReceiveNextPacketsDln720p(st_session_impl_t *s, struct rte_mbuf *mbuf)
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
RvRtpReceiveNextPacketsDln1080p(st_session_impl_t *s, struct rte_mbuf *mbuf)
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
	if (pktFmt == ST_INTEL_SLN_RFC4175_PKT)
	{
		switch (vscan)
		{
		case ST21_2160P:
			return RvRtpReceiveNextPacketsSln2160p;
		case ST21_1080P:
			return RvRtpReceiveNextPacketsSln1080p;
		case ST21_720P:
			return RvRtpReceiveNextPacketsSln720p;
		default:
			return NULL;
		}
	}
	else if (pktFmt == ST_OTHER_SLN_RFC4175_PKT)
	{
		switch (vscan)
		{
		case ST21_2160P:
			return RvRtpReceiveNextPackets2160p;
		case ST21_1080P:
			return RvRtpReceiveNextPackets1080p;
		case ST21_720P:
			return RvRtpReceiveNextPackets720p;
		case ST21_2160I:
			return RvRtpReceiveNextPackets2160i;
		case ST21_1080I:
			return RvRtpReceiveNextPackets1080i;
		case ST21_720I:
			return RvRtpReceiveNextPackets720i;
		default:
			return NULL;
		}
	}
	else if (pktFmt == ST_INTEL_DLN_RFC4175_PKT)
	{
		switch (vscan)
		{
		case ST21_1080P:
			return RvRtpReceiveNextPacketsDln1080p;
		case ST21_720P:
			return RvRtpReceiveNextPacketsDln720p;
		default:
			return NULL;
		}
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
RvRtpReceivePacketCallback(st_session_impl_t *s, struct rte_mbuf *m)
{
	struct rte_ipv4_hdr *ip = rte_pktmbuf_mtod_offset(m, struct rte_ipv4_hdr *, m->l2_len);
	struct st_rfc4175_rtp_dual_hdr *rtp = rte_pktmbuf_mtod_offset(
		m, struct st_rfc4175_rtp_dual_hdr *, m->l2_len + m->l3_len + m->l4_len);

	if (!(s->cons.St21RecvRtpPkt))
		ST_ASSERT;

	if (m->pkt_len < MIN_PKT_SIZE)
		return ST_PKT_DROP_BAD_PKT_LEN;

	/* Validate the IP & UDP header */
	st_status_t res;
	if ((res = StRtpIpUdpHdrCheck(s, ip)) != ST_OK)
	{
		return res;
	}

	uint32_t hdrSize = m->l2_len + m->l3_len + m->l4_len + sizeof(st_rfc4175_rtp_single_hdr_t);
	uint8_t *rtpPayload = (uint8_t *)&rtp[1];
	uint32_t payloadSize = m->pkt_len - hdrSize;
	uint8_t *pktHdr = rte_pktmbuf_mtod_offset(m, uint8_t *, 0);

	switch (s->cons.consType)
	{
	case ST21_CONS_RAW_L2_PKT:
#if (RTE_VER_YEAR < 21)
		return s->cons.St21RecvRtpPkt(s->cons.appHandle, pktHdr, hdrSize, rtpPayload, payloadSize,
									  m->timestamp);
#else
	{
		pktpriv_data_t *ptr = rte_mbuf_to_priv(m);
		return s->cons.St21RecvRtpPkt(s->cons.appHandle, pktHdr, hdrSize, rtpPayload, payloadSize,
									  ptr->timestamp);
	}
#endif

	case ST21_CONS_RAW_RTP:
#if (RTE_VER_YEAR < 21)
		return s->cons.St21RecvRtpPkt(s->cons.appHandle, (uint8_t *)rtp,
									  sizeof(st_rfc4175_rtp_single_hdr_t), rtpPayload, payloadSize,
									  m->timestamp);
#else
	{
		pktpriv_data_t *ptr = rte_mbuf_to_priv(m);
		return s->cons.St21RecvRtpPkt(s->cons.appHandle, (uint8_t *)rtp,
									  sizeof(st_rfc4175_rtp_single_hdr_t), rtpPayload, payloadSize,
									  ptr->timestamp);
	}
#endif
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
RvRtpReceiveFirstPacketsInline(st_session_impl_t *s, struct rte_mbuf *m, st21_vscan_t vscan,
							   st21_pkt_fmt_t pktFmt)
{
	struct rte_ipv4_hdr *ip = rte_pktmbuf_mtod_offset(m, struct rte_ipv4_hdr *, m->l2_len);
	struct st_rfc4175_rtp_dual_hdr *rtp = rte_pktmbuf_mtod_offset(
		m, struct st_rfc4175_rtp_dual_hdr *, (m->l2_len + m->l3_len + m->l4_len));
	int frameId = FRAME_PREV;

	s->vctx.data = NULL;  //pointer to right frameBuffer

	/* Validate the IP & UDP & RTP header */
	st_status_t res;
	if (((res = StRtpIpUdpHdrCheck(s, ip)) != ST_OK)
		|| ((res = StRtpHdrCheck(s, rtp, pktFmt, vscan)) != ST_OK))
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

	if (rtpTmstamp == s->vctx.tmstamp)	//tmstamp match most cases here
	{
#ifdef ST_DONT_IGNORE_PKT_CHECK
		if (s->consBufs[FRAME_PREV].tmstamp == rtpTmstamp)
#endif
		{
			s->vctx.data = s->consBufs[FRAME_PREV].buf;
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
#if (RTE_VER_YEAR < 21)
			RvRtpCalculatePacketEbu(s, m->timestamp, s->consBufs[FRAME_PREV].pkts);
#else
			pktpriv_data_t *ptr = rte_mbuf_to_priv(m);
			RvRtpCalculatePacketEbu(s, ptr->timestamp, s->consBufs[FRAME_PREV].pkts);
#endif
		}
#endif
	}
	else if ((rtpTmstamp > s->vctx.tmstamp)
			 || ((rtpTmstamp & (0x1 << 31))
				 < (s->vctx.tmstamp & (0x1 << 31))))  //new frame condition
	{
		if (unlikely(s->vctx.tmstamp == 0))
		{
			//1st time packet arrived on the sessions
			s->consBufs[FRAME_PREV].pkts = 1;
			s->vctx.data = s->consBufs[FRAME_PREV].buf;
			s->consBufs[FRAME_PREV].tmstamp = rtpTmstamp;
			s->sn.pktsRecv++;
			s->lastTmstamp = rtpTmstamp - s->vctx.tmstampEvenInc;
		}
		else
		{
			//new frame condition when yet receiving previous
			//so previous is incomplete -> drop it and keep previous as state
#ifdef RX_RECV_DEBUG
			RTE_LOG(INFO, USER3,
					"Incomplete 1st frame tmstamp of %u received pkts %u, shall be %u rtpTmstamp "
					"%x s->ctx.tmstamp %x\n",
					s->sn.timeslot, s->consBufs[FRAME_PREV].pkts, s->fmt.v.pktsInFrame, rtpTmstamp,
					s->vctx.tmstamp);
#endif
			uint32_t complete = 1;
			if (RvRtpReceiveFastFragCheckInline(s, vscan, pktFmt, FRAME_PREV)
				== ST_PKT_DROP_INCOMPL_FRAME)
			{
				s->consBufs[FRAME_PEND].pkts = s->consBufs[FRAME_PREV].pkts;
				s->consBufs[FRAME_PEND].tmstamp = s->vctx.tmstamp;
				RvRtpCopyFragHistInline(s, vscan, pktFmt);
				s->consBufs[FRAME_PEND].buf = s->consBufs[FRAME_PREV].buf;
				s->consBufs[FRAME_PREV].buf
					= s->cons.St21GetNextFrameBuf(s->cons.appHandle, s->consBufs[FRAME_PREV].buf,
												  s->cons.frameSize, s->vctx.fieldId);
				s->vctx.data = s->consBufs[FRAME_PREV].buf;
				s->consBufs[FRAME_PREV].pkts = 0;
				RvRtpClearFragHistInline(s, vscan, pktFmt);
				complete = 0;
			}
			if (complete)
			{
				s->sn.frmsRecv++;
				s->cons.St21NotifyFrameRecv(s->cons.appHandle, s->vctx.data, s->vctx.tmstamp,
											s->vctx.fieldId);
				s->cons.St21NotifyFrameDone(s->cons.appHandle, s->consBufs[FRAME_PREV].buf,
											s->vctx.fieldId);
				s->consState = FRAME_CURR;
				s->consBufs[FRAME_CURR].pkts = 0;
				s->consBufs[FRAME_CURR].tmstamp = rtpTmstamp;

				s->RecvRtpPkt = RvRtpGetReceiveFunction(vscan, pktFmt);
				if (NULL == s->RecvRtpPkt)
					return ST_GENERAL_ERR;

				s->consBufs[FRAME_CURR].buf
					= s->cons.St21GetNextFrameBuf(s->cons.appHandle, s->consBufs[FRAME_PREV].buf,
												  s->cons.frameSize, s->vctx.fieldId);
				if (unlikely(s->consBufs[FRAME_CURR].buf == NULL))
				{
					return RvRtpDropFrameAtTmstamp(s, rtpTmstamp, ST_PKT_DROP_NO_FRAME_BUF);
				}
				s->vctx.data = s->consBufs[FRAME_PREV].buf;
				RvRtpClearFragHistInline(s, vscan, pktFmt);
			}
		}
#ifdef ST_EBU_IN_1ST_PACKET
		if (stMainParams.isEbuCheck)
		{
#if (RTE_VER_YEAR < 21)
			RvRtpCalculateFrameEbu(s, rtpTmstamp, m->timestamp);
#else
			pktpriv_data_t *ptr = rte_mbuf_to_priv(m);
			RvRtpCalculateFrameEbu(s, rtpTmstamp, ptr->timestamp);
#endif
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
	else if (s->vctx.tmstamp > rtpTmstamp)
	{
		if (s->consBufs[FRAME_PEND].buf && rtpTmstamp == s->consBufs[FRAME_PEND].tmstamp)
		{
			frameId = FRAME_PEND;
			s->vctx.data = s->consBufs[FRAME_PEND].buf;
		}
		else
		{
			s->pktsDrop++;
			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_BAD_RTP_TMSTAMP)]++;
			return ST_PKT_DROP_BAD_RTP_TMSTAMP;
		}
	}

	if (frameId != FRAME_PEND)
		s->vctx.tmstamp = rtpTmstamp;

	res = RvRtpReceiveFastCopyInline(s, rtp, vscan, pktFmt, frameId);
	if (unlikely(res != ST_OK))
	{
		if (res == ST_PKT_DROP_REDUNDANT_PATH)
		{
			s->pktsDrop++;
			s->sn.pktsDrop[ST_PKT_DROP(ST_PKT_DROP_REDUNDANT_PATH)]++;
		}
		return res;
	}
	s->consBufs[frameId].pkts++;

	/* is frame complete ?*/
	if (rtp->marker || s->consBufs[frameId].pkts == s->fmt.v.pktsInFrame)
	{
		if (s->consBufs[frameId].pkts != s->fmt.v.pktsInFrame)
		{
#ifdef RX_RECV_DEBUG
			RTE_LOG(INFO, USER3,
					"Frame complete: Incomplete 1st frame of %u received pkts %u, shall be %u\n",
					s->sn.timeslot, s->consBufs[FRAME_PREV].pkts, s->fmt.v.pktsInFrame);
#endif
			return ST_OK;
			//check frag histogram if it is complete i.e. we have 1st frame completed from several
			if (RvRtpReceiveFastFragCheckInline(s, vscan, pktFmt, frameId)
				== ST_PKT_DROP_INCOMPL_FRAME)
			{
				return RvRtpIncompleteDropNCont(s, rtpTmstamp, FRAME_PREV, FALSE, vscan, pktFmt);
			}
		}

		//here frame completed as we have full histogram
		s->sn.frmsRecv++;
#ifdef ST_EBU_IN_1ST_PACKET
		if (stMainParams.isEbuCheck)
		{
			RvRtpCalculateEbuAvg(s);
		}
#endif
		s->cons.St21NotifyFrameRecv(s->cons.appHandle, s->vctx.data, rtpTmstamp, s->vctx.fieldId);
		s->cons.St21NotifyFrameDone(s->cons.appHandle, s->consBufs[frameId].buf, s->vctx.fieldId);
		s->consState = FRAME_CURR;
		s->RecvRtpPkt = RvRtpGetReceiveFunction(vscan, pktFmt);
		if (NULL == s->RecvRtpPkt)
			return ST_GENERAL_ERR;

		if (frameId == FRAME_PEND)
		{
			s->consBufs[FRAME_CURR].pkts = s->consBufs[FRAME_PREV].pkts;
			s->consBufs[FRAME_CURR].tmstamp = s->vctx.tmstamp;
			s->consBufs[FRAME_CURR].buf = s->consBufs[FRAME_PREV].buf;
		}
		else
		{
			s->consBufs[FRAME_CURR].pkts = 0;
			s->consBufs[FRAME_CURR].tmstamp = 0;
			s->consBufs[FRAME_CURR].buf = s->cons.St21GetNextFrameBuf(
				s->cons.appHandle, s->consBufs[FRAME_PREV].buf, s->cons.frameSize, s->vctx.fieldId);
			RvRtpClearFragHistInline(s, vscan, pktFmt);
		}
		s->consBufs[FRAME_PEND].buf = NULL;
		s->pendCnt = 0;
		s->tmstampDone = rtpTmstamp;
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
RvRtpReceiveFirstPackets720p(st_session_impl_t *s, struct rte_mbuf *mbuf)
{
	return (stMainParams.numPorts == 2) ?
		RvRtpReceiveNextPacketsRedundantInline(s, mbuf, ST21_720P, ST_OTHER_SLN_RFC4175_PKT):
		RvRtpReceiveNextPacketsPrimaryInline(s, mbuf, ST21_720P, ST_OTHER_SLN_RFC4175_PKT);
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
RvRtpReceiveFirstPackets720i(st_session_impl_t *s, struct rte_mbuf *mbuf)
{
	return (stMainParams.numPorts == 2) ? 
		RvRtpReceiveNextPacketsRedundantInline(s, mbuf, ST21_720I, ST_OTHER_SLN_RFC4175_PKT):
		RvRtpReceiveNextPacketsPrimaryInline(s, mbuf, ST21_720I, ST_OTHER_SLN_RFC4175_PKT);
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
RvRtpReceiveFirstPacketsSln720p(st_session_impl_t *s, struct rte_mbuf *mbuf)
{
	return (stMainParams.numPorts == 2) ?
		RvRtpReceiveNextPacketsRedundantInline(s, mbuf, ST21_720P, ST_INTEL_SLN_RFC4175_PKT):
		RvRtpReceiveNextPacketsPrimaryInline(s, mbuf, ST21_720P, ST_INTEL_SLN_RFC4175_PKT);
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
RvRtpReceiveFirstPackets1080i(st_session_impl_t *s, struct rte_mbuf *mbuf)
{
	return (stMainParams.numPorts == 2) ?
		RvRtpReceiveNextPacketsRedundantInline(s, mbuf, ST21_1080I, ST_OTHER_SLN_RFC4175_PKT):
		RvRtpReceiveNextPacketsPrimaryInline(s, mbuf, ST21_1080I, ST_OTHER_SLN_RFC4175_PKT);
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
RvRtpReceiveFirstPackets1080p(st_session_impl_t *s, struct rte_mbuf *mbuf)
{
	return (stMainParams.numPorts == 2) ?
		RvRtpReceiveNextPacketsRedundantInline(s, mbuf, ST21_1080P, ST_OTHER_SLN_RFC4175_PKT):
		RvRtpReceiveNextPacketsPrimaryInline(s, mbuf, ST21_1080P, ST_OTHER_SLN_RFC4175_PKT);
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
RvRtpReceiveFirstPacketsSln1080p(st_session_impl_t *s, struct rte_mbuf *mbuf)
{
	return (stMainParams.numPorts == 2) ?
		RvRtpReceiveNextPacketsRedundantInline(s, mbuf, ST21_1080P, ST_INTEL_SLN_RFC4175_PKT) :
		RvRtpReceiveNextPacketsPrimaryInline(s, mbuf, ST21_1080P, ST_INTEL_SLN_RFC4175_PKT);
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
RvRtpReceiveFirstPackets2160p(st_session_impl_t *s, struct rte_mbuf *mbuf)
{
	return (stMainParams.numPorts == 2) ?
		RvRtpReceiveNextPacketsRedundantInline(s, mbuf, ST21_2160P, ST_OTHER_SLN_RFC4175_PKT):
		RvRtpReceiveNextPacketsPrimaryInline(s, mbuf, ST21_2160P, ST_OTHER_SLN_RFC4175_PKT);
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
RvRtpReceiveFirstPackets2160i(st_session_impl_t *s, struct rte_mbuf *mbuf)
{
	return (stMainParams.numPorts == 2) ?
		RvRtpReceiveNextPacketsRedundantInline(s, mbuf, ST21_2160I, ST_OTHER_SLN_RFC4175_PKT):
		RvRtpReceiveNextPacketsPrimaryInline(s, mbuf, ST21_2160I, ST_OTHER_SLN_RFC4175_PKT);
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
RvRtpReceiveFirstPacketsSln2160p(st_session_impl_t *s, struct rte_mbuf *mbuf)
{
	return (stMainParams.numPorts == 2) ?
		RvRtpReceiveNextPacketsRedundantInline(s, mbuf, ST21_2160P, ST_INTEL_SLN_RFC4175_PKT):
		RvRtpReceiveNextPacketsPrimaryInline(s, mbuf, ST21_2160P, ST_INTEL_SLN_RFC4175_PKT);
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
RvRtpReceiveFirstPacketsSln2160i(st_session_impl_t *s, struct rte_mbuf *mbuf)
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
RvRtpReceiveFirstPacketsDln720p(st_session_impl_t *s, struct rte_mbuf *mbuf)
{
	return (stMainParams.numPorts == 2) ?
		RvRtpReceiveNextPacketsRedundantInline(s, mbuf, ST21_720P, ST_INTEL_DLN_RFC4175_PKT):
		RvRtpReceiveNextPacketsPrimaryInline(s, mbuf, ST21_720P, ST_INTEL_DLN_RFC4175_PKT);
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
RvRtpReceiveFirstPacketsDln1080p(st_session_impl_t *s, struct rte_mbuf *mbuf)
{
	return (stMainParams.numPorts == 2) ? 
		RvRtpReceiveNextPacketsRedundantInline(s, mbuf, ST21_1080P, ST_INTEL_DLN_RFC4175_PKT):
		RvRtpReceiveNextPacketsPrimaryInline(s, mbuf, ST21_1080P, ST_INTEL_DLN_RFC4175_PKT);
}

/*****************************************************************************************
 *
 * StRtpDispatchPacket
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
StRtpDispatchPacket(const st_device_impl_t *dev, st_flow_t *flow, struct rte_mbuf *m)
#else
static inline st_status_t
StRtpDispatchPacket(const st_device_impl_t *dev, uint16_t dstPort, struct rte_mbuf *m)
#endif
{
	st_status_t status = ST_OK;
	st_session_impl_t *s;

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

	for (uint32_t i = 0; i < sn_count; i++)
	{
		s = sn[i];

		if (unlikely(!s))
			continue;

		if ((port == s->fl[0].dst.addr4.sin_port) || (port == s->fl[1].dst.addr4.sin_port))
		{
			if (likely(s->state == ST_SN_STATE_RUN))
			{
				status = s->RecvRtpPkt(s, m);
			}
			break;
		}
	}
	rte_pktmbuf_free(m);
	return status;
}

int
StGetRtpType(struct rte_mbuf *m)
{
	int def_ret = -1;

	struct st_rfc3550_audio_hdr *rtp
		= (struct st_rfc3550_audio_hdr *)(rte_pktmbuf_mtod(m, char *) + m->l2_len + m->l3_len
										  + m->l4_len);
	//	rte_hexdump(stdout, "check hdr",rtp, 16);

	if (rtp->version != RVRTP_VERSION_2)
		return def_ret;

	if (rtp->payloadType == RARTP_PAYLOAD_TYPE_PCM_AUDIO)
		return ST_ESSENCE_AUDIO;

	else if (rtp->payloadType == RVRTP_PAYLOAD_TYPE_RAW_VIDEO)
		return ST_ESSENCE_VIDEO;

	/* TODO
     *  Add anc later */

	return def_ret;
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
	//uint32_t threadId = (uint32_t)((uint64_t)args);
	const userargs_t *uargs = (userargs_t *)args;
	const uint32_t threadId = uargs->threadId;
	const uint16_t rxQ = uargs->queueP[0];
	const st_main_params_t *mp = &stMainParams;
	const st_device_impl_t *dev = &stRecvDevice;
	const uint16_t pPort = uargs->portP;
	const uint16_t rPort = uargs->portR;
	const struct timespec tim = { .tv_sec = 0, .tv_nsec = 1 };
	struct timespec tim2;

	if (threadId
		>= stDevParams->maxRcvThrds + stDevParams->maxAudioRcvThrds + stDevParams->maxAncRcvThrds)
		rte_exit(ST_GENERAL_ERR, "Receiver threadId is invalid\n");

	uint64_t rx_count = 0;
	struct rte_mbuf *rxVect[RX_BURTS_SIZE * 2];

	if (uargs->sn_type == ST_ESSENCE_VIDEO)
	{
		for (int i = mp->rcvThrds[threadId].thrdSnFirst; i < mp->rcvThrds[threadId].thrdSnLast;
			 sn_count++, i++)
		{
			sn[sn_count] = dev->snTable[i];
		}
		RTE_LOG(DEBUG, USER3, "RECEIVER ON %d LCORE THREAD %u RxQ:%d first Sn %d lastSn %d\n",
				rte_lcore_id(), threadId, rxQ, mp->rcvThrds[threadId].thrdSnFirst,
				mp->rcvThrds[threadId].thrdSnLast);
	}
	else if (uargs->sn_type == ST_ESSENCE_AUDIO)

	{
		int audioThId = threadId - mp->maxRcvThrds;
		for (int i = mp->audioRcvThrds[audioThId].thrdSnFirst;
			 sn_count < mp->audioRcvThrds[audioThId].thrdSnLast; sn_count++, i++)
		{
			sn[sn_count] = dev->sn30Table[i];
		}
		RTE_LOG(DEBUG, USER3, "RECEIVER ON %d LCORE THREAD %u RxQ:%d first Sn %d lastSn %d\n",
				rte_lcore_id(), threadId, rxQ, mp->audioRcvThrds[audioThId].thrdSnFirst,
				mp->audioRcvThrds[audioThId].thrdSnLast);
	}
	else if (uargs->sn_type == ST_ESSENCE_ANC)

	{
		int ancThId = threadId - mp->maxRcvThrds - mp->maxAudioRcvThrds;
		for (int i = mp->ancRcvThrds[ancThId].thrdSnFirst;
			 sn_count < mp->ancRcvThrds[ancThId].thrdSnLast; sn_count++, i++)
		{
			sn[sn_count] = dev->sn40Table[i];
		}
		RTE_LOG(DEBUG, USER3, "RECEIVER ON %d LCORE THREAD %u RxQ:%d first Sn %d lastSn %d\n",
				rte_lcore_id(), threadId, rxQ, mp->ancRcvThrds[ancThId].thrdSnFirst,
				mp->ancRcvThrds[ancThId].thrdSnLast);
	}

	for (int p = 0; p < mp->numPorts; ++p)
	{
		int vlanOffload = rte_eth_dev_get_vlan_offload(mp->rxPortId[p]);
		vlanOffload |= ETH_VLAN_STRIP_OFFLOAD;
		rte_eth_dev_set_vlan_offload(mp->rxPortId[p], vlanOffload);
	}

#if (RTE_VER_YEAR >= 21)
	uint8_t checkHwTstamp[MAX_RXTX_PORTS] = { 0 };
	struct rte_eth_dev_info dev_info = { 0 };
	if (0 == rte_eth_dev_info_get(mp->rxPortId[ST_PPORT], &dev_info))
	{
		if (strncmp("net_ice", dev_info.driver_name, 7) == 0)
		{
			for (int i = 0; i < mp->numPorts; i++)
				checkHwTstamp[i] = (hwts_dynfield_offset[mp->rxPortId[i]] != -1) ? 1 : 0;
		}
	}
#endif

	RTE_LOG(INFO, USER1, "Receiver ready - receiving packets STARTED\n");

#ifdef DEBUG
	int cnt;
#endif
	while (rte_atomic32_read(&isRxDevToDestroy) == 0)
	{
		int rv = 0;

#ifdef ST_RECV_TIME_PRINT
		uint64_t cycles0 = StPtpGetTime();
#endif

		rv += rte_eth_rx_burst(pPort, rxQ, &rxVect[rv], RX_BURTS_SIZE);
		rv += rte_eth_rx_burst(rPort, rxQ, &rxVect[rv], RX_BURTS_SIZE);

		if (unlikely(rv == 0))
		{
#ifdef DEBUG
			struct rte_eth_stats stats = { 0 };
			if ((cnt++ % 100000) == 0)
			{
				if (rte_eth_stats_get(dev->dev.port[0], &stats) == 0)
				{

					printf("%d:%d queue q_ipackets %lu q_errors %lu missed %lu\n", dev->dev.port[0],
						   rxQ, stats.q_ipackets[rxQ], stats.q_errors[rxQ], stats.imissed);
				}
				if (rte_eth_stats_get(dev->dev.port[1], &stats) == 0)
				{
					printf("%d:%d queue q_ipackets %lu q_errors %lu\n", dev->dev.port[1], rxQ,
						   stats.q_ipackets[rxQ], stats.q_errors[rxQ], stats.imissed);
				}
			}
#endif
			nanosleep(&tim, &tim2);
			continue;
		}

		uint64_t ptpTime = StPtpGetTime();

		for (int i = 0; (i < rv) && (rv < 2 * RX_BURTS_SIZE); i++)
		{
#ifndef ST_FLOW_CLASS_IN_HW
			if (unlikely((m->packet_type & RTE_PTYPE_L4_UDP) != RTE_PTYPE_L4_UDP))
			{
				rte_pktmbuf_free(rxVect[i]);
				continue;
			}
#endif
			rx_count++;

#if (RTE_VER_YEAR < 21)
			rxVect[i]->timestamp = ptpTime;
#else
			pktpriv_data_t *ptr = rte_mbuf_to_priv(rxVect[i]);
			if ((checkHwTstamp[rxVect[i]->port]) && (rxVect[i]->port < mp->numPorts))
			{
				RTE_LOG(DEBUG, USER1, "checkHwTstamp is enabled, sw %lx hw %lx\n", ptpTime,
						*(RTE_MBUF_DYNFIELD(rxVect[i],
											hwts_dynfield_offset[mp->rxPortId[rxVect[i]->port]],
											uint64_t *)));
				ptpTime = (rte_flow_dynf_metadata_avail()) ? *(RTE_MBUF_DYNFIELD(
							  rxVect[i], hwts_dynfield_offset[mp->rxPortId[rxVect[i]->port]],
							  uint64_t *))
														   : ptpTime;
			}
			if (ptr)
				ptr->timestamp = ptpTime;
#endif

#ifdef DEBUG
			type = StGetRtpType(rxVect[i]);
#endif

			const struct rte_ether_hdr *ethHdr
				= rte_pktmbuf_mtod(rxVect[i], struct rte_ether_hdr *);
#ifndef ST_FLOW_CLASS_IN_HW
			const struct rte_ipv4_hdr *ip
				= (struct rte_ipv4_hdr *)((char *)ethHdr + sizeof(struct rte_ether_hdr));
#endif
			const struct rte_udp_hdr *udp
				= (struct rte_udp_hdr *)((char *)ethHdr + sizeof(struct rte_ether_hdr)
										 + sizeof(struct rte_ipv4_hdr));

#ifndef ST_FLOW_CLASS_IN_HW
			st_flow_t flow;
			flow.dst.addr4.sin_port = udp->src_port;
			flow.src.addr4.sin_port = udp->dst_port;
			flow.src.addr4.sin_addr.s_addr = ip->src_addr;
			flow.dst.addr4.sin_addr.s_addr = ip->dst_addr;
			StRtpDispatchPacket(dev, &flow, rxVect[i]);
#else
			const uint16_t dstPort = udp->dst_port;
			StRtpDispatchPacket(dev, dstPort, rxVect[i]);
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
	RTE_LOG(INFO, USER1, "Receiver closed - received (%lu) packets STOPPED\n", rx_count);
	return 0;
}
