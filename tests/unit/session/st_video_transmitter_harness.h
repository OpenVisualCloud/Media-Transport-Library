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
/* the plan's absorb_ns, 0 by default: every counted stall then holds */
void ut_trs_set_absorb(ut_trs_ctx* ctx, int port, long double absorb_ns);
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
/* burst calls number call (from 1) to call + calls - 1 accept nothing, as from a full
 * NIC ring */
void ut_trs_set_burst_refuse(ut_trs_ctx* ctx, uint32_t call, uint32_t calls);
/* burst calls with the mock clock in [from, to) accept nothing, as from a NIC ring full
 * for a while */
void ut_trs_set_burst_refuse_window(ut_trs_ctx* ctx, uint64_t from, uint64_t to);
/* the burst mock accepts at most limit pkts per call, 0 = all */
void ut_trs_set_burst_accept_limit(ut_trs_ctx* ctx, uint16_t limit);
/* burst call number call (from 1) stalls stall_ns, as on an interrupt or a preemption:
 * before its doorbell, so the NIC gets the pkts late, or after it, so only the return is
 * late; the mock clock stays stall_ns ahead from then */
void ut_trs_set_burst_stall(ut_trs_ctx* ctx, uint32_t call, uint64_t stall_ns,
                            bool after_doorbell);
uint32_t ut_trs_burst_call_count(const ut_trs_ctx* ctx);
/* The PMD holds batches done rs batches it has not freed, and frees the oldest in each
 * burst, empty ones too, taking its free_ns before the doorbell, while more than
 * ut_trs_rl_kept_batches() are in use (done, or pads the shaper has yet to drain);
 * rs_thresh is the port's mt_if_tx_reclaim_rs_thresh(), 0 for a port that does not
 * reclaim. _add_ puts more behind those. */
void ut_trs_set_pmd_done_batches(ut_trs_ctx* ctx, uint16_t rs_thresh, uint32_t batches,
                                 uint64_t free_ns);
void ut_trs_add_pmd_done_batches(ut_trs_ctx* ctx, uint32_t batches, uint64_t free_ns);
/* the done batches freed, in any call */
uint32_t ut_trs_pmd_frees(const ut_trs_ctx* ctx);
/* the done rs batches dev_start_port()'s tx_free_thresh lets a reclaiming port keep */
uint32_t ut_trs_rl_kept_batches(void);
/* the done batches pad bursts freed */
uint32_t ut_trs_pmd_pad_burst_frees(const ut_trs_ctx* ctx);
/* the mt_txq_reclaim() calls, empty bursts */
uint32_t ut_trs_reclaim_calls(const ut_trs_ctx* ctx);
/* ST_TX_VIDEO_RL_STALL_NS */
uint64_t ut_trs_rl_stall_ns(void);
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
/* the port P status of its last tasklet call, > 0 when it sent */
void ut_trs_set_stat_trs_ret_code(ut_trs_ctx* ctx, int ret);
/* whether the port has its NIC TX queue */
void ut_trs_set_queue_port(ut_trs_ctx* ctx, int port, bool present);
int ut_trs_get_stat_pkts_burst(const ut_trs_ctx* ctx);

uint16_t ut_trs_call_burst_pad(ut_trs_ctx* ctx);
/* Burst call i takes d_ns[i % n] of mock time before its doorbell, and the tsc read
 * after its return stamp, or the tasklet's return, g_ns[i % n] more; n 0: none. */
void ut_trs_set_burst_cost(ut_trs_ctx* ctx, const uint64_t* d_ns, const uint64_t* g_ns,
                           int n);
/* the tasklet is off the CPU: the mock clock moves ns */
void ut_trs_advance_mock_tsc(ut_trs_ctx* ctx, uint64_t ns);
/* The lcore thread stops ns[i] at mock time at_tsc[i] (sorted): inside a tasklet call
 * the call takes it, before the doorbell when inside a burst's cost; while the tasklet is
 * off the CPU, ut_trs_advance_mock_tsc() passes it. */
void ut_trs_set_cpu_stalls(ut_trs_ctx* ctx, const uint64_t* at_tsc, const uint64_t* ns,
                           int n);
int ut_trs_call_rl_tasklet(ut_trs_ctx* ctx);
/* a call carrying pads then pkts takes n of them, 0 all */
void ut_trs_set_mixed_accept(ut_trs_ctx* ctx, uint16_t n);
/* such a call stalls ns before its doorbell */
void ut_trs_set_mixed_stall(ut_trs_ctx* ctx, uint64_t ns);
/* the session buffers RTCP: the transmitter's mt_rtcp_tx_buffer_rtp_packets() is a mock
 */
void ut_trs_set_rtcp(ut_trs_ctx* ctx, bool on);
/* packet 0's frame (st_tx_mbuf_set_priv()) and bytes */
void ut_trs_set_first_pkt_frame(ut_trs_ctx* ctx, void* frame, uint16_t len);
/* st20_frame_tx_start() calls, and the frame of the last */
uint32_t ut_trs_frame_starts(const ut_trs_ctx* ctx, void** frame);
/* the mbufs buffered for RTCP, and the pads among them */
uint32_t ut_trs_rtcp_buffered(const ut_trs_ctx* ctx, uint32_t* pads);
/* the burst calls that carried pads then pkts */
uint32_t ut_trs_mixed_calls(const ut_trs_ctx* ctx);
/* the port's video stats */
uint64_t ut_trs_port_bytes(const ut_trs_ctx* ctx);
uint64_t ut_trs_port_packets(const ut_trs_ctx* ctx);

void ut_trs_enqueue_ring_pkt(ut_trs_ctx* ctx);
/* the port paces by RL, as the transmitter tasklet sees it */
void ut_trs_set_rl_port(ut_trs_ctx* ctx, int port);
/* ops.num_port of the session */
void ut_trs_set_num_port(ut_trs_ctx* ctx, int num_port);
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
/* mt_sch_schedule_ns(), 0 by default */
void ut_trs_set_sch_schedule_ns(ut_trs_ctx* ctx, uint32_t ns);
/* One visit of the transmitter tasklet of a scheduler to the n sessions, RL on port P,
 * on ctx[0]'s clock and NIC. */
int ut_trs_call_transmitter(ut_trs_ctx** ctx, int n);
/* the builder fills the ring with mid-frame pkts */
void ut_trs_stream_fill(ut_trs_ctx* ctx);
/* as ut_trs_stream(), through the transmitter tasklet, one visit every round_ns from
 * the current time */
void ut_trs_stream_rounds(ut_trs_ctx* ctx, uint64_t duration_ns, uint64_t round_ns);
/* the scheduler thread loses its CPU: the clock moves, the tasklet does not run */
void ut_trs_stall_stream(ut_trs_ctx* ctx, uint64_t stall_ns);
/* how far the latest pkt left behind its slot in a stream that never pauses */
long double ut_trs_stream_max_late_ns(const ut_trs_ctx* ctx);
/* the wire time the NIC TX queue holds ahead of the tasklet's clock */
long double ut_trs_stream_ahead_ns(const ut_trs_ctx* ctx);
uint32_t ut_trs_stream_nic_desc_used(const ut_trs_ctx* ctx);

/* ut_trs_wire() of a pad */
#define UT_TRS_WIRE_PAD (UINT32_MAX - 1)

/* Frames mode: frames of total_pkts pkts reach the ring as tv_tasklet_frame() builds
 * them, bulk pkts at a time, dummies past a frame's end. Pkt 0 is due hold_ns after its
 * bulk is queued, or long past when hold_ns is 0. */
struct ut_trs_frames_case {
  unsigned int bulk;
  int total_pkts;
  int frames;
  float pad_interval;
  uint64_t hold_ns;
  uint16_t accept_limit; /* most pkts a burst takes, 0 = all */
  uint32_t fail_every;   /* every fail_every-th burst call takes none, 0 = never */
  /* the last lead pkts of an earlier frame precede frame 0 in its first bulk, as an RTP
   * level timestamp change mid-bulk leaves them, so frame 0's bulks are not 4-aligned */
  unsigned int lead;
  /* with drain_ns an armed plan pre-arms packet 0 from warm_pkts trs before its target */
  long double drain_ns, credit_ns, absorb_ns;
  uint32_t warm_pkts;
  uint16_t mixed_accept; /* ut_trs_set_mixed_accept() */
};
/* runs the RL tasklet until every frame is out; < 0 if it stops short */
int ut_trs_run_frames(ut_trs_ctx* ctx, const struct ut_trs_frames_case* c);
/* what the bursts took, in order: a pkt's index in its frame, or UT_TRS_WIRE_PAD */
uint32_t ut_trs_wire_count(const ut_trs_ctx* ctx);
uint32_t ut_trs_wire(const ut_trs_ctx* ctx, uint32_t pos);
uint64_t ut_trs_wire_tsc(const ut_trs_ctx* ctx, uint32_t pos);
uint64_t ut_trs_frame_target(const ut_trs_ctx* ctx, int frame);
int ut_trs_stat_pkts_burst_dummy(const ut_trs_ctx* ctx);
unsigned int ut_trs_frame_pkts_in_use(const ut_trs_ctx* ctx);
/* A scheduler running the video transmitter tasklet over ctxs[0..n), one RL port each,
 * in that order. Its lcore clock is *now: a session's visit starts there and the clock
 * moves to where the visit ended. before(i, arg) runs ahead of each visit of session i
 * and skips it when false; after(i, arg) runs after it. */
typedef struct ut_trs_sch ut_trs_sch;
typedef bool (*ut_trs_visit_fn)(int i, void* arg);
ut_trs_sch* ut_trs_sch_create(ut_trs_ctx** ctxs, int n, uint64_t* now);
/* frees sch; returns the bursts its sessions sent while the transmitter was in another
 * session's slot, 0 for a protocol a writer can rely on */
uint32_t ut_trs_sch_destroy(ut_trs_sch* sch);
/* takes or drops session i's slot lock, as the control plane does */
void ut_trs_sch_lock_session(ut_trs_sch* sch, int i, bool lock);
void ut_trs_sch_set_visit(ut_trs_sch* sch, ut_trs_visit_fn before, ut_trs_visit_fn after,
                          void* arg);
/* one video_trs_tasklet_handler() call */
int ut_trs_sch_call(ut_trs_sch* sch);
/* the tx_video_session_dp_get() calls of the last ut_trs_sch_call() */
uint32_t ut_trs_sch_session_gets(const ut_trs_sch* sch);

#ifdef __cplusplus
}
#endif

#endif /* _ST_VIDEO_TRANSMITTER_HARNESS_H_ */
