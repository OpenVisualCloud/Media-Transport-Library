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

//#define TX_RINGS_DEBUG 1
//#define ST_RING_TIME_PRINT
//#define TX_TIMER_DEBUG 1
//#define _TX_RINGS_DEBUG_ 1

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

#if (RTE_VER_YEAR < 21)
	m->timestamp = (U64)(s->vctx.epochs * s->fmt.v.frameTime) + s->sn.trOffset - s->nicTxTime
				   + ((int64_t)tprsSlots * s->sn.tprs);
#else
	/* No access to portid, hence we have rely on pktpriv_data */
	pktpriv_data_t *ptr = rte_mbuf_to_priv(m);
	ptr->timestamp = (U64)(s->vctx.epochs * s->fmt.v.frameTime) + s->sn.trOffset - s->nicTxTime
					 + ((int64_t)tprsSlots * s->sn.tprs);
#endif
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
	U64 epochs = (U64)(ntime / s->fmt.v.frameTime);

	int areSameEpochs = 0, isOneLate = 0;

	if (s->vctx.epochs == 0)
	{
		s->vctx.epochs = epochs;
	}
	else if ((int64_t)epochs - s->vctx.epochs > 1)
	{
		s->vctx.epochs = epochs;
		__sync_fetch_and_add(&adjustCount[0], 1);
	}
	else if ((int64_t)epochs - s->vctx.epochs == 0)
	{
		areSameEpochs++;
		__sync_fetch_and_add(&adjustCount[1], 1);
	}
	else if ((int64_t)epochs - s->vctx.epochs == 1)
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

	U64 toEpoch;
	int64_t toElapse;
	U64 st21Tmstamp90k;
	U64 advance = s->nicTxTime + ST_TPRS_SLOTS_ADVANCE * s->sn.tprs;
	long double frmTime90k = 1.0L * s->fmt.v.clockRate * s->fmt.v.frmRateDen / s->fmt.v.frmRateMul;
	s->vctx.alignTmstamp = 0;
	ntime = StPtpGetTime();
	ntimeCpu = StGetCpuTimeNano();
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
#if (RTE_VER_YEAR < 21)
		m->timestamp = (U64)(s->vctx.epochs * s->fmt.v.frameTime) + s->sn.trOffset - advance;
#else
		/* No access to portid, hence we have rely on pktpriv_data */
		pktpriv_data_t *ptr = rte_mbuf_to_priv(m);
		ptr->timestamp = (U64)(s->vctx.epochs * s->fmt.v.frameTime) + s->sn.trOffset - advance;
#endif
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
#if (RTE_VER_YEAR < 21)
		m->timestamp = (U64)(s->vctx.epochs * s->fmt.v.frameTime) + s->sn.trOffset - advance;
#else
		/* We do not have access to portid, hence we have rely on pktpriv_data */
		pktpriv_data_t *ptr = rte_mbuf_to_priv(m);
		ptr->timestamp = (U64)(s->vctx.epochs * s->fmt.v.frameTime) + s->sn.trOffset - advance;
#endif
	}

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
#ifdef TX_TIMER_DEBUG
		if (s->sn.timeslot == 0)
		{
			uint64_t tmstamp64_ = (U64)ntimeLast / s->tmstampTime;
			uint32_t tmstamp32_ = (uint32_t)tmstamp64_;
			uint32_t tmstamp32 = (uint32_t)st21Tmstamp90k;
#if (RTE_VER_YEAR < 21)

			uint64_t mtmtstamp = m->timestamp;
#else
			/* No access to portid, hence we have rely on pktpriv_data */
			pktpriv_data_t *ptr = rte_mbuf_to_priv(m);
			uint64_t mtmtstamp = ptr->timestamp;
#endif

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
	uint32_t eth_ip_size = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr);
	uint8_t *udp = l2 + eth_ip_size;
	uint8_t *udpR = l2R + eth_ip_size;

	memcpy(l2R, &s->hdrPrint[ST_RPORT], eth_ip_size);
	memcpy(udpR, udp, sizeof(s->hdrPrint[ST_RPORT]) - eth_ip_size);
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

#ifdef TX_RINGS_DEBUG
	RTE_LOG(INFO, USER1, "PKT ENQUEUE RUNNING ON LCORE %d SOCKET %d THREAD %d\n", rte_lcore_id(),
			rte_lcore_to_socket_id(rte_lcore_id()), threadId);
#endif

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

#ifdef TX_RINGS_DEBUG
	RTE_LOG(INFO, USER2, "Thread params: %u %u %u %u\n", thrdSnFirst, thrdSnLast, pktsCount,
			threadId);
#endif
#ifdef ST_RING_TIME_PRINT
	uint64_t ratioClk = rte_get_tsc_hz();
#endif

	// wait for scheduler threads to be ready
	RVRTP_SEMAPHORE_WAIT(mp->ringStart, mp->maxSchThrds * mp->numPorts);
	RTE_LOG(INFO, USER2, "Transmitter ready - sending packet STARTED\n");

	//DPDKMS-482 - additional workaround
	rte_delay_us_sleep(10 * 1000 * 1000);

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
#if (RTE_VER_YEAR < 21)
			pktVect[ij]->timestamp = 0;
#else
			pktpriv_data_t *ptr = rte_mbuf_to_priv(pktVect[ij]);
			ptr->timestamp = 0;
#endif

			do
			{
				if (unlikely(s->vctx.tmstamp == 0))
				{
					s->vctx.tmstamp
						= RvRtpGetFrameTmstamp(s, firstSnInRound, &roundTime, pktVect[ij]);
					firstSnInRound = 0;
				}
			} while (RvRtpSessionCheckRunState(s) == 0);

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

				if (s->vctx.alignTmstamp)
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

#ifdef TX_RINGS_DEBUG
	RTE_LOG(INFO, USER1, "PKT ENQUEUE RUNNING ON LCORE %d SOCKET %d THREAD %d\n", rte_lcore_id(),
			rte_lcore_to_socket_id(rte_lcore_id()), threadId);
#endif

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

#ifdef TX_RINGS_DEBUG
	RTE_LOG(INFO, USER2, "Thread params: %u %u %u %u\n", thrdSnFirst, thrdSnLast, pktsCount,
			threadId);
#endif
#ifdef ST_RING_TIME_PRINT
	uint64_t ratioClk = rte_get_tsc_hz();
#endif

	// wait for scheduler threads to be ready
	RVRTP_SEMAPHORE_WAIT(mp->ringStart, mp->maxSchThrds * mp->numPorts);
	RTE_LOG(INFO, USER2, "Transmitter ready - sending packet STARTED\n");

	//DPDKMS-482 - additional workaround
	rte_delay_us_sleep(10 * 1000 * 1000);

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

			sendR = (redRing && (s->sn.caps & ST_SN_DUAL_PATH)) ? 1 : 0;
			uint32_t ij = (i - thrdSnFirst) * ST_DEFAULT_PKTS_IN_LN;
#if (RTE_VER_YEAR < 21)
			pktVect[ij]->timestamp = 0;
#else
			pktpriv_data_t *ptr = rte_mbuf_to_priv(pktVect[ij]);
			ptr->timestamp = 0;
#endif

			do
			{
				if (unlikely(s->vctx.tmstamp == 0))
				{
					s->vctx.tmstamp
						= RvRtpGetFrameTmstamp(s, firstSnInRound, &roundTime, pktVect[ij]);
					firstSnInRound = 0;
				}
			} while (RvRtpSessionCheckRunState(s) == 0);

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

				if (s->vctx.alignTmstamp)
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

				if (unlikely(!pktVect[ij]))
					continue;

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

	if (redRing)
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
