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

/* Maximum size for single User Data Words defined in ST 0291-1  */
#define MAX_UDW_SIZE 255

enum {
  PROP_ST40P_TX_FRAMEBUFF_CNT = PROP_GENERAL_MAX,
  PROP_ST40P_TX_FRAMERATE,
  PROP_ST40P_TX_DID,
  PROP_ST40P_TX_SDID,
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
}

static gboolean gst_mtl_st40p_tx_start(GstBaseSink* bsink) {
  Gst_Mtl_St40p_Tx* sink = GST_MTL_ST40P_TX(bsink);

  GST_DEBUG_OBJECT(sink, "start");
  GST_DEBUG("Media Transport Initialization start");
  gst_base_sink_set_async_enabled(bsink, FALSE);

  sink->mtl_lib_handle =
      gst_mtl_common_init_handle(&(sink->devArgs), &(sink->log_level), FALSE);

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
    gst_mtl_common_set_general_arguments(object, prop_id, value, pspec, &(self->devArgs),
                                         &(self->portArgs), &self->log_level);
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
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void gst_mtl_st40p_tx_get_property(GObject* object, guint prop_id, GValue* value,
                                          GParamSpec* pspec) {
  Gst_Mtl_St40p_Tx* sink = GST_MTL_ST40P_TX(object);

  if (prop_id < PROP_GENERAL_MAX) {
    gst_mtl_common_get_general_arguments(object, prop_id, value, pspec, &(sink->devArgs),
                                         &(sink->portArgs), &sink->log_level);
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
  ops_tx.port.num_port = 1;
  if (sink->framebuff_cnt) {
    ops_tx.framebuff_cnt = sink->framebuff_cnt;
  } else {
    ops_tx.framebuff_cnt = 3;
  }
  if (inet_pton(AF_INET, sink->portArgs.session_ip_string,
                ops_tx.port.dip_addr[MTL_PORT_P]) != 1) {
    GST_ERROR("Invalid destination IP address: %s", sink->portArgs.session_ip_string);
    return FALSE;
  }

  if (strlen(sink->portArgs.port) == 0) {
    strncpy(ops_tx.port.port[MTL_PORT_P], sink->devArgs.port, MTL_PORT_MAX_LEN);
  } else {
    strncpy(ops_tx.port.port[MTL_PORT_P], sink->portArgs.port, MTL_PORT_MAX_LEN);
  }

  if ((sink->portArgs.udp_port < 0) || (sink->portArgs.udp_port > 0xFFFF)) {
    GST_ERROR("%s, invalid UDP port: %d\n", __func__, sink->portArgs.udp_port);
    return FALSE;
  }

  ops_tx.port.udp_port[0] = sink->portArgs.udp_port;

  if ((sink->portArgs.payload_type < 0) || (sink->portArgs.payload_type > 0x7F)) {
    GST_ERROR("%s, invalid payload_type: %d\n", __func__, sink->portArgs.payload_type);
    return FALSE;
  }
  ops_tx.port.payload_type = sink->portArgs.payload_type;

  if (sink->did > 0xFF) {
    GST_ERROR("Invalid DID value: %d", sink->did);
    return FALSE;
  }
  if (sink->sdid > 0xFF) {
    GST_ERROR("Invalid SDID value: %d", sink->sdid);
    return FALSE;
  }

  ops_tx.fps = st_frame_rate_to_st_fps((double)sink->fps_n / sink->fps_d);
  if (ops_tx.fps == ST_FPS_MAX) {
    GST_ERROR("Invalid framerate: %d/%d", sink->fps_n, sink->fps_d);
    return FALSE;
  }

  ops_tx.interlaced = false;
  /* Only single ANC data packet is possible per frame. ANC_Count = 1 TODO: allow more */
  sink->frame_size = MAX_UDW_SIZE;
  ops_tx.max_udw_buff_size = MAX_UDW_SIZE;

  ops_tx.flags |= ST30P_TX_FLAG_BLOCK_GET;
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
 * Note: Most of the fields are hardcoded, and only first metadata block is filled as
 * only one ANC Data packet per frame is allowed.
 *
 * @param frame Pointer to the st40_frame structure to be filled. Cannot be NULL.
 * @param data Pointer to the data to be associated with the frame.
 * @param data_size Size of the data in bytes.
 * @param did Data Identifier (DID) to be set in the metadata.
 * @param sdid Secondary Data Identifier (SDID) to be set in the metadata.
 */
static void fill_st40_meta(struct st40_frame* frame, void* data, guint32 data_size,
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

/*
 * Takes the buffer from the source pad and sends it to the mtl library via
 * frame buffers, supports incomplete frames. But buffers needs to add up to the
 * actual frame size.
 */
static GstFlowReturn gst_mtl_st40p_tx_chain(GstPad* pad, GstObject* parent,
                                            GstBuffer* buf) {
  Gst_Mtl_St40p_Tx* sink = GST_MTL_ST40P_TX(parent);
  gint buffer_n = gst_buffer_n_memory(buf);
  struct st40_frame_info* frame_info = NULL;
  GstMemory* gst_buffer_memory;
  GstMapInfo map_info;
  gint bytes_to_write, bytes_to_write_cur;
  void* cur_addr_buf;

  if (!sink->tx_handle) {
    GST_ERROR("Tx handle not initialized");
    return GST_FLOW_ERROR;
  }

  for (int i = 0; i < buffer_n; i++) {
    bytes_to_write = gst_buffer_get_size(buf);
    gst_buffer_memory = gst_buffer_peek_memory(buf, i);

    if (!gst_memory_map(gst_buffer_memory, &map_info, GST_MAP_READ)) {
      GST_ERROR("Failed to map memory");
      return GST_FLOW_ERROR;
    }

    while (bytes_to_write > 0) {
      frame_info = st40p_tx_get_frame(sink->tx_handle);
      if (!frame_info) {
        GST_ERROR("Failed to get frame");
        return GST_FLOW_ERROR;
      }
      cur_addr_buf = map_info.data + gst_buffer_get_size(buf) - bytes_to_write;
      bytes_to_write_cur =
          bytes_to_write > sink->frame_size ? sink->frame_size : bytes_to_write;
      mtl_memcpy(frame_info->udw_buff_addr, cur_addr_buf, bytes_to_write_cur);
      fill_st40_meta(frame_info->anc_frame, frame_info->udw_buff_addr, bytes_to_write_cur,
                     sink->did, sink->sdid);
      st40p_tx_put_frame(sink->tx_handle, frame_info);
      bytes_to_write -= bytes_to_write_cur;
    }
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
    if (gst_mtl_common_deinit_handle(sink->mtl_lib_handle)) {
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
