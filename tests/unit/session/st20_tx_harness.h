/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness API for the ST 2110-20 (video) TX epoch/pacing math unit tests.
 *
 * Wraps a single `st_tx_video_session_impl` and exposes thin wrappers over
 * the file-local `calc_frame_count_since_epoch()` and `tv_sync_pacing()`.
 * Both mocked time sources (PTP and TSC) are fully controllable so tests can
 * hit exact boundary values deterministically. All MTL internal types are
 * kept opaque so the C++ test layer never includes libmtl headers.
 */

#ifndef _ST20_TX_SESSION_HARNESS_H_
#define _ST20_TX_SESSION_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#include "st_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_txv_ctx ut_txv_ctx;

enum ut_txv_pacing_way {
  UT_TXV_PACING_RL = 1,
  UT_TXV_PACING_TSC = 2,
  UT_TXV_PACING_PTP = 4,
};

/* Initialise the shared DPDK EAL. Idempotent — safe to call once per gtest
 * fixture SetUp(). Returns 0 on success, < 0 on failure. */
int ut_txv_init(void);

/* Create a context with default pacing (frame_time=1ms, max_onward_epochs=3,
 * all other pacing fields zeroed). Caller owns the returned pointer and must
 * free it with ut_txv_destroy(). Returns NULL on allocation failure. */
ut_txv_ctx* ut_txv_create(void);
void ut_txv_destroy(ut_txv_ctx* ctx);

/* ── pacing setup ──────────────────────────────────────────────────────── */
/* pacing->frame_time and the epoch period of fps_tm, both frame_time_ns */
void ut_txv_set_frame_time(ut_txv_ctx* ctx, uint32_t frame_time_ns);
void ut_txv_set_max_onward_epochs(ut_txv_ctx* ctx, uint32_t max_onward_epochs);
void ut_txv_set_cur_epochs(ut_txv_ctx* ctx, uint64_t cur_epochs);
void ut_txv_set_tr_offset(ut_txv_ctx* ctx, long double tr_offset_ns);
void ut_txv_set_vrx(ut_txv_ctx* ctx, uint32_t vrx);
void ut_txv_set_trs(ut_txv_ctx* ctx, long double trs_ns);
void ut_txv_set_warm_pkts(ut_txv_ctx* ctx, uint32_t warm_pkts);
/* ops.interlaced: selects the ST 2110-21 6.2 field/frame-grid schedule. */
void ut_txv_set_interlaced(ut_txv_ctx* ctx, bool enable);
/* s->s_type: the ST22 shape st22_tx_create() gives an ST22_TYPE_RTP_LEVEL session.
 * st22_info stays NULL, which is what distinguishes it from ST22 frame level and
 * makes its field parity unreadable. */
void ut_txv_set_st22_rtp_level(ut_txv_ctx* ctx, bool enable);
/* s->second_field -- the parity tv_init_next_meta() hands the NEXT frame. The
 * tasklet paths read it from there, and production flips it after each frame. */
void ut_txv_set_second_field(ut_txv_ctx* ctx, bool second_field);
/* Toggle ST20_TX_FLAG_EXACT_USER_PACING on the session's ops.flags. */
void ut_txv_set_exact_user_pacing(ut_txv_ctx* ctx, bool enable);
/* Toggle ST20_TX_FLAG_USER_PACING on the session's ops.flags. */
void ut_txv_set_user_pacing(ut_txv_ctx* ctx, bool enable);
/* Toggle ST20_TX_FLAG_USER_TIMESTAMP on the session's ops.flags: the RTP
 * timestamp is taken verbatim from the app-supplied timestamp instead of the
 * pacing-scheduled instant. Selects the branch of tv_update_rtp_time_stamp()
 * whose returned "reporting" TAI instant must match the RTP timestamp it
 * derived. */
void ut_txv_set_user_timestamp(ut_txv_ctx* ctx, bool enable);
/* Toggle ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH on the session's ops.flags: the RTP
 * timestamp is derived from the bare epoch, omitting tr_offset. Added to pin
 * that this basis (not pacing->ptp_time_cursor) is also what gets reported
 * back as frame->timestamp. */
void ut_txv_set_rtp_timestamp_epoch(ut_txv_ctx* ctx, bool enable);
/* Set ops.rtp_timestamp_delta_us, added to the RTP-timestamp source instant
 * (never to the actual TX schedule). Added to pin that the reported
 * frame->timestamp includes this delta too, so it still reconstructs
 * frame->rtp_timestamp. */
void ut_txv_set_rtp_timestamp_delta_us(ut_txv_ctx* ctx, int32_t delta_us);
/* Media (RTP) sampling clock rate, e.g. 90000 for video. */
void ut_txv_set_sampling_clock_rate(ut_txv_ctx* ctx, uint32_t sampling_rate);
/* Real TAI wall-clock cursor used as the source time for the non-user-
 * timestamp RTP derivation path in tv_update_rtp_time_stamp(). */
void ut_txv_set_ptp_time_cursor(ut_txv_ctx* ctx, uint64_t tai_ns);

/* Mocked time sources consumed by tv_sync_pacing() via mt_get_ptp_time()/
 * mt_get_tsc(). Take effect immediately; no auto-advance between calls. */
void ut_txv_set_mock_ptp_time(ut_txv_ctx* ctx, uint64_t ptp_ns);
void ut_txv_set_mock_tsc_time(ut_txv_ctx* ctx, uint64_t tsc_ns);

/* ── code under test ───────────────────────────────────────────────────── */
uint64_t ut_txv_calc_frame_count_since_epoch(ut_txv_ctx* ctx, uint64_t cur_tai,
                                             uint64_t required_tai);
/* Drives the mocked ptp/tsc values set above; required_tai as in the
 * production st20_tx_ops (0 = no user-supplied timestamp). */
int ut_txv_sync_pacing(ut_txv_ctx* ctx, uint64_t required_tai);
uint64_t ut_txv_pacing_required_tai(ut_txv_ctx* ctx, enum st10_timestamp_fmt tfmt,
                                    uint64_t timestamp);
int ut_txv_run_frame_tasklet(ut_txv_ctx* ctx, enum st10_timestamp_fmt tfmt,
                             uint64_t timestamp, uint64_t* packet_tsc,
                             uint64_t* packet_ptp);
/* Drives tv_tasklet_rtp() with one app packet whose st20_rfc4175_rtp_hdr row
 * number carries the given field parity, so the production builders' F-bit
 * extraction is what tv_sync_pacing() is told. tx_no_chain selects tv_build_rtp()
 * over tv_build_rtp_chain(). Sets ops.type to ST20_TYPE_RTP_LEVEL; combine with
 * ut_txv_set_st22_rtp_level() for the ST22 shape. Outputs pacing->cur_epochs, and
 * fails if no packet reached the TX ring. Returns 0 on success. */
int ut_txv_run_rtp_tasklet(ut_txv_ctx* ctx, bool second_field, bool tx_no_chain,
                           uint64_t* epoch);
/* Drives tv_tasklet_st22()'s frame->tx_st22_meta assignment -- the compressed-
 * video (ST22) mirror of tv_tasklet_frame()'s frame->tv_meta assignment above,
 * so both production call sites are held to the same timestamp/rtp_timestamp
 * contract, not just the uncompressed-video one. tv_tasklet_st22() returns
 * before building any packet, so this needs only a ring (for its
 * rte_ring_full() guard), no mempool or packet payload setup. Outputs the
 * frame's timestamp/rtp_timestamp directly rather than routing through a
 * notify_frame_done callback, since st22_tx_video_info has no equivalent mock
 * plumbing in this harness yet. */
int ut_txv_run_st22_next_frame_step(ut_txv_ctx* ctx, enum st10_timestamp_fmt tfmt,
                                    uint64_t timestamp, uint64_t* frame_timestamp,
                                    uint32_t* frame_rtp_timestamp);
/* What the harness session has to do in ut_txv_run_tasklet_slots(). */
enum ut_txv_work {
  UT_TXV_WORK_NONE,  /* frame level, get_next_frame busy */
  UT_TXV_WORK_FRAME, /* frame level, starts a two-packet frame */
  UT_TXV_WORK_RTP,   /* RTP level, one app packet queued */
  UT_TXV_WORK_ST22,  /* ST22 frame level, starts a frame */
};
enum ut_txv_slot {
  UT_TXV_SLOT_SELF,     /* the harness session */
  UT_TXV_SLOT_IDLE,     /* a frame-level peer whose get_next_frame is always busy */
  UT_TXV_SLOT_INACTIVE, /* the same peer, not active */
  UT_TXV_SLOT_EMPTY,    /* no session */
};
/* Runs tvs_tasklet_handler() once over the manager slots in slots[], exactly one of
 * them UT_TXV_SLOT_SELF. Returns the handler's result, < 0 on setup failure. */
int ut_txv_run_tasklet_slots(ut_txv_ctx* ctx, enum ut_txv_work work,
                             const enum ut_txv_slot* slots, int nb_slots);
/* get_next_frame calls the peers of the last ut_txv_run_tasklet_slots() received. */
int ut_txv_peer_get_next_frame_calls(const ut_txv_ctx* ctx);

/* A frame-level session of UT_TXV_ZC_FRAMES one-pkt frames, run by tvs_tasklet_handler()
 * against an app that hands over a chosen frame, or is busy, with reclaim.enabled or not.
 * Pkts keep a frame referenced (refs); mt_txq_reclaim() frees those ut_txv_zc_nic_sent()
 * marks sent. Frames are built by copy: the build path is not under test. Every leg last
 * burst at setup, and the hang threshold is off until ut_txv_zc_set_hang_ns(). */
int ut_txv_zc_setup(ut_txv_ctx* ctx, bool tx_reclaim, int num_port);
void ut_txv_zc_teardown(ut_txv_ctx* ctx);
void ut_txv_zc_set_nic_refs(ut_txv_ctx* ctx, int frame, int refs);
void ut_txv_zc_nic_sent(ut_txv_ctx* ctx, int frame);
void ut_txv_zc_set_app(ut_txv_ctx* ctx, int frame, bool busy);
/* the frame the session built last, whose pkts are still on their way */
void ut_txv_zc_set_last_built(ut_txv_ctx* ctx, int frame);
void ut_txv_zc_run(ut_txv_ctx* ctx);
/* tv_frames_out() alone, the step before the app is asked, for any num_port */
bool ut_txv_zc_frames_out(ut_txv_ctx* ctx);
int ut_txv_zc_reclaim_calls(const ut_txv_ctx* ctx);
/* mt_txq_reclaim() calls on the queue of session port leg */
int ut_txv_zc_leg_reclaims(const ut_txv_ctx* ctx, int leg);
void ut_txv_zc_set_hang_ns(ut_txv_ctx* ctx, uint64_t hang_ns);
/* the transmitter of leg sent a burst at the mock time */
void ut_txv_zc_leg_burst_now(ut_txv_ctx* ctx, int leg);
/* recovery of leg pending, cleared as the transmitter would on handling it */
bool ut_txv_zc_take_recovery(ut_txv_ctx* ctx, int leg);
/* packet 0 target of the frame built last, pacing.tsc_time_frame_start */
void ut_txv_zc_set_next_frame_due(ut_txv_ctx* ctx, uint64_t tsc_ns);
unsigned int ut_txv_zc_built_pkts(const ut_txv_ctx* ctx);
/* ST_TX_VIDEO_RECLAIM_NS: the least time between two reclaims of a session */
uint64_t ut_txv_reclaim_interval_ns(void);
int ut_txv_run_transmitter_boundary(ut_txv_ctx* ctx, enum ut_txv_pacing_way way,
                                    uint64_t delta_ns, int* bursts_before_target,
                                    int* bursts_at_target);
/* Drives tv_init_hw() for a one-port session whose logical port P maps to
 * phy_port, after recording a rate-limiter training result (trained_bps) for the
 * session's bitrate on phy_port only. The TX queue request is intercepted and
 * failed; outputs the physical port and bytes_per_sec it carried. Returns 0 if
 * exactly one queue was requested. */
int ut_txv_run_init_hw_rl_lookup(ut_txv_ctx* ctx, enum mtl_port phy_port,
                                 uint64_t trained_bps, enum mtl_port* queue_port,
                                 uint64_t* queue_bps);
/* Toggle MTL_FLAG_RANDOM_SRC_PORT in the instance's init flags. */
void ut_txv_set_random_src_port(ut_txv_ctx* ctx, bool enable);
/* Drives tv_update_dst() on a one-port session with the given ops.udp_src_port and
 * the source port st20_tx_create() chose (created_src_port). A user-supplied
 * destination MAC keeps ARP out of it. Outputs the UDP ports now in the session's
 * packet header template, host order. Returns 0 on success. */
int ut_txv_update_dst(ut_txv_ctx* ctx, uint16_t udp_src_port, uint16_t created_src_port,
                      uint16_t new_udp_port, uint16_t* hdr_src_port,
                      uint16_t* hdr_dst_port);
/* Drives tv_update_rtp_time_stamp() directly with the pacing state set up
 * above (ptp_time_cursor, sampling_clock_rate). Returns the TAI the RTP timestamp
 * was derived from. */
uint64_t ut_txv_update_rtp_time_stamp(ut_txv_ctx* ctx, enum st10_timestamp_fmt tfmt,
                                      uint64_t timestamp);
/* Drives the real tv_init_pacing() -- the only place pacing->tr_offset is derived
 * for a live session. Forces TSC pacing so no NIC queue or rate-limiter training
 * is needed. Returns tv_init_pacing()'s status; read the result with
 * ut_txv_pacing_tr_offset() below. */
int ut_txv_init_pacing(ut_txv_ctx* ctx, uint32_t height, bool interlaced,
                       enum st_fps fps);
/* tv_train_pacing() of a one-port 1080p59.94 RL session without a pre-train result,
 * GPM_SL or BPM, against an RL queue of nb_tx_desc descriptors draining a NORMAL pad in
 * pad_ns and any other by its bytes. */
int ut_txv_train_rl_pacing(ut_txv_ctx* ctx, uint16_t nb_tx_desc, long double pad_ns,
                           bool bpm);
float ut_txv_pad_interval(const ut_txv_ctx* ctx);
/* ops.type of the next ut_txv_train_rl_pacing(): RTP level trains on NORMAL pads only */
void ut_txv_set_rtp_level(ut_txv_ctx* ctx, bool enable);
/* mt_pacing_train_pad_result_add() and _search() of port P with that pad mix, on a queue
 * programmed at queue_bps */
int ut_txv_pad_result_add(ut_txv_ctx* ctx, uint64_t bps, uint64_t queue_bps,
                          uint64_t bytes, uint32_t normal, uint32_t pkts,
                          float pad_interval);
int ut_txv_pad_result_search(ut_txv_ctx* ctx, uint64_t bps, uint64_t queue_bps,
                             uint64_t bytes, uint32_t normal, uint32_t pkts,
                             float* pad_interval);
/* a bps retrain result for the rate of the session ut_txv_train_rl_pacing() set up */
int ut_txv_add_trained_bps(ut_txv_ctx* ctx, uint64_t bps);
/* Training traffic still in that queue, as shaper time. */
uint64_t ut_txv_train_queued_ns(const ut_txv_ctx* ctx);
uint64_t ut_txv_slept_ns(const ut_txv_ctx* ctx);
/* the bytes/s on the RL queue of session port port, as the last training left it */
uint64_t ut_txv_rl_queue_bps(const ut_txv_ctx* ctx, int port);
/* A session for tv_tx_reclaims() and tv_init_tx_chain(), on ports that reclaim or not,
 * on shared TX queues or dedicated ones */
struct ut_txv_reclaim_case {
  uint16_t port_rs_thresh[2]; /* mt_if_tx_reclaim_rs_thresh(), 0 for no reclaim */
  int num_port;
  bool shared_txq;
  bool rtcp;
  bool rtp_level;
  int total_pkts;
  int frames_cnt;
  bool no_chain; /* the ports cannot chain mbufs */
};
bool ut_txv_tx_reclaims(ut_txv_ctx* ctx, const struct ut_txv_reclaim_case* c);
/* tv_init_tx_chain() of that session at nb_tx_desc: zero copy, and reclaimed or not */
void ut_txv_init_tx_chain(ut_txv_ctx* ctx, const struct ut_txv_reclaim_case* c,
                          uint16_t nb_tx_desc, bool* zero_copy, bool* tx_reclaim);
/* ut_txv_init_rl_pacing() without the trim pads tv_init_hw() builds for RL ports */
void ut_txv_set_no_trim_pads(ut_txv_ctx* ctx);
/* tv_init_trim_pads() of port P with that pacing way and NORMAL pkt size: the lengths
 * of the pads it builds, 0 for none */
int ut_txv_init_trim_pads(ut_txv_ctx* ctx, enum st21_tx_pacing_way way,
                          uint32_t normal_size, uint32_t lens[2]);
/* Drives the real tv_init_pacing() for a GPM RL session of ut_txv_set_rl_format() on
 * num_port ports, each with its RL queue (port i on physical port i, an iavf VF unless
 * ut_txv_set_ice_pf() made it an ice PF). tv_train_pacing() resolves pad_interval without
 * the NIC: from user_pad_interval when non-zero, else the static table when static_pad,
 * else the pre-train result trained_pad_interval[port]. */
int ut_txv_init_rl_pacing(ut_txv_ctx* ctx, int num_port, uint16_t user_pad_interval,
                          bool static_pad, const float* trained_pad_interval, bool wide);
/* The YUV 4:2:2 10-bit GPM format of the next ut_txv_init_rl_pacing(), 1080p59.94 unless
 * set; fps is the field rate when interlaced. */
void ut_txv_set_rl_format(ut_txv_ctx* ctx, uint32_t width, uint32_t height,
                          bool interlaced, enum st_fps fps);
/* The bytes/s on the RL queue of session port port at ut_txv_init_rl_pacing(), as a
 * retrain left it; unset, what tv_init_hw() asks mt_txq_get() for. */
void ut_txv_set_rl_queue_bps(ut_txv_ctx* ctx, int port, uint64_t bps);
/* Physical port is a DPDK-driven ice PF with this mtl_port_init_params.rl_burst_size
 * (0 = the PMD default). Call before ut_txv_init_rl_pacing(). */
void ut_txv_set_ice_pf(ut_txv_ctx* ctx, int port, uint32_t rl_burst_size);
long double ut_txv_pacing_trs(const ut_txv_ctx* ctx);
uint32_t ut_txv_pacing_warm_pkts(const ut_txv_ctx* ctx);
/* the rl_plan[] pad_ns / bucket_ns of a session port, in ns */
long double ut_txv_rl_drain(const ut_txv_ctx* ctx, int port);
long double ut_txv_rl_credit(const ut_txv_ctx* ctx, int port);
/* the rest of the rl_plan[] the transmitter's pre-arm reads */
bool ut_txv_rl_armed(const ut_txv_ctx* ctx, int port);
uint16_t ut_txv_rl_pad_size(const ut_txv_ctx* ctx, int port);
int64_t ut_txv_rl_trim_pads(const ut_txv_ctx* ctx, int port);
/* Drives st_tai_round_to_media_clk_ns() -- the same media-clock snap
 * tv_sync_pacing() applies to the scheduled instant, so tests can express an
 * expectation as the raw ST 2110-21 sum instead of a pre-rounded constant. */
uint64_t ut_txv_round_to_media_clk(uint64_t tai_ns, uint32_t sampling_rate);

/* ── session-owned mempool release (tv_mempool_free) ──────────────────── */
/* Install a private header mempool on port P, as tv_mempool_init() would.
 * Returns 0 on success. ut_txv_destroy() releases whatever is left over. */
int ut_txv_install_hdr_mempool(ut_txv_ctx* ctx);
/* Take one mbuf out of the installed pool, mimicking an mbuf the transmitter
 * still holds. Returns 0 on success. */
int ut_txv_hold_hdr_mbuf(ut_txv_ctx* ctx);
void ut_txv_release_hdr_mbuf(ut_txv_ctx* ctx);
/* Mark the installed pool as borrowed rather than session-owned. */
void ut_txv_set_tx_mono_pool(ut_txv_ctx* ctx, bool enable);
void ut_txv_set_hdr_mempool_reuse_rx(ut_txv_ctx* ctx, bool enable);
/* Drives tv_mempool_free(): 0 when every pool was released, < 0 otherwise. */
int ut_txv_mempool_free(ut_txv_ctx* ctx);
/* Whether the session still points at the pool. */
bool ut_txv_hdr_mempool_installed(const ut_txv_ctx* ctx);
/* Whether the pool itself still exists, independent of the session's pointer. */
bool ut_txv_hdr_mempool_alive(const ut_txv_ctx* ctx);

/* ── ext-frame completion (tv_frame_free_cb) ──────────────────────────── */
enum { UT_TXV_FRAME_SIZE = 4 };
/* the frames of the ut_txv_zc_*() zero-copy session */
enum { UT_TXV_ZC_FRAMES = 2 };
/* One ST_FT_FLAG_EXT frame holding buf, fully built and still in flight in the
 * transmitter. Buffers are UT_TXV_FRAME_SIZE bytes. Returns 0 on success. */
int ut_txv_ext_frame_setup(ut_txv_ctx* ctx, void* buf);
/* st20_tx_set_ext_frame() on the session's public handle. */
int ut_txv_set_ext_frame(ut_txv_ctx* ctx, uint16_t idx, void* buf);
/* Releases the ext buffer the way the last mbuf free does (tv_frame_free_cb). */
void ut_txv_ext_frame_complete(ut_txv_ctx* ctx);
/* notify_frame_done re-arms the completed frame with buf, as st20p_tx_put_ext_frame()
 * does. */
void ut_txv_set_rearm_on_done(ut_txv_ctx* ctx, void* buf);
/* notify_frame_done runs the builder once; get_next_frame hands back the frame. */
void ut_txv_set_build_on_done(ut_txv_ctx* ctx);
int ut_txv_rearm_ret(const ut_txv_ctx* ctx);
/* st20_tx_get_framebuffer() of the completed frame, read inside notify_frame_done. */
void* ut_txv_done_framebuffer(const ut_txv_ctx* ctx);
/* The same, read inside notify_frame_done after the re-arm. */
void* ut_txv_rearmed_framebuffer(const ut_txv_ctx* ctx);
/* Packets on the TX ring after the builder run started from notify_frame_done. */
unsigned int ut_txv_built_on_done(const ut_txv_ctx* ctx);
/* Whether the meta notify_frame_done got was unchanged after that builder run. */
bool ut_txv_done_meta_kept(const ut_txv_ctx* ctx);
void* ut_txv_framebuffer(const ut_txv_ctx* ctx);
mtl_iova_t ut_txv_ext_frame_iova(const ut_txv_ctx* ctx);

/* ── accessors ─────────────────────────────────────────────────────────── */
uint64_t ut_txv_cur_epochs(const ut_txv_ctx* ctx);
long double ut_txv_pacing_tr_offset(const ut_txv_ctx* ctx);
long double ut_txv_tsc_time_cursor(const ut_txv_ctx* ctx);
long double ut_txv_ptp_time_cursor(const ut_txv_ctx* ctx);
uint64_t ut_txv_tsc_time_frame_start(const ut_txv_ctx* ctx);
uint64_t ut_txv_stat_epoch_onward(const ut_txv_ctx* ctx);
uint64_t ut_txv_stat_epoch_drop(const ut_txv_ctx* ctx);
uint64_t ut_txv_stat_error_user_timestamp(const ut_txv_ctx* ctx);
uint64_t ut_txv_stat_epoch_mismatch(const ut_txv_ctx* ctx);
int ut_txv_notify_late_calls(const ut_txv_ctx* ctx);
uint64_t ut_txv_notify_late_last_delta(const ut_txv_ctx* ctx);
int ut_txv_get_next_frame_calls(const ut_txv_ctx* ctx);
int ut_txv_notify_frame_done_calls(const ut_txv_ctx* ctx);
uint16_t ut_txv_notify_frame_done_idx(const ut_txv_ctx* ctx);
uint64_t ut_txv_notify_frame_done_timestamp(const ut_txv_ctx* ctx);
uint64_t ut_txv_notify_frame_done_epoch(const ut_txv_ctx* ctx);
/* Parity the session actually scheduled, as the app sees it in notify_frame_done(). */
bool ut_txv_notify_frame_done_second_field(const ut_txv_ctx* ctx);
/* The frame->rtp_timestamp the app would see in notify_frame_done(); paired
 * with ut_txv_notify_frame_done_timestamp() above to check that one
 * reconstructs the other via st10_tai_to_media_clk(). */
uint32_t ut_txv_notify_frame_done_rtp_timestamp(const ut_txv_ctx* ctx);
bool ut_txv_frame_is_waiting(const ut_txv_ctx* ctx);
int ut_txv_frame_refcnt(const ut_txv_ctx* ctx);
uint64_t ut_txv_stat_port_build(const ut_txv_ctx* ctx);
uint64_t ut_txv_stat_port_frames(const ut_txv_ctx* ctx);
uint64_t ut_txv_stat_exceed_frame_time(const ut_txv_ctx* ctx);
/* Result of the last ut_txv_update_rtp_time_stamp() call. */
uint32_t ut_txv_rtp_time_stamp(const ut_txv_ctx* ctx);

/* Run the periodic TX video stats dump (tv_sessions_stat) with a capturing log
 * printer. Returns the number of log lines emitted; *locked_lines receives how
 * many of them were emitted while the session spinlock was held. */
int ut_txv_run_sessions_stat(ut_txv_ctx* ctx, int* locked_lines);
/* First log line captured by the last ut_txv_run_sessions_stat() call. */
const char* ut_txv_stat_first_log_line(void);
void ut_txv_set_stat_port_frames(ut_txv_ctx* ctx, uint64_t frames);
uint64_t ut_txv_stat_snapshot_port_frames(const ut_txv_ctx* ctx);

/* tv_pkts_capable_chain() of a one-port frame-level session, with RTCP or not: whether
 * zero copy keeps a frame buffer free with nb_tx_desc descriptors per TX queue, deep at
 * 2048 */
bool ut_txv_pkts_capable_chain(ut_txv_ctx* ctx, uint16_t nb_tx_desc, int total_pkts,
                               uint16_t frames_cnt, bool rtcp);

/* st20_tx_queue_fatal_error() of a session on num_port legs, leg i on physical port i,
 * each on a queue entry of its own; mt_txq_get() swaps it for the leg's other one. The
 * session pools are the port's tx mono pool, so recovery replaces them without DPDK. Its
 * one frame is still referenced by pkts on their way. */
int ut_txv_recovery_setup(ut_txv_ctx* ctx, int num_port);
int ut_txv_recovery_frame_refcnt(const ut_txv_ctx* ctx);
/* the session ended unrecoverable */
bool ut_txv_recovery_session_dead(const ut_txv_ctx* ctx);
/* the ST_EVENT_FATAL_ERROR and ST_EVENT_RECOVERY_ERROR notify_event got */
int ut_txv_recovery_fatal_events(const ut_txv_ctx* ctx);
int ut_txv_recovery_recovered_events(const ut_txv_ctx* ctx);
/* the bytes/s programmed on leg's first queue, 0 for none */
void ut_txv_recovery_set_queue_bps(ut_txv_ctx* ctx, int leg, uint64_t bps);
/* the bytes/s of the last mt_txq_get() */
uint64_t ut_txv_txq_get_bps(const ut_txv_ctx* ctx);
/* what mt_txq_recovery_flush() of a queue of leg returns, 0 unless set */
void ut_txv_recovery_set_flush_ret(ut_txv_ctx* ctx, int leg, int ret);
/* st20_tx_queue_fatal_error() of leg */
int ut_txv_recover(ut_txv_ctx* ctx, int leg);
/* warnings the recoveries logged */
int ut_txv_recovery_warns(const ut_txv_ctx* ctx);
/* mt_txq_recovery_flush() calls on the queues of leg */
int ut_txv_recovery_flushes(const ut_txv_ctx* ctx, int leg);
/* mt_txq_flush() calls on the leg, which may fall back to the per-pad loop */
int ut_txv_recovery_fallback_flushes(const ut_txv_ctx* ctx, int leg);
int ut_txv_recovery_done_cleanups(const ut_txv_ctx* ctx, int leg);

/* ── builder and transmitter over a modeled NIC ring ──────────────────── */
/* A frame-level BPM 4:2:2 10-bit 59.94p zero-copy session on one port whose TX queue is
 * a common/ut_tx_ring.h ring of nb_tx_desc (rs 32, the PMD's free_thresh), draining a
 * descriptor a ns. The production builder (tvs_tasklet_handler()) and TSC transmitter
 * run on the mock clock; the app hands the frames over in turn. */
struct ut_txv_pipe_case {
  uint32_t width;
  uint32_t height;
  uint16_t frames_cnt;
  uint16_t nb_tx_desc;
  int ring_mode; /* enum ut_txr_mode */
  uint16_t free_thresh;
};
int ut_txv_pipe_setup(ut_txv_ctx* ctx, const struct ut_txv_pipe_case* c);
/* stops once the builder met a frame still referenced */
void ut_txv_pipe_run(ut_txv_ctx* ctx, uint64_t duration_ns, uint64_t step_ns);
/* -EBUSY if a pool still had mbufs out once the rings and inflights were freed */
int ut_txv_pipe_teardown(ut_txv_ctx* ctx);
/* the run step the app handed over a frame still referenced, 0 for none */
uint64_t ut_txv_pipe_first_frame_busy(const ut_txv_ctx* ctx);
uint64_t ut_txv_pipe_frames_asked(const ut_txv_ctx* ctx);
int ut_txv_pipe_total_pkts(const ut_txv_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* _ST20_TX_SESSION_HARNESS_H_ */
