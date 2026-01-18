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
 * SECTION:element-mtl_tx_sink
 *
 * The mtl_tx_sink element is a GStreamer sink plugin designed to interface with
 * the Media Transport Library (MTL).
 * MTL is a software-based solution optimized for high-throughput, low-latency
 * transmission and reception of media data.
 *
 * It features an efficient user-space LibOS UDP stack specifically crafted for
 * media transport and includes a built-in  SMPTE ST 2110-compliant
 * implementation for Professional Media over Managed IP Networks.
 *
 * This element allows GStreamer pipelines to send media data using the MTL
 * framework, ensuring efficient and reliable media transport over IP networks.
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gst/gst.h>
#include <st40_api.h>
#include <unistd.h>

#include "gst_mtl_st40p_tx.h"

#ifndef G_PARAM_DEPRECATED
#define G_PARAM_DEPRECATED 0
#endif

static GType gst_mtl_st40p_tx_input_format_get_type(void) {
  static gsize g_define_type_id__ = 0;
  if (g_once_init_enter(&g_define_type_id__)) {
    static const GEnumValue values[] = {
        {GST_MTL_ST40P_TX_INPUT_FORMAT_RAW_UDW, "RawUDW", "raw-udw"},
        {GST_MTL_ST40P_TX_INPUT_FORMAT_RFC8331_PACKED, "RFC8331Packed", "rfc8331-packed"},
        {GST_MTL_ST40P_TX_INPUT_FORMAT_RFC8331_SIMPLIFIED, "RFC8331Simplified",
         "rfc8331"},
        {0, NULL, NULL}};
    GType g_define_type_id = g_enum_register_static("GstMtlSt40pTxInputFormat", values);
    g_once_init_leave(&g_define_type_id__, g_define_type_id);
  }
  return g_define_type_id__;
}
#define GST_TYPE_MTL_ST40P_TX_INPUT_FORMAT (gst_mtl_st40p_tx_input_format_get_type())

static GType gst_mtl_st40p_tx_test_mode_get_type(void) {
  static gsize g_define_type_id__ = 0;
  if (g_once_init_enter(&g_define_type_id__)) {
    static const GEnumValue values[] = {
        {GST_MTL_ST40P_TX_TEST_MODE_NONE, "None", "none"},
        {GST_MTL_ST40P_TX_TEST_MODE_NO_MARKER, "NoMarker", "no-marker"},
        {GST_MTL_ST40P_TX_TEST_MODE_SEQ_GAP, "SeqGap", "seq-gap"},
        {GST_MTL_ST40P_TX_TEST_MODE_BAD_PARITY, "BadParity", "bad-parity"},
        {GST_MTL_ST40P_TX_TEST_MODE_PACED, "Paced", "paced"},
        {0, NULL, NULL}};
    GType g_define_type_id = g_enum_register_static("GstMtlSt40pTxTestMode", values);
    g_once_init_leave(&g_define_type_id__, g_define_type_id);
  }
  return g_define_type_id__;
}
#define GST_TYPE_MTL_ST40P_TX_TEST_MODE (gst_mtl_st40p_tx_test_mode_get_type())

GST_DEBUG_CATEGORY_STATIC(gst_mtl_st40p_tx_debug);
#define GST_CAT_DEFAULT gst_mtl_st40p_tx_debug
#ifndef GST_LICENSE
#define GST_LICENSE "LGPL"
#endif
#ifndef GST_API_VERSION
#define GST_API_VERSION "1.0"
#endif
#ifndef GST_PACKAGE_NAME
#define GST_PACKAGE_NAME "Media Transport Library SMPTE ST 2110-40 Tx plugin"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://github.com/OpenVisualCloud/Media-Transport-Library"
#endif
#ifndef PACKAGE
#define PACKAGE "gst-mtl-st40-tx"
#endif
#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "1.0"
#endif

enum {
  PROP_ST40P_TX_FRAMEBUFF_CNT = PROP_GENERAL_MAX,
  PROP_ST40P_TX_FRAMERATE,
  PROP_ST40P_TX_DID,
  PROP_ST40P_TX_SDID,
  PROP_ST40P_TX_INTERLACED,
  PROP_ST40P_TX_USE_PTS_FOR_PACING,
  PROP_ST40P_TX_PTS_PACING_OFFSET,
  PROP_ST40P_TX_PARSE_8331_META,
  PROP_ST40P_TX_MAX_UDW_SIZE,
  PROP_ST40P_TX_INPUT_FORMAT,
  PROP_ST40P_TX_SPLIT_ANC_BY_PKT,
  PROP_ST40P_TX_TEST_MODE,
  PROP_ST40P_TX_TEST_PKT_COUNT,
  PROP_ST40P_TX_TEST_PACING_NS,
  PROP_MAX
};

/* pad template */
static GstStaticPadTemplate gst_mtl_st40p_tx_sink_pad_template =
    GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);

#define gst_mtl_st40p_tx_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE(Gst_Mtl_St40p_Tx, gst_mtl_st40p_tx, GST_TYPE_BASE_SINK,
                        GST_DEBUG_CATEGORY_INIT(gst_mtl_st40p_tx_debug, "mtl_st40p_tx", 0,
                                                "MTL St2110 st40 transmission sink"));

GST_ELEMENT_REGISTER_DEFINE(mtl_st40p_tx, "mtl_st40p_tx", GST_RANK_NONE,
                            GST_TYPE_MTL_ST40P_TX);

static void gst_mtl_st40p_tx_set_property(GObject* object, guint prop_id,
                                          const GValue* value, GParamSpec* pspec);
static void gst_mtl_st40p_tx_get_property(GObject* object, guint prop_id, GValue* value,
                                          GParamSpec* pspec);
static void gst_mtl_st40p_tx_finalize(GObject* object);

static gboolean gst_mtl_st40p_tx_sink_event(GstPad* pad, GstObject* parent,
                                            GstEvent* event);
static GstFlowReturn gst_mtl_st40p_tx_chain(GstPad* pad, GstObject* parent,
                                            GstBuffer* buf);

static gboolean gst_mtl_st40p_tx_start(GstBaseSink* bsink);

static gboolean gst_mtl_st40p_tx_session_create(Gst_Mtl_St40p_Tx* sink);

/**
 * Main processing functions for ST40p TX GStreamer plugin.
 */
static GstFlowReturn gst_mtl_st40p_tx_parse_memory_block(Gst_Mtl_St40p_Tx* sink,
                                                         GstMapInfo map_info,
                                                         GstBuffer* buf);
static GstFlowReturn gst_mtl_st40p_tx_parse_8331_memory_block(Gst_Mtl_St40p_Tx* sink,
                                                              GstMapInfo map_info,
                                                              GstBuffer* buf);
static GstFlowReturn gst_mtl_st40p_tx_parse_8331_simple_block(Gst_Mtl_St40p_Tx* sink,
                                                              GstMapInfo map_info,
                                                              GstBuffer* buf);

static void gst_mtl_st40p_tx_fill_meta(struct st40_frame_info* frame_info, void* data,
                                       guint32 data_size, guint did, guint sdid);

static GstFlowReturn gst_mtl_st40p_tx_parse_8331_meta(
    struct st40_frame_info* frame_info, struct st40_rfc8331_payload_hdr payload_header,
    guint anc_idx, guint data_offset);

static GstFlowReturn gst_mtl_st40p_tx_parse_8331_anc_words(
    Gst_Mtl_St40p_Tx* sink, GstMapInfo map_info, gint bytes_left_to_process,
    struct gst_st40_rfc8331_meta rfc8331_meta, guint anc_count, GstBuffer* buf);

static GstFlowReturn gst_mtl_st40p_tx_prepare_test_frame(Gst_Mtl_St40p_Tx* sink,
                                                         GstMapInfo map_info,
                                                         GstBuffer* buf) {
  guint meta_count = sink->test_pkt_count;
  if (!meta_count) {
    switch (sink->test_mode) {
      case GST_MTL_ST40P_TX_TEST_MODE_SEQ_GAP:
        meta_count = 2;
        break;
      case GST_MTL_ST40P_TX_TEST_MODE_PACED:
        meta_count = 8;
        break;
      default:
        meta_count = 1;
        break;
    }
  }

  meta_count = MIN(meta_count, (guint)ST40_MAX_META);

  size_t max_udw = st40p_tx_max_udw_buff_size(sink->tx_handle);
  if (!max_udw) {
    GST_ERROR("Failed to query max UDW size for test frame");
    return GST_FLOW_ERROR;
  }

  guint per_udw = 4;
  if ((size_t)meta_count * per_udw > max_udw) {
    per_udw = max_udw / meta_count;
  }
  if (per_udw == 0) {
    GST_ERROR("Insufficient buffer for test frame (meta_count=%u)", meta_count);
    return GST_FLOW_ERROR;
  }

  struct st40_frame_info* frame_info = st40p_tx_get_frame(sink->tx_handle);
  if (!frame_info) {
    GST_ERROR("Failed to get frame for test mode");
    return GST_FLOW_ERROR;
  }

  guint total_bytes = per_udw * meta_count;
  memset(frame_info->udw_buff_addr, 0, total_bytes);
  guint copy_bytes = MIN(total_bytes, (guint)map_info.size);
  if (copy_bytes > 0) memcpy(frame_info->udw_buff_addr, map_info.data, copy_bytes);

  for (guint i = 0; i < meta_count; i++) {
    frame_info->meta[i].c = 0;
    frame_info->meta[i].line_number = 0;
    frame_info->meta[i].hori_offset = 0;
    frame_info->meta[i].s = 0;
    frame_info->meta[i].stream_num = 0;
    frame_info->meta[i].did = sink->did;
    frame_info->meta[i].sdid = sink->sdid;
    frame_info->meta[i].udw_size = per_udw;
    frame_info->meta[i].udw_offset = i * per_udw;
  }

  frame_info->meta_num = meta_count;
  frame_info->udw_buffer_fill = total_bytes;

  if (sink->use_pts_for_pacing) {
    frame_info->timestamp = GST_BUFFER_PTS(buf) + sink->pts_for_pacing_offset;
    frame_info->tfmt = ST10_TIMESTAMP_FMT_TAI;
  } else {
    frame_info->timestamp = 0;
  }

  if (st40p_tx_put_frame(sink->tx_handle, frame_info)) {
    GST_ERROR("Failed to put frame in test mode");
    return GST_FLOW_ERROR;
  }

  return GST_FLOW_OK;
}

static void gst_mtl_st40p_tx_class_init(Gst_Mtl_St40p_TxClass* klass) {
  GObjectClass* gobject_class;
  GstElementClass* gstelement_class;
  GstBaseSinkClass* gstbasesink_class;

  gobject_class = G_OBJECT_CLASS(klass);
  gstelement_class = GST_ELEMENT_CLASS(klass);
  gstbasesink_class = GST_BASE_SINK_CLASS(klass);

  gst_element_class_set_metadata(
      gstelement_class, "MtlTxSt40Sink", "Sink/Metadata",
      "MTL transmission plugin for SMPTE ST 2110-40 standard (ancillary data)",
      "Marek Kasiewicz <marek.kasiewicz@intel.com>");

  gst_element_class_add_static_pad_template(gstelement_class,
                                            &gst_mtl_st40p_tx_sink_pad_template);

  gobject_class->set_property = GST_DEBUG_FUNCPTR(gst_mtl_st40p_tx_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR(gst_mtl_st40p_tx_get_property);
  gobject_class->finalize = GST_DEBUG_FUNCPTR(gst_mtl_st40p_tx_finalize);
  gstbasesink_class->start = GST_DEBUG_FUNCPTR(gst_mtl_st40p_tx_start);

  gst_mtl_common_init_general_arguments(gobject_class);

  g_object_class_install_property(
      gobject_class, PROP_ST40P_TX_FRAMEBUFF_CNT,
      g_param_spec_uint("tx-framebuff-cnt", "Number of framebuffers",
                        "Number of framebuffers to be used for transmission.", 0,
                        G_MAXUINT, 3, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST40P_TX_FRAMERATE,
      gst_param_spec_fraction("tx-fps", "Video framerate", "Framerate of the video.", 1,
                              1, G_MAXINT, 1, DEFAULT_FRAMERATE, 1,
                              G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST40P_TX_DID,
      g_param_spec_uint("tx-did", "Data ID", "Data ID for the ancillary data", 0, 0xff, 0,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST40P_TX_SDID,
      g_param_spec_uint("tx-sdid", "Secondary Data ID",
                        "Secondary Data ID for the ancillary data", 0, 0xff, 0,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST40P_TX_INTERLACED,
      g_param_spec_boolean("tx-interlaced", "Interlaced stream",
                           "Set to true if ancillary stream is interlaced", FALSE,
                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST40P_TX_SPLIT_ANC_BY_PKT,
      g_param_spec_boolean("split-anc-by-pkt", "One ANC per RTP",
                           "Force one ANC packet per RTP with split mode enabled", FALSE,
                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST40P_TX_USE_PTS_FOR_PACING,
      g_param_spec_boolean("use-pts-for-pacing", "Use PTS for packet pacing",
                           "This property modifies the default behavior where "
                           "MTL handles packet pacing. "
                           "Instead, it uses the buffer's PTS (Presentation "
                           "Timestamp) to determine the "
                           "precise time for sending packets.",
                           FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST40P_TX_PTS_PACING_OFFSET,
      g_param_spec_uint("pts-pacing-offset", "PTS offset for packet pacing",
                        "Specifies the offset (in nanoseconds) to be added to the "
                        "Presentation Timestamp (PTS) "
                        "for precise packet pacing. This allows fine-tuning of the "
                        "transmission timing when using PTS-based pacing.",
                        0, G_MAXUINT, 1080, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST40P_TX_PARSE_8331_META,
      g_param_spec_boolean(
          "parse-8331-meta", "Parse 8331 meta",
          "Interpret incoming buffers as RFC 8331 payload.", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_DEPRECATED));

  g_object_class_install_property(
      gobject_class, PROP_ST40P_TX_INPUT_FORMAT,
      g_param_spec_enum(
          "input-format", "Input Format", "Encoding used by incoming ANC buffers.",
          GST_TYPE_MTL_ST40P_TX_INPUT_FORMAT, GST_MTL_ST40P_TX_INPUT_FORMAT_RAW_UDW,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST40P_TX_MAX_UDW_SIZE,
      g_param_spec_uint("max-combined-udw-size", "Max combined UDW size",
                        "Maximum combined size of all user data words to send in "
                        "single st40p frame",
                        0, G_MAXUINT, DEFAULT_MAX_UDW_SIZE,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST40P_TX_TEST_MODE,
      g_param_spec_enum("tx-test-mode", "Test mutation mode",
                        "Apply test-only RTP/ANC mutations (for validation only)",
                        GST_TYPE_MTL_ST40P_TX_TEST_MODE, GST_MTL_ST40P_TX_TEST_MODE_NONE,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST40P_TX_TEST_PKT_COUNT,
      g_param_spec_uint("tx-test-pkt-count", "Test packet count",
                        "Number of ANC packets to emit when a test mode is active"
                        " (0 uses a mode-specific default)",
                        0, ST40_MAX_META, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST40P_TX_TEST_PACING_NS,
      g_param_spec_uint("tx-test-pacing-ns", "Test pacing interval (ns)",
                        "Inter-packet spacing to use when tx-test-mode=paced (ns)", 0,
                        G_MAXUINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static gboolean gst_mtl_st40p_tx_start(GstBaseSink* bsink) {
  Gst_Mtl_St40p_Tx* sink = GST_MTL_ST40P_TX(bsink);

  GST_DEBUG_OBJECT(sink, "start");
  GST_DEBUG("Media Transport Initialization start");
  gst_base_sink_set_async_enabled(bsink, FALSE);

  sink->mtl_lib_handle = gst_mtl_common_init_handle(&(sink->generalArgs), FALSE);

  if (!sink->mtl_lib_handle) {
    GST_ERROR("Could not initialize MTL");
    return FALSE;
  }

  gst_mtl_st40p_tx_session_create(sink);

  gst_element_set_state(GST_ELEMENT(sink), GST_STATE_PLAYING);

  return true;
}

static void gst_mtl_st40p_tx_init(Gst_Mtl_St40p_Tx* sink) {
  GstElement* element = GST_ELEMENT(sink);
  GstPad* sinkpad;

  sink->fps_n = DEFAULT_FRAMERATE;
  sink->fps_d = 1;
  sink->input_format = GST_MTL_ST40P_TX_INPUT_FORMAT_RAW_UDW;
  sink->interlaced = FALSE;
  sink->split_anc_by_pkt = FALSE;
  sink->test_mode = GST_MTL_ST40P_TX_TEST_MODE_NONE;
  sink->test_pkt_count = 0;
  sink->test_pacing_ns = 0;

  sinkpad = gst_element_get_static_pad(element, "sink");
  if (!sinkpad) {
    GST_ERROR_OBJECT(sink, "Failed to get sink pad from child element");
    return;
  }

  gst_pad_set_event_function(sinkpad, GST_DEBUG_FUNCPTR(gst_mtl_st40p_tx_sink_event));

  gst_pad_set_chain_function(sinkpad, GST_DEBUG_FUNCPTR(gst_mtl_st40p_tx_chain));
}

static void gst_mtl_st40p_tx_set_property(GObject* object, guint prop_id,
                                          const GValue* value, GParamSpec* pspec) {
  Gst_Mtl_St40p_Tx* self = GST_MTL_ST40P_TX(object);

  if (prop_id < PROP_GENERAL_MAX) {
    gst_mtl_common_set_general_arguments(object, prop_id, value, pspec,
                                         &(self->generalArgs), &(self->portArgs));
    return;
  }

  switch (prop_id) {
    case PROP_ST40P_TX_FRAMEBUFF_CNT:
      self->framebuff_cnt = g_value_get_uint(value);
      break;
    case PROP_ST40P_TX_FRAMERATE:
      self->fps_n = gst_value_get_fraction_numerator(value);
      self->fps_d = gst_value_get_fraction_denominator(value);
      break;
    case PROP_ST40P_TX_DID:
      self->did = g_value_get_uint(value);
      break;
    case PROP_ST40P_TX_SDID:
      self->sdid = g_value_get_uint(value);
      break;
    case PROP_ST40P_TX_INTERLACED:
      self->interlaced = g_value_get_boolean(value);
      break;
    case PROP_ST40P_TX_SPLIT_ANC_BY_PKT:
      self->split_anc_by_pkt = g_value_get_boolean(value);
      break;
    case PROP_ST40P_TX_USE_PTS_FOR_PACING:
      self->use_pts_for_pacing = g_value_get_boolean(value);
      break;
    case PROP_ST40P_TX_PTS_PACING_OFFSET:
      self->pts_for_pacing_offset = g_value_get_uint(value);
      break;
    case PROP_ST40P_TX_PARSE_8331_META:
      if (g_value_get_boolean(value)) {
        self->input_format = GST_MTL_ST40P_TX_INPUT_FORMAT_RFC8331_PACKED;
      } else if (self->input_format == GST_MTL_ST40P_TX_INPUT_FORMAT_RFC8331_PACKED) {
        self->input_format = GST_MTL_ST40P_TX_INPUT_FORMAT_RAW_UDW;
      }
      break;
    case PROP_ST40P_TX_INPUT_FORMAT:
      self->input_format = (GstMtlSt40pTxInputFormat)g_value_get_enum(value);
      break;
    case PROP_ST40P_TX_MAX_UDW_SIZE:
      self->max_combined_udw_size = g_value_get_uint(value);
      break;
    case PROP_ST40P_TX_TEST_MODE:
      self->test_mode = (GstMtlSt40pTxTestMode)g_value_get_enum(value);
      break;
    case PROP_ST40P_TX_TEST_PKT_COUNT:
      self->test_pkt_count = g_value_get_uint(value);
      break;
    case PROP_ST40P_TX_TEST_PACING_NS:
      self->test_pacing_ns = g_value_get_uint(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void gst_mtl_st40p_tx_get_property(GObject* object, guint prop_id, GValue* value,
                                          GParamSpec* pspec) {
  Gst_Mtl_St40p_Tx* sink = GST_MTL_ST40P_TX(object);

  if (prop_id < PROP_GENERAL_MAX) {
    gst_mtl_common_get_general_arguments(object, prop_id, value, pspec,
                                         &(sink->generalArgs), &(sink->portArgs));
    return;
  }

  switch (prop_id) {
    case PROP_ST40P_TX_FRAMEBUFF_CNT:
      g_value_set_uint(value, sink->framebuff_cnt);
      break;
    case PROP_ST40P_TX_FRAMERATE:
      gst_value_set_fraction(value, sink->fps_n, sink->fps_d);
      break;
    case PROP_ST40P_TX_DID:
      g_value_set_uint(value, sink->did);
      break;
    case PROP_ST40P_TX_SDID:
      g_value_set_uint(value, sink->sdid);
      break;
    case PROP_ST40P_TX_INTERLACED:
      g_value_set_boolean(value, sink->interlaced);
      break;
    case PROP_ST40P_TX_SPLIT_ANC_BY_PKT:
      g_value_set_boolean(value, sink->split_anc_by_pkt);
      break;
    case PROP_ST40P_TX_USE_PTS_FOR_PACING:
      g_value_set_boolean(value, sink->use_pts_for_pacing);
      break;
    case PROP_ST40P_TX_PTS_PACING_OFFSET:
      g_value_set_uint(value, sink->pts_for_pacing_offset);
      break;
    case PROP_ST40P_TX_PARSE_8331_META:
      g_value_set_boolean(
          value, sink->input_format == GST_MTL_ST40P_TX_INPUT_FORMAT_RFC8331_PACKED);
      break;
    case PROP_ST40P_TX_INPUT_FORMAT:
      g_value_set_enum(value, sink->input_format);
      break;
    case PROP_ST40P_TX_MAX_UDW_SIZE:
      g_value_set_uint(value, sink->max_combined_udw_size);
      break;
    case PROP_ST40P_TX_TEST_MODE:
      g_value_set_enum(value, sink->test_mode);
      break;
    case PROP_ST40P_TX_TEST_PKT_COUNT:
      g_value_set_uint(value, sink->test_pkt_count);
      break;
    case PROP_ST40P_TX_TEST_PACING_NS:
      g_value_set_uint(value, sink->test_pacing_ns);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

/*
 * Create MTL session tx handle and initialize the session with the parameters
 * from caps negotiated by the pipeline.
 */
static gboolean gst_mtl_st40p_tx_session_create(Gst_Mtl_St40p_Tx* sink) {
  struct st40p_tx_ops ops_tx = {0};

  if (!sink->mtl_lib_handle) {
    GST_ERROR("MTL library not initialized");
    return FALSE;
  }
  if (sink->tx_handle) {
    /* TODO: old session should be removed if exists*/
    GST_ERROR("Tx handle already initialized");
    return FALSE;
  }

  ops_tx.name = "st40sink";
  ops_tx.priv = sink;
  if (sink->framebuff_cnt) {
    ops_tx.framebuff_cnt = sink->framebuff_cnt;
  } else {
    ops_tx.framebuff_cnt = 3;
  }

  gst_mtl_common_copy_general_to_session_args(&(sink->generalArgs), &(sink->portArgs));

  ops_tx.port.num_port =
      gst_mtl_common_parse_tx_port_arguments(&ops_tx.port, &sink->portArgs);
  if (!ops_tx.port.num_port) {
    GST_ERROR("Failed to parse port arguments");
    return FALSE;
  }

  if (sink->input_format != GST_MTL_ST40P_TX_INPUT_FORMAT_RAW_UDW &&
      (sink->did || sink->sdid))
    GST_WARNING("DID %d and SDID %d ignored when using 8331 meta parsing", sink->did,
                sink->sdid);
  else {
    if (sink->did > 0xFF) {
      GST_ERROR("Invalid DID value: %d", sink->did);
      return FALSE;
    }

    if (sink->sdid > 0xFF) {
      GST_ERROR("Invalid SDID value: %d", sink->sdid);
      return FALSE;
    }
  }

  ops_tx.fps = st_frame_rate_to_st_fps((double)sink->fps_n / sink->fps_d);
  if (ops_tx.fps == ST_FPS_MAX) {
    GST_ERROR("Invalid framerate: %d/%d", sink->fps_n, sink->fps_d);
    return FALSE;
  }

  ops_tx.interlaced = sink->interlaced;
  /* Only single ANC packet fits in "raw" mode since metadata is synthesized from
   * element properties. TODO: lift this limitation by queueing multiple frames. */
  sink->frame_size = MAX_UDW_SIZE;

  if (sink->max_combined_udw_size)
    ops_tx.max_udw_buff_size = sink->max_combined_udw_size;
  else
    ops_tx.max_udw_buff_size = DEFAULT_MAX_UDW_SIZE;

  ops_tx.flags |= ST40P_TX_FLAG_BLOCK_GET;
  if (sink->use_pts_for_pacing) {
    ops_tx.flags |= ST40P_TX_FLAG_USER_PACING;
  } else if (sink->pts_for_pacing_offset) {
    GST_WARNING("PTS offset specified but PTS-based pacing is not enabled");
  }

  if (sink->split_anc_by_pkt) {
    ops_tx.flags |= ST40P_TX_FLAG_SPLIT_ANC_BY_PKT;
    GST_DEBUG_OBJECT(sink, "TX START: enabling split ANC per RTP packet");
  }

  switch (sink->test_mode) {
    case GST_MTL_ST40P_TX_TEST_MODE_NO_MARKER:
      ops_tx.test.pattern = ST40_TX_TEST_NO_MARKER;
      break;
    case GST_MTL_ST40P_TX_TEST_MODE_SEQ_GAP:
      ops_tx.test.pattern = ST40_TX_TEST_SEQ_GAP;
      break;
    case GST_MTL_ST40P_TX_TEST_MODE_BAD_PARITY:
      ops_tx.test.pattern = ST40_TX_TEST_BAD_PARITY;
      break;
    case GST_MTL_ST40P_TX_TEST_MODE_PACED:
      ops_tx.test.pattern = ST40_TX_TEST_PACED;
      break;
    case GST_MTL_ST40P_TX_TEST_MODE_NONE:
    default:
      ops_tx.test.pattern = ST40_TX_TEST_NONE;
      break;
  }
  if (ops_tx.test.pattern != ST40_TX_TEST_NONE) {
    ops_tx.test.frame_count = 1; /* default to one mutated frame */
    ops_tx.test.paced_pkt_count = sink->test_pkt_count;
    ops_tx.test.paced_gap_ns = sink->test_pacing_ns;
    if (!sink->split_anc_by_pkt) {
      ops_tx.flags |= ST40P_TX_FLAG_SPLIT_ANC_BY_PKT;
      sink->split_anc_by_pkt = TRUE;
    }
  }

  sink->tx_handle = st40p_tx_create(sink->mtl_lib_handle, &ops_tx);
  if (!sink->tx_handle) {
    GST_ERROR("Failed to create st40p tx handle");
    return FALSE;
  }

  return TRUE;
}

static gboolean gst_mtl_st40p_tx_sink_event(GstPad* pad, GstObject* parent,
                                            GstEvent* event) {
  Gst_Mtl_St40p_Tx* sink;
  gint ret;

  sink = GST_MTL_ST40P_TX(parent);

  GST_LOG_OBJECT(sink, "Received %s event: %" GST_PTR_FORMAT, GST_EVENT_TYPE_NAME(event),
                 event);

  ret = GST_EVENT_TYPE(event);

  switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_SEGMENT:
      if (!sink->tx_handle) {
        GST_ERROR("Tx handle not initialized");
        return FALSE;
      }
      ret = gst_pad_event_default(pad, parent, event);
      break;
    case GST_EVENT_EOS:
      ret = gst_pad_event_default(pad, parent, event);
      gst_element_post_message(GST_ELEMENT(sink), gst_message_new_eos(GST_OBJECT(sink)));
      break;
    default:
      ret = gst_pad_event_default(pad, parent, event);
      break;
  }

  return ret;
}

/**
 * @brief Fills the metadata for a st40 frame.
 *
 * This function initializes the metadata fields of a given st40 frame with the
 * provided data and metadata values. It sets the first metadata entry with the
 * specified DID, SDID, and data size, and assigns the data pointer to the
 * frame. Note: Most of the fields are hardcoded, and only first metadata block
 * is filled as only one ANC Data packet per frame is allowed.
 *
 * @param frame Pointer to the st40_frame structure to be filled. Cannot be
 * NULL.
 * @param data Pointer to the data to be associated with the frame.
 * @param data_size Size of the data in bytes.
 * @param did Data Identifier (DID) to be set in the metadata.
 * @param sdid Secondary Data Identifier (SDID) to be set in the metadata.
 */
static void gst_mtl_st40p_tx_fill_meta(struct st40_frame_info* frame_info, void* data,
                                       guint32 data_size, guint did, guint sdid) {
  frame_info->meta[0].c = 0;
  frame_info->meta[0].line_number = 0;
  frame_info->meta[0].hori_offset = 0;
  frame_info->meta[0].s = 0;
  frame_info->meta[0].stream_num = 0;
  frame_info->meta[0].did = did;
  frame_info->meta[0].sdid = sdid;
  frame_info->meta[0].udw_size = data_size;
  frame_info->meta[0].udw_offset = 0;
  frame_info->udw_buffer_fill = data_size;
  frame_info->meta_num = 1;
}

/* we dont' really check the data here we let the st40 ancillary data to do so
 */
static GstFlowReturn gst_mtl_st40p_tx_parse_8331_meta(
    struct st40_frame_info* frame_info, struct st40_rfc8331_payload_hdr payload_header,
    guint anc_idx, guint udw_offset) {
  if (!frame_info || !frame_info->meta) {
    GST_ERROR("Failed to parse rfc8331 payload meta Null frame_info pointer");
    return GST_FLOW_ERROR;
  }

  if (anc_idx >= ST40_RFC8331_PAYLOAD_MAX_ANCILLARY_COUNT) {
    GST_ERROR("ANC index out of bounds: %u", anc_idx);
    return GST_FLOW_ERROR;
  }

  frame_info->meta[anc_idx].c = payload_header.first_hdr_chunk.c;
  frame_info->meta[anc_idx].line_number = payload_header.first_hdr_chunk.line_number;
  frame_info->meta[anc_idx].hori_offset =
      payload_header.first_hdr_chunk.horizontal_offset;
  frame_info->meta[anc_idx].s = payload_header.first_hdr_chunk.s;
  frame_info->meta[anc_idx].stream_num = payload_header.first_hdr_chunk.stream_num;
  frame_info->meta[anc_idx].did = payload_header.second_hdr_chunk.did & 0xff;
  frame_info->meta[anc_idx].sdid = payload_header.second_hdr_chunk.sdid & 0xff;
  frame_info->meta[anc_idx].udw_size = payload_header.second_hdr_chunk.data_count & 0xff;
  frame_info->meta[anc_idx].udw_offset = udw_offset;
  frame_info->meta_num = anc_idx + 1;

  return GST_FLOW_OK;
}

static GstFlowReturn gst_mtl_st40p_tx_parse_8331_anc_words(
    Gst_Mtl_St40p_Tx* sink, GstMapInfo map_info, gint bytes_left_to_process,
    struct gst_st40_rfc8331_meta rfc8331_meta, guint anc_count, GstBuffer* buf) {
  struct st40_frame_info* frame_info = NULL;
  struct st40_rfc8331_payload_hdr payload_header;
  uint8_t* payload_cursor;
  guint data_count, buffer_size = map_info.size, udw_byte_size;
  guint16 udw;

  if (buffer_size < bytes_left_to_process) {
    GST_ERROR("Buffer size (%u) is smaller than bytes left to process (%d)", buffer_size,
              bytes_left_to_process);
    return GST_FLOW_ERROR;
  }

  frame_info = st40p_tx_get_frame(sink->tx_handle);
  if (!frame_info) {
    GST_ERROR("Failed to get frame");
    return GST_FLOW_ERROR;
  }

  frame_info->meta_num = 0;

  for (int i = 0; i < anc_count; i++) {
    /* Processing of the input 8331 header */
    if (bytes_left_to_process < sizeof(struct st40_rfc8331_payload_hdr)) {
      GST_ERROR("Buffer size (%u) is too small to contain rfc8331 header (%lu)",
                bytes_left_to_process, sizeof(struct st40_rfc8331_payload_hdr));
      return GST_FLOW_ERROR;
    }

    payload_cursor = (uint8_t*)map_info.data + (buffer_size - bytes_left_to_process);

    rfc8331_meta.headers[i] = (struct st40_rfc8331_payload_hdr*)payload_cursor;
    payload_header.swapped_first_hdr_chunk =
        ntohl(rfc8331_meta.headers[i]->swapped_first_hdr_chunk);
    payload_header.swapped_second_hdr_chunk =
        ntohl(rfc8331_meta.headers[i]->swapped_second_hdr_chunk);

    payload_cursor = (uint8_t*)&rfc8331_meta.headers[i]->swapped_second_hdr_chunk;
    /*
     * In RFC 8331, the header struct occupies only 30 bits, not 32.
     * The layout is:
     *   |C|   Line_Number=9     |   Horizontal_Offset   |S| StreamNum=0 |
     *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *   |         DID       |        SDID       |  Data_Count=0x84  |
     * We are currently skipping 2 more bits than needed here.
     * This will be accommodated a little bit later in the parsing logic with
     * RFC_8331_PAYLOAD_HEADER_LOST_BITS define.
     */
    bytes_left_to_process -= sizeof(struct st40_rfc8331_payload_hdr);
    data_count = payload_header.second_hdr_chunk.data_count & 0xff;

    /* data count * 10 bits + 10 bit checksum - 2 lost bit from the
     * st40_rfc8331_payload_hdr */
    udw_byte_size = (data_count * UDW_WORD_BIT_SIZE) + UDW_WORD_BIT_SIZE -
                    RFC_8331_PAYLOAD_HEADER_LOST_BITS;
    /* round up to the nearest byte */
    udw_byte_size = (udw_byte_size + 7) / 8;

    if (bytes_left_to_process < udw_byte_size) {
      GST_ERROR("Buffer size (%u) is too small for data count (%d)",
                bytes_left_to_process, udw_byte_size);
    }

    /* Use data_size as the offset for the next UDW block.
     * data_size points to the correct offset for the current ANC packet
     */
    if (gst_mtl_st40p_tx_parse_8331_meta(frame_info, payload_header, i,
                                         frame_info->udw_buffer_fill)) {
      GST_ERROR("Failed to parse 8331 meta");
      return GST_FLOW_ERROR;
    }

    /*
     * Skip the first three UDW entries:
     * - 0th UDW: DID (Data Identifier)
     * - 1st UDW: SDID (Secondary Data Identifier)
     * - 2nd UDW: Data_Count (number of user data words)
     * Start processing actual user data words from the 3rd UDW onward.
     */
    for (int j = 0; j < data_count; j++) {
      if (frame_info->udw_buffer_fill >= frame_info->udw_buffer_size) {
        GST_ERROR("UDW buffer overflow: fill=%u size=%zu", frame_info->udw_buffer_fill,
                  frame_info->udw_buffer_size);
        return GST_FLOW_ERROR;
      }

      udw = st40_get_udw((j + 3), payload_cursor);
      if (!st40_check_parity_bits(udw)) {
        GST_ERROR("Ancillary data parity bits check failed");
        return GST_FLOW_ERROR;
      }
      frame_info->udw_buff_addr[frame_info->udw_buffer_fill++] = (udw & 0xff);
    }

    bytes_left_to_process -= udw_byte_size;
    /* Get checksum and promptly ignore it */
    udw = st40_get_udw((data_count + 3), payload_cursor);
    printf("Checksum UDW: 0x%04x\n", udw);

    if (sink->use_pts_for_pacing) {
      frame_info->timestamp = GST_BUFFER_PTS(buf) + sink->pts_for_pacing_offset;
      frame_info->tfmt = ST10_TIMESTAMP_FMT_TAI;
    } else {
      frame_info->timestamp = 0;
    }

    /* word align */
    if (bytes_left_to_process > 0)
      bytes_left_to_process -= bytes_left_to_process % RFC_8331_WORD_BYTE_SIZE;
  }

  if (st40p_tx_put_frame(sink->tx_handle, frame_info)) {
    GST_ERROR("Failed to put frame");
    return GST_FLOW_ERROR;
  }

  if (bytes_left_to_process > 0) {
    GST_WARNING("Bytes left to process after parsing 8331 meta: %d",
                bytes_left_to_process);
    /* If there are still bytes left, we can ignore them for now */
  }

  return GST_FLOW_OK;
}

static GstFlowReturn gst_mtl_st40p_tx_parse_8331_memory_block(Gst_Mtl_St40p_Tx* sink,
                                                              GstMapInfo map_info,
                                                              GstBuffer* buf) {
  struct gst_st40_rfc8331_meta rfc8331_meta;
  struct st40_rfc8331_payload_hdr_common meta;
  guint bytes_left_to_process = map_info.size;
  guint ret;

  if (bytes_left_to_process < sizeof(struct st40_rfc8331_payload_hdr_common)) {
    GST_ERROR("Buffer too small for rfc8331 header");
    return GST_FLOW_ERROR;
  }

  /* convert to network byte order */
  rfc8331_meta.header_common = (struct st40_rfc8331_payload_hdr_common*)map_info.data;
  meta.swapped_handle = ntohl(rfc8331_meta.header_common->swapped_handle);
  bytes_left_to_process -= sizeof(struct st40_rfc8331_payload_hdr_common);

  /* ignore an ANC data packet with an F field value of 0b01 */
  if (meta.first_hdr_chunk.f == 1) {
    GST_INFO("Ignoring ANC data packet with F field value 0b01");
    return GST_FLOW_OK;
  } else if (meta.first_hdr_chunk.f != 0) {
    GST_ERROR("Unsupported F field value: 0b%d", meta.first_hdr_chunk.f);
    return GST_FLOW_ERROR;
  }

  ret = gst_mtl_st40p_tx_parse_8331_anc_words(sink, map_info, bytes_left_to_process,
                                              rfc8331_meta,
                                              meta.first_hdr_chunk.anc_count, buf);

  if (ret) {
    GST_ERROR("Failed to parse 8331 gst buffer %d", ret);
    return ret;
  }

  return GST_FLOW_OK;
}

static GstFlowReturn gst_mtl_st40p_tx_parse_8331_simple_block(Gst_Mtl_St40p_Tx* sink,
                                                              GstMapInfo map_info,
                                                              GstBuffer* buf) {
  const gsize payload_size = map_info.size;
  gsize cursor = 0;
  struct st40_frame_info* frame_info = NULL;
  guint anc_idx = 0;

  if (!payload_size) {
    GST_DEBUG_OBJECT(sink, "Simplified RFC8331 buffer empty; nothing to send");
    return GST_FLOW_OK;
  }

  frame_info = st40p_tx_get_frame(sink->tx_handle);
  if (!frame_info) {
    GST_ERROR("Failed to get frame for simplified RFC8331 payload");
    return GST_FLOW_ERROR;
  }

  frame_info->meta_num = 0;
  frame_info->udw_buffer_fill = 0;

  while (cursor < payload_size) {
    gsize remaining = payload_size - cursor;
    if (remaining < RFC_8331_PAYLOAD_HEADER_SIZE) {
      GST_ERROR("Truncated simplified RFC8331 header (need %u, have %zu)",
                RFC_8331_PAYLOAD_HEADER_SIZE, remaining);
      return GST_FLOW_ERROR;
    }

    if (anc_idx >= ST40_RFC8331_PAYLOAD_MAX_ANCILLARY_COUNT) {
      GST_ERROR("Too many ANC packets in buffer (max %d)",
                ST40_RFC8331_PAYLOAD_MAX_ANCILLARY_COUNT);
      return GST_FLOW_ERROR;
    }

    const uint8_t* header = map_info.data + cursor;
    cursor += RFC_8331_PAYLOAD_HEADER_SIZE;

    guint8 data_count = header[7];
    if ((payload_size - cursor) < data_count) {
      GST_ERROR("ANC payload shorter than declared (%u > %zu)", data_count,
                payload_size - cursor);
      return GST_FLOW_ERROR;
    }

    if (frame_info->udw_buffer_fill + data_count > frame_info->udw_buffer_size) {
      GST_ERROR("UDW buffer overflow (fill=%u, request=%u, size=%zu)",
                frame_info->udw_buffer_fill, data_count, frame_info->udw_buffer_size);
      return GST_FLOW_ERROR;
    }

    struct st40_meta* meta = &frame_info->meta[anc_idx];
    meta->line_number = ((uint16_t)header[0] << 8) | header[1];
    meta->hori_offset = ((uint16_t)header[2] << 8) | header[3];
    meta->c = (header[4] >> 7) & 0x1;
    meta->s = (header[4] >> 6) & 0x1;
    meta->stream_num = header[4] & 0x3F;
    meta->did = header[5];
    meta->sdid = header[6];
    meta->udw_size = data_count;
    meta->udw_offset = frame_info->udw_buffer_fill;

    memcpy(frame_info->udw_buff_addr + frame_info->udw_buffer_fill,
           map_info.data + cursor, data_count);
    frame_info->udw_buffer_fill += data_count;
    cursor += data_count;
    anc_idx++;
  }

  frame_info->meta_num = anc_idx;

  if (sink->use_pts_for_pacing) {
    frame_info->timestamp = GST_BUFFER_PTS(buf) + sink->pts_for_pacing_offset;
    frame_info->tfmt = ST10_TIMESTAMP_FMT_TAI;
  } else {
    frame_info->timestamp = 0;
  }

  if (st40p_tx_put_frame(sink->tx_handle, frame_info)) {
    GST_ERROR("Failed to enqueue simplified RFC8331 frame");
    return GST_FLOW_ERROR;
  }

  return GST_FLOW_OK;
}

/**
 * @brief Parses a GstBuffer and prepares a frame for transmission.
 *
 * This function retrieves a frame from the ST40p transmitter handle, copies the
 * relevant data from the provided GstBuffer into the frame buffer, fills in
 * ancillary metadata, and submits the frame for transmission. It also handles
 * timestamping if pacing is enabled. No memory management is done here, the
 * caller is responsible for managing the GstBuffer and its associated memory.
 *
 * @param sink Pointer to the Gst_Mtl_St40p_Tx sink element.
 * @param gst_buffer_memory GstMemory associated with the GstBuffer.
 * @param map_info GstMapInfo structure containing mapped buffer data.
 * @param buf GstBuffer only to pass PTS value if needed.
 *
 * @return GST_FLOW_OK on success, GST_FLOW_ERROR on failure.
 */
static GstFlowReturn gst_mtl_st40p_tx_parse_memory_block(Gst_Mtl_St40p_Tx* sink,
                                                         GstMapInfo map_info,
                                                         GstBuffer* buf) {
  if (sink->test_mode != GST_MTL_ST40P_TX_TEST_MODE_NONE) {
    return gst_mtl_st40p_tx_prepare_test_frame(sink, map_info, buf);
  }
  struct st40_frame_info* frame_info = NULL;
  uint8_t* cur_addr_buf;
  gint bytes_left_to_process, bytes_left_to_process_cur;

  bytes_left_to_process = map_info.size;

  while (bytes_left_to_process > 0) {
    frame_info = st40p_tx_get_frame(sink->tx_handle);
    if (!frame_info) {
      GST_ERROR("Failed to get frame");
      return GST_FLOW_ERROR;
    }

    cur_addr_buf = map_info.data + map_info.size - bytes_left_to_process;
    bytes_left_to_process_cur = bytes_left_to_process > sink->frame_size
                                    ? sink->frame_size
                                    : bytes_left_to_process;

    memcpy(frame_info->udw_buff_addr, cur_addr_buf, bytes_left_to_process_cur);

    gst_mtl_st40p_tx_fill_meta(frame_info, frame_info->udw_buff_addr,
                               bytes_left_to_process_cur, sink->did, sink->sdid);

    if (sink->use_pts_for_pacing) {
      frame_info->timestamp = GST_BUFFER_PTS(buf) + sink->pts_for_pacing_offset;
      frame_info->tfmt = ST10_TIMESTAMP_FMT_TAI;
    } else {
      frame_info->timestamp = 0;
    }

    if (st40p_tx_put_frame(sink->tx_handle, frame_info)) {
      GST_ERROR("Failed to put frame");
      return GST_FLOW_ERROR;
    }

    bytes_left_to_process -= bytes_left_to_process_cur;
  }

  return GST_FLOW_OK;
}

/*
 * Takes the buffer from the source pad and sends it to the mtl library via
 * frame buffers, supports incomplete frames. But buffers needs to add up to the
 * actual frame size.
 */
static GstFlowReturn gst_mtl_st40p_tx_chain(GstPad* pad, GstObject* parent,
                                            GstBuffer* buf) {
  Gst_Mtl_St40p_Tx* sink = GST_MTL_ST40P_TX(parent);
  gint buffer_n = gst_buffer_n_memory(buf);
  GstMemory* gst_buffer_memory;
  GstMapInfo map_info;
  GstFlowReturn ret = GST_FLOW_OK;

  if (!sink->tx_handle) {
    GST_ERROR("Tx handle not initialized");
    return GST_FLOW_ERROR;
  }

  for (int i = 0; i < buffer_n; i++) {
    gst_buffer_memory = gst_buffer_peek_memory(buf, i);

    if (!gst_memory_map(gst_buffer_memory, &map_info, GST_MAP_READ)) {
      GST_ERROR("Failed to map memory");
      return GST_FLOW_ERROR;
    }

    switch (sink->input_format) {
      case GST_MTL_ST40P_TX_INPUT_FORMAT_RAW_UDW:
        ret = gst_mtl_st40p_tx_parse_memory_block(sink, map_info, buf);
        break;
      case GST_MTL_ST40P_TX_INPUT_FORMAT_RFC8331_PACKED:
        ret = gst_mtl_st40p_tx_parse_8331_memory_block(sink, map_info, buf);
        break;
      case GST_MTL_ST40P_TX_INPUT_FORMAT_RFC8331_SIMPLIFIED:
        ret = gst_mtl_st40p_tx_parse_8331_simple_block(sink, map_info, buf);
        break;
      default:
        GST_ERROR("Unsupported input format %d", sink->input_format);
        ret = GST_FLOW_ERROR;
        break;
    }

    if (ret) {
      GST_ERROR("Failed to parse gst buffer %d", ret);
      gst_memory_unmap(gst_buffer_memory, &map_info);
      gst_buffer_unref(buf);
      return ret;
    }

    /* Unmap memory after processing */
    gst_memory_unmap(gst_buffer_memory, &map_info);
  }

  gst_buffer_unref(buf);
  return GST_FLOW_OK;
}

static void gst_mtl_st40p_tx_finalize(GObject* object) {
  Gst_Mtl_St40p_Tx* sink = GST_MTL_ST40P_TX(object);

  if (sink->tx_handle) {
    if (st40p_tx_free(sink->tx_handle)) {
      GST_ERROR("Failed to free tx handle");
      return;
    }
  }

  if (sink->mtl_lib_handle) {
    if (gst_mtl_common_deinit_handle(&sink->mtl_lib_handle)) {
      GST_ERROR("Failed to uninitialize MTL library");
      return;
    }
  }
}

static gboolean plugin_init(GstPlugin* mtl_st40p_tx) {
  return gst_element_register(mtl_st40p_tx, "mtl_st40p_tx", GST_RANK_SECONDARY,
                              GST_TYPE_MTL_ST40P_TX);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, mtl_st40p_tx,
                  "software-based solution designed for high-throughput transmission",
                  plugin_init, PACKAGE_VERSION, GST_LICENSE, GST_PACKAGE_NAME,
                  GST_PACKAGE_ORIGIN)
