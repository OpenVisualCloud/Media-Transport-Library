/*
 * GStreamer
 * Copyright (C) 2005 Thomas Vander Stichele <thomas@apestaart.org>
 * Copyright (C) 2005 Ronald S. Bultje <rbultje@ronald.bitfreak.net>
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

/**
 * SECTION:element-mtl_st40p_rx
 *
 * The mtl_st40p_rx element is a GStreamer src plugin designed to interface with
 * the Media Transport Library (MTL) using the pipeline API.
 * MTL is a software-based solution optimized for high-throughput, low-latency
 * transmission and reception of media data.
 *
 * It features an efficient user-space LibOS UDP stack specifically crafted for
 * media transport and includes a built-in SMPTE ST 2110-compliant
 * implementation for Professional Media over Managed IP Networks.
 *
 * This element allows GStreamer pipelines to receive ST2110-40 ancillary data
 * using the MTL pipeline API, ensuring efficient and reliable media transport
 * over IP networks.
 *
 */

#define _GNU_SOURCE

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gst/gstinfo.h>
#include <inttypes.h>
#include <string.h>

#include "gst_mtl_st40p_rx.h"

typedef enum {
  GST_MTL_ST40P_RX_OUTPUT_FORMAT_RFC8331 = 0,
  GST_MTL_ST40P_RX_OUTPUT_FORMAT_RAW_UDW,
} GstMtlSt40pRxOutputFormat;

static GType gst_mtl_st40p_rx_output_format_get_type(void) {
  static gsize g_define_type_id__ = 0;
  if (g_once_init_enter(&g_define_type_id__)) {
    static const GEnumValue values[] = {
        {GST_MTL_ST40P_RX_OUTPUT_FORMAT_RFC8331, "RFC8331", "rfc8331"},
        {GST_MTL_ST40P_RX_OUTPUT_FORMAT_RAW_UDW, "RawUDW", "raw-udw"},
        {0, NULL, NULL}};
    GType g_define_type_id = g_enum_register_static("GstMtlSt40pRxOutputFormat", values);
    g_once_init_leave(&g_define_type_id__, g_define_type_id);
  }
  return g_define_type_id__;
}
#define GST_TYPE_MTL_ST40P_RX_OUTPUT_FORMAT (gst_mtl_st40p_rx_output_format_get_type())

GST_DEBUG_CATEGORY_STATIC(gst_mtl_st40p_rx_debug);
#define GST_CAT_DEFAULT gst_mtl_st40p_rx_debug

#ifndef GST_LICENSE
#define GST_LICENSE "LGPL"
#endif

#ifndef GST_API_VERSION
#define GST_API_VERSION "1.0"
#endif

#ifndef GST_PACKAGE_NAME
#define GST_PACKAGE_NAME "Media Transport Library st2110 st40p rx plugin"
#endif

#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://github.com/OpenVisualCloud/Media-Transport-Library"
#endif

#ifndef PACKAGE
#define PACKAGE "gst-mtl-st40p-rx"
#endif

#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "1.0"
#endif

#define DEFAULT_MAX_UDW_SIZE (128 * 1024) /* 128KB default */
#define DEFAULT_RTP_RING_SIZE 1024        /* power-of-two default */

enum {
  PROP_ST40P_RX_FRAMEBUFF_CNT = PROP_GENERAL_MAX,
  PROP_ST40P_RX_MAX_UDW_SIZE,
  PROP_ST40P_RX_RTP_RING_SIZE,
  PROP_ST40P_RX_TIMEOUT,
  PROP_ST40P_RX_INTERLACED,
  PROP_ST40P_RX_OUTPUT_FORMAT,
  PROP_ST40P_RX_FRAME_INFO_PATH,
  PROP_MAX
};

/* pad template */
static GstStaticPadTemplate gst_mtl_st40p_rx_src_pad_template =
    GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);

#define gst_mtl_st40p_rx_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE(Gst_Mtl_St40p_Rx, gst_mtl_st40p_rx, GST_TYPE_BASE_SRC,
                        GST_DEBUG_CATEGORY_INIT(gst_mtl_st40p_rx_debug, "mtl_st40p_rx", 0,
                                                "MTL St2110 st40p pipeline rx src"));

GST_ELEMENT_REGISTER_DEFINE(mtl_st40p_rx, "mtl_st40p_rx", GST_RANK_NONE,
                            GST_TYPE_MTL_ST40P_RX);

static void gst_mtl_st40p_rx_set_property(GObject* object, guint prop_id,
                                          const GValue* value, GParamSpec* pspec);
static void gst_mtl_st40p_rx_get_property(GObject* object, guint prop_id, GValue* value,
                                          GParamSpec* pspec);
static void gst_mtl_st40p_rx_finalize(GObject* object);

static gboolean gst_mtl_st40p_rx_start(GstBaseSrc* basesrc);
static GstFlowReturn gst_mtl_st40p_rx_create(GstBaseSrc* basesrc, guint64 offset,
                                             guint length, GstBuffer** buffer);

static gboolean gst_mtl_st40p_rx_serialize_meta_blocks(Gst_Mtl_St40p_Rx* src,
                                                       struct st40_frame_info* frame_info,
                                                       uint8_t** out_data,
                                                       size_t* out_size) {
  if (!frame_info->meta_num) return TRUE;
  if (!frame_info->meta) {
    GST_ERROR_OBJECT(src, "RFC8331 serialization requested but meta array is NULL");
    return FALSE;
  }

  if (!frame_info->udw_buff_addr && frame_info->udw_buffer_fill) {
    GST_ERROR_OBJECT(src,
                     "RFC8331 serialization requested but UDW buffer is NULL (fill=%u)",
                     frame_info->udw_buffer_fill);
    return FALSE;
  }

  /* Each ANC packet contributes an 8-byte header plus its UDW payload. */
  size_t header_bytes = (size_t)frame_info->meta_num * 8;
  size_t estimate = header_bytes + frame_info->udw_buffer_fill;
  GByteArray* serialized = g_byte_array_sized_new(estimate);

  for (uint32_t idx = 0; idx < frame_info->meta_num; idx++) {
    const struct st40_meta* meta = &frame_info->meta[idx];
    uint16_t udw_size = meta->udw_size;
    uint32_t udw_offset = meta->udw_offset;

    if ((uint32_t)udw_size + udw_offset > frame_info->udw_buffer_fill) {
      GST_ERROR_OBJECT(src,
                       "ANC packet %u exceeds UDW buffer (offset=%u size=%u fill=%u)",
                       idx, udw_offset, udw_size, frame_info->udw_buffer_fill);
      g_byte_array_free(serialized, TRUE);
      return FALSE;
    }

    if (udw_size > UINT8_MAX) {
      GST_ERROR_OBJECT(src, "ANC packet %u exceeds supported UDW size (%u > 255)", idx,
                       udw_size);
      g_byte_array_free(serialized, TRUE);
      return FALSE;
    }

    uint8_t header[8];
    header[0] = (uint8_t)((meta->line_number >> 8) & 0xFF);
    header[1] = (uint8_t)(meta->line_number & 0xFF);
    header[2] = (uint8_t)((meta->hori_offset >> 8) & 0xFF);
    header[3] = (uint8_t)(meta->hori_offset & 0xFF);
    header[4] = (uint8_t)(((meta->c & 0x1) << 7) | ((meta->s & 0x1) << 6) |
                          (meta->stream_num & 0x3F));
    header[5] = (uint8_t)(meta->did & 0xFF);
    header[6] = (uint8_t)(meta->sdid & 0xFF);
    header[7] = (uint8_t)(udw_size & 0xFF);

    g_byte_array_append(serialized, header, sizeof(header));

    if (udw_size > 0) {
      g_byte_array_append(serialized, frame_info->udw_buff_addr + udw_offset, udw_size);
    }
  }

  if (!serialized->len) {
    g_byte_array_free(serialized, TRUE);
    return TRUE;
  }

  size_t size = serialized->len;
  uint8_t* data = g_byte_array_free(serialized, FALSE);

  *out_data = data;
  *out_size = size;
  GST_DEBUG_OBJECT(src, "Serialized RFC8331 frame to %zu bytes (meta=%u)", size,
                   frame_info->meta_num);
  return TRUE;
}

static gboolean gst_mtl_st40p_rx_serialize_frame(Gst_Mtl_St40p_Rx* src,
                                                 struct st40_frame_info* frame_info,
                                                 uint8_t** out_data, size_t* out_size) {
  g_return_val_if_fail(src != NULL, FALSE);
  g_return_val_if_fail(frame_info != NULL, FALSE);
  g_return_val_if_fail(out_data != NULL, FALSE);
  g_return_val_if_fail(out_size != NULL, FALSE);

  *out_data = NULL;
  *out_size = 0;

  switch ((GstMtlSt40pRxOutputFormat)src->output_format) {
    case GST_MTL_ST40P_RX_OUTPUT_FORMAT_RFC8331:
      return gst_mtl_st40p_rx_serialize_meta_blocks(src, frame_info, out_data, out_size);
    case GST_MTL_ST40P_RX_OUTPUT_FORMAT_RAW_UDW: {
      uint32_t raw_size = frame_info->udw_buffer_fill;
      if (raw_size == 0) {
        /* Nothing to serialize, still considered success */
        return TRUE;
      }

      if (!frame_info->udw_buff_addr) {
        GST_ERROR_OBJECT(
            src, "RAW serialization requested but udw buffer address is NULL (fill=%u)",
            raw_size);
        return FALSE;
      }

      uint8_t* raw_copy = g_malloc(raw_size);
      if (!raw_copy) {
        GST_ERROR_OBJECT(src, "Failed to allocate %u bytes for RAW ANC serialization",
                         raw_size);
        return FALSE;
      }
      memcpy(raw_copy, frame_info->udw_buff_addr, raw_size);
      *out_data = raw_copy;
      *out_size = raw_size;
      GST_DEBUG_OBJECT(src, "Serialized RAW UDW frame size=%u bytes", raw_size);
      return TRUE;
    }
    default:
      GST_ERROR_OBJECT(src, "Unknown output format %d", src->output_format);
      return FALSE;
  }
}

static gboolean gst_mtl_st40p_is_power_of_two(uint32_t value) {
  return value && ((value & (value - 1)) == 0);
}

static void gst_mtl_st40p_rx_log_frame_info(Gst_Mtl_St40p_Rx* src,
                                            struct st40_frame_info* frame_info) {
  if (!src->frame_info_fp || !frame_info) return;

  /* Log per-frame sequencing details for the validator */
  fprintf(src->frame_info_fp,
          "ts=%" PRIu64
          " meta=%u rtp_marker=%u seq_discont=%u seq_lost=%u pkts_total=%u "
          "pkts_recv_p=%u pkts_recv_r=%u\n",
          frame_info->timestamp, frame_info->meta_num, frame_info->rtp_marker,
          frame_info->seq_discont, frame_info->seq_lost, frame_info->pkts_total,
          frame_info->pkts_recv[MTL_SESSION_PORT_P],
          frame_info->pkts_recv[MTL_SESSION_PORT_R]);
  fflush(src->frame_info_fp);
}

static void gst_mtl_st40p_rx_class_init(Gst_Mtl_St40p_RxClass* klass) {
  GObjectClass* gobject_class;
  GstElementClass* gstelement_class;
  GstBaseSrcClass* gstbasesrc_class;

  gobject_class = (GObjectClass*)klass;
  gstelement_class = (GstElementClass*)klass;
  gstbasesrc_class = (GstBaseSrcClass*)klass;

  gobject_class->set_property = gst_mtl_st40p_rx_set_property;
  gobject_class->get_property = gst_mtl_st40p_rx_get_property;
  gobject_class->finalize = gst_mtl_st40p_rx_finalize;

  gstbasesrc_class->start = GST_DEBUG_FUNCPTR(gst_mtl_st40p_rx_start);
  gstbasesrc_class->create = GST_DEBUG_FUNCPTR(gst_mtl_st40p_rx_create);

  gst_mtl_common_init_general_arguments(gobject_class);

  g_object_class_install_property(
      gobject_class, PROP_ST40P_RX_FRAMEBUFF_CNT,
      g_param_spec_uint("rx-framebuff-cnt", "RX Frame Buffer Count",
                        "Number of frame buffers for RX pipeline", 0, G_MAXUINT,
                        GST_MTL_DEFAULT_FRAMEBUFF_CNT,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST40P_RX_MAX_UDW_SIZE,
      g_param_spec_uint("max-udw-size", "Max UDW Size",
                        "Maximum User Data Word buffer size in bytes", 1024, 1024 * 1024,
                        DEFAULT_MAX_UDW_SIZE,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST40P_RX_RTP_RING_SIZE,
      g_param_spec_uint("rtp-ring-size", "RTP Ring Size",
                        "RTP ring queue size (power of 2) used for packet buffering", 64,
                        16384, DEFAULT_RTP_RING_SIZE,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST40P_RX_TIMEOUT,
      g_param_spec_uint("timeout", "Timeout", "Timeout for receiving frames in seconds",
                        0, 300, 60, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST40P_RX_INTERLACED,
      g_param_spec_boolean("rx-interlaced", "Interlaced",
                           "Set to true if ancillary stream is interlaced", FALSE,
                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST40P_RX_OUTPUT_FORMAT,
      g_param_spec_enum("output-format", "Output Format",
                        "Serialization format for received ANC frames",
                        GST_TYPE_MTL_ST40P_RX_OUTPUT_FORMAT,
                        GST_MTL_ST40P_RX_OUTPUT_FORMAT_RAW_UDW,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST40P_RX_FRAME_INFO_PATH,
      g_param_spec_string("frame-info-path", "Frame info log file",
                          "Optional path to append frame info/seq stats per frame", NULL,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_set_static_metadata(
      gstelement_class, "MTL ST2110-40 Pipeline RX Source", "Source/Network",
      "Receive ST2110-40 ancillary data streams using MTL pipeline API",
      "Intel Corporation");

  gst_element_class_add_static_pad_template(gstelement_class,
                                            &gst_mtl_st40p_rx_src_pad_template);
}

static void gst_mtl_st40p_rx_init(Gst_Mtl_St40p_Rx* src) {
  src->rx_framebuff_cnt = GST_MTL_DEFAULT_FRAMEBUFF_CNT;
  src->max_udw_size = DEFAULT_MAX_UDW_SIZE;
  src->rtp_ring_size = DEFAULT_RTP_RING_SIZE;
  src->timeout_s = 60;
  src->interlaced = FALSE;
  src->output_format = GST_MTL_ST40P_RX_OUTPUT_FORMAT_RAW_UDW;
  src->frame_info_path = NULL;
  src->frame_info_fp = NULL;
  /* init stats */
  src->stats_total_frames = 0;
  src->stats_frames_with_meta = 0;
  src->stats_frames_with_meta2 = 0;
  src->stats_total_headers_written = 0;
  src->stats_meta2_headers_written = 0;
}

static void gst_mtl_st40p_rx_set_property(GObject* object, guint prop_id,
                                          const GValue* value, GParamSpec* pspec) {
  Gst_Mtl_St40p_Rx* src = GST_MTL_ST40P_RX(object);

  /* Only delegate general/common properties (IDs < PROP_GENERAL_MAX) */
  if (prop_id < PROP_GENERAL_MAX) {
    gst_mtl_common_set_general_arguments(object, prop_id, value, pspec, &src->generalArgs,
                                         &src->portArgs);
    return;
  }

  switch (prop_id) {
    case PROP_ST40P_RX_FRAMEBUFF_CNT:
      src->rx_framebuff_cnt = g_value_get_uint(value);
      break;
    case PROP_ST40P_RX_MAX_UDW_SIZE:
      src->max_udw_size = g_value_get_uint(value);
      break;
    case PROP_ST40P_RX_RTP_RING_SIZE:
      src->rtp_ring_size = g_value_get_uint(value);
      break;
    case PROP_ST40P_RX_TIMEOUT:
      src->timeout_s = g_value_get_uint(value);
      break;
    case PROP_ST40P_RX_INTERLACED:
      src->interlaced = g_value_get_boolean(value);
      break;
    case PROP_ST40P_RX_OUTPUT_FORMAT:
      src->output_format = g_value_get_enum(value);
      break;
    case PROP_ST40P_RX_FRAME_INFO_PATH:
      g_free(src->frame_info_path);
      src->frame_info_path = g_value_dup_string(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void gst_mtl_st40p_rx_get_property(GObject* object, guint prop_id, GValue* value,
                                          GParamSpec* pspec) {
  Gst_Mtl_St40p_Rx* src = GST_MTL_ST40P_RX(object);

  /* Only delegate general/common properties (IDs < PROP_GENERAL_MAX) */
  if (prop_id < PROP_GENERAL_MAX) {
    gst_mtl_common_get_general_arguments(object, prop_id, value, pspec, &src->generalArgs,
                                         &src->portArgs);
    return;
  }

  switch (prop_id) {
    case PROP_ST40P_RX_FRAMEBUFF_CNT:
      g_value_set_uint(value, src->rx_framebuff_cnt);
      break;
    case PROP_ST40P_RX_MAX_UDW_SIZE:
      g_value_set_uint(value, src->max_udw_size);
      break;
    case PROP_ST40P_RX_RTP_RING_SIZE:
      g_value_set_uint(value, src->rtp_ring_size);
      break;
    case PROP_ST40P_RX_TIMEOUT:
      g_value_set_uint(value, src->timeout_s);
      break;
    case PROP_ST40P_RX_INTERLACED:
      g_value_set_boolean(value, src->interlaced);
      break;
    case PROP_ST40P_RX_OUTPUT_FORMAT:
      g_value_set_enum(value, src->output_format);
      break;
    case PROP_ST40P_RX_FRAME_INFO_PATH:
      g_value_set_string(value, src->frame_info_path);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static gboolean gst_mtl_st40p_rx_start(GstBaseSrc* basesrc) {
  struct st40p_rx_ops ops_rx = {0};
  Gst_Mtl_St40p_Rx* src = GST_MTL_ST40P_RX(basesrc);

  GST_DEBUG_OBJECT(src, "RX START: Beginning MTL ST40P RX initialization");

  src->mtl_lib_handle = gst_mtl_common_init_handle(&(src->generalArgs), FALSE);

  if (!src->mtl_lib_handle) {
    GST_ERROR("RX START: Could not initialize MTL");
    return FALSE;
  }

  ops_rx.name = "st40p_rx";
  ops_rx.framebuff_cnt =
      src->rx_framebuff_cnt ? src->rx_framebuff_cnt : GST_MTL_DEFAULT_FRAMEBUFF_CNT;
  ops_rx.max_udw_buff_size = src->max_udw_size;
  ops_rx.flags = 0; /* Use non-blocking mode - blocking causes preroll timeout */

  GST_DEBUG_OBJECT(src, "RX START: framebuff_cnt=%d, max_udw_buff_size=%d",
                   ops_rx.framebuff_cnt, ops_rx.max_udw_buff_size);

  uint32_t ring_sz = src->rtp_ring_size ? src->rtp_ring_size : DEFAULT_RTP_RING_SIZE;
  /* ST40 pipeline requires ring size to be 2^n; fail fast on invalid input. */
  if (!gst_mtl_st40p_is_power_of_two(ring_sz)) {
    GST_ERROR_OBJECT(src, "RX START: rtp-ring-size %u must be a power of two", ring_sz);
    return FALSE;
  }
  ops_rx.rtp_ring_size = ring_sz;

  GST_DEBUG_OBJECT(src, "RX START: rtp_ring_size=%d", ops_rx.rtp_ring_size);

  ops_rx.interlaced = src->interlaced;

  /* Optional frame info logging */
  if (src->frame_info_path && !src->frame_info_fp) {
    src->frame_info_fp = fopen(src->frame_info_path, "a");
    if (!src->frame_info_fp) {
      GST_WARNING_OBJECT(src, "RX START: failed to open frame info log %s",
                         src->frame_info_path);
    } else {
      GST_INFO_OBJECT(src, "RX START: writing frame info to %s", src->frame_info_path);
    }
  }

  if (src->portArgs.payload_type == 0) {
    ops_rx.port.payload_type = 113;  // Default ST2110-40 payload type
  } else if ((src->portArgs.payload_type < 0) || (src->portArgs.payload_type > 0x7F)) {
    GST_ERROR("RX START: Invalid payload_type: %d", src->portArgs.payload_type);
    return FALSE;
  } else {
    ops_rx.port.payload_type = src->portArgs.payload_type;
  }

  GST_DEBUG_OBJECT(src, "RX START: payload_type=%d", ops_rx.port.payload_type);

  gst_mtl_common_copy_general_to_session_args(&(src->generalArgs), &(src->portArgs));

  ops_rx.port.num_port =
      gst_mtl_common_parse_rx_port_arguments(&ops_rx.port, &src->portArgs);
  if (!ops_rx.port.num_port) {
    GST_ERROR("Failed to parse port arguments");
    return FALSE;
  }

  GST_DEBUG_OBJECT(src, "RX START: Parsed %d ports, calling st40p_rx_create",
                   ops_rx.port.num_port);

  src->rx_handle = st40p_rx_create(src->mtl_lib_handle, &ops_rx);
  if (!src->rx_handle) {
    GST_ERROR("Failed to create st40p rx handle");
    return FALSE;
  }

  /* Set block timeout */
  if (src->timeout_s > 0) {
    st40p_rx_set_block_timeout(src->rx_handle, (uint64_t)src->timeout_s * NS_PER_S);
  }

  return TRUE;
}

static GstFlowReturn gst_mtl_st40p_rx_create(GstBaseSrc* basesrc, guint64 offset,
                                             guint length, GstBuffer** buffer) {
  Gst_Mtl_St40p_Rx* src = GST_MTL_ST40P_RX(basesrc);
  struct st40_frame_info* frame_info = NULL;
  GstMapInfo dest_info;
  GstFlowReturn ret = GST_FLOW_OK;
  guint32 max_retries = src->timeout_s * 1000; /* timeout_s converted to ms attempts */
  guint32 retry = 0;

  GST_DEBUG_OBJECT(src, "RX CREATE: Starting frame retrieval (timeout=%us)",
                   src->timeout_s);

  /* Non-blocking get - poll until frame arrives or timeout */
  while (retry < max_retries) {
    frame_info = st40p_rx_get_frame(src->rx_handle);
    if (frame_info) {
      GST_DEBUG_OBJECT(src, "RX CREATE: Got frame with meta_num=%u, udw_fill=%u",
                       frame_info->meta_num, frame_info->udw_buffer_fill);
      break;
    }

    /* Check if element is shutting down */
    GstState cur_state = GST_STATE(basesrc);
    if (cur_state == GST_STATE_NULL || cur_state == GST_STATE_READY) {
      GST_DEBUG_OBJECT(src, "RX CREATE: Element stopping (state=%s)",
                       gst_element_state_get_name(cur_state));
      return GST_FLOW_FLUSHING;
    }

    /* No frame yet - sleep and retry */
    if (retry % 100 == 0 && retry > 0) {
      GST_DEBUG_OBJECT(src, "RX CREATE: Still waiting for frame, retry=%u/%u", retry,
                       max_retries);
    }
    g_usleep(1000); /* 1ms */
    retry++;
  }

  if (!frame_info) {
    GST_WARNING_OBJECT(src, "RX CREATE: No frame received after %u ms timeout",
                       max_retries);
    return GST_FLOW_EOS;
  }

  /* Stats: count every frame fetched (valid or not) */
  src->stats_total_frames++;
  if (frame_info->meta_num > 0) {
    src->stats_frames_with_meta++;
    if (frame_info->meta_num >= 3) src->stats_frames_with_meta2++;
  }

  GST_DEBUG_OBJECT(
      src, "RX CREATE: Received frame with %u metadata entries, %u bytes UDW data",
      frame_info->meta_num, frame_info->udw_buffer_fill);

  src->stats_total_headers_written += frame_info->meta_num;
  if (frame_info->meta_num >= 3) src->stats_meta2_headers_written++;

  gst_mtl_st40p_rx_log_frame_info(src, frame_info);

  uint8_t* serialized_data = NULL;
  size_t serialized_size = 0;
  if (!gst_mtl_st40p_rx_serialize_frame(src, frame_info, &serialized_data,
                                        &serialized_size)) {
    st40p_rx_put_frame(src->rx_handle, frame_info);
    return GST_FLOW_ERROR;
  }

  *buffer = gst_buffer_new_allocate(NULL, serialized_size, NULL);
  if (!*buffer) {
    GST_ERROR_OBJECT(src, "Failed to allocate buffer of size %zu", serialized_size);
    g_free(serialized_data);
    st40p_rx_put_frame(src->rx_handle, frame_info);
    return GST_FLOW_ERROR;
  }

  if (serialized_size > 0) {
    if (!gst_buffer_map(*buffer, &dest_info, GST_MAP_WRITE)) {
      GST_ERROR_OBJECT(src, "Failed to map buffer for write");
      gst_buffer_unref(*buffer);
      *buffer = NULL;
      g_free(serialized_data);
      st40p_rx_put_frame(src->rx_handle, frame_info);
      return GST_FLOW_ERROR;
    }

    memcpy(dest_info.data, serialized_data, serialized_size);
    gst_buffer_unmap(*buffer, &dest_info);
  }
  g_free(serialized_data);

  if (frame_info->timestamp > 0) {
    GST_BUFFER_PTS(*buffer) = frame_info->timestamp;
    GST_BUFFER_DTS(*buffer) = frame_info->timestamp;
  }

  st40p_rx_put_frame(src->rx_handle, frame_info);
  return ret;
}

static void gst_mtl_st40p_rx_finalize(GObject* object) {
  Gst_Mtl_St40p_Rx* src = GST_MTL_ST40P_RX(object);

  /* Log statistics before resources are freed */
  GST_INFO_OBJECT(
      src,
      "RX ANC stats: total_frames=%" G_GUINT64_FORMAT
      " frames_with_meta=%" G_GUINT64_FORMAT " frames_with_meta2=%" G_GUINT64_FORMAT
      " total_headers_written=%" G_GUINT64_FORMAT
      " meta2_headers_written=%" G_GUINT64_FORMAT,
      src->stats_total_frames, src->stats_frames_with_meta, src->stats_frames_with_meta2,
      src->stats_total_headers_written, src->stats_meta2_headers_written);
  if (src->stats_frames_with_meta2 != src->stats_meta2_headers_written) {
    GST_WARNING_OBJECT(src,
                       "Mismatch: frames_with_meta2 (%" G_GUINT64_FORMAT
                       ") != meta2_headers_written (%" G_GUINT64_FORMAT ")",
                       src->stats_frames_with_meta2, src->stats_meta2_headers_written);
  }

  if (src->frame_info_fp) {
    fclose(src->frame_info_fp);
    src->frame_info_fp = NULL;
  }
  g_clear_pointer(&src->frame_info_path, g_free);

  if (src->rx_handle) {
    if (st40p_rx_free(src->rx_handle)) {
      GST_ERROR("Failed to free rx handle");
    }
    src->rx_handle = NULL;
  }

  if (src->mtl_lib_handle) {
    if (gst_mtl_common_deinit_handle(&src->mtl_lib_handle)) {
      GST_ERROR("Failed to uninitialize MTL library");
    }
  }

  G_OBJECT_CLASS(parent_class)->finalize(object);
}

static gboolean plugin_init(GstPlugin* mtl_st40p_rx) {
  return gst_element_register(mtl_st40p_rx, "mtl_st40p_rx", GST_RANK_SECONDARY,
                              GST_TYPE_MTL_ST40P_RX);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, mtl_st40p_rx,
                  "MTL ST2110-40 pipeline receiver plugin", plugin_init, PACKAGE_VERSION,
                  GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN);
