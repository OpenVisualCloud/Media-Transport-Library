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

#include "dpdk_common.h"
#include "rvrtp_main.h"

#include <unistd.h>

extern int hwts_dynfield_offset[RTE_MAX_ETHPORTS];

st_enqueue_stats_t enqStats[RTE_MAX_LCORE] __rte_cache_aligned;

//#define TX_RINGS_DEBUG
//#define ST_RING_TIME_PRINT
//#define TX_TIMER_DEBUG

#define ST_TPRS_SLOTS_ADVANCE 8

uint64_t adjustCount[6] = { 0, 0, 0, 0, 0, 0 };

/**
* Align each 8th packet to reduce burstiness
*/
static inline void
RvRtpAlignPacket(st_session_impl_t *s, struct rte_mbuf *m)
{
	// set mbuf timestmap to expected nanoseconds on a wire - 8 times Tprs time
	int tprsSlots = (s->vctx.line1Number + 1) * s->fmt.v.pktsInLine - ST_TPRS_SLOTS_ADVANCE;

	StMbufSetTimestamp(m, (U64)(s->vctx.epochs * s->fmt.v.frameTime) + s->sn.trOffset - s->nicTxTime
				   + ((int64_t)tprsSlots * s->sn.tprs));
	s->vctx.alignTmstamp = 0;
}

/**
* Get the ST2110-21 timestamp of 90k aligned to the epoch
*/
uint32_t
RvRtpGetFrameTmstamp(st_session_impl_t *s, uint32_t firstWaits, U64 *roundTime, struct rte_mbuf *m)
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
	int64_t epochs = (int64_t)(ntime / s->fmt.v.frameTime);

	int areSameEpochs = 0, isOneLate = 0;

	if (s->vctx.epochs == 0)
	{
		s->vctx.epochs = epochs;
	}
	else if (epochs - s->vctx.epochs > 1)
	{
		s->vctx.epochs = epochs;
		__sync_fetch_and_add(&adjustCount[0], 1);
	}
	else if (epochs - s->vctx.epochs == 0)
	{
		areSameEpochs++;
		__sync_fetch_and_add(&adjustCount[1], 1);
	}
	else if (epochs - s->vctx.epochs == 1)
	{
		isOneLate++;
		s->vctx.epochs++;
		__sync_fetch_and_add(&adjustCount[2], 1);
	}
	else
	{
		//? case
		s->vctx.epochs = epochs;
		__sync_fetch_and_add(&adjustCount[0], 1);
	}

	int64_t toEpoch;
	int64_t toElapse;
	U64 st21Tmstamp90k;
	U64 advance = s->nicTxTime + ST_TPRS_SLOTS_ADVANCE * s->sn.tprs;
	long double frmTime90k = 1.0L * s->fmt.v.clockRate * s->fmt.v.frmRateDen / s->fmt.v.frmRateMul;
	s->vctx.alignTmstamp = 0;
	ntime = StPtpGetTime();
	ntimeCpu = StGetTscTimeNano();
	U64 epochs_r = (U64)(ntime / s->fmt.v.frameTime);
	U64 remaind = ntime - (U64)(epochs_r * s->fmt.v.frameTime);

	if ((isOneLate || (!areSameEpochs)) && (remaind < s->sn.trOffset - advance))
	{
		if (remaind > s->sn.trOffset / 2)
		{
			toElapse = 0;
			__sync_fetch_and_add(&adjustCount[3], 1);
		}
		else
		{
			toElapse = s->sn.trOffset - advance - remaind;
			__sync_fetch_and_add(&adjustCount[4], 1);
		}

		// set 90k timestamp aligned to epoch
		st21Tmstamp90k = s->vctx.epochs * frmTime90k;
		StMbufSetTimestamp(m, (U64)(s->vctx.epochs * s->fmt.v.frameTime) + s->sn.trOffset - advance);
	}
	else
	{
		s->vctx.epochs++;
		toEpoch = (U64)(s->vctx.epochs * s->fmt.v.frameTime) - ntime;
		toElapse = toEpoch + s->sn.trOffset - advance;
		// set 90k timestamp aligned to epoch
		st21Tmstamp90k = s->vctx.epochs * frmTime90k;
		__sync_fetch_and_add(&adjustCount[5], 1);

		// set mbuf timestmap to expected nanoseconds on a wire - ST_TPRS_SLOTS_ADVANCE times Tprs time
		StMbufSetTimestamp(m, (U64)(s->vctx.epochs * s->fmt.v.frameTime) + s->sn.trOffset - advance);
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
			ntimeCpuLast = StGetTscTimeNano();
			elapsed = ntimeCpuLast - ntimeCpu;
			if (elapsed + MAX(req.tv_nsec, ST_CLOCK_PRECISION_TIME) > toElapse)
				break;
			repeats++;
		}
#ifdef TX_TIMER_DEBUG
		if (s->sn.timeslot == 0)
		{
			uint64_t tmstamp64_ = (U64)ntimeLast / s->tmstampTime;
			uint32_t tmstamp32_ = (uint32_t)tmstamp64_;
			uint32_t tmstamp32 = (uint32_t)st21Tmstamp90k;
			uint64_t mtmtstamp = StMbufGetTimestamp(m);

			RTE_LOG(INFO, USER2, "RvRtpGetFrameTmstamp: elapsed %llu diff %lld tmdelta %d\n",
					elapsed, (long long int)toElapse - elapsed, tmstamp32 - s->lastTmstamp);
			RTE_LOG(INFO, USER2, "RvRtpGetFrameTmstamp: waiting %llu tmstamp %u delta %d\n",
					toElapse, tmstamp32, (int32_t)(tmstamp32_ - tmstamp32));
			RTE_LOG(INFO, USER2, "RvRtpGetFrameTmstamp: mbuf->tmstamp %lu time %lu timeDiff %lu\n",
					mtmtstamp, (uint64_t)ntimeLast, mtmtstamp - ntimeLast);
		}
#endif
	}
	s->lastTmstamp = st21Tmstamp90k;

	return (uint32_t)st21Tmstamp90k;
}

/*****************************************************************************
 *
 * StRtpFillHdrs - Copy and fill all pkts headers
 *
 * DESCRIPTION
 * Constructs the pkt header from the template
 *
 * RETURNS: IP header location
 */
void *
StRtpFillHeader(st_session_impl_t *s, struct rte_ether_hdr *l2)
{
	struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)&l2[1];
	memcpy(l2, &s->hdrPrint[ST_PPORT], sizeof(s->hdrPrint[ST_PPORT]));
	return ip;
}

void
StRtpFillHeaderR(st_session_impl_t *s, uint8_t *l2R, uint8_t *l2)
{
	memcpy(l2R, l2, sizeof(s->hdrPrint[ST_RPORT]));
	// update eth ip, udp,set us RPORT
	struct rte_ether_hdr *dst_l2 = (struct rte_ether_hdr *)l2R;
	memcpy(&dst_l2->d_addr, s->fl[ST_RPORT].dstMac, ETH_ADDR_LEN);
	memcpy(&dst_l2->s_addr, s->fl[ST_RPORT].srcMac, ETH_ADDR_LEN);
	struct rte_ipv4_hdr *dip = (struct rte_ipv4_hdr *)(l2R + sizeof(struct rte_ether_hdr));
	dip->src_addr = s->fl[ST_RPORT].src.addr4.sin_addr.s_addr;
	dip->dst_addr = s->fl[ST_RPORT].dst.addr4.sin_addr.s_addr;
	struct rte_udp_hdr *dudp = (struct rte_udp_hdr *)(l2R + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_ether_hdr));
	dudp->src_port = s->fl[ST_RPORT].src.addr4.sin_port;
	dudp->dst_port = s->fl[ST_RPORT].dst.addr4.sin_port;
}

/* packet enqueue for redudant port */
int
LcoreMainPktRingEnqueue_withoutRedudant(void *args)
{
	unsigned coreId = rte_lcore_index(rte_lcore_id());
	uint32_t threadId = (uint32_t)((uint64_t)args);
	st_main_params_t *mp = &stMainParams;
	int count = 0;

	if (threadId >= mp->maxEnqThrds)
	{
		RTE_LOG(INFO, USER2, "PKT ENQUEUE RUNNING ON %d LCORE in threadId of %u\n", rte_lcore_id(),
				threadId);
		rte_exit(127, "Invalid threadId - exiting PKT ENQUEUE\n");
	}

	uint32_t ring = 0;
	st_device_impl_t *dev = &stSendDevice;
	st_session_impl_t *s;

	while (count < dev->dev.snCount)
	{
		//sleep(1);
		if (dev->snTable[count] != NULL)
		{
			count++;
		}
	}

	const uint32_t pktsCount = mp->enqThrds[threadId].pktsCount;
	struct rte_mbuf *pktVect[pktsCount];
	struct rte_mbuf *pktExt[pktsCount];

	char name[35] = "enq-thread-";
	snprintf(name + strlen(name), 35, "%3d", threadId);

	struct rte_mempool *pool = dev->mbufPool;
	if (!pool)
		rte_exit(127, "Packets mbufPool is invalid\n");

	uint32_t thrdSnFirst = mp->enqThrds[threadId].thrdSnFirst;
	uint32_t thrdSnLast = mp->enqThrds[threadId].thrdSnLast;
	RTE_LOG(INFO, USER2, "%s[%d], thread params: %u %u %u %u\n", __func__,
		threadId, thrdSnFirst, thrdSnLast, pktsCount, threadId);

#ifdef ST_RING_TIME_PRINT
	uint64_t ratioClk = rte_get_tsc_hz();
#endif

	// wait for scheduler threads to be ready
	RVRTP_SEMAPHORE_WAIT(mp->ringStart, mp->maxSchThrds * mp->numPorts);

	//DPDKMS-482 - additional workaround
	rte_delay_us_sleep(10 * 1000 * 1000);
	RTE_LOG(INFO, USER2, "%s[%d], sending packet STARTED on lcore %d\n", __func__, threadId, rte_lcore_id());

	while (rte_atomic32_read(&isTxDevToDestroy) == 0)
	{
#ifdef ST_RING_TIME_PRINT
		uint64_t cycles0 = rte_get_tsc_cycles();
#endif
		/* allocate pkts */
		if (rte_pktmbuf_alloc_bulk(pool, pktVect, pktsCount) < 0)
		{
			RTE_LOG(INFO, USER2, "Packets allocation problem after: %lu for %u\n",
					(uint64_t)enqStats[coreId].pktsBuild, pktsCount);
			enqStats[coreId].pktsPriAllocFail += 1;
			continue;
		}
		/* allocate mbufs for external buffer*/
		if (rte_pktmbuf_alloc_bulk(pool, pktExt, pktsCount) < 0)
		{
			rte_pktmbuf_free_bulk(&pktVect[0], pktsCount);
			RTE_LOG(INFO, USER2, "Packets Ext allocation problem after: %u for %u\n",
					(uint32_t)enqStats[coreId].pktsBuild, pktsCount);
			enqStats[coreId].pktsExtAllocFail += 1;
			continue;
		}

		U64 roundTime = 0;
		uint32_t firstSnInRound = 1;

		RVRTP_BARRIER_SYNC(mp->ringBarrier1, threadId, mp->maxEnqThrds);

		for (uint32_t i = thrdSnFirst; i < thrdSnLast; i++)
		{
			s = dev->snTable[i];
			if (unlikely(s == NULL))
			{
				for (uint32_t j = 0; j < ST_DEFAULT_PKTS_IN_LN; j++)
				{
					uint32_t ij = (i - thrdSnFirst) * ST_DEFAULT_PKTS_IN_LN + j;
					rte_pktmbuf_free(pktVect[ij]);
					rte_pktmbuf_free(pktExt[ij]);
					pktVect[ij] = NULL;
					pktExt[ij] = NULL;
				}
				enqStats[coreId].sessionLkpFail += 1;
				continue;
			}

			uint32_t ij = (i - thrdSnFirst) * ST_DEFAULT_PKTS_IN_LN;

			StMbufSetTimestamp(pktVect[ij], 0);

			do
			{
				if (unlikely(s->vctx.tmstamp == 0))
				{
					if (!stMainParams.userTmstamp)
						s->vctx.tmstamp
							= RvRtpGetFrameTmstamp(s, firstSnInRound, &roundTime, pktVect[ij]);
					else
						s->vctx.tmstamp = s->vctx.usertmstamp;
					firstSnInRound = 0;
				}
			} while ((RvRtpSessionCheckRunState(s) == 0) && (rte_atomic32_read(&isTxDevToDestroy) == 0));

			for (uint32_t j = 0; j < ST_DEFAULT_PKTS_IN_LN; j++)
			{
				uint32_t ij = (i - thrdSnFirst) * ST_DEFAULT_PKTS_IN_LN + j;

				if (unlikely(s->state != ST_SN_STATE_RUN))
				{
					rte_pktmbuf_free(pktVect[ij]);
					rte_pktmbuf_free(pktExt[ij]);
					pktVect[ij] = NULL;
					pktExt[ij] = NULL;
					enqStats[coreId].sessionStateFail += 1;
					continue;
				}
				struct rte_ether_hdr *l2 = rte_pktmbuf_mtod(pktVect[ij], struct rte_ether_hdr *);
				struct rte_ipv4_hdr *ip = StRtpFillHeader(s, l2);

				if ((s->vctx.alignTmstamp) && (!stMainParams.userTmstamp))
					RvRtpAlignPacket(s, pktVect[ij]);

				// assemble the RTP packet accordingly to the format
				s->UpdateRtpPkt(s, ip, pktExt[ij]);

				pktVect[ij]->data_len = s->fmt.v.pktSize - pktExt[ij]->data_len;

				if (unlikely(pktExt[ij]->data_len == 0))
				{
					enqStats[coreId].pktsChainPriFail += 1;
					rte_pktmbuf_free(pktExt[ij]);
				}
				rte_pktmbuf_chain(pktVect[ij], pktExt[ij]);

				pktVect[ij]->pkt_len = s->fmt.v.pktSize;
				pktVect[ij]->l2_len = 14;
				pktVect[ij]->l3_len = 20;
				pktVect[ij]->ol_flags = PKT_TX_IPV4;
				pktVect[ij]->ol_flags |= PKT_TX_IP_CKSUM;

				enqStats[coreId].pktsBuild += 1;
			}
		}
#ifdef ST_RING_TIME_PRINT
		uint64_t cycles1 = rte_get_tsc_cycles();
		printf("Ring pkt assembly time elapsed %llu ratio %llu pktTime %llu\n",
			   (U64)(cycles1 - cycles0), (U64)ratioClk, (U64)(cycles1 - cycles0) / pktsCount);
#endif
		for (uint32_t j = 0; j < ST_DEFAULT_PKTS_IN_LN; j += 4)
		{
			for (uint32_t i = thrdSnFirst; i < thrdSnLast; i++)
			{
				if (unlikely(!dev->snTable[i]))
				{
					enqStats[coreId].sessionLkpFail += 1;
					continue;
				}

				uint32_t ij = (i - thrdSnFirst) * ST_DEFAULT_PKTS_IN_LN + j;

				if (!pktVect[ij])
				{
					enqStats[coreId].pktsQueuePriFail += 1;
					continue;
				}

				ring = dev->snTable[i]->sn.timeslot;

				while (rte_ring_sp_enqueue_bulk(dev->txRing[ST_PPORT][ring], (void **)&pktVect[ij],
												4, NULL)
					   == 0)
				{
					RVRTP_SEMAPHORE_GIVE(mp->schedStart, 1);
#ifdef _TX_RINGS_DEBUG_
					if ((enqStats[coreId].pktsQueued % 1000) == 0)
						RTE_LOG(INFO, USER2, "Packets enqueue ring %u after: %llu times %u\n", ring,
								(U64)enqStats[coreId].pktsQueued, noFails);
#endif
					__sync_synchronize();
					if (rte_atomic32_read(&isTxDevToDestroy) != 0)
						break;
				}

				enqStats[coreId].pktsQueued += 1;
			}
		}
		RVRTP_BARRIER_SYNC(mp->ringBarrier2, threadId, mp->maxEnqThrds);

		RVRTP_SEMAPHORE_GIVE(mp->schedStart, 1);

#ifdef ST_RING_TIME_PRINT
		uint64_t cycles2 = rte_get_tsc_cycles();
		printf("Ring finish time elapsed %llu ratio %llu pktTime %llu\n", (U64)(cycles2 - cycles1),
			   (U64)ratioClk, (U64)(cycles2 - cycles1) / pktsCount);
#endif
	}
	RTE_LOG(INFO, USER2, "Transmitter closed - sending packet STOPPED\n");
	return 0;
}

/* Packet creator thread runned on master lcore*/
int
LcoreMainPktRingEnqueue_withRedudant(void *args)
{
	unsigned coreId = rte_lcore_index(rte_lcore_id());
	uint32_t threadId = (uint32_t)((uint64_t)args);
	st_main_params_t *mp = &stMainParams;
	int count = 0;

	if (threadId >= mp->maxEnqThrds)
	{
		RTE_LOG(INFO, USER2, "PKT ENQUEUE RUNNING ON %d LCORE in threadId of %u\n", rte_lcore_id(),
				threadId);
		rte_exit(127, "Invalid threadId - exiting PKT ENQUEUE\n");
	}

	uint32_t ring = 0;
	st_device_impl_t *dev = &stSendDevice;
	st_session_impl_t *s;

	while (count < dev->dev.snCount)
	{
		//sleep(1);
		if (dev->snTable[count] != NULL)
		{
			count++;
		}
	}

	const uint32_t pktsCount = mp->enqThrds[threadId].pktsCount;
	struct rte_mbuf *pktVect[pktsCount];
	struct rte_mbuf *pktVectR[pktsCount];
	struct rte_mbuf *pktExt[pktsCount];

	struct rte_mempool *pool = dev->mbufPool;
	if (!pool)
		rte_exit(127, "Packets mbufPool is invalid\n");

	uint32_t thrdSnFirst = mp->enqThrds[threadId].thrdSnFirst;
	uint32_t thrdSnLast = mp->enqThrds[threadId].thrdSnLast;
	bool redRing = (mp->numPorts > 1) ? 1 : 0;
	RTE_LOG(INFO, USER2, "%s[%d], thread params: %u %u %u %u\n", __func__,
		threadId, thrdSnFirst, thrdSnLast, pktsCount, threadId);

#ifdef ST_RING_TIME_PRINT
	uint64_t ratioClk = rte_get_tsc_hz();
#endif

	// wait for scheduler threads to be ready
	RVRTP_SEMAPHORE_WAIT(mp->ringStart, mp->maxSchThrds * mp->numPorts);

	//DPDKMS-482 - additional workaround
	rte_delay_us_sleep(10 * 1000 * 1000);
	RTE_LOG(INFO, USER2, "%s[%d], sending packet STARTED on lcore %d\n", __func__, threadId, rte_lcore_id());

	while (rte_atomic32_read(&isTxDevToDestroy) == 0)
	{
#ifdef ST_RING_TIME_PRINT
		uint64_t cycles0 = rte_get_tsc_cycles();
#endif
		/* allocate pkts */
		if (rte_pktmbuf_alloc_bulk(pool, pktVect, pktsCount) < 0)
		{
			enqStats[coreId].pktsPriAllocFail += 1;
			RTE_LOG(INFO, USER2, "Packets allocation problem after: %u for %u\n",
					(uint32_t)enqStats[coreId].pktsPriAllocFail, pktsCount);
			continue;
		}
		/* allocate mbufs for external buffer*/
		if (rte_pktmbuf_alloc_bulk(pool, pktExt, pktsCount) < 0)
		{
			enqStats[coreId].pktsExtAllocFail += 1;
			rte_pktmbuf_free_bulk(&pktVect[0], pktsCount);
			RTE_LOG(INFO, USER2, "Packets Ext allocation problem after: %lu for %u\n",
					(uint64_t)enqStats[coreId].pktsBuild, pktsCount);
			continue;
		}
		if (redRing && rte_pktmbuf_alloc_bulk(pool, pktVectR, pktsCount) < 0)
		{
			enqStats[coreId].pktsRedAllocFail += 1;
			rte_pktmbuf_free_bulk(&pktVect[0], pktsCount);
			rte_pktmbuf_free_bulk(&pktExt[0], pktsCount);
			RTE_LOG(INFO, USER2, "Packets allocation problem after: %lu for %u\n",
					(uint64_t)enqStats[coreId].pktsBuild, pktsCount);
			continue;
		}
		U64 roundTime = 0;
		uint32_t firstSnInRound = 1;

		RVRTP_BARRIER_SYNC(mp->ringBarrier1, threadId, mp->maxEnqThrds);

		for (uint32_t i = thrdSnFirst; i < thrdSnLast; i++)
		{
			bool sendR = 0;
			bool sendP = 0;

			s = dev->snTable[i];

			if (unlikely(s == NULL))
			{
				for (uint32_t j = 0; j < ST_DEFAULT_PKTS_IN_LN; j++)
				{
					uint32_t ij = (i - thrdSnFirst) * ST_DEFAULT_PKTS_IN_LN + j;
					rte_pktmbuf_free(pktVect[ij]);
					rte_pktmbuf_free(pktExt[ij]);
					if (redRing)
					{
						rte_pktmbuf_free(pktVectR[ij]);
						pktVectR[ij] = NULL;
					}
					pktVect[ij] = NULL;
					pktExt[ij] = NULL;
				}
				enqStats[coreId].sessionLkpFail += 1;
				continue;
			}

			sendR = (redRing && (s->sn.caps & ST_SN_DUAL_PATH) && mp->rTx == 1) ? 1 : 0;
			sendP = ((s->sn.caps & ST_SN_DUAL_PATH) && mp->pTx == 1) ? 1 : 0;
			uint32_t ij = (i - thrdSnFirst) * ST_DEFAULT_PKTS_IN_LN;

			StMbufSetTimestamp(pktVect[ij], 0);

			do
			{
				if (unlikely(s->vctx.tmstamp == 0))
				{
					if (!stMainParams.userTmstamp)
						s->vctx.tmstamp
							= RvRtpGetFrameTmstamp(s, firstSnInRound, &roundTime, pktVect[ij]);
					else
						s->vctx.tmstamp = s->vctx.usertmstamp;
					firstSnInRound = 0;
				}
			} while ((RvRtpSessionCheckRunState(s) == 0) && (rte_atomic32_read(&isTxDevToDestroy) == 0));


			for (uint32_t j = 0; j < ST_DEFAULT_PKTS_IN_LN; j++)
			{
				uint32_t ij = (i - thrdSnFirst) * ST_DEFAULT_PKTS_IN_LN + j;

				if (unlikely(s->state != ST_SN_STATE_RUN))
				{
					rte_pktmbuf_free(pktVect[ij]);
					rte_pktmbuf_free(pktExt[ij]);
					if (redRing)
					{
						rte_pktmbuf_free(pktVectR[ij]);
						pktVectR[ij] = NULL;
					}
					pktVect[ij] = NULL;
					pktExt[ij] = NULL;
					enqStats[coreId].sessionStateFail += 1;
					continue;
				}
				struct rte_ether_hdr *l2 = rte_pktmbuf_mtod(pktVect[ij], struct rte_ether_hdr *);
				struct rte_ipv4_hdr *ip = StRtpFillHeader(s, l2);

				if ((s->vctx.alignTmstamp) && (!stMainParams.userTmstamp))
					RvRtpAlignPacket(s, pktVect[ij]);

				// assemble the RTP packet accordingly to the format
				s->UpdateRtpPkt(s, ip, pktExt[ij]);

				pktVect[ij]->data_len = s->fmt.v.pktSize - pktExt[ij]->data_len;

				if (pktExt[ij]->data_len == 0)
				{
					rte_pktmbuf_free(pktExt[ij]);
					enqStats[coreId].pktsChainPriFail += 1;
				}
				rte_pktmbuf_chain(pktVect[ij], pktExt[ij]);

				pktVect[ij]->pkt_len = s->fmt.v.pktSize;
				pktVect[ij]->l2_len = 14;
				pktVect[ij]->l3_len = 20;
				pktVect[ij]->ol_flags = PKT_TX_IPV4;
				pktVect[ij]->ol_flags |= PKT_TX_IP_CKSUM;

				if (sendR)
				{
					pktVectR[ij]->data_len = pktVect[ij]->data_len;
					pktVectR[ij]->pkt_len = pktVect[ij]->pkt_len;
					pktVectR[ij]->l2_len = pktVect[ij]->l2_len;
					pktVectR[ij]->l3_len = pktVect[ij]->l3_len;
					pktVectR[ij]->ol_flags = pktVect[ij]->ol_flags;
					pktVectR[ij]->nb_segs = 2;

					pktVectR[ij]->next = pktExt[ij];
					rte_mbuf_refcnt_set(pktExt[ij], 2);

					uint8_t *l2R = rte_pktmbuf_mtod(pktVectR[ij], uint8_t *);
					StRtpFillHeaderR(s, l2R, (uint8_t *)l2);

					/* ToDo: should not we chain the Redudant packets? */
				}
				else if (redRing)
				{
					rte_pktmbuf_free(pktVectR[ij]);
					pktVectR[ij] = NULL;
				}
				if (!sendP)
				{
					rte_pktmbuf_free(pktVect[ij]);
					pktVect[ij] = NULL;
				}
				enqStats[coreId].pktsBuild += 1;
			}
		}
#ifdef ST_RING_TIME_PRINT
		uint64_t cycles1 = rte_get_tsc_cycles();
		printf("Ring pkt assembly time elapsed %llu ratio %llu pktTime %llu\n",
			   (U64)(cycles1 - cycles0), (U64)ratioClk, (U64)(cycles1 - cycles0) / pktsCount);
#endif
		for (uint32_t j = 0; j < ST_DEFAULT_PKTS_IN_LN; j += 4)
		{
			for (uint32_t i = thrdSnFirst; i < thrdSnLast; i++)
			{
				if (unlikely(!dev->snTable[i]))
				{
					enqStats[coreId].sessionLkpFail += 1;
					continue;
				}

				uint32_t ij = (i - thrdSnFirst) * ST_DEFAULT_PKTS_IN_LN + j;

				ring = dev->snTable[i]->sn.timeslot;

				while (pktVect[ij] && rte_ring_sp_enqueue_bulk(dev->txRing[ST_PPORT][ring], (void **)&pktVect[ij],
												4, NULL)
					   == 0)
				{
					RVRTP_SEMAPHORE_GIVE(mp->schedStart, 1);
#ifdef _TX_RINGS_DEBUG_
					if ((enqStats[coreId].pktsQueued % 1000) == 0)
						RTE_LOG(INFO, USER2, "Packets enqueue ring %u after: %llu times %u\n", ring,
								(U64)enqStats[coreId].pktsQueued, noFails);
#endif
					__sync_synchronize();
					if (rte_atomic32_read(&isTxDevToDestroy) != 0)
						break;
				}

				while (redRing && pktVectR[ij]
					   && (rte_ring_sp_enqueue_bulk(dev->txRing[ST_RPORT][ring],
													(void **)&pktVectR[ij], 4, NULL)
						   == 0))
				{
					RVRTP_SEMAPHORE_GIVE(mp->schedStart, 1);
#ifdef _TX_RINGS_DEBUG_
					if ((enqStats[coreId].pktsQueued % 1000) == 0)
						RTE_LOG(INFO, USER2, "Packets enqueue ring %u after: %llu times %u\n", ring,
								(U64)enqStats[coreId].pktsQueued, noFails);
#endif
					__sync_synchronize();
					if (rte_atomic32_read(&isTxDevToDestroy) != 0)
						break;
				}
				enqStats[coreId].pktsQueued += 1;
			}
		}
		RVRTP_BARRIER_SYNC(mp->ringBarrier2, threadId, mp->maxEnqThrds);

		RVRTP_SEMAPHORE_GIVE(mp->schedStart, 1);

#ifdef ST_RING_TIME_PRINT
		uint64_t cycles2 = rte_get_tsc_cycles();
		printf("Ring finish time elapsed %llu ratio %llu pktTime %llu\n", (U64)(cycles2 - cycles1),
			   (U64)ratioClk, (U64)(cycles2 - cycles1) / pktsCount);
#endif
	}
	RTE_LOG(INFO, USER2, "Transmitter closed - sending packet STOPPED\n");
	return 0;
}

static int TimePacingInit(st_session_impl_t *s, uint32_t idx)
{
	rvrtp_pacing_t *pacing = &s->pacing;
	st21_format_t *vfmt = &s->fmt.v;
	long double frameTime = vfmt->frameTime;

	pacing->trs = frameTime / (vfmt->pktsInLine * vfmt->totalLines);
	pacing->trOffset = frameTime * vfmt->trOffsetLines / vfmt->totalLines;
	pacing->vrx = ST_VRX_FULL_NARROW;
	pacing->trafficTime = 0;

	pacing->curEpochs = StPtpGetTime() / frameTime;
	pacing->timeCursor = StGetTscTimeNano();
	pacing->idx = idx;
	pacing->epochMismatch = 0;

	pacing->pktIdx = 0;
	if (StIsNicRlPacing())
	{
#define PACING_RL_TROFFSET_COMP (4) /* window time for sch troffset sync */
		pacing->warmPktsForRL = 16 + PACING_RL_TROFFSET_COMP; /* For vero CINST */
		pacing->vrx += 4 + PACING_RL_TROFFSET_COMP; /* time for warm pkts */
		pacing->padIntervalForRL = StGetRlPadsInterval(); /* For vero VRX compensate */
		RTE_LOG(DEBUG, USER2, "%s[%02d], padIntervalForRL %f\n", __func__, idx, pacing->padIntervalForRL);
	}

	RTE_LOG(DEBUG, USER2, "%s[%02d], trs %f trOffset %f\n", __func__, idx, pacing->trs, pacing->trOffset);
	return 0;
}

static int TimePacingSyncTrOffset(st_session_impl_t *s, bool sync)
{
	rvrtp_pacing_t *pacing = &s->pacing;
	st21_format_t *vfmt = &s->fmt.v;
	long double frameTime = vfmt->frameTime;
	uint64_t ptp_time = StPtpGetTime();
	uint64_t epochs = ptp_time / frameTime;
	long double toEpochTroffset;

	if (epochs == pacing->curEpochs)
	{ /* likely most previous frame can enqueue within previous timing */
		epochs++;
	}

	toEpochTroffset = (epochs * frameTime) + pacing->trOffset - ptp_time;
	if (toEpochTroffset < 0)
	{/* current time run out of trOffset already, sync to next epochs */
		pacing->epochMismatch++;
		epochs++;
		toEpochTroffset = (epochs * frameTime) + pacing->trOffset - ptp_time;
	}

	if (toEpochTroffset < 0)
	{ /* should never happen */
		toEpochTroffset = 0;
		RTE_LOG(DEBUG, USER2, "%s(%02d), error toEpochTroffset %Lf, ptp_time %ld pre epochs %ld\n",
			__func__, pacing->idx,
			toEpochTroffset, ptp_time, pacing->curEpochs);
	}

	pacing->curEpochs = epochs;
	long double frmTime90k = 1.0L * vfmt->clockRate * vfmt->frmRateDen / vfmt->frmRateMul;
	uint64_t tmstamp64 = epochs * frmTime90k;
	uint32_t tmstamp32 = tmstamp64;
	s->vctx.tmstamp = tmstamp32;

	/* timeCursor foward to epoch trOffset */
	double epochTime = StGetTscTimeNano() + toEpochTroffset;
	/* timeCursor to vrx */
	epochTime -= pacing->vrx * pacing->trs;
	/* timeCursor to traffic */
	epochTime -= pacing->trafficTime;

	if (epochTime < pacing->timeCursor) /* happened only if ptp sync the first time */
	{
		RTE_LOG(DEBUG, USER2, "%s(%02d), error epochTime %f %f\n", __func__,
			pacing->idx, epochTime, pacing->timeCursor);
	}
	pacing->timeCursor = epochTime;

	if (sync)
	{
		StTscTimeNanoSleepTo(epochTime);
	}

	pacing->pktIdx = 0;

	return 0;
}

static int
LcoreMainPktRingEnqueueTscPacing(void *args)
{
	uint32_t threadId = (uint32_t)((uint64_t)args);
	st_main_params_t *mp = &stMainParams;
	st_device_impl_t *dev = &stSendDevice;
	st_session_impl_t *s;
	const uint32_t pktsCount = mp->enqThrds[threadId].pktsCount;
	struct rte_mbuf *pktVect[pktsCount];
	struct rte_mbuf *pktVectR[pktsCount];
	struct rte_mbuf *pktExt[pktsCount];
	struct rte_mempool *pool = dev->mbufPool;
	if (!pool)
		rte_exit(127, "Packets mbufPool is invalid\n");

	uint32_t thrdSnFirst = mp->enqThrds[threadId].thrdSnFirst;
	uint32_t thrdSnLast = mp->enqThrds[threadId].thrdSnLast;
	bool redRing = (mp->numPorts > 1) ? 1 : 0;
	RTE_LOG(INFO, USER2, "%s[%d], thread params: %u %u %u %u, redRing %s\n", __func__,
		threadId, thrdSnFirst, thrdSnLast, pktsCount, threadId, redRing ? "yes" : "no");

	// wait for scheduler threads to be ready
	RVRTP_SEMAPHORE_WAIT(mp->ringStart, mp->maxSchThrds * mp->numPorts);

	//DPDKMS-482 - additional workaround
	rte_delay_us_sleep(5 * 1000 * 1000);
	RTE_LOG(INFO, USER2, "%s[%d], sending packet STARTED on lcore %d\n", __func__, threadId, rte_lcore_id());

	for (uint32_t i = thrdSnFirst; i < thrdSnLast; i++)
	{
		TimePacingInit(dev->snTable[i], i);
	}

	RVRTP_SEMAPHORE_GIVE(mp->schedStart, 1);

	while (rte_atomic32_read(&isTxDevToDestroy) == 0)
	{
		/* allocate pkts */
		if (rte_pktmbuf_alloc_bulk(pool, pktVect, pktsCount) < 0)
		{
			RTE_LOG(ERR, USER2, "%s, pktVect alloc fail\n", __func__);
			continue;
		}
		/* allocate mbufs for external buffer*/
		if (rte_pktmbuf_alloc_bulk(pool, pktExt, pktsCount) < 0)
		{
			rte_pktmbuf_free_bulk(&pktVect[0], pktsCount);
			RTE_LOG(ERR, USER2, "%s, pktExt alloc fail\n", __func__);
			continue;
		}
		if (redRing && rte_pktmbuf_alloc_bulk(pool, pktVectR, pktsCount) < 0)
		{
			rte_pktmbuf_free_bulk(&pktVect[0], pktsCount);
			rte_pktmbuf_free_bulk(&pktExt[0], pktsCount);
			RTE_LOG(ERR, USER2, "%s, pktVectR alloc fail\n", __func__);
			continue;
		}

		/* loop each line for all sn */
		for (uint32_t i = thrdSnFirst; i < thrdSnLast; i++)
		{
			bool sendR = false, sendP = true;

			s = dev->snTable[i];

			if (unlikely(s == NULL))
			{
				for (uint32_t j = 0; j < ST_DEFAULT_PKTS_IN_LN; j++)
				{
					uint32_t ij = (i - thrdSnFirst) * ST_DEFAULT_PKTS_IN_LN + j;
					rte_pktmbuf_free(pktVect[ij]);
					rte_pktmbuf_free(pktExt[ij]);
					if (redRing)
					{
						rte_pktmbuf_free(pktVectR[ij]);
						pktVectR[ij] = NULL;
					}
					pktVect[ij] = NULL;
					pktExt[ij] = NULL;
				}
				continue;
			}

			// TBD - how is TimePacingSyncTrOffset affecting future timestamp
			do
			{
				if (unlikely(s->vctx.tmstamp == 0))
				{
					TimePacingSyncTrOffset(s, false);
				}
			} while ((RvRtpSessionCheckRunState(s) == 0) && (rte_atomic32_read(&isTxDevToDestroy) == 0));

			if (redRing)
			{
				sendR = ((s->sn.caps & ST_SN_DUAL_PATH) && mp->rTx == 1) ? true : false;
				sendP = ((s->sn.caps & ST_SN_DUAL_PATH) && mp->pTx == 1) ? true : false;
			}

			for (uint32_t j = 0; j < ST_DEFAULT_PKTS_IN_LN; j++)
			{
				uint32_t ij = (i - thrdSnFirst) * ST_DEFAULT_PKTS_IN_LN + j;

				if (unlikely(s->state != ST_SN_STATE_RUN))
				{
					rte_pktmbuf_free(pktVect[ij]);
					rte_pktmbuf_free(pktExt[ij]);
					if (redRing)
					{
						rte_pktmbuf_free(pktVectR[ij]);
						pktVectR[ij] = NULL;
					}
					pktVect[ij] = NULL;
					pktExt[ij] = NULL;
					continue;
				}

				struct rte_ether_hdr *l2 = rte_pktmbuf_mtod(pktVect[ij], struct rte_ether_hdr *);
				struct rte_ipv4_hdr *ip = StRtpFillHeader(s, l2);

				// assemble the RTP packet accordingly to the format
				s->UpdateRtpPkt(s, ip, pktExt[ij]);

				pktVect[ij]->data_len = s->fmt.v.pktSize - pktExt[ij]->data_len;

				if (unlikely(pktExt[ij]->data_len == 0))
				{
					rte_pktmbuf_free(pktExt[ij]);
				}
				rte_pktmbuf_chain(pktVect[ij], pktExt[ij]);

				pktVect[ij]->pkt_len = s->fmt.v.pktSize;
				pktVect[ij]->l2_len = 14;
				pktVect[ij]->l3_len = 20;
				pktVect[ij]->ol_flags = PKT_TX_IPV4;
				pktVect[ij]->ol_flags |= PKT_TX_IP_CKSUM;

				if (redRing)
				{
					if (sendR)
					{
						pktVectR[ij]->data_len = pktVect[ij]->data_len;
						pktVectR[ij]->pkt_len = pktVect[ij]->pkt_len;
						pktVectR[ij]->l2_len = pktVect[ij]->l2_len;
						pktVectR[ij]->l3_len = pktVect[ij]->l3_len;
						pktVectR[ij]->ol_flags = pktVect[ij]->ol_flags;
						pktVectR[ij]->nb_segs = 2;

						pktVectR[ij]->next = pktExt[ij];
						rte_mbuf_refcnt_set(pktExt[ij], 2);

						uint8_t *l2R = rte_pktmbuf_mtod(pktVectR[ij], uint8_t *);
						StRtpFillHeaderR(s, l2R, (uint8_t *)l2);
					}
					else
					{
						rte_pktmbuf_free(pktVectR[ij]);
						pktVectR[ij] = NULL;
					}
				}
				if (!sendP)
				{
					rte_pktmbuf_free(pktVect[ij]);
					pktVect[ij] = NULL;
				}

			}
		}

		/* enqueue */
		for (uint32_t i = thrdSnFirst; i < thrdSnLast; i++)
		{
			if (unlikely(!dev->snTable[i]))
			{
				continue;
			}
			uint32_t ij = (i - thrdSnFirst) * ST_DEFAULT_PKTS_IN_LN;
			if (unlikely(!pktVect[ij]) && unlikely(!pktVectR[ij]))
			{
				continue;
			}

			s = dev->snTable[i];
			rvrtp_pacing_t *pacing = &s->pacing;
			uint32_t ring = s->sn.timeslot;

			for (int k = 0; k < ST_DEFAULT_PKTS_IN_LN && (pktVect[ij] || pktVectR[ij]); k++) {
				if (pktVect[ij+k])
				{
					StMbufSetTimestamp(pktVect[ij + k], (uint64_t)pacing->timeCursor);
					StMbufSetIdx(pktVect[ij + k], pacing->pktIdx);
				}
				if (redRing && pktVectR[ij+k])
				{
					StMbufSetTimestamp(pktVectR[ij + k], (uint64_t)pacing->timeCursor);
					StMbufSetIdx(pktVectR[ij + k], pacing->pktIdx);
				}
				pacing->timeCursor += pacing->trs; /* pkt foward */
				pacing->pktIdx += 1;
			}

			while (pktVect[ij] && 0 == rte_ring_sp_enqueue_bulk(dev->txRing[ST_PPORT][ring], (void **)&pktVect[ij],
						ST_DEFAULT_PKTS_IN_LN, NULL))
			{
				if (rte_atomic32_read(&isTxDevToDestroy))
					break;
			}

			if (redRing && pktVectR[ij])
			{ /* Sending redundant */
				while (0 == rte_ring_sp_enqueue_bulk(dev->txRing[ST_RPORT][ring], (void **)&pktVectR[ij],
							ST_DEFAULT_PKTS_IN_LN, NULL))
				{
					if (rte_atomic32_read(&isTxDevToDestroy))
						break;
				}
			}
		}
	}

	RTE_LOG(INFO, USER2, "%s[%d], sending packet STOPPED\n", __func__, threadId);
	return 0;
}

/* Packet creator thread runned on master lcore*/
int
LcoreMainPktRingEnqueue(void *args)
{
	st_main_params_t *mp = &stMainParams;
	uint32_t threadId = (uint32_t)((uint64_t)args);
	bool redRing = (mp->numPorts > 1) ? 1 : 0;

	if (threadId >= mp->maxEnqThrds)
	{
		RTE_LOG(INFO, USER2, "PKT ENQUEUE RUNNING ON %d LCORE in threadId of %u\n", rte_lcore_id(),
				threadId);
		rte_exit(127, "Invalid threadId - exiting PKT ENQUEUE\n");
	}

#ifdef TX_RINGS_DEBUG
	RTE_LOG(INFO, USER1, "PKT ENQUEUE RUNNING ON LCORE %d SOCKET %d THREAD %d\n", rte_lcore_id(),
			rte_lcore_to_socket_id(rte_lcore_id()), threadId);
#endif

	if (StIsTscPacing())
	{
		LcoreMainPktRingEnqueueTscPacing(args);
	}
	else if (StIsNicRlPacing())
	{ /* use tsc pacing also, we may enhance rl scheduler with tsc later */
		LcoreMainPktRingEnqueueTscPacing(args);
	}
	else if (redRing)
	{
		LcoreMainPktRingEnqueue_withRedudant(args);
	}
	else
	{
		LcoreMainPktRingEnqueue_withoutRedudant(args);
	}

	RTE_LOG(INFO, USER2, "Transmitter closed - sending packet STOPPED\n");
	return 0;
}
