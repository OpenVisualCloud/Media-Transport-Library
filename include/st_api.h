/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

/**
 * @file st_api.h
 *
 * Interfaces to media streaming(st2110) transport.
 *
 */

#include "mtl_api.h"

#ifndef _ST_API_HEAD_H_
#define _ST_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/** 90k video sampling rate */
#define ST10_VIDEO_SAMPLING_RATE_90K (90 * 1000)
/** 48k audio sampling rate */
#define ST10_AUDIO_SAMPLING_RATE_48K (48 * 1000)
/** 44.1k audio sampling rate */
#define ST10_AUDIO_SAMPLING_RATE_44K (441 * 100)
/** 96k audio sampling rate */
#define ST10_AUDIO_SAMPLING_RATE_96K (96 * 1000)

/**
 * Timestamp type of st2110-10
 */
enum st10_timestamp_fmt {
  /** the media clock time in nanoseconds since the TAI epoch */
  ST10_TIMESTAMP_FMT_TAI = 0,
  /**
   * the raw media clock value defined in ST2110-10, whose units vary by essence
   * sampling rate(90k for video, 48K/96K for audio).
   */
  ST10_TIMESTAMP_FMT_MEDIA_CLK,
  /** max value of this enum */
  ST10_TIMESTAMP_FMT_MAX,
};

/** ST RX timing parser compliant result */
enum st_rx_tp_compliant {
  /** Fail */
  ST_RX_TP_COMPLIANT_FAILED = 0,
  /** Wide */
  ST_RX_TP_COMPLIANT_WIDE,
  /** Narrow */
  ST_RX_TP_COMPLIANT_NARROW,
  /** max value of this enum */
  ST_RX_TP_COMPLIANT_MAX,
};

/**
 * FPS type of media streaming, Frame per second or Field per second
 */
enum st_fps {
  ST_FPS_P59_94 = 0, /**< 59.94 fps */
  ST_FPS_P50,        /**< 50 fps */
  ST_FPS_P29_97,     /**< 29.97 fps */
  ST_FPS_P25,        /**< 25 fps */
  ST_FPS_P119_88,    /**< 119.88 fps */
  ST_FPS_P120,       /**< 120 fps */
  ST_FPS_P100,       /**< 100 fps */
  ST_FPS_P60,        /**< 60 fps */
  ST_FPS_P30,        /**< 30 fps */
  ST_FPS_P24,        /**< 24 fps */
  ST_FPS_P23_98,     /**< 23.98 fps */
  ST_FPS_MAX,        /**< max value of this enum */
};

/**
 * frame status type for TX done/drop and RX receive result.
 */
enum st_frame_status {
  /** All pixels of the frame were received */
  ST_FRAME_STATUS_COMPLETE = 0,
  /**
   * There was some packet loss, but the complete frame was reconstructed using packets
   * from primary and redundant streams
   */
  ST_FRAME_STATUS_RECONSTRUCTED,
  /** Packets were lost */
  ST_FRAME_STATUS_CORRUPTED,
  /** Frame was dropped  */
  ST_FRAME_STATUS_DROPPED,
  /** Max value of this enum */
  ST_FRAME_STATUS_MAX,
};

#ifndef __MTL_PYTHON_BUILD__
/**
 * A structure describing rfc3550 rtp header, size: 12
 */
#ifdef MTL_LITTLE_ENDIAN
MTL_PACK(struct st_rfc3550_rtp_hdr {
  /** CSRC count(CC) */
  uint8_t csrc_count : 4;
  /** extension(X) */
  uint8_t extension : 1;
  /** padding(P) */
  uint8_t padding : 1;
  /** version(V) */
  uint8_t version : 2;
  /** payload type(PT) */
  uint8_t payload_type : 7;
  /** marker(M) */
  uint8_t marker : 1;
  /** sequence number */
  uint16_t seq_number;
  /** timestamp */
  uint32_t tmstamp;
  /** synchronization source */
  uint32_t ssrc;
});

#else
MTL_PACK(struct st_rfc3550_rtp_hdr {
  /** version(V) */
  uint8_t version : 2;
  /** padding(P) */
  uint8_t padding : 1;
  /** extension(X) */
  uint8_t extension : 1;
  /** CSRC count(CC) */
  uint8_t csrc_count : 4;
  /** marker(M) */
  uint8_t marker : 1;
  /** payload type(PT) */
  uint8_t payload_type : 7;
  /** sequence number */
  uint16_t seq_number;
  /** timestamp */
  uint32_t tmstamp;
  /** synchronization source */
  uint32_t ssrc;
});
#endif
#endif

/**
 * The structure describing the destination address(ip addr and port) info for TX.
 * Leave redundant info to zero if the session only has primary port.
 */
struct st_tx_dest_info {
  /** destination IP address of sender */
  uint8_t dip_addr[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN];
  /** UDP port number */
  uint16_t udp_port[MTL_SESSION_PORT_MAX];
};

/**
 * The structure describing the source address(ip addr and port) info for RX.
 * Leave redundant info to zero if the session only has primary port.
 */
struct st_rx_source_info {
  union {
    /** Mandatory. multicast IP address or sender IP for unicast */
    uint8_t ip_addr[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN];
    /** deprecated, use ip_addr instead, sip_addr is confused */
    uint8_t sip_addr[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN] __mtl_deprecated_msg(
        "Use ip_addr instead");
  };
  /** UDP port number */
  uint16_t udp_port[MTL_SESSION_PORT_MAX];
  /** Optional. source filter IP address of multicast */
  uint8_t mcast_sip_addr[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN];
};

/**
 * Pcap dump meta data for synchronous st**_rx_pcapng_dump.
 */
struct st_pcap_dump_meta {
  /** file path for the pcap dump file */
  char file_name[MTL_SESSION_PORT_MAX][MTL_PCAP_FILE_MAX_LEN];
  /** number of packets dumped */
  uint32_t dumped_packets[MTL_SESSION_PORT_MAX];
};

/**
 * The structure describing queue info attached to one session.
 */
struct st_queue_meta {
  /** 1 or 2, num of ports this session attached to */
  uint8_t num_port;
  /** queue id this session attached to */
  uint8_t queue_id[MTL_PORT_MAX];
};

/**
 * Vsync callback meta data
 */
struct st10_vsync_meta {
  /** current vsync epoch */
  uint64_t epoch;
  /** current ptp time */
  uint64_t ptp;
  /** frame time in ns */
  double frame_time;
};

/**
 * Event type on a media session.
 */
enum st_event {
  /**
   * vsync(ptp come to a new epoch) event on each frame, freq is fps,
   * args point to struct st10_vsync_meta.
   */
  ST_EVENT_VSYNC = 0,
  /** the error occurred and session recovery successfully */
  ST_EVENT_RECOVERY_ERROR,
  /** fatal error and session can't recovery, app should free the session then */
  ST_EVENT_FATAL_ERROR,
  /** max value of this enum */
  ST_EVENT_MAX,
};

/**
 * A structure used to retrieve varied info for an media instance.
 */
struct st_var_info {
  /** st20 tx session count */
  uint16_t st20_tx_sessions_cnt;
  /** st22 tx session count */
  uint16_t st22_tx_sessions_cnt;
  /** st30 tx session count */
  uint16_t st30_tx_sessions_cnt;
  /** st40 tx session count */
  uint16_t st40_tx_sessions_cnt;
  /** st41 tx session count */
  uint16_t st41_tx_sessions_cnt;
  /** st20 rx session count */
  uint16_t st20_rx_sessions_cnt;
  /** st22 rx session count */
  uint16_t st22_rx_sessions_cnt;
  /** st30 rx session count */
  uint16_t st30_rx_sessions_cnt;
  /** st40 rx session count */
  uint16_t st40_rx_sessions_cnt;
  /** st41 rx session count */
  uint16_t st41_rx_sessions_cnt;
};

/**
 * A structure used to retrieve general statistics(I/O) for a session tx port.
 */
struct st_tx_port_stats {
  /** Total number of transmitted packets. */
  uint64_t packets;
  /** Total number of transmitted bytes. */
  uint64_t bytes;
  /** Total number of build packets. */
  uint64_t build;
  /** Total number of transmitted frames / memory buffers. */
  uint64_t frames;
};

/**
 * A structure used to retrieve general statistics(I/O) for a session rx port.
 * All counters are pre-redundancy (counted before redundancy filtering).
 */
struct st_rx_port_stats {
  /** Total number of received packets (pre-redundancy). */
  uint64_t packets;
  /** Total number of received bytes (pre-redundancy). */
  uint64_t bytes;
  /**
   * Per-port frame counter. Two flavors, picked by frame size:
   *
   *   - Many packets per frame  (Video ST20 / ST22):
   *       Increments when a frame is **completed by this port**, i.e. this
   *       port delivered enough packets to satisfy completion on its own.
   *       Sister field (video-only): `st20_rx_user_stats::frames_partial[i]`
   *       (this port was short, but the redundant port covered the gap).
   *
   *   - Few packets per frame   (Audio ST30, Ancillary ST40, Metadata ST41):
   *       Increments when a new frame's **first** packet arrives on this
   *       port — i.e. this port "won the race" for that frame.
   *
   * port[i].frames is per-port, not session-wide. The two semantics above
   * do not compose: summing across ports does not yield total frames
   * delivered to the app. For end-to-end frame accounting use the common
   * counters in st_rx_user_stats: stat_frames_received,
   * stat_frames_dropped, stat_frames_corrupted.
   */
  uint64_t frames;
  /** Total number of received packets rejected by handler. */
  uint64_t err_packets;
  /**
   * Per-port count of packets missing on this port (pre-redundancy).
   * Counts actual missing packets, not gap events.
   * For video: sum of pkt_idx gaps within frames.
   * For audio/anc/fmd: sum of RTP sequence number gaps.
   * In a redundant session, these losses are typically recovered from the
   * other port — see stat_pkts_unrecovered for true post-redundancy loss.
   */
  uint64_t lost_packets;
  /**
   * Per-port count of packets that arrived out of order on this port.
   *
   * For audio (ST30), ancillary (ST40), fast-metadata (ST41): incremented
   * when an RTP packet arrives with seq strictly less than the highest seq
   * previously seen on this port (intra-port reorder; same-port duplicates
   * go to duplicates_same_port instead).
   *
   * For video (ST20/ST22): incremented when a packet arrives with frame
   * pkt_idx strictly less than last_pkt_idx on this port (intra-frame
   * reorder).  Cross-frame reorders are not tracked because video has no
   * per-port latest_seq_id.
   *
   * A non-zero value typically points to ECMP, QoS reorder, or LAG hashing
   * upstream of MTL.  The library still accepts the packet if it carries
   * unique data; this counter is purely an observability signal.
   */
  uint64_t reordered_packets;
  /**
   * Per-port count of packets whose seq matched the highest seq already
   * accepted on this port (genuine same-port duplicate, distinct from the
   * normal cross-port redundant copy counted by stat_pkts_redundant).
   *
   * Tracked for audio (ST30), ancillary (ST40), fast-metadata (ST41).
   * Always 0 for video (ST20/ST22): the slot+bitmap completion model
   * cannot distinguish a same-port duplicate from a normal cross-port
   * redundant copy, so both surface as stat_pkts_redundant.
   *
   * A non-zero value strongly suggests a switch loop, cable fault, LAG
   * misconfiguration, or a tcpreplay loop upstream of MTL.
   */
  uint64_t duplicates_same_port;
};

/**
 * A structure used to retrieve general statistics for a tx session.
 * Contains per-port statistics and additional counters for transmission events.
 *
 * Fields are visually grouped by which layer fills them. Callers can
 * ignore the grouping — every field is always present and zero-
 * initialized when the corresponding layer is inactive.
 */
struct st_tx_user_stats {
  /* ------------------------------------------------------------------ */
  /*  Transport layer — filled by the TX session tasklet, all types.    */
  /* ------------------------------------------------------------------ */

  struct st_tx_port_stats port[MTL_SESSION_PORT_MAX]; /**< Per-port TX statistics */
  /** Total number of epoch mismatch events */
  uint64_t stat_epoch_drop;
  /** Total number of onward epoch events */
  uint64_t stat_epoch_onward;
  /** Total number of frames exceeding expected frame time */
  uint64_t stat_exceed_frame_time;
  /** Total number of errors due to user timestamp issues */
  uint64_t stat_error_user_timestamp;
  /** Total number of epoch mismatch events */
  uint64_t stat_epoch_mismatch;
  /** Total number of recoverable transmission errors (session auto-recovered) */
  uint64_t stat_recoverable_error;
  /** Total number of unrecoverable transmission errors (session needed restart) */
  uint64_t stat_unrecoverable_error;

  /* ------------------------------------------------------------------ */
  /*  Pipeline layer — filled by ST20p / ST22p / ST30p / ST40p only.    */
  /*  Always 0 for transport-only TX paths.                             */
  /* ------------------------------------------------------------------ */

  /**
   * Total frames whose final packet was committed to the wire, equivalent
   * to the count of notify_frame_done(status=ST_FRAME_STATUS_COMPLETE)
   * callbacks. Populated by pipeline TX session types
   * (ST20p / ST22p / ST30p / ST40p); 0 for transport-only TX where the
   * caller drives packet emission directly.
   */
  uint64_t stat_frames_sent;
  /**
   * Total frames the TX pipeline dropped because the application handed
   * them over too late for their target wire time. Equivalent to the count
   * of notify_frame_done(status=ST_FRAME_STATUS_DROPPED) callbacks.
   * Distinct from stat_error_user_timestamp, which counts user RTP
   * timestamp validation failures during pacing setup. Populated by
   * pipeline TX session types only.
   */
  uint64_t stat_frames_dropped;
};

/**
 * A structure used to retrieve general statistics for a rx session.
 * Contains per-port statistics and additional counters for reception events.
 *
 * Fields are visually grouped by which layer fills them. Callers can
 * ignore the grouping — every field is always present and zero-
 * initialized when the corresponding layer is inactive.
 */
struct st_rx_user_stats {
  /* ------------------------------------------------------------------ */
  /*  Transport layer — filled by the RX session tasklet, all types.    */
  /* ------------------------------------------------------------------ */

  struct st_rx_port_stats port[MTL_SESSION_PORT_MAX]; /**< Per-port RX statistics */
  /** Total number of accepted packets (post-redundancy). */
  uint64_t stat_pkts_received;
  /**
   * Session-wide total of per-port packet loss (pre-redundancy).
   * Equals sum(port[].lost_packets) across all ports of this session.
   * In a redundant session this counts packets missed on individual ports;
   * losses recovered from the other port are still counted here.
   * For video: sum of pkt_idx gaps within frames.
   * For audio/anc/fmd: sum of RTP sequence number gaps.
   */
  uint64_t stat_lost_packets;
  /**
   * Total packets lost post-redundancy (unrecoverable by redundancy).
   * For audio/anc/fmd: number of missing packets inferred from RTP sequence gaps.
   * For video: number of missing packets in corrupted/incomplete frames.
   * Invariant: stat_pkts_unrecovered <= stat_lost_packets.
   */
  uint64_t stat_pkts_unrecovered;
  /** Total number of packets dropped due to wrong SSRC */
  uint64_t stat_pkts_wrong_ssrc_dropped;
  /** Total number of packets dropped due to wrong payload type */
  uint64_t stat_pkts_wrong_pt_dropped;
  /** Total number of redundant packets filtered (post-redundancy duplicates). */
  uint64_t stat_pkts_redundant;

  /* ------------------------------------------------------------------ */
  /*  Pipeline layer — filled by ST20p / ST22p / ST30p / ST40p only.    */
  /*  Always 0 for transport-only RX paths (e.g. raw RTP, ST41).        */
  /* ------------------------------------------------------------------ */

  /**
   * Total frames delivered to the application via the get-frame /
   * notify-frame-available path, regardless of frame status. This is the
   * canonical session-wide "frames received" counter — independent of
   * port[i].frames, whose semantics differ between video and
   * audio/anc/fmd. Populated by pipeline RX session types
   * (ST20p / ST22p / ST30p / ST40p); 0 for transport-only RX paths that
   * deliver RTP packets rather than frames (e.g. ST41).
   */
  uint64_t stat_frames_received;
  /**
   * Total frames the RX pipeline could not deliver because no free user
   * slot was available (back-pressure / app not draining fast enough).
   * The frame's pre-staged data is discarded; the application never sees
   * a notify-frame-available for it. Populated by pipeline RX session
   * types only.
   */
  uint64_t stat_frames_dropped;
  /**
   * Total frames delivered with status ST_FRAME_STATUS_CORRUPTED, i.e.
   * frames whose constituent packets had unrecoverable gaps after
   * redundancy. The frame is still handed to the application; the
   * application should consult frame->status to decide what to do.
   * Populated by RX session types that classify per-frame integrity
   * (ST20p, ST40p). Always 0 for types with no per-frame corruption
   * concept (audio ST30, ST41).
   */
  uint64_t stat_frames_corrupted;
};

/**
 * Retrieve the varied info of the media transport device context.
 *
 * @param mt
 *   The handle to the media transport device context.
 * @param info
 *   A pointer to info structure.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int st_get_var_info(mtl_handle mt, struct st_var_info* info);

/**
 * Inline function to check the  rx frame is a completed frame.
 * @param status
 *   The input frame status.
 * @return
 *     Complete or not.
 */
static inline bool st_is_frame_complete(enum st_frame_status status) {
  if ((status == ST_FRAME_STATUS_COMPLETE) || (status == ST_FRAME_STATUS_RECONSTRUCTED))
    return true;
  else
    return false;
}

/**
 * Helper function returning accurate frame rate from enum st_fps
 * @param fps
 *   enum st_fps fps.
 * @return
 *   frame rate number
 */
double st_frame_rate(enum st_fps fps);

/**
 * Helper function returning enum st_fps from frame rate
 *
 * @param framerate
 *   frame rate number
 * @return
 *   enum st_fps fps.
 */
enum st_fps st_frame_rate_to_st_fps(double framerate);

/**
 * Helper function returning enum st_fps from frame name
 *
 * @param name
 *   fps name
 * @return
 *   enum st_fps fps.
 */
enum st_fps st_name_to_fps(const char* name);

/**
 * Helper function to convert ST10_TIMESTAMP_FMT_TAI to ST10_TIMESTAMP_FMT_MEDIA_CLK.
 *
 * The conversion quantises the continuous TAI time to a discrete media-clock tick
 * by computing: tick = round(tai_ns * sampling_rate / 1e9).  Because the media
 * clock is discrete, a difference of just 1 ns in the input can change the output
 * by one tick when the value lies exactly on a tick boundary.
 *
 * Callers that derive tai_ns from a frame number and frame period (which is a
 * rational number, e.g. 1001/60000 s for 59.94 fps) must use truncation toward
 * zero — **not** rounding — when converting the rational time to nanoseconds,
 * i.e.  tai_ns = (uint64_t)(frame_number * period_num / period_den).
 * Using roundl() or similar can push the value past a tick boundary, producing
 * repeated or skipped RTP timestamps.
 *
 * @param tai_ns
 *   time in nanoseconds since the TAI epoch.
 * @param sampling_rate
 *   sampling rate(90k for video, 48K/96K for audio).
 * @return
 *   the raw media clock value defined in ST2110-10, whose units vary by sampling_rate
 */
uint32_t st10_tai_to_media_clk(uint64_t tai_ns, uint32_t sampling_rate);

/**
 * Helper function to convert ST10_TIMESTAMP_FMT_MEDIA_CLK to nanoseconds.
 *
 * @param media_ts
 *   the raw media clock value defined in ST2110-10, whose units vary by sampling_rate.
 * @param sampling_rate
 *   sampling rate(90k for video, 48K/96K for audio).
 * @return
 *   time in nanoseconds.
 */
uint64_t st10_media_clk_to_ns(uint32_t media_ts, uint32_t sampling_rate);

/**
 * Helper function to get tai for both ST10_TIMESTAMP_FMT_TAI and
 * ST10_TIMESTAMP_FMT_MEDIA_CLK.
 *
 * @param tfmt
 *   timestamp fmt.
 * @param timestamp
 *   timestamp value with tfmt.
 * @param sampling_rate
 *   sampling rate(90k for video, 48K/96K for audio).
 * @return
 *   time in nanoseconds since the TAI epoch.
 */
static inline uint64_t st10_get_tai(enum st10_timestamp_fmt tfmt, uint64_t timestamp,
                                    uint32_t sampling_rate) {
  if (tfmt == ST10_TIMESTAMP_FMT_TAI) return timestamp;
  return st10_media_clk_to_ns((uint32_t)timestamp, sampling_rate);
}

/**
 * Helper function to get media clock value defined in ST2110-10 for both
 * ST10_TIMESTAMP_FMT_TAI and ST10_TIMESTAMP_FMT_MEDIA_CLK.
 *
 * @param tfmt
 *   timestamp fmt.
 * @param timestamp
 *   timestamp value with tfmt.
 * @param sampling_rate
 *   sampling rate(90k for video, 48K/96K for audio).
 * @return
 *   the raw media clock value defined in ST2110-10, whose units vary by sampling_rate.
 */
static inline uint32_t st10_get_media_clk(enum st10_timestamp_fmt tfmt,
                                          uint64_t timestamp, uint32_t sampling_rate) {
  if (tfmt == ST10_TIMESTAMP_FMT_MEDIA_CLK) return (uint32_t)timestamp;
  return st10_tai_to_media_clk(timestamp, sampling_rate);
}

/**
 * Helper function to get tx queues cnt for st sessions.
 *
 * @param st20_sessions
 * st20 tx sessions count.
 * @param st30_sessions
 * st30 tx sessions count.
 * @param st40_sessions
 * st40 sessions count.
 * @param st41_sessions
 * st41 sessions count.
 * @return
 *   queues count.
 */
static inline uint16_t st_tx_sessions_queue_cnt(uint16_t st20_sessions,
                                                uint16_t st30_sessions,
                                                uint16_t st40_sessions,
                                                uint16_t st41_sessions) {
  uint16_t queues = st20_sessions;
  if (st30_sessions) queues++;
  if (st40_sessions) queues++;
  if (st41_sessions) queues++;
  return queues;
}

/**
 * Helper function to get tx queues cnt for st sessions.
 *
 * @param st20_sessions
 * st20 tx sessions count.
 * @param st30_sessions
 * st30 tx sessions count.
 * @param st40_sessions
 * st40 sessions count.
 * @param st41_sessions
 * st41 sessions count.
 * @return
 *   queues count.
 */
static inline uint16_t st_rx_sessions_queue_cnt(uint16_t st20_sessions,
                                                uint16_t st30_sessions,
                                                uint16_t st40_sessions,
                                                uint16_t st41_sessions) {
  return st20_sessions + st30_sessions + st40_sessions + st41_sessions;
}

#if defined(__cplusplus)
}
#endif

#endif
