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
void ut_txa_set_frame_time(ut_txa_ctx* ctx, long double frame_time_ns);
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
/* Queue an app RTP packet as st40_tx_get_mbuf() and st40_tx_put_mbuf() do. */
int ut_txa_put_app_rtp(ut_txa_ctx* ctx, const uint8_t* rtp, uint16_t len);
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
/* The refcnt of frame 0: above 0 while the frame is still in flight. */
void ut_txa_set_frame_refcnt(ut_txa_ctx* ctx, int refcnt);
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
/* `meta_num` ANC meta entries of `udw_size` bytes; entry i starts at data byte i. */
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
/* get_next_frame hands out `tfmt`/`timestamp` from its next call on. */
void ut_txa_set_app_timestamp(ut_txa_ctx* ctx, enum st10_timestamp_fmt tfmt,
                              uint64_t timestamp);
struct st40_tx_ops;
struct st40_frame;
/* Take frames from `src`'s get_next_frame/notify_frame_done/priv, `frame` as frame 0. */
void ut_txa_set_frame_source(ut_txa_ctx* ctx, const struct st40_tx_ops* src,
                             struct st40_frame* frame);

/* Chain mode, with a 32 mbuf payload pool of the room mempool_init() gives it. */
int ut_txa_enable_chain(ut_txa_ctx* ctx);
/* After ut_txa_init_hdr(): port R with its own ring and 32 mbuf header pool. Its header
 * is hdr[P] with another dst MAC, IPv4 and UDP port, so init_hdr(R) is not needed. */
int ut_txa_enable_redundant(ut_txa_ctx* ctx);
enum ut_txa_pool { UT_TXA_POOL_P, UT_TXA_POOL_R, UT_TXA_POOL_CHAIN };
/* Free mbufs of a pool, 32 when every mbuf is back; 0 if the pool does not exist. */
unsigned int ut_txa_pool_count(const ut_txa_ctx* ctx, enum ut_txa_pool pool);
/* Take every free mbuf of a pool; ut_txa_cleanup_frame_tasklet() gives them back. */
void ut_txa_hold_pool(ut_txa_ctx* ctx, enum ut_txa_pool pool);
struct ut_txa_pkt_meta {
  uint64_t tsc;
  uint32_t idx;
  uint16_t nb_segs;
  bool ipv4_init; /* the l2_len, l3_len and ol_flags that mt_mbuf_init_ipv4() sets */
};
/* ut_txa_pop_packet() for the ring of `port`, with the mbuf fields of the packet. */
int ut_txa_pop_port_packet(ut_txa_ctx* ctx, enum mtl_session_port port,
                           struct ut_txa_pkt_meta* meta, uint8_t* out, uint32_t cap);
/* Fill the ring of `port` with ut_pool() mbufs. */
int ut_txa_fill_ring(ut_txa_ctx* ctx, enum mtl_session_port port);
struct st_tx_port_stats ut_txa_port_stats(const ut_txa_ctx* ctx,
                                          enum mtl_session_port port);
uint16_t ut_txa_seq(const ut_txa_ctx* ctx);
/* The eth/ipv4/udp template of `port`: the first 42 bytes of each packet. */
const uint8_t* ut_txa_hdr(const ut_txa_ctx* ctx, enum mtl_session_port port);
/* ops.interlaced, and second_field of the frame in flight. */
void ut_txa_set_interlaced(ut_txa_ctx* ctx, bool interlaced, bool second_field);
void ut_txa_set_max_pkt_len(ut_txa_ctx* ctx, uint32_t len);
void ut_txa_set_cksum_offload(ut_txa_ctx* ctx, enum mtl_session_port port, bool on);
void ut_txa_set_sch_schedule_ns(ut_txa_ctx* ctx, uint32_t ns);
#ifdef MTL_SIMULATE_PACKET_DROPS
/* ops.test as attach takes it: an st40_tx_test_pattern for `frames` frames. */
void ut_txa_set_test_pattern(ut_txa_ctx* ctx, int pattern, uint16_t frames);
#endif

#ifdef __cplusplus
}
#endif

#endif /* _ST40_TX_SESSION_HARNESS_H_ */
