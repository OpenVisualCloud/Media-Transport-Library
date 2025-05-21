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

#define ST40_RFC8331_PAYLOAD_MAX_ANCILLARY_COUNT 20
/* Maximum size for single User Data Words */
#define DEFAULT_MAX_UDW_SIZE (ST40_RFC8331_PAYLOAD_MAX_ANCILLARY_COUNT * 255)
/* rfc8331 header consist of rows 3 * 10 bits + 2 bits  */
#define RFC_8331_WORD_BYTE_SIZE ((3 * 10 + 2) / 8)
#define RFC_8331_PAYLOAD_HEADER_SIZE 8

#define ST40P_TX_SHIFT_BUFFER(buffer_ptr, bytes_left_to_process, shift) \
    { \
        (buffer_ptr) += (shift); \
        (bytes_left_to_process) -= (shift); \
    }

enum {
  PROP_ST40P_TX_FRAMEBUFF_CNT = PROP_GENERAL_MAX,
  PROP_ST40P_TX_FRAMERATE,
  PROP_ST40P_TX_DID,
  PROP_ST40P_TX_SDID,
  PROP_ST40P_TX_USE_PTS_FOR_PACING,
  PROP_ST40P_TX_PTS_PACING_OFFSET,
  PROP_ST40P_TX_PARSE_8331_META,
  PROP_ST40P_TX_PARSE_8331_META_ENDIANNESS,
  PROP_ST40P_TX_MAX_UDW_SIZE,
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
static GstFlowReturn st40p_tx_parse_gstbuffer(Gst_Mtl_St40p_Tx* sink,
                                              GstMapInfo map_info,
                                              gint* bytes_left_to_process,
                                              GstBuffer* buf);

static void st40p_tx_fill_meta(struct st40_frame* frame, void* data, guint32 data_size,
                           guint did, guint sdid);

static GstFlowReturn st40p_tx_parse_8331_meta(struct st40_frame* frame,
                                              struct gst_st40_rfc8331_meta meta,
                                              guint anc_idx,
                                              void* data_ptr,
                                              guint data_offset);

static GstFlowReturn st40p_tx_parse_8331_gstbuffer(Gst_Mtl_St40p_Tx* sink,
                                              GstMapInfo map_info,
                                              gint* bytes_left_to_process,
                                              GstBuffer* buf,
                                              guint anc_count);

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
      gobject_class, PROP_ST40P_TX_USE_PTS_FOR_PACING,
      g_param_spec_boolean(
          "use-pts-for-pacing", "Use PTS for packet pacing",
          "This property modifies the default behavior where MTL handles packet pacing. "
          "Instead, it uses the buffer's PTS (Presentation Timestamp) to determine the "
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
      g_param_spec_boolean("parse-8331-meta", "Parse 8331 meta",
                           "Parse 8331 meta data from the ancillary data, requires you "
                           "to send the whole 8331 header in the buffer",
                           FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
  gobject_class, PROP_ST40P_TX_PARSE_8331_META_ENDIANNESS,
  g_param_spec_uint("parse-8331-meta-endianness",
                            "Parse 8331 meta endianness",
                            "Parse 8331 meta data endianness, "
                            "0 - system endianness, 1 - big endian, 2 - little endian",
                            0, ST40_RFC8331_PAYLOAD_ENDIAN_MAX - 1,
                            ST40_RFC8331_PAYLOAD_ENDIAN_SYSTEM,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

g_object_class_install_property(
    gobject_class, PROP_ST40P_TX_MAX_UDW_SIZE,
    g_param_spec_uint("max-combined-udw-size", "Max combined UDW size",
                     "Maximum combined size of all user data words to send in "
                     "single st40p frame",
                     0, G_MAXUINT, DEFAULT_MAX_UDW_SIZE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));


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
    case PROP_ST40P_TX_USE_PTS_FOR_PACING:
      self->use_pts_for_pacing = g_value_get_boolean(value);
      break;
    case PROP_ST40P_TX_PTS_PACING_OFFSET:
      self->pts_for_pacing_offset = g_value_get_uint(value);
      break;
    case PROP_ST40P_TX_PARSE_8331_META:
      self->parse_8331_meta_from_gstbuffer = g_value_get_boolean(value);
      break;
    case PROP_ST40P_TX_PARSE_8331_META_ENDIANNESS:
      self->parse_8331_meta_endianness =
          g_value_get_uint(value);
      break;
    case PROP_ST40P_TX_MAX_UDW_SIZE:
      self->max_combined_udw_size = g_value_get_uint(value);
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
    case PROP_ST40P_TX_USE_PTS_FOR_PACING:
      g_value_set_boolean(value, sink->use_pts_for_pacing);
      break;
    case PROP_ST40P_TX_PTS_PACING_OFFSET:
      g_value_set_uint(value, sink->pts_for_pacing_offset);
      break;
    case PROP_ST40P_TX_PARSE_8331_META:
      g_value_set_boolean(value, sink->parse_8331_meta_from_gstbuffer);
      break;
    case PROP_ST40P_TX_PARSE_8331_META_ENDIANNESS:
      g_value_set_uint(value, sink->parse_8331_meta_endianness);
      break;
    case PROP_ST40P_TX_MAX_UDW_SIZE:
      g_value_set_uint(value, sink->max_combined_udw_size);
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

  if (sink->parse_8331_meta_from_gstbuffer && (sink->did || sink->sdid))
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

  ops_tx.interlaced = false;
  /* Only single ANC data packet is possible when metadata is not being parsed from
   * parse_8331_meta_from_gstbuffer mode per frame. anc_count = 1 TODO: allow more */
  sink->frame_size = 255;

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
 * This function initializes the metadata fields of a given st40 frame with the provided
 * data and metadata values. It sets the first metadata entry with the specified DID,
 * SDID, and data size, and assigns the data pointer to the frame.
 * Note: Most of the fields are hardcoded.
 *
 * This function is expected to be called multiple times per frame, with the anc_count
 * parameter indicating the current ANC packet index. Calls should be made in increasing
 * anc_count order for each ANC packet in the frame.
 *
 * @param frame Pointer to the st40_frame structure to be filled. Cannot be NULL.
 * @param data Pointer to the data to be associated with the frame.
 * @param data_size Size of the data in bytes.
 * @param did Data Identifier (DID) to be set in the metadata.
 * @param sdid Secondary Data Identifier (SDID) to be set in the metadata.
 * @param anc_count The index of the ANC packet for which metadata is being filled.
 */
static void st40p_tx_fill_meta(struct st40_frame* frame, void* data, guint32 data_size,
                           guint did, guint sdid) {
  frame->meta[0].c = 0;
  frame->meta[0].line_number = 0;
  frame->meta[0].hori_offset = 0;
  frame->meta[0].s = 0;
  frame->meta[0].stream_num = 0;
  frame->meta[0].did = did;
  frame->meta[0].sdid = sdid;
  frame->meta[0].udw_size = data_size;
  frame->meta[0].udw_offset = 0;
  frame->data = data;
  frame->meta_num = 1;
}

/**
 * @brief Parses a GstBuffer and prepares a frame for transmission.
 *
 * This function retrieves a frame from the ST40p transmitter handle, copies the relevant
 * data from the provided GstBuffer into the frame buffer, fills in ancillary metadata,
 * and submits the frame for transmission. It also handles timestamping if pacing is enabled.
 *
 * @param sink Pointer to the Gst_Mtl_St40p_Tx sink element.
 * @param map_info GstMapInfo structure containing mapped buffer data.
 * @param bytes_left_to_process Pointer to the number of bytes left to process in the buffer.
 * @param buf GstBuffer to be parsed and transmitted.
 *
 * @return GST_FLOW_OK on success, GST_FLOW_ERROR on failure.
 */
static GstFlowReturn st40p_tx_parse_gstbuffer(Gst_Mtl_St40p_Tx* sink,
                                              GstMapInfo map_info,
                                              gint* bytes_left_to_process,
                                              GstBuffer* buf) {
  struct st40_frame_info* frame_info = NULL;
  uint8_t* cur_addr_buf;
  gint bytes_left_to_process_cur;

  frame_info = st40p_tx_get_frame(sink->tx_handle);
  if (!frame_info) {
    GST_ERROR("Failed to get frame");
    return GST_FLOW_ERROR;
  }

  cur_addr_buf = map_info.data + gst_buffer_get_size(buf) - *bytes_left_to_process;
  bytes_left_to_process_cur =
      *bytes_left_to_process > sink->frame_size ? sink->frame_size : *bytes_left_to_process;

  mtl_memcpy(frame_info->udw_buff_addr, cur_addr_buf, bytes_left_to_process_cur);

  st40p_tx_fill_meta(frame_info->anc_frame, frame_info->udw_buff_addr, bytes_left_to_process_cur,
                 sink->did, sink->sdid);

  if (sink->use_pts_for_pacing) {
    frame_info->timestamp = GST_BUFFER_PTS(buf) += sink->pts_for_pacing_offset;
    frame_info->tfmt = ST10_TIMESTAMP_FMT_TAI;
  }

  if (st40p_tx_put_frame(sink->tx_handle, frame_info)) {
    GST_ERROR("Failed to put frame");
    return GST_FLOW_ERROR;
  }
  bytes_left_to_process -= bytes_left_to_process_cur;

  return GST_FLOW_OK;
}

/* we dont' really check the data here we let the st40 ancillary data to do so */
static GstFlowReturn st40p_tx_parse_8331_meta(struct st40_frame* frame,
                                              struct gst_st40_rfc8331_meta meta,
                                              guint anc_idx,
                                              void* data_ptr,
                                              guint udw_offset) {
  if (!frame || !data_ptr) {
    GST_ERROR("Invalid parameters for parsing 8331 meta");
    return GST_FLOW_ERROR;
  }

  if (anc_idx >= ST40_RFC8331_PAYLOAD_MAX_ANCILLARY_COUNT) {
    GST_ERROR("ANC index out of bounds: %u", anc_idx);
    return GST_FLOW_ERROR;
  }

  frame->meta[anc_idx].c = meta.hdr.header.c;
  frame->meta[anc_idx].line_number = meta.hdr.header.line_number;
  frame->meta[anc_idx].hori_offset = meta.hdr.header.horizontal_offset;
  frame->meta[anc_idx].s = meta.hdr.header.s;
  frame->meta[anc_idx].stream_num = meta.hdr.header.stream_num;
  frame->meta[anc_idx].did = meta.did;
  frame->meta[anc_idx].sdid = meta.sdid;
  frame->meta[anc_idx].udw_size = meta.data_count;
  frame->meta[anc_idx].udw_offset = udw_offset;

  return GST_FLOW_OK;
}



static GstFlowReturn st40p_tx_parse_8331_gstbuffer(Gst_Mtl_St40p_Tx* sink,
                                              GstMapInfo map_info,
                                              gint* bytes_left_to_process,
                                              GstBuffer* buf,
                                              guint anc_count) {
  struct st40_frame_info* frame_info = NULL;
  /* Pointer used to navigate through the RFC8331 payload headers and data in the GstBuffer */
  uint8_t* payload_cursor, *udw_cursor;
  guint buffer_size = gst_buffer_get_size(buf);
  struct gst_st40_rfc8331_meta rfc8331_meta = {0};
  guint count_bytes_to_hold_udw, bytes_to_hold_udw = 0;
  guint16 udw;

  if (buffer_size < *bytes_left_to_process) {
    GST_ERROR("Buffer size (%u) is smaller than bytes left to process (%u)", buffer_size, *bytes_left_to_process);
    return GST_FLOW_ERROR; 
  }

  frame_info = st40p_tx_get_frame(sink->tx_handle);
  if (!frame_info) {
    GST_ERROR("Failed to get frame");
    return GST_FLOW_ERROR;
  }

  frame_info->anc_frame->data = frame_info->udw_buff_addr;

  count_bytes_to_hold_udw = 0;

  for (int i = 0; i < anc_count; i++) {
    /* Processing of the input 8331 header */
    if (*bytes_left_to_process < RFC_8331_WORD_BYTE_SIZE * 2) {
      GST_ERROR("Buffer size (%u) is too small to contain rfc8331 header (%lu)",
                *bytes_left_to_process, sizeof(struct st40_rfc8331_payload_hdr));
      return GST_FLOW_ERROR;
    }

    /* Get the ANC 8331 header and move the goalpost variables */
    payload_cursor = map_info.data + (buffer_size - *bytes_left_to_process);
    memcpy(&rfc8331_meta.hdr, payload_cursor, sizeof(struct gst_st40_rfc8331_hdr1_le));

    ST40P_TX_SHIFT_BUFFER(payload_cursor, *bytes_left_to_process, RFC_8331_WORD_BYTE_SIZE);
    /*TODO add support for parity bit checking we are ignoring the parity bits  */
    rfc8331_meta.did = st40_get_udw(0, payload_cursor) & 0xff;
    rfc8331_meta.sdid = st40_get_udw(1, payload_cursor) & 0xff;
    rfc8331_meta.data_count = st40_get_udw(2, payload_cursor) & 0xff;
    bytes_to_hold_udw = (rfc8331_meta.data_count * 10); // 10 bits per UDW
    bytes_to_hold_udw = (bytes_to_hold_udw + 7) / 8; // crude round up to byte size

    if (count_bytes_to_hold_udw + bytes_to_hold_udw > frame_info->udw_buffer_size) {
      GST_ERROR("UDW buffer address (%p) exceeds maximum allowed size (%u)",
                frame_info->udw_buff_addr + count_bytes_to_hold_udw, sink->max_combined_udw_size);
      return GST_FLOW_ERROR;
    } else {
      udw_cursor = frame_info->udw_buff_addr + count_bytes_to_hold_udw;
      count_bytes_to_hold_udw += bytes_to_hold_udw;
    }

    if (bytes_to_hold_udw > *bytes_left_to_process) {
      GST_ERROR("Buffer size (%u) is too small to contain rfc8331 payload (%u)",
                *bytes_left_to_process, bytes_to_hold_udw);
      return GST_FLOW_ERROR;
    }

    if (st40p_tx_parse_8331_meta(frame_info->anc_frame, rfc8331_meta, i, payload_cursor, 0)) {
      GST_ERROR("Failed to fill RFC 8331 meta");
      return GST_FLOW_ERROR;
    }

    if (st40p_tx_parse_8331_meta(frame_info->anc_frame, rfc8331_meta, i, payload_cursor, 0)) {
      GST_ERROR("Failed to fill RFC 8331 meta");
      return GST_FLOW_ERROR;
    }

      /*
    * Skip the first three UDW entries:
    * - 0th UDW: DID (Data Identifier)
    * - 1st UDW: SDID (Secondary Data Identifier)
    * - 2nd UDW: Data_Count (number of user data words)
    * Start processing actual user data words from the 3rd UDW onward.
    */
    for (int j = 0; j < rfc8331_meta.data_count; j++) {
      udw = st40_get_udw((j + 3), payload_cursor);
      st40_set_udw(j ,udw, udw_cursor);
    }

    if (sink->use_pts_for_pacing) {
      frame_info->timestamp = GST_BUFFER_PTS(buf) += sink->pts_for_pacing_offset;
      frame_info->tfmt = ST10_TIMESTAMP_FMT_TAI;
    }

    /* calculate the shift via calculating the size of UDW and checksum */
    bytes_to_hold_udw = ((rfc8331_meta.data_count + 1) * 10);
    bytes_to_hold_udw = (bytes_to_hold_udw + 7) / 8;
    ST40P_TX_SHIFT_BUFFER(payload_cursor, *bytes_left_to_process, bytes_to_hold_udw + RFC_8331_WORD_BYTE_SIZE);

    /* word align */
    if (*bytes_left_to_process > 0)
      ST40P_TX_SHIFT_BUFFER(payload_cursor, *bytes_left_to_process, *bytes_left_to_process % RFC_8331_WORD_BYTE_SIZE);

  }

  frame_info->anc_frame->data_size = count_bytes_to_hold_udw;
  if (st40p_tx_put_frame(sink->tx_handle, frame_info)) {
    GST_ERROR("Failed to put frame");
    return GST_FLOW_ERROR;
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
  gint bytes_left_to_process;
  GstFlowReturn ret = GST_FLOW_OK;
  guint8 rfc8331_f, rfc8331_anc_count;

  if (!sink->tx_handle) {
    GST_ERROR("Tx handle not initialized");
    return GST_FLOW_ERROR;
  }

  for (int i = 0; i < buffer_n; i++) {
    bytes_left_to_process = gst_buffer_get_size(buf);
    gst_buffer_memory = gst_buffer_peek_memory(buf, i);

    if (!gst_memory_map(gst_buffer_memory, &map_info, GST_MAP_READ)) {
      GST_ERROR("Failed to map memory");
      return GST_FLOW_ERROR;
    }

    if (sink->parse_8331_meta_from_gstbuffer) {
      if (bytes_left_to_process < RFC_8331_WORD_BYTE_SIZE) {
        GST_ERROR("Buffer too small for rfc8331 header");
        gst_memory_unmap(gst_buffer_memory, &map_info);
        gst_buffer_unref(buf);
        return GST_FLOW_ERROR;
      }

      rfc8331_anc_count = map_info.data[0];
      /* next 2 bits are the f field 0b00000111 */
      rfc8331_f = (map_info.data[1] & 0x03);

      /* ignore an ANC data packet with an F field value of 0b01 */
      if (rfc8331_f == 1){
        gst_memory_unmap(gst_buffer_memory, &map_info);
        continue;
      /* TODO: Support */
      } else if (rfc8331_f != 0) {
        GST_ERROR("Unsupported F field value: 0b%d", rfc8331_f);
        gst_memory_unmap(gst_buffer_memory, &map_info);
        gst_buffer_unref(buf);
        return GST_FLOW_ERROR;
      }

      bytes_left_to_process -= RFC_8331_WORD_BYTE_SIZE;
      ret = st40p_tx_parse_8331_gstbuffer(sink, map_info, &bytes_left_to_process, buf, rfc8331_anc_count);

      if (ret) {
        GST_ERROR("Failed to parse 8331 gst buffer %d", ret);
        gst_memory_unmap(gst_buffer_memory, &map_info);
        gst_buffer_unref(buf);
        return ret;
      } else if (bytes_left_to_process) {
        GST_WARNING("Leftover bytes in the buffer %d", bytes_left_to_process);
      }
    }

    /* main processing the GSTbuffer into the anc frames omit if parsing 8331 gst_buffer */
    while (ret != GST_FLOW_OK && bytes_left_to_process > 0 && !sink->parse_8331_meta_from_gstbuffer)
      ret = st40p_tx_parse_gstbuffer(sink, map_info, &bytes_left_to_process, buf);

    if (ret) {
      GST_ERROR("Failed to parse gst buffer %d", ret);
      gst_memory_unmap(gst_buffer_memory, &map_info);
      gst_buffer_unref(buf);
      return ret;
    }

    gst_memory_unmap(gst_buffer_memory, &map_info);
  }
  gst_buffer_unref(buf);
  return ret;
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
