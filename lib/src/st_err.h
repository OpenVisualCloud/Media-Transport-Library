/*
 * Copyright (C) 2022 Intel Corporation.
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

#include <rte_log.h>

#ifndef _ST_LIB_ERR_HEAD_H_
#define _ST_LIB_ERR_HEAD_H_

/* internal ret code for debug usage */
enum sti_ret_code {
  STI_SUSS = 0,
  STI_ERROR_1,
  /* rl trs stat */
  STI_RLTRS_BURST_INFILGHT_FAIL = 100,
  STI_RLTRS_BURST_INFILGHT2_FAIL,
  STI_RLTRS_BURST_PAD_INFILGHT_FAIL,
  STI_RLTRS_TARGET_TSC_NOT_REACH,
  STI_RLTRS_DEQUEUE_FAIL,
  STI_RLTRS_BURST_HAS_DUMMY,
  STI_RLTRS_1ST_PKT_TSC,
  /* tsc trs stat */
  STI_TSCTRS_BURST_INFILGHT_FAIL = 140,
  STI_TSCTRS_TARGET_TSC_NOT_REACH,
  STI_TSCTRS_DEQUEUE_FAIL,
  STI_TSCTRS_BURST_HAS_DUMMY,
  /* st20 frame build stat */
  STI_FRAME_RING_FULL = 200,
  STI_FRAME_INFLIGHT_ENQUEUE_FAIL,
  STI_FRAME_INFLIGHT_R_ENQUEUE_FAIL,
  STI_FRAME_APP_GET_FRAME_BUSY,
  STI_FRAME_APP_SLICE_NOT_READY,
  STI_FRAME_PKT_ALLOC_FAIL,
  STI_FRAME_PKT_ENQUEUE_FAIL,
  STI_FRAME_PKT_R_ENQUEUE_FAIL,
  /* st20 rtp build stat */
  STI_RTP_RING_FULL = 240,
  STI_RTP_INFLIGHT_ENQUEUE_FAIL,
  STI_RTP_INFLIGHT_R_ENQUEUE_FAIL,
  STI_RTP_APP_DEQUEUE_FAIL,
  STI_RTP_PKT_ALLOC_FAIL,
  STI_RTP_PKT_ENQUEUE_FAIL,
  STI_RTP_PKT_R_ENQUEUE_FAIL,
  /* st22 frame build stat */
  STI_ST22_RING_FULL = 280,
  STI_ST22_INFLIGHT_ENQUEUE_FAIL,
  STI_ST22_INFLIGHT_R_ENQUEUE_FAIL,
  STI_ST22_APP_GET_FRAME_BUSY,
  STI_ST22_APP_GET_FRAME_ERR_SIZE,
  STI_ST22_APP_SLICE_NOT_READY,
  STI_ST22_PKT_ALLOC_FAIL,
  STI_ST22_PKT_ENQUEUE_FAIL,
  STI_ST22_PKT_R_ENQUEUE_FAIL,
};

#endif
