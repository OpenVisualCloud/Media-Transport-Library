/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef _ST_VIDEO_TRANSMITTER_HARNESS_H_
#define _ST_VIDEO_TRANSMITTER_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_trs_ctx ut_trs_ctx;

int ut_trs_init(void);

ut_trs_ctx* ut_trs_create(void);
void ut_trs_destroy(ut_trs_ctx* ctx);

void ut_trs_set_trs(ut_trs_ctx* ctx, long double trs_ns);
void ut_trs_set_warm_pkts_cap(ut_trs_ctx* ctx, uint32_t warm_pkts_cap);
void ut_trs_set_target_tsc(ut_trs_ctx* ctx, uint64_t target_tsc);
void ut_trs_set_target_tsc_port(ut_trs_ctx* ctx, int port, uint64_t target_tsc);
/* the rl_plan[] pad_ns / bucket_ns of a session port, in ns; a drain arms the plan */
void ut_trs_set_drain(ut_trs_ctx* ctx, int port, long double drain_ns);
void ut_trs_set_credit(ut_trs_ctx* ctx, int port, long double credit_ns);
/* the bucket of the modeled shaper, in ns, when it differs from the plan's credit */
void ut_trs_set_shaper_bucket(ut_trs_ctx* ctx, int port, long double bucket_ns);
/* The modeled shaper adds its tokens every slot_ns, the last time phase_ns (< slot_ns)
 * before it started the pads from a full bucket: packet 0 then leaves up to a slot before
 * or after the fluid drain end. */
void ut_trs_set_shaper_timeslot(ut_trs_ctx* ctx, int port, long double slot_ns,
                                long double phase_ns);

void ut_trs_set_mock_tsc_script(ut_trs_ctx* ctx, const uint64_t* values, int count);

/* the trs-counted warm-up of a port without a plan */
void ut_trs_warm_up(ut_trs_ctx* ctx);
/* the pad plan of a port with a drain and credit set; false: hold packet 0 */
bool ut_trs_pre_arm(ut_trs_ctx* ctx);
bool ut_trs_pre_arm_port(ut_trs_ctx* ctx, int port);

/* When the shaper starts the first real packet of the port: the later of when it was
 * queued (without one yet, the last tsc read) and the drain end of the pads accepted
 * ahead of it, drained = max(drained, tsc - bucket_ns) + pad_ns per pad, a token
 * bucket bucket_ns deep. It reads the same pad_ns / bucket_ns the plan uses, unless
 * ut_trs_set_shaper_bucket() gave it another bucket, so it checks the plan arithmetic,
 * not those values. */
long double ut_trs_modeled_launch_tsc(const ut_trs_ctx* ctx, int port);
/* the shaper time of ST_TX_VIDEO_RL_TRIM_FINE bytes of a NORMAL pad, the trim's step */
long double ut_trs_fine_trim_ns(const ut_trs_ctx* ctx, int port);
/* refcnt of the port P pad ST_TX_VIDEO_RL_TRIM_COARSE (j 0) or _FINE (j 1) bytes short */
uint16_t ut_trs_trim_pad_refcnt(const ut_trs_ctx* ctx, int j);
uint32_t ut_trs_train_pads(const ut_trs_ctx* ctx, int port);
void ut_trs_clear_train(ut_trs_ctx* ctx);

uint32_t ut_trs_pad_send_count(const ut_trs_ctx* ctx);
uint64_t ut_trs_last_tsc(const ut_trs_ctx* ctx);
uint64_t ut_trs_get_mock_tsc(ut_trs_ctx* ctx);
uint64_t ut_trs_stat_troffset_mismatch(const ut_trs_ctx* ctx);
uint64_t ut_trs_stat_recalculate_warmup(const ut_trs_ctx* ctx);

void ut_trs_set_burst_force_fail(ut_trs_ctx* ctx, bool fail);
/* the burst mock accepts at most limit pkts per call, 0 = all */
void ut_trs_set_burst_accept_limit(ut_trs_ctx* ctx, uint16_t limit);
/* burst call number call (from 1) stalls stall_ns, as on an interrupt or a preemption:
 * before its doorbell, so the NIC gets the pkts late, or after it, so only the return is
 * late; the mock clock stays stall_ns ahead from then */
void ut_trs_set_burst_stall(ut_trs_ctx* ctx, uint32_t call, uint64_t stall_ns,
                            bool after_doorbell);
uint32_t ut_trs_burst_call_count(const ut_trs_ctx* ctx);
/* the most rte_mbuf_refcnt_update() calls between a tsc read and the burst after it */
uint32_t ut_trs_max_refcnt_updates_before_burst(const ut_trs_ctx* ctx);
uint32_t ut_trs_real_send_count(const ut_trs_ctx* ctx);
uint32_t ut_trs_sent_pkt_idx(const ut_trs_ctx* ctx, uint32_t pos);
uint64_t ut_trs_last_pad_send_tsc(const ut_trs_ctx* ctx);
uint64_t ut_trs_last_real_send_tsc(const ut_trs_ctx* ctx);
uint16_t ut_trs_pad_refcnt(const ut_trs_ctx* ctx);
unsigned int ut_trs_pad_inflight_num(const ut_trs_ctx* ctx);
unsigned int ut_trs_inflight_num(const ut_trs_ctx* ctx);
uint64_t ut_trs_target_tsc(const ut_trs_ctx* ctx);
int ut_trs_rl_state(const ut_trs_ctx* ctx);
bool ut_trs_recovery_pending(const ut_trs_ctx* ctx);

int ut_trs_rl_state_port(const ut_trs_ctx* ctx, int port);
void ut_trs_set_rl_state_port(ut_trs_ctx* ctx, int port, int state);
bool ut_trs_recovery_pending_port(const ut_trs_ctx* ctx, int port);
void ut_trs_set_recovery_pending_port(ut_trs_ctx* ctx, int port, bool pending);
bool ut_trs_process_recovery_cleanup(ut_trs_ctx* ctx, int port);
void ut_trs_call_recovery_cleanup(ut_trs_ctx* ctx);

void ut_trs_set_pad_inflight_num(ut_trs_ctx* ctx, unsigned int n);
void ut_trs_set_inflight_num(ut_trs_ctx* ctx, unsigned int n);
void ut_trs_set_inflight_num2(ut_trs_ctx* ctx, unsigned int n);
bool ut_trs_prepare_cleanup_state(ut_trs_ctx* ctx);
bool ut_trs_prepare_redundant_cleanup_state(ut_trs_ctx* ctx);
void ut_trs_cleanup_state(ut_trs_ctx* ctx);
unsigned int ut_trs_inflight_num2(const ut_trs_ctx* ctx);
unsigned int ut_trs_inflight_idx(const ut_trs_ctx* ctx);
unsigned int ut_trs_inflight_idx2(const ut_trs_ctx* ctx);
unsigned int ut_trs_priv_pool_avail(void);

void ut_trs_set_last_burst_succ_tsc(ut_trs_ctx* ctx, uint64_t tsc);
uint64_t ut_trs_get_last_burst_succ_tsc(const ut_trs_ctx* ctx);
void ut_trs_set_hang_detect_thresh_ns(ut_trs_ctx* ctx, uint64_t thresh_ns);

int ut_trs_get_stat_trs_ret_code(const ut_trs_ctx* ctx);
int ut_trs_get_stat_pkts_burst(const ut_trs_ctx* ctx);

uint16_t ut_trs_call_burst_pad(ut_trs_ctx* ctx);
int ut_trs_call_rl_tasklet(ut_trs_ctx* ctx);

void ut_trs_enqueue_ring_pkt(ut_trs_ctx* ctx);
void ut_trs_enqueue_first_pkt(ut_trs_ctx* ctx, uint64_t target_tsc);
void ut_trs_enqueue_frame_boundary(ut_trs_ctx* ctx, uint64_t target_tsc);

/* MT_DEV_TX_DESC_DEEP, the TX ring of a deep port */
uint16_t ut_trs_deep_nb_tx_desc(void);
/* Stream mode: the RL tasklet runs mid-frame against a NIC TX queue of nic_desc
 * descriptors, each pkt taking segs_per_pkt of them (2 for a zero-copy chained pkt)
 * until the shaper finishes it, one pkt per trs. */
int ut_trs_setup_stream(ut_trs_ctx* ctx, long double trs_ns, uint16_t nic_desc,
                        uint16_t segs_per_pkt);
/* calls the tasklet every call_ns for duration_ns, the ring kept full */
void ut_trs_stream(ut_trs_ctx* ctx, uint64_t duration_ns, uint64_t call_ns);
/* the scheduler thread loses its CPU: the clock moves, the tasklet does not run */
void ut_trs_stall_stream(ut_trs_ctx* ctx, uint64_t stall_ns);
/* how far the latest pkt left behind its slot in a stream that never pauses */
long double ut_trs_stream_max_late_ns(const ut_trs_ctx* ctx);
uint32_t ut_trs_stream_nic_desc_used(const ut_trs_ctx* ctx);
/* Stream mode: put pkts mid-frame pkts in the ring, pkt i due at first_tsc + i * step_ns,
 * and move the clock to now_ns. */
int ut_trs_fill_stream(ut_trs_ctx* ctx, uint32_t pkts, uint64_t first_tsc,
                       uint64_t step_ns, uint64_t now_ns);
/* the pkts the NIC took in stream mode */
uint64_t ut_trs_stream_nic_pkts(const ut_trs_ctx* ctx);
/* ST_TX_VIDEO_LATE_BULKS_MAX and ST_SESSION_MAX_BULK */
int ut_trs_bulks_per_call(void);
int ut_trs_max_bulk(void);

#ifdef __cplusplus
}
#endif

#endif /* _ST_VIDEO_TRANSMITTER_HARNESS_H_ */
