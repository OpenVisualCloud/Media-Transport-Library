/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/**
 * @file ipmx_api.h
 *
 * Interfaces for IPMX (VSF TR-10), only present in a library built with enable_ipmx.
 *
 */

#include "../st_pipeline_api.h"

#ifndef _IPMX_API_HEAD_H_
/** Macro for re-include protect */
#define _IPMX_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * The IPMX Info Block content of an st20 stream, TR-10-1 section 8.7 and TR-10-2 section
 * 10, or the Media Info Block 0x0003 of an st22 stream (TR-10-11 section 12). The strings
 * are fixed size and zero padded as on the wire: a string that fills its field has no
 * terminating NUL, so read them with strnlen() or the field size. From an RX they are
 * what the sender sent.
 */
struct ipmx_video_info {
  /** block version, see ipmx_st20_tx_set_block_version() */
  uint8_t version;
  /** ts-refclk, the SDP ts-refclk attribute value, TR-10-1 section 10.4; can change */
  char ts_refclk[64];
  /** mediaclk, the SDP mediaclk attribute value, TR-10-1 section 10.5 */
  char mediaclk[12];
  /** sampling, as the SDP sampling parameter */
  char sampling[16];
  /** floating point samples */
  bool floating;
  /** bits per sample */
  uint8_t depth;
  /** ST 2110-20 general packing mode, false for block packing mode */
  bool gpm;
  /** interlaced */
  bool interlaced;
  /** segmented frame */
  bool segmented;
  /** pixel aspect ratio width */
  uint8_t par_w;
  /** pixel aspect ratio height */
  uint8_t par_h;
  /** range, as the SDP RANGE parameter */
  char range[12];
  /** colorimetry, as the SDP colorimetry parameter */
  char colorimetry[20];
  /** transfer characteristic system, as the SDP TCS parameter */
  char tcs[16];
  /** active width in pixels */
  uint16_t width;
  /** active height in lines */
  uint16_t height;
  /** frame rate numerator */
  uint32_t rate_num;
  /** frame rate denominator */
  uint32_t rate_den;
  /** measured pixel clock in Hz, width x height x rate for a non-baseband sender */
  uint64_t pixclk;
  /** total width, width for a non-baseband sender */
  uint16_t htotal;
  /** total height, height for a non-baseband sender */
  uint16_t vtotal;
};

/**
 * IPMX sender timing seen by one RX session port, VSF TR-10-1 section 8.1 only (not a
 * TP-1 section 13.5 pass, which also needs the TR-10-9 section 11.2 RTCP and first
 * packet timing), accumulated since the timing parser started. Levels are in packets.
 *
 * A frame is judged, CINST included, only if the port had delivered all of it when the
 * session completed the frame: a burst that makes the RX drop packets shows as
 * unmeasured, not failed, and with ST 2022-7 the later port is usually unmeasured.
 * Meaningful only with NIC RX timestamps; with software ones untimed packets make CINST
 * read low.
 *
 * Known limitation: the VRX drain assumes height / vtotal = 1080 / 1125, so a sender with
 * other blanking, e.g. CVT-RB 1200 / 1235, can read as VRX underflow.
 */
struct ipmx_rx_tp_stat {
  /** CINST limit, MAX(16, INT(NPACKETS / (21600 x TFRAME))) */
  int32_t cmax;
  /** VRX limit, CMAX x 2 */
  int32_t vrx_full;
  /** the max CINST measured, 0 until a frame is measured */
  int32_t cinst_max;
  /** the max VRX level measured, INT32_MIN until a frame is measured */
  int32_t vrx_max;
  /**
   * the min VRX level measured, INT32_MAX until measured. Below 0 is an underflow and
   * fails the frame, per the ST 2110-21 VRX rules TR-10-1 refers to; stricter than the
   * TR-10 TP-1 pass criteria, which check only the max
   */
  int32_t vrx_min;
  /** frames within all the limits */
  uint32_t frames_compliant;
  /** frames outside a limit */
  uint32_t frames_failed;
  /** frames not judged: not all delivered by this port, or no packet timed */
  uint32_t frames_unmeasured;
  /** the cause of the last failed frame, empty if none failed */
  char failed_cause[64];
};

/**
 * Get the IPMX timing result of an st20 RX session created with ST20_RX_FLAG_IPMX and
 * ST20_RX_FLAG_TIMING_PARSER_STAT or ST20_RX_FLAG_TIMING_PARSER_META. The RX tasklet
 * keeps updating it, so a read may mix two frames.
 *
 * @param handle
 *   The handle to the rx st2110-20(video) session.
 * @param port
 *   The session port.
 * @param stat
 *   The pointer to save the result.
 * @return
 *   - 0: Success.
 *   - -EIO: invalid handle, or IPMX or the timing parser not enabled on the session.
 *   - -EBUSY: the timing parser not started yet, it waits for the stream detection.
 *   - -EINVAL: port not used by the session.
 */
int ipmx_st20_rx_get_tp_stat(st20_rx_handle handle, enum mtl_session_port port,
                             struct ipmx_rx_tp_stat* stat);

/**
 * ipmx_st20_rx_get_tp_stat() of the transport session of an st20p RX pipeline created
 * with ST20P_RX_FLAG_IPMX and ST20P_RX_FLAG_TIMING_PARSER_STAT or _META.
 */
int ipmx_st20p_rx_get_tp_stat(st20p_rx_handle handle, enum mtl_session_port port,
                              struct ipmx_rx_tp_stat* stat);

/**
 * The JPEG XS Media Info Block 0x0008 of an st22 stream, TR-10-15 Part 1 section 9: the
 * RFC 9134 packetization and the JPEG XS profile and level of the codestream.
 */
struct ipmx_jxs_info {
  /** RFC 9134 T, true for sequential transmission order */
  bool transmode;
  /** RFC 9134 K, true for slice packetization mode, false for codestream mode */
  bool packetmode;
  /** JPEG XS Ppih, the profile of the codestream (ISO/IEC 21122-2) */
  uint16_t ppih;
  /** JPEG XS Plev, the level, sublevel and fbblevel of the codestream (ISO/IEC
   * 21122-2) */
  uint16_t plev;
};

/** The longest SDES CNAME, RFC 3550 section 6.5 */
#define IPMX_RX_SR_CNAME_MAX (255)

/**
 * The IPMX RTCP Sender Reports (VSF TR-10-1 section 8.7) one RX session port got on
 * udp_port + 1: counters since the session or its last st20_rx_update_source(), and the
 * last Sender Report accepted. The Sender Report fields are raw, the NTP timestamp field
 * is the PTP truncated timestamp of the sender, not converted.
 */
struct ipmx_rx_sr_info {
  /** Sender Reports accepted, with or without an IPMX Info Block */
  uint64_t received;
  /** RTCP packets dropped: not RTCP version 2, or a length, count or padding past the
   * packet */
  uint64_t malformed;
  /** accepted Sender Reports with a defective IPMX Info Block: a block or Media Info
   * Block length past its container, a block shorter than its 84 octet header, a video
   * Media Info Block shorter than 92 octets or, on st22, a 0x0008 one shorter than 12 */
  uint64_t info_malformed;
  /** accepted Sender Reports without an IPMX Info Block (no profile extension tagged
   * 0x5831) */
  uint64_t no_info_block;
  /** Sender Reports dropped as their SSRC is not the one of the media on the port */
  uint64_t ssrc_mismatch;
  /** Media Info Blocks skipped: of a type other than the session's video one (0x0001
   * uncompressed on st20, 0x0003 constant size compressed on st22) or, on st20, the
   * JPEG XS 0x0008 */
  uint64_t unknown_mib;
  /** accepted IPMX Info Blocks with a block version other than the one before */
  uint64_t version_changes;
  /** SSRC of the last Sender Report */
  uint32_t ssrc;
  /** NTP timestamp most significant word, the PTP seconds */
  uint32_t ptp_sec;
  /** NTP timestamp least significant word, the PTP nanoseconds */
  uint32_t ptp_ns;
  /** RTP timestamp of the last Sender Report */
  uint32_t rtp_timestamp;
  /** sender's packet count */
  uint32_t pkt_count;
  /** sender's octet count */
  uint32_t octet_count;
  /** the receiver PTP time in ns of the last Sender Report, see arrival_hw */
  uint64_t arrival_ns;
  /** arrival_ns is the NIC RX timestamp of the packet; false: the PTP time when the RX
   * tasklet read it */
  bool arrival_hw;
  /** the SDES CNAME of the same SSRC in the same RTCP packet, empty if none */
  char cname[IPMX_RX_SR_CNAME_MAX + 1];
  /** the last Sender Report carries an IPMX Info Block with a valid header: info has
   * the block version, ts_refclk and mediaclk */
  bool has_info;
  /** the Info Block holds the session's video Media Info Block (0x0001 on st20, 0x0003
   * on st22, the first one if several): the other info fields are set. A defect counted
   * in info_malformed stops the Media Info Block walk, so false can also mean a broken
   * one */
  bool has_video_mib;
  /** st22 only: the Info Block holds a JPEG XS Media Info Block 0x0008, the first one if
   * several, jxs is set; the same info_malformed rule as has_video_mib */
  bool has_jxs_mib;
  /** the Info Block content; all zero without has_info, the Media Info Block fields
   * zero without has_video_mib */
  struct ipmx_video_info info;
  /** the 0x0008 fields with their reserved bits dropped, all zero without has_jxs_mib */
  struct ipmx_jxs_info jxs;
};

/**
 * Get the IPMX Sender Reports one port of an st20 RX session created with
 * ST20_RX_FLAG_IPMX received. The fields are of one consistent state. A Sender Report
 * is accepted only if its SSRC is the ssrc of the ops when that is set, else the SSRC of
 * the media last seen on the port; before any media every SSRC is accepted. Sender
 * Reports never change the media RX: a stream without them, from a plain ST 2110 sender,
 * is received as without the flag. The library does no timing recovery from them. There
 * is no callback: poll version_changes and read the new Info Block when it grows. They
 * come through the CNI queue with MTL_FLAG_RX_USE_CNI, or when the port has one and CNI
 * runs as a tasklet (MTL_FLAG_CNI_TASKLET, or the PTP service without
 * MTL_FLAG_CNI_THREAD); otherwise each port takes one more RX queue.
 *
 * @param handle
 *   The handle to the rx st2110-20(video) session.
 * @param port
 *   The session port, each ST 2022-7 leg has its own Sender Reports.
 * @param info
 *   The pointer to save the info.
 * @return
 *   - 0: Success.
 *   - -EBUSY: no Sender Report accepted yet, only the counters in info are set.
 *   - -EIO: invalid handle, or IPMX not enabled on the session.
 *   - -EINVAL: port not used by the session, or NULL info.
 *   - -ENOTSUP: the session has ST20_RX_FLAG_DATA_PATH_ONLY, it gets no Sender Report.
 */
int ipmx_st20_rx_get_sr_info(st20_rx_handle handle, enum mtl_session_port port,
                             struct ipmx_rx_sr_info* info);

/**
 * ipmx_st20_rx_get_sr_info() of the transport session of an st20p RX pipeline created
 * with ST20P_RX_FLAG_IPMX.
 */
int ipmx_st20p_rx_get_sr_info(st20p_rx_handle handle, enum mtl_session_port port,
                              struct ipmx_rx_sr_info* info);

/**
 * ipmx_st20_rx_get_sr_info() of an st22 RX session created with ST22_RX_FLAG_IPMX: the
 * video Media Info Block is the constant size compressed video 0x0003 (TR-10-11 section
 * 12), the JPEG XS 0x0008 that follows it goes to jxs. The st22 media RX never depends on
 * the Sender Reports.
 *
 * @param handle
 *   The handle to the rx st2110-22(compressed video) session.
 * @param port
 *   The session port, each ST 2022-7 leg has its own Sender Reports.
 * @param info
 *   The pointer to save the info.
 * @return
 *   - 0: Success.
 *   - -EBUSY: no Sender Report accepted yet, only the counters in info are set.
 *   - -EIO: invalid handle, or IPMX not enabled on the session.
 *   - -EINVAL: port not used by the session, or NULL info.
 *   - -ENOTSUP: the session has ST22_RX_FLAG_DATA_PATH_ONLY, it gets no Sender Report.
 */
int ipmx_st22_rx_get_sr_info(st22_rx_handle handle, enum mtl_session_port port,
                             struct ipmx_rx_sr_info* info);

/**
 * ipmx_st22_rx_get_sr_info() of the transport session of an st22p RX pipeline created
 * with ST22P_RX_FLAG_IPMX.
 */
int ipmx_st22p_rx_get_sr_info(st22p_rx_handle handle, enum mtl_session_port port,
                              struct ipmx_rx_sr_info* info);

/**
 * Set the IPMX Info Block block version of an st20 TX session created with
 * ST20_TX_FLAG_IPMX. Callable at any time, it goes into every Sender Report built after
 * the call; 0 until set. TR-10-1 section 8.7 ties the version to the Info Block content,
 * so change it only with the stream, normally once before the first frame.
 *
 * The library itself adds 1, wrapping from 255 to 0, in the first Sender Report with a
 * new ts-refclk (see ipmx_st20_tx_get_info()), not before the first Sender Report. A set
 * racing that bump can undo it, so set the version before the first frame.
 *
 * The library keeps no state across sessions. When the application re-creates a
 * session with stream parameters that change the Info Block content it sets the version
 * to the one ipmx_st20_tx_get_info() reported for the old session + 1, wrapping from 255
 * to 0, before the get_next_frame callback returns the first frame, so every Sender
 * Report of the new stream carries it.
 *
 * @param handle
 *   The handle to the tx st2110-20(video) session.
 * @param version
 *   The block version.
 * @return
 *   - 0: Success.
 *   - -EIO: invalid handle, or IPMX not enabled on the session.
 */
int ipmx_st20_tx_set_block_version(st20_tx_handle handle, uint8_t version);

/**
 * ipmx_st20_tx_set_block_version() of the transport session of an st20p TX pipeline
 * created with ST20P_TX_FLAG_IPMX; on re-create, set it before the first
 * st20p_tx_put_frame().
 */
int ipmx_st20p_tx_set_block_version(st20p_tx_handle handle, uint8_t version);

/**
 * Override the ts-refclk of an st20 TX session created with ST20_TX_FLAG_IPMX, for an
 * application that supplies the PTP time (ptp_get_time_fn, e.g. linuxptp ptp4l and
 * phc2sys), which the library cannot trace to a grandmaster. Read the grandmaster
 * identity and domain from ptp4l (pmc "GET PARENT_DATA_SET", "GET TIME_STATUS_NP"), set
 * "ptp=IEEE1588-2008:<grandmasterIdentity as XX-XX-XX-XX-XX-XX-XX-XX>:<domain>" and set
 * it again on a change; set NULL or a localmac string while gmPresent is false.
 *
 * Callable at any time from the control plane, it goes into the next Sender Report built
 * and ipmx_st20_tx_get_info() reports it from then. While set, the library does not
 * follow the built-in PTP for the session. A string other than the one on the wire adds
 * 1 to the block version in the same Sender Report, but not before the first Sender
 * Report. Concurrent calls on one session from several threads are not supported.
 *
 * @param handle
 *   The handle to the tx st2110-20(video) session.
 * @param ts_refclk
 *   The value after "a=ts-refclk:": "ptp=" or "localmac=" and more visible ASCII, at
 *   most 63 chars. NULL goes back to the string the library derives.
 * @return
 *   - 0: Success.
 *   - -EIO: invalid handle, or IPMX not enabled on the session.
 *   - -EINVAL: malformed ts_refclk.
 */
int ipmx_st20_tx_set_ts_refclk(st20_tx_handle handle, const char* ts_refclk);

/**
 * ipmx_st20_tx_set_ts_refclk() of the transport session of an st20p TX pipeline created
 * with ST20P_TX_FLAG_IPMX.
 */
int ipmx_st20p_tx_set_ts_refclk(st20p_tx_handle handle, const char* ts_refclk);

/**
 * Get the IPMX Info Block content the Sender Reports of an st20 TX session created with
 * ST20_TX_FLAG_IPMX carry: the current block version and the stream values. All but
 * ts_refclk and the version are fixed at create; the copy has the ts_refclk and the
 * version of one Sender Report state.
 *
 * ts_refclk is "ptp=IEEE1588-2008:<grandmaster EUI-64>:<domain>" while the built-in PTP
 * (MTL_FLAG_PTP_ENABLE, without ptp_get_time_fn or MTL_FLAG_PTP_SOURCE_TSC) of the
 * primary port has a master it exchanged delays with, in domain 0 to 127, whose last
 * Announce is younger than 3 announce intervals, else "localmac=<primary port MAC>", or
 * the string of ipmx_st20_tx_set_ts_refclk(). The transmitter checks it at each frame
 * start, so it changes without an application call. The Info Block follows the P port
 * PTP only, the R port PTP is not read, and both ST 2022-7 legs send it. A change shows
 * on both legs in the same frame unless one leg trails by a whole frame. In the localmac
 * form it is the P port MAC; advertise it in the SDP media sections of both legs. TR-10
 * TP-1 sections 13.2 and 13.3 compare the Sender Report ts-refclk with the SDP: poll this
 * function and republish the SDP of ipmx_st20_tx_get_sdp() whenever the version changes,
 * also after ipmx_st20_tx_set_ts_refclk().
 *
 * @param handle
 *   The handle to the tx st2110-20(video) session.
 * @param info
 *   The pointer to save the info.
 * @return
 *   - 0: Success.
 *   - -EIO: invalid handle, or IPMX not enabled on the session.
 *   - -EINVAL: NULL info.
 */
int ipmx_st20_tx_get_info(st20_tx_handle handle, struct ipmx_video_info* info);

/**
 * ipmx_st20_tx_get_info() of the transport session of an st20p TX pipeline created with
 * ST20P_TX_FLAG_IPMX.
 */
int ipmx_st20p_tx_get_info(st20p_tx_handle handle, struct ipmx_video_info* info);

/**
 * ipmx_st20_tx_set_block_version() of an st22 TX session created with ST22_TX_FLAG_IPMX.
 *
 * @param handle
 *   The handle to the tx st2110-22(compressed video) session.
 * @param version
 *   The block version.
 * @return
 *   - 0: Success.
 *   - -EIO: invalid handle, or IPMX not enabled on the session.
 */
int ipmx_st22_tx_set_block_version(st22_tx_handle handle, uint8_t version);

/**
 * ipmx_st22_tx_set_block_version() of the transport session of an st22p TX pipeline
 * created with ST22P_TX_FLAG_IPMX; on re-create, set it before the first
 * st22p_tx_put_frame().
 */
int ipmx_st22p_tx_set_block_version(st22p_tx_handle handle, uint8_t version);

/**
 * ipmx_st20_tx_set_ts_refclk() of an st22 TX session created with ST22_TX_FLAG_IPMX;
 * ipmx_st22_tx_get_sdp() shows it once a Sender Report carries it.
 *
 * @param handle
 *   The handle to the tx st2110-22(compressed video) session.
 * @param ts_refclk
 *   As ipmx_st20_tx_set_ts_refclk(); NULL goes back to the string the library derives.
 * @return
 *   - 0: Success.
 *   - -EIO: invalid handle, or IPMX not enabled on the session.
 *   - -EINVAL: malformed ts_refclk.
 */
int ipmx_st22_tx_set_ts_refclk(st22_tx_handle handle, const char* ts_refclk);

/**
 * ipmx_st22_tx_set_ts_refclk() of the transport session of an st22p TX pipeline created
 * with ST22P_TX_FLAG_IPMX.
 */
int ipmx_st22p_tx_set_ts_refclk(st22p_tx_handle handle, const char* ts_refclk);

/**
 * ipmx_st20_tx_get_info() of an st22 TX session created with ST22_TX_FLAG_IPMX: the
 * values of the Sender Report Media Info Block 0x0003 (TR-10-11 section 12), RGB 4:4:4
 * 8-bit (TR-10-15 Part 1 section 8, TR-08 section 8.1.1), gpm false. The JPEG XS Media
 * Info Block 0x0008 that follows it carries T 1, K 0, Ppih High444.12 and the Plev of the
 * session's jxpl box (TR-10-15 Part 1 section 9). Republish the SDP of
 * ipmx_st22_tx_get_sdp() whenever the version changes.
 *
 * @param handle
 *   The handle to the tx st2110-22(compressed video) session.
 * @param info
 *   The pointer to save the info.
 * @return
 *   - 0: Success.
 *   - -EIO: invalid handle, or IPMX not enabled on the session.
 *   - -EINVAL: NULL info.
 */
int ipmx_st22_tx_get_info(st22_tx_handle handle, struct ipmx_video_info* info);

/**
 * ipmx_st22_tx_get_info() of the transport session of an st22p TX pipeline created with
 * ST22P_TX_FLAG_IPMX.
 */
int ipmx_st22p_tx_get_info(st22p_tx_handle handle, struct ipmx_video_info* info);

/** A buffer of this size holds any SDP ipmx_st20_tx_get_sdp() or ipmx_st22_tx_get_sdp()
 * builds, NUL included */
#define IPMX_SDP_MAX (2048)

/**
 * Get the IPMX SDP (VSF TR-10-1 section 10, TR-10-2 section 7) of an st20 TX session
 * created with ST20_TX_FLAG_IPMX, for the application to publish; the library writes no
 * file. Lines end with CRLF. The a=fmtp, a=ts-refclk and a=mediaclk values come from the
 * same state as ipmx_st20_tx_get_info() and the Sender Reports, so they cannot disagree;
 * the addresses, UDP port and payload type are the ones on the wire, the source address
 * that of the session port. With ST 2022-7 it is one SDP with a=group:DUP and a media
 * section per leg (a=mid:primary, a=mid:secondary), both with the same ts-refclk.
 *
 * The o= session id is the PTP time in ns at create. The session version starts at 1 and
 * goes up by 1 when a call finds the content changed since the call before: the block
 * version, the ts-refclk (see ipmx_st20_tx_get_info()) or st20_tx_update_destination().
 * There is no callback: poll this function, or the version of ipmx_st20_tx_get_info(),
 * and republish the SDP when the session version changes.
 *
 * @param handle
 *   The handle to the tx st2110-20(video) session.
 * @param buf
 *   The buffer to write the NUL terminated SDP to, IPMX_SDP_MAX is always enough.
 * @param len
 *   The size of buf.
 * @return
 *   - >0: the SDP length, without the NUL.
 *   - -ENOSPC: buf too short, it holds an empty string.
 *   - -EIO: invalid handle, or IPMX not enabled on the session.
 *   - -EINVAL: NULL buf or zero len.
 *   - -ENOTSUP: interlaced, segmented or non-square PAR, which no session takes today;
 *     buf is left untouched.
 */
int ipmx_st20_tx_get_sdp(st20_tx_handle handle, char* buf, size_t len);

/**
 * ipmx_st20_tx_get_sdp() of the transport session of an st20p TX pipeline created with
 * ST20P_TX_FLAG_IPMX.
 */
int ipmx_st20p_tx_get_sdp(st20p_tx_handle handle, char* buf, size_t len);

/**
 * ipmx_st20_tx_get_sdp() of an st22 TX session created with ST22_TX_FLAG_IPMX: the
 * video/jxsv SDP (VSF TR-10-11 section 10, ST 2110-22 section 7, RFC 9134 section 7,
 * TR-10-1 section 10). Each media section has a=rtpmap:<pt> jxsv/90000 and, after c=,
 * b=AS: the maximum IP-layer rate in kbit/s, rounded up, of a frame of the boxes and
 * framebuff_max_size in packets of the session payload size, each with its IPv4, UDP,
 * RTP and RFC 9134 headers; fixed at create. The a=fmtp carries packetmode and
 * transmode (the RFC 9134 K and T bits), profile, level and sublevel (the Ppih and Plev
 * of the Media Info Block 0x0008, see ipmx_st22_tx_get_info()), the Media Info Block
 * 0x0003 values, SSN=ST2110-22:2022, TP=2110TPW (TR-08 section 9: IPMX senders are type
 * W, whatever the pacing), IPMX, measuredpixclk, vtotal and htotal; no PM, fbblevel or
 * interlace. Addressing, ts-refclk, mediaclk, ST 2022-7 and the o= line follow
 * ipmx_st20_tx_get_sdp(), the session version also goes up on
 * st22_tx_update_destination().
 *
 * @param handle
 *   The handle to the tx st2110-22(compressed video) session.
 * @param buf
 *   The buffer to write the NUL terminated SDP to, IPMX_SDP_MAX is always enough.
 * @param len
 *   The size of buf.
 * @return
 *   - >0: the SDP length, without the NUL.
 *   - -ENOSPC: buf too short, it holds an empty string.
 *   - -EIO: invalid handle, or IPMX not enabled on the session.
 *   - -EINVAL: NULL buf or zero len.
 *   - -ENOTSUP: interlaced, segmented or non-square PAR, or a profile, level or
 *     sublevel without an SDP name, which no IPMX session takes today; buf is left
 *     untouched.
 */
int ipmx_st22_tx_get_sdp(st22_tx_handle handle, char* buf, size_t len);

/**
 * ipmx_st22_tx_get_sdp() of the transport session of an st22p TX pipeline created with
 * ST22P_TX_FLAG_IPMX.
 */
int ipmx_st22p_tx_get_sdp(st22p_tx_handle handle, char* buf, size_t len);

#if defined(__cplusplus)
}
#endif

#endif
