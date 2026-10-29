/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

#ifndef _IPMX_SR_HARNESS_H_
#define _IPMX_SR_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#include "st20_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * An st20 TX session for lib/src/ipmx/ipmx_sr.c and the pacing tasklets of
 * st_video_transmitter.c, compiled here with mt_txq_burst() a mock that logs every
 * packet each session queue takes. Port P only, no IPv4 checksum offload.
 */
typedef struct ut_sr_ctx ut_sr_ctx;

#define UT_SR_PKT_MAX (320)
#define UT_SR_UDP_HDR_LEN (42) /* Ethernet, IPv4, UDP */

enum ut_sr_wire_kind {
  UT_SR_WIRE_PAD,
  UT_SR_WIRE_SR,
  UT_SR_WIRE_PKT,
};

struct ut_sr_wire {
  enum ut_sr_wire_kind kind;
  int port;
  uint32_t pkt_idx; /* UT_SR_WIRE_PKT */
  uint32_t len;
  bool launch;
  uint64_t launch_time;
};

struct ut_sr_counts {
  uint64_t sent;
  uint64_t late;
  uint64_t alloc_fail;
  uint64_t burst_fail;
  uint64_t dropped;
};

int ut_sr_init(void);
ut_sr_ctx* ut_sr_create(const uint8_t sip[4], uint32_t ssrc, uint16_t media_dst_port);
void ut_sr_destroy(ut_sr_ctx* ctx);
/* the session's st20_pkt_info[type], read by ipmx_sr_tx_init() */
void ut_sr_set_pkt_info(ut_sr_ctx* ctx, int type, uint32_t size, uint32_t number);
/* the session's st20_total_pkts, read by ipmx_sr_tx_init() */
void ut_sr_set_frame_pkts(ut_sr_ctx* ctx, uint32_t total_pkts);
/* the session's ops and fps_tm ipmx_sr_tx_init() describes, 1080p59.94 YUV 4:2:2 10-bit
 * BPM by default */
int ut_sr_set_video(ut_sr_ctx* ctx, enum st20_fmt fmt, uint32_t width, uint32_t height,
                    enum st20_packing packing, enum st_fps fps);
/* the port P MAC, 02-00-00-00-00-01 by default */
void ut_sr_set_mac(ut_sr_ctx* ctx, const uint8_t mac[6]);
struct ipmx_video_info;
/* ipmx_video_info_from_session() of the session, 0 or its negative errno */
int ut_sr_video_info(ut_sr_ctx* ctx, struct ipmx_video_info* info);
/* ipmx_sr_tx_set_block_version() of the session */
int ut_sr_set_block_version(ut_sr_ctx* ctx, uint8_t version);
/* ipmx_sr_tx_set_ts_refclk() of the session */
int ut_sr_set_ts_refclk(ut_sr_ctx* ctx, const char* ts_refclk);
/* ipmx_sr_tx_set_ts_refclk() stopped before the write ends, and its end */
void ut_sr_set_ts_refclk_begin(ut_sr_ctx* ctx, const char* ts_refclk);
void ut_sr_set_ts_refclk_end(ut_sr_ctx* ctx);
/* ipmx_sr_tx_get_info() of the session */
int ut_sr_get_info(ut_sr_ctx* ctx, struct ipmx_video_info* info);
/* The built-in PTP state of port P, as mt_ptp.c leaves it */
struct ut_sr_ptp {
  bool ptp_service; /* MTL_FLAG_PTP_ENABLE */
  bool active;
  bool connected;
  uint64_t announce_ns; /* the TSC ns of the master's last Announce, 0 for none */
  int8_t log_announce_interval;
  uint64_t gm_id;
  uint8_t domain;
};
/* port P gets a PTP instance in this state, none for NULL; callable between frames */
void ut_sr_set_ptp(ut_sr_ctx* ctx, const struct ut_sr_ptp* ptp);
/* the stores of one Announce of the master as mt_ptp.c does them, stopped before the
 * write ends, and its end */
void ut_sr_ptp_announce_begin(ut_sr_ctx* ctx, uint64_t gm_id, uint8_t domain,
                              uint64_t announce_ns);
void ut_sr_ptp_announce_end(ut_sr_ctx* ctx);
/* the application's MTL_FLAG_PTP_SOURCE_TSC and its own ptp_get_time_fn */
void ut_sr_set_ptp_source(ut_sr_ctx* ctx, bool tsc, bool user_fn);
/* ipmx_sr_tx_init(), 0 or its negative errno */
int ut_sr_attach(ut_sr_ctx* ctx);
void ut_sr_detach(ut_sr_ctx* ctx);
bool ut_sr_attached(const ut_sr_ctx* ctx);
/* the static ipmx_sr_tx_build() of the attached session */
uint16_t ut_sr_build(ut_sr_ctx* ctx, uint8_t* rtcp, uint64_t tai_ns, uint32_t rtp_ts);
/* What the transmitter does with packet 0 of a frame: ipmx_sr_tx_frame_start() on
 * dequeue, then ipmx_sr_tx_before_burst() and its burst on the s_port queue. */
void ut_sr_frame(ut_sr_ctx* ctx, int s_port, uint64_t ptp_ns, uint32_t rtp_ts, bool late);
/* the two halves of ut_sr_frame() */
void ut_sr_dequeue(ut_sr_ctx* ctx, int s_port, uint64_t ptp_ns, uint32_t rtp_ts,
                   bool late);
/* video_trs_burst() of a packet 0, the packets the queue took */
int ut_sr_burst_pkt0(ut_sr_ctx* ctx, int s_port, uint64_t ptp_ns, uint32_t rtp_ts);
/* video_trs_burst() of none, tx_pkts[0] a packet 0 of the frame, as an all-dummy bulk */
int ut_sr_burst_empty(ut_sr_ctx* ctx, uint64_t ptp_ns, uint32_t rtp_ts);

/* the burst mock refuses every burst whose first packet is of kind */
void ut_sr_set_refuse(ut_sr_ctx* ctx, enum ut_sr_wire_kind kind, bool refuse);
/* the burst mock keeps the SR mbufs, as a NIC queue does until flushed */
void ut_sr_set_hold(ut_sr_ctx* ctx, bool hold);
/* frees the SR mbufs the mock keeps, as the session queue flush does */
void ut_sr_flush(ut_sr_ctx* ctx);
int ut_sr_wire_count(const ut_sr_ctx* ctx);
struct ut_sr_wire ut_sr_wire_at(const ut_sr_ctx* ctx, int i);
/* the last SR the mock took, its length, 0 if none */
uint16_t ut_sr_last_sr(const ut_sr_ctx* ctx, uint8_t pkt[UT_SR_PKT_MAX]);
void ut_sr_counts(const ut_sr_ctx* ctx, struct ut_sr_counts* counts);
unsigned int ut_sr_pool_in_use(const ut_sr_ctx* ctx);
/* the SR mbufs ipmx_sr_tx_init() asks the pool for, 64 TX descriptors */
unsigned int ut_sr_pool_n(ut_sr_ctx* ctx);
/* the SR pool name, empty before attach */
const char* ut_sr_pool_name(const ut_sr_ctx* ctx);

/* Transmitter: bulk, trs and warm-up cap of the session, the clock (TSC = PTP) at now */
void ut_sr_trs_setup(ut_sr_ctx* ctx, unsigned int bulk, long double trs_ns,
                     uint32_t warm_pkts);
void ut_sr_set_now(ut_sr_ctx* ctx, uint64_t now_ns);
/* an RL pad plan of pad_ns per NORMAL pad, a credit_ns bucket, with the trim pads */
int ut_sr_trs_arm(ut_sr_ctx* ctx, long double pad_ns, long double credit_ns);
/* enqueue a frame packet with its TSC and PTP target and RTP timestamp */
int ut_sr_trs_enqueue(ut_sr_ctx* ctx, uint32_t pkt_idx, uint32_t rtp_ts, uint64_t target);
int ut_sr_call_rl(ut_sr_ctx* ctx);
int ut_sr_call_tsc(ut_sr_ctx* ctx);
int ut_sr_call_ptp(ut_sr_ctx* ctx);
int ut_sr_call_launch_time(ut_sr_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif
