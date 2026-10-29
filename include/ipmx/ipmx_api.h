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
 * The IPMX Info Block content of an st20 TX stream, TR-10-1 section 8.7 and TR-10-2
 * section 10. The strings are zero padded to their field size, not always terminated.
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
 * Set the IPMX Info Block block version of an st20 TX session created with
 * ST20_TX_FLAG_IPMX. Callable at any time, it goes into every Sender Report built after
 * the call; 0 until set. TR-10-1 section 8.7 ties the version to the Info Block content,
 * so change it only with the stream, normally once before the first frame.
 *
 * The library itself adds 1, wrapping from 255 to 0, in the first Sender Report with a
 * new ts-refclk (see ipmx_st20_tx_get_info()), not before the first Sender Report. A set
 * racing that bump can undo it, so set the version before the first frame.
 *
 * The library keeps no state across sessions. When the application
 * re-creates a session with new parameters it sets the version to the one
 * ipmx_st20_tx_get_info() reported for the old session + 1, wrapping from 255 to 0,
 * before the get_next_frame callback returns the first frame, so every Sender Report of
 * the new stream carries it.
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
 * start, so it changes without an application call. TR-10 TP-1 sections 13.2 and 13.3
 * compare the Sender Report ts-refclk with the SDP: poll this function and republish
 * the SDP whenever the version changes, also after ipmx_st20_tx_set_ts_refclk().
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

#if defined(__cplusplus)
}
#endif

#endif
