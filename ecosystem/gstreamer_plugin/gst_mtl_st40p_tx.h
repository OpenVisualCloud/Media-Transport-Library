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

#include <experimental/st40_pipeline_api.h>

#include "gst_mtl_common.h"
#include <st40_api.h>

G_BEGIN_DECLS

#define GST_TYPE_MTL_ST40P_TX (gst_mtl_st40p_tx_get_type())
G_DECLARE_FINAL_TYPE(Gst_Mtl_St40p_Tx, gst_mtl_st40p_tx, GST, MTL_ST40P_TX, GstBaseSink)

enum gst_st40p_rfc8331_payload_endian {
  ST40_RFC8331_PAYLOAD_ENDIAN_SYSTEM,
  ST40_RFC8331_PAYLOAD_ENDIAN_BIG,
  ST40_RFC8331_PAYLOAD_ENDIAN_LITTLE,
  ST40_RFC8331_PAYLOAD_ENDIAN_MAX
};

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
  gboolean use_pts_for_pacing;
  guint pts_for_pacing_offset;
  gboolean parse_8331_meta_from_gstbuffer;
  guint parse_8331_meta_endianness;
  guint max_combined_udw_size;
};


struct gst_st40_rfc8331_hdr1_le {
  union {
    struct {
      /** the ANC data uses luma (Y) data channel */
      uint32_t c : 1;
      /** line number corresponds to the location (vertical) of the ANC data packet */
      uint32_t line_number : 11;
      /** the location of the ANC data packet in the SDI raster */
      uint32_t horizontal_offset : 12;
      /** whether the data stream number of a multi-stream data mapping */
      uint32_t s : 1;
      /** the source data stream number of the ANC data packet */
      uint32_t stream_num : 7;
    } header;
    uint32_t handle;
  };
};


struct gst_st40_rfc8331_meta {
  struct gst_st40_rfc8331_hdr1_le hdr;
  /** Data Count */
  guint16 data_count;
  /** Secondary Data Identification Word */
  guint16 sdid;
  /** Data Identification Word */
  guint16 did;
  guint16 size;
};

G_END_DECLS

#endif /* __GST_MTL_ST40P_TX_H__ */
