/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef _ST40_TX_SESSION_HARNESS_H_
#define _ST40_TX_SESSION_HARNESS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "st_api.h"

#ifdef __cplusplus
extern "C" {
#endif

struct rte_mbuf;
typedef struct ut_txa_ctx ut_txa_ctx;

int ut_txa_init(void);
ut_txa_ctx* ut_txa_create(void);
void ut_txa_destroy(ut_txa_ctx* ctx);
void ut_txa_set_cur_epochs(ut_txa_ctx* ctx, uint64_t cur_epochs);
/* pacing->frame_time and the epoch period of fps_tm, both frame_time_ns */
void ut_txa_set_frame_time(ut_txa_ctx* ctx, uint32_t frame_time_ns);
/* fps_tm and the pacing of fps, as session attach sets them */
int ut_txa_set_fps(ut_txa_ctx* ctx, enum st_fps fps);
void ut_txa_set_user_pacing(ut_txa_ctx* ctx, bool enable);
void ut_txa_set_exact_user_pacing(ut_txa_ctx* ctx, bool enable);
void ut_txa_set_user_timestamp(ut_txa_ctx* ctx, bool enable);
void ut_txa_set_mock_ptp_time(ut_txa_ctx* ctx, uint64_t ptp_ns);
void ut_txa_set_mock_tsc_time(ut_txa_ctx* ctx, uint64_t tsc_ns);
uint64_t ut_txa_calc_epoch(ut_txa_ctx* ctx, uint64_t cur_tai, uint64_t required_tai);
uint64_t ut_txa_pacing_required_tai(ut_txa_ctx* ctx, enum st10_timestamp_fmt tfmt,
                                    uint64_t timestamp);
int ut_txa_sync_pacing(ut_txa_ctx* ctx, uint64_t required_tai);
/* Drives tx_ancillary_update_rtp_time_stamp() directly with the pacing state set
 * up above (ptp_time_cursor, sampling_clock_rate). */
void ut_txa_update_rtp_time_stamp(ut_txa_ctx* ctx, enum st10_timestamp_fmt tfmt,
                                  uint64_t timestamp);
int ut_txa_prepare_frame_tasklet(ut_txa_ctx* ctx, enum st10_timestamp_fmt tfmt,
                                 uint64_t timestamp, unsigned int packets);
/* prepare_frame_tasklet(), then RTP level with an app packet ring, as without chain. */
int ut_txa_prepare_rtp_tasklet(ut_txa_ctx* ctx);
/* Queue an app RTP packet as st40_tx_put_mbuf() does without chain. */
int ut_txa_put_app_rtp(ut_txa_ctx* ctx, const uint8_t* rtp, uint16_t len);
/* After prepare_frame_tasklet(): chained, two ports, R header alloc fails. */
int ut_txa_set_chain_r_dry(ut_txa_ctx* ctx);
unsigned int ut_txa_chain_pool_in_use(const ut_txa_ctx* ctx);
int ut_txa_step_frame_tasklet(ut_txa_ctx* ctx);
unsigned int ut_txa_queued_packets(const ut_txa_ctx* ctx);
/* get_next_frame hands out frame 0 this many times; 1 after create. */
void ut_txa_set_frame_count(ut_txa_ctx* ctx, int frames);
/* Dequeue one queued packet: its scheduled TSC and its first `cap` bytes. Returns the
 * bytes copied, or < 0 if none is queued. */
int ut_txa_pop_packet(ut_txa_ctx* ctx, uint64_t* packet_tsc, uint8_t* out, uint32_t cap);
int ut_txa_pop_packet_tsc(ut_txa_ctx* ctx, uint64_t* packet_tsc);
void ut_txa_cleanup_frame_tasklet(ut_txa_ctx* ctx);
int ut_txa_run_frame_tasklet(ut_txa_ctx* ctx, enum st10_timestamp_fmt tfmt,
                             uint64_t timestamp, uint64_t* packet_tsc);
uint64_t ut_txa_cur_epochs(const ut_txa_ctx* ctx);
uint64_t ut_txa_ptp_time_cursor(const ut_txa_ctx* ctx);
uint64_t ut_txa_tsc_time_cursor(const ut_txa_ctx* ctx);
uint64_t ut_txa_stat_epoch_onward(const ut_txa_ctx* ctx);
uint64_t ut_txa_stat_epoch_drop(const ut_txa_ctx* ctx);
uint64_t ut_txa_stat_error_user_timestamp(const ut_txa_ctx* ctx);
uint64_t ut_txa_stat_epoch_mismatch(const ut_txa_ctx* ctx);
int ut_txa_notify_late_calls(const ut_txa_ctx* ctx);
uint64_t ut_txa_notify_late_last_delta(const ut_txa_ctx* ctx);
int ut_txa_get_next_frame_calls(const ut_txa_ctx* ctx);
int ut_txa_notify_frame_done_calls(const ut_txa_ctx* ctx);
uint16_t ut_txa_notify_frame_done_idx(const ut_txa_ctx* ctx);
uint64_t ut_txa_notify_frame_done_epoch(const ut_txa_ctx* ctx);
uint64_t ut_txa_notify_frame_done_timestamp(const ut_txa_ctx* ctx);
enum st10_timestamp_fmt ut_txa_notify_frame_done_tfmt(const ut_txa_ctx* ctx);
uint32_t ut_txa_notify_frame_done_rtp_timestamp(const ut_txa_ctx* ctx);
bool ut_txa_frame_is_waiting(const ut_txa_ctx* ctx);
int ut_txa_frame_refcnt(const ut_txa_ctx* ctx);
uint64_t ut_txa_stat_port_build(const ut_txa_ctx* ctx);
uint64_t ut_txa_stat_port_packets(const ut_txa_ctx* ctx);
uint64_t ut_txa_stat_port_bytes(const ut_txa_ctx* ctx);
uint32_t ut_txa_packet_len(const ut_txa_ctx* ctx);
uint64_t ut_txa_stat_port_frames(const ut_txa_ctx* ctx);
uint64_t ut_txa_stat_recoverable_error(const ut_txa_ctx* ctx);
uint64_t ut_txa_stat_unrecoverable_error(const ut_txa_ctx* ctx);
/* Result of the last ut_txa_update_rtp_time_stamp() call. */
uint32_t ut_txa_rtp_time_stamp(const ut_txa_ctx* ctx);

/* Enable ST40_TX_FLAG_FAST_METADATA and size max_pkt_len as session attach does. */
void ut_txa_set_fmd(ut_txa_ctx* ctx, uint32_t fmd_dit, uint8_t fmd_k_bit);
/* tx_ancillary_session_init_hdr() for port P; payload_type 0 picks the default. */
int ut_txa_init_hdr(ut_txa_ctx* ctx, uint8_t payload_type);
/* Make `size` bytes of `data` (not copied) the frame in flight. */
void ut_txa_set_frame_payload(ut_txa_ctx* ctx, uint8_t* data, uint32_t size);
/* `meta_num` ANC meta entries of `udw_size` bytes, all at the start of the frame data. */
void ut_txa_set_anc_meta(ut_txa_ctx* ctx, uint16_t udw_size, uint32_t meta_num);
void ut_txa_set_ssrc(ut_txa_ctx* ctx, uint32_t ssrc);
void ut_txa_set_seq(ut_txa_ctx* ctx, uint16_t seq);
/* An mbuf with exactly `room` bytes of tailroom, or NULL if that exceeds the pool. */
struct rte_mbuf* ut_txa_alloc_mbuf(size_t room);
void ut_txa_free_mbuf(struct rte_mbuf* m);
/* tx_ancillary_session_build_packet(): eth/ipv4/udp + RTP (no-chain path). */
int ut_txa_build_packet(ut_txa_ctx* ctx, struct rte_mbuf* pkt);
/* tx_ancillary_session_build_rtp_packet(): RTP only (chain payload path). */
int ut_txa_build_rtp_packet(ut_txa_ctx* ctx, struct rte_mbuf* pkt);
uint8_t* ut_txa_pkt_data(struct rte_mbuf* pkt);
uint32_t ut_txa_pkt_data_len(const struct rte_mbuf* pkt);
uint32_t ut_txa_pkt_pkt_len(const struct rte_mbuf* pkt);
void ut_txa_set_pkt_len(struct rte_mbuf* pkt, uint32_t len);
/* sizeof(struct mt_udp_hdr): eth + ipv4 + udp. */
size_t ut_txa_udp_hdr_len(void);
/* sizeof(struct st_fmd_hdr): eth + ipv4 + udp + FMD RTP header. */
size_t ut_txa_fmd_hdr_len(void);
/* Chain payload mbuf room, as tx_ancillary_session_mempool_init() sizes it. */
size_t ut_txa_chain_room(void);
int ut_txa_stat_build_ret_code(const ut_txa_ctx* ctx);
int ut_txa_err_anc_too_large(void);
/* Live frame state, unlike the post-run snapshot of ut_txa_frame_is_waiting(). */
bool ut_txa_session_waiting_frame(const ut_txa_ctx* ctx);
/* Switch to ST40_TYPE_RTP_LEVEL; `interlaced` arms the RFC 8331 F-bit handling. */
void ut_txa_set_rtp_level(ut_txa_ctx* ctx, bool interlaced);
/* tx_ancillary_session_rtp_update_packet(): app RTP packet behind eth/ipv4/udp. */
int ut_txa_rtp_update_packet(ut_txa_ctx* ctx, struct rte_mbuf* pkt);
/* tx_ancillary_session_build_packet_chain() for port P; `pkt` then owns `rtp`. */
int ut_txa_build_packet_chain(ut_txa_ctx* ctx, struct rte_mbuf* pkt,
                              struct rte_mbuf* rtp);
uint64_t ut_txa_stat_interlace_first(const ut_txa_ctx* ctx);
uint64_t ut_txa_stat_interlace_second(const ut_txa_ctx* ctx);
/* tx_ancillary_ops_check() on otherwise valid single-port frame-level ops. */
int ut_txa_ops_check(uint32_t flags, uint32_t fmd_dit, uint8_t fmd_k_bit,
                     uint8_t payload_type);

/* ── session and transmitter over modeled NIC rings ───────────────────── */
enum { UT_TXA_PIPE_HOLD_MAX = 1024, UT_TXA_PIPE_CHAIN = 2 };
/* A frame-level 59.94 fps session of one pkt a frame on num_port legs, leg i on port i
 * whose TX queue is a common/ut_tx_ring.h ring of nb_tx_desc[i] (rs 32, the PMD's
 * free_thresh), hung or draining a descriptor a ns. The production session tasklet and,
 * when shared, the ancillary transmitter run on the mock clocks. */
struct ut_txa_pipe_case {
  int num_port;
  bool copy;
  bool shared; /* the manager ring and ancillary transmitter, the default */
  uint16_t nb_tx_desc[2];
  int ring_mode; /* enum ut_txr_mode */
  uint16_t free_thresh;
  bool hung[2];
  /* exact header pool of each leg and chain pool; tx_ancillary_session_mempool_init()'s
   * when 0 */
  unsigned int hdr_pool[2];
  unsigned int chain_pool;
  /* mbufs taken out of each exact pool first, as other lcores' caches keep them */
  unsigned int hold;
};
int ut_txa_pipe_setup(ut_txa_ctx* ctx, const struct ut_txa_pipe_case* c);
/* the leg's ring completes no descriptor posted from now on */
void ut_txa_pipe_hang(ut_txa_ctx* ctx, int leg);
/* stops at the first failed pkt alloc or refused burst */
void ut_txa_pipe_run(ut_txa_ctx* ctx, uint64_t duration_ns, uint64_t step_ns);
/* -EBUSY if a pool still had mbufs out once the rings and inflights were freed */
int ut_txa_pipe_teardown(ut_txa_ctx* ctx);
/* the run step a pkt alloc first failed and a NIC ring first refused a pkt; 0 for none */
uint64_t ut_txa_pipe_first_alloc_fail(const ut_txa_ctx* ctx);
uint64_t ut_txa_pipe_first_burst_fail(const ut_txa_ctx* ctx);
/* the mbufs of a leg's header pool, or UT_TXA_PIPE_CHAIN */
unsigned int ut_txa_pipe_pool_size(const ut_txa_ctx* ctx, int pool);
/* tx_ancillary_session_mempool_n(): the mbufs the production pool init asks for the
 * leg's header pool */
unsigned int ut_txa_pipe_mempool_n(ut_txa_ctx* ctx, int leg);

#ifdef __cplusplus
}
#endif

#endif /* _ST40_TX_SESSION_HARNESS_H_ */
