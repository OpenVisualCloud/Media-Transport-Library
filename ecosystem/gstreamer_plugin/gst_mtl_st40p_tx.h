/*
 * GStreamer
 * Copyright (C) 2005 Thomas Vander Stichele <thomas@apestaart.org>
 * Copyright (C) 2005 Ronald S. Bultje <rbultje@ronald.bitfreak.net>
 * Copyright (C) 2020 Niels De Graef <niels.degraef@gmail.com>
 * Copyright (C) 2025 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Alternatively, the contents of this file may be used under the
 * GNU Lesser General Public License Version 2.1 (the "LGPL"), in
 * which case the following provisions apply instead of the ones
 * mentioned above:
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef __GST_MTL_ST40P_TX_H__
#define __GST_MTL_ST40P_TX_H__

#define ST40_RFC8331_PAYLOAD_MAX_ANCILLARY_COUNT 20
/* Maximum size for single User Data Words */
#define MAX_UDW_SIZE 255
#define UDW_WORD_BIT_SIZE 10
/* Maximum buffer size for User Data Words */
#define DEFAULT_MAX_UDW_SIZE (ST40_RFC8331_PAYLOAD_MAX_ANCILLARY_COUNT * MAX_UDW_SIZE)
/* rfc8331 header consist of rows 3 * 10 bits + 2 bits  */
#define RFC_8331_WORD_BYTE_SIZE (4)
#define RFC_8331_PAYLOAD_HEADER_SIZE 8
#define RFC_8331_PAYLOAD_HEADER_LOST_BITS 2

#include <mtl/st40_pipeline_api.h>
#include <st40_api.h>

#include "gst_mtl_common.h"

G_BEGIN_DECLS

typedef enum {
  GST_MTL_ST40P_TX_INPUT_FORMAT_RAW_UDW = 0,
  GST_MTL_ST40P_TX_INPUT_FORMAT_RFC8331_PACKED,
  GST_MTL_ST40P_TX_INPUT_FORMAT_RFC8331_SIMPLIFIED,
} GstMtlSt40pTxInputFormat;

typedef enum {
  GST_MTL_ST40P_TX_TEST_MODE_NONE = 0,
  GST_MTL_ST40P_TX_TEST_MODE_NO_MARKER,
  GST_MTL_ST40P_TX_TEST_MODE_SEQ_GAP,
  GST_MTL_ST40P_TX_TEST_MODE_BAD_PARITY,
  GST_MTL_ST40P_TX_TEST_MODE_PACED,
} GstMtlSt40pTxTestMode;

#define GST_TYPE_MTL_ST40P_TX (gst_mtl_st40p_tx_get_type())
G_DECLARE_FINAL_TYPE(Gst_Mtl_St40p_Tx, gst_mtl_st40p_tx, GST, MTL_ST40P_TX, GstBaseSink)

struct _Gst_Mtl_St40p_Tx {
  GstBaseSink element;
  mtl_handle mtl_lib_handle;
  st40p_tx_handle tx_handle;
  guint frame_size;

  /* arguments */
  guint log_level;
  GeneralArgs generalArgs;  /* imtl initialization arguments */
  SessionPortArgs portArgs; /* imtl session device */
  guint framebuff_cnt;
  guint fps_n, fps_d;
  guint did;
  guint sdid;
  gboolean interlaced;
  gboolean use_pts_for_pacing;
  guint pts_for_pacing_offset;
  gboolean split_anc_by_pkt;
  GstMtlSt40pTxInputFormat input_format;
  guint max_combined_udw_size;
  GstMtlSt40pTxTestMode test_mode;
  guint test_pkt_count;
  guint test_pacing_ns;
};

struct gst_st40_rfc8331_meta {
  struct st40_rfc8331_payload_hdr_common* header_common;
  struct st40_rfc8331_payload_hdr* headers[ST40_RFC8331_PAYLOAD_MAX_ANCILLARY_COUNT];
};

G_END_DECLS

#endif /* __GST_MTL_ST40P_TX_H__ */
