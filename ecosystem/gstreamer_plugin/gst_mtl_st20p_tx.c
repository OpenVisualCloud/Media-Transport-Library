/*
 * GStreamer
 * Copyright (C) 2005 Thomas Vander Stichele <thomas@apestaart.org>
 * Copyright (C) 2005 Ronald S. Bultje <rbultje@ronald.bitfreak.net>
 * Copyright (C) 2024 Intel Corporation
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

#include "gst_mtl_st20p_tx.h"

GST_DEBUG_CATEGORY_STATIC(gst_mtl_st20p_tx_debug);
#define GST_CAT_DEFAULT gst_mtl_st20p_tx_debug
#ifndef GST_LICENSE
#define GST_LICENSE "LGPL"
#endif
#ifndef GST_API_VERSION
#define GST_API_VERSION "1.0"
#endif
#ifndef GST_PACKAGE_NAME
#define GST_PACKAGE_NAME "Media Transport Library st2110 st20 tx plugin"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://github.com/OpenVisualCloud/Media-Transport-Library"
#endif
#ifndef PACKAGEf
#define PACKAGE "gst-mtl-st20p-tx"
#endif
#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "1.0"
#endif

enum {
  PROP_ST20P_TX_RETRY = PROP_GENERAL_MAX,
  PROP_ST20P_TX_FRAMERATE,
  PROP_ST20P_TX_FRAMEBUFF_NUM,
  PROP_MAX
};

/* pad template */
static GstStaticPadTemplate gst_mtl_st20p_tx_sink_pad_template =
    GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
                            GST_STATIC_CAPS("video/x-raw, "
                                            "format = (string) {v210, I422_10LE},"
                                            "width = (int) [64, 16384], "
                                            "height = (int) [64, 8704], "
                                            "framerate = (fraction) [0, MAX]"));

#define gst_mtl_st20p_tx_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE(Gst_Mtl_St20p_Tx, gst_mtl_st20p_tx, GST_TYPE_VIDEO_SINK,
                        GST_DEBUG_CATEGORY_INIT(gst_mtl_st20p_tx_debug, "mtl_st20p_tx", 0,
                                                "MTL St2110 st20 transmission sink"));

GST_ELEMENT_REGISTER_DEFINE(mtl_st20p_tx, "mtl_st20p_tx", GST_RANK_NONE,
                            GST_TYPE_MTL_ST20P_TX);

static void gst_mtl_st20p_tx_set_property(GObject* object, guint prop_id,
                                          const GValue* value, GParamSpec* pspec);
static void gst_mtl_st20p_tx_get_property(GObject* object, guint prop_id, GValue* value,
                                          GParamSpec* pspec);
static void gst_mtl_st20p_tx_finalize(GObject* object);

static gboolean gst_mtl_st20p_tx_sink_event(GstPad* pad, GstObject* parent,
                                            GstEvent* event);
static GstFlowReturn gst_mtl_st20p_tx_chain(GstPad* pad, GstObject* parent,
                                            GstBuffer* buf);

static gboolean gst_mtl_st20p_tx_start(GstBaseSink* bsink);

static void gst_mtl_st20p_tx_class_init(Gst_Mtl_St20p_TxClass* klass) {
  GObjectClass* gobject_class;
  GstElementClass* gstelement_class;
  GstVideoSinkClass* gstvideosinkelement_class;

  gobject_class = G_OBJECT_CLASS(klass);
  gstelement_class = GST_ELEMENT_CLASS(klass);
  gstvideosinkelement_class = GST_VIDEO_SINK_CLASS(klass);

  gst_element_class_set_metadata(
      gstelement_class, "MtlTxSt20Sink", "Sink/Video",
      "MTL transmission plugin for SMPTE ST 2110-20 standard (uncompressed video)",
      "Dawid Wesierski <dawid.wesierski@intel.com>");

  gst_element_class_add_static_pad_template(gstelement_class,
                                            &gst_mtl_st20p_tx_sink_pad_template);

  gobject_class->set_property = GST_DEBUG_FUNCPTR(gst_mtl_st20p_tx_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR(gst_mtl_st20p_tx_get_property);
  gobject_class->finalize = GST_DEBUG_FUNCPTR(gst_mtl_st20p_tx_finalize);
  gstvideosinkelement_class->parent_class.start =
      GST_DEBUG_FUNCPTR(gst_mtl_st20p_tx_start);

  gst_mtl_common_init_general_argumetns(gobject_class);

  g_object_class_install_property(
      gobject_class, PROP_ST20P_TX_RETRY,
      g_param_spec_uint("retry", "Retry Count",
                        "Number of times the MTL will try to get a frame.", 0, G_MAXUINT,
                        10, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST20P_TX_FRAMERATE,
      g_param_spec_uint("tx-fps", "Video framerate", "Framerate of the video.", 0,
                        G_MAXUINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST20P_TX_FRAMEBUFF_NUM,
      g_param_spec_uint("tx-framebuff-num", "Number of framebuffers",
                        "Number of framebuffers to be used for transmission.", 0,
                        G_MAXUINT, 3, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static gboolean gst_mtl_st20p_tx_start(GstBaseSink* bsink) {
  struct mtl_init_params mtl_init_params = {0};

  Gst_Mtl_St20p_Tx* sink = GST_MTL_ST20P_TX(bsink);

  GST_DEBUG_OBJECT(sink, "start");
  GST_DEBUG("Media Transport Initialization start");
  gst_base_sink_set_async_enabled(bsink, FALSE);

  sink->mtl_lib_handle =
      gst_mtl_common_init_handle(&mtl_init_params, &(sink->devArgs), &(sink->log_level));

  if (!sink->mtl_lib_handle) {
    GST_ERROR("Could not initialize MTL");
    return FALSE;
  }

  if (sink->retry_frame == 0)
    sink->retry_frame = 10;
  else if (sink->retry_frame < 3) {
    GST_WARNING("Retry count is too low, setting to 3");
    sink->retry_frame = 3;
  }

  gst_element_set_state(GST_ELEMENT(sink), GST_STATE_PLAYING);

  return true;
}

static void gst_mtl_st20p_tx_init(Gst_Mtl_St20p_Tx* sink) {
  GstElement* element = GST_ELEMENT(sink);
  GstPad* sinkpad;

  sinkpad = gst_element_get_static_pad(element, "sink");
  if (!sinkpad) {
    GST_ERROR_OBJECT(sink, "Failed to get sink pad from child element");
    return;
  }

  gst_pad_set_event_function(sinkpad, GST_DEBUG_FUNCPTR(gst_mtl_st20p_tx_sink_event));

  gst_pad_set_chain_function(sinkpad, GST_DEBUG_FUNCPTR(gst_mtl_st20p_tx_chain));
}

static void gst_mtl_st20p_tx_set_property(GObject* object, guint prop_id,
                                          const GValue* value, GParamSpec* pspec) {
  Gst_Mtl_St20p_Tx* self = GST_MTL_ST20P_TX(object);

  if (prop_id < PROP_GENERAL_MAX) {
    gst_mtl_common_set_general_argumetns(object, prop_id, value, pspec, &(self->devArgs),
                                         &(self->portArgs), &self->log_level);
    return;
  }

  switch (prop_id) {
    case PROP_ST20P_TX_RETRY:
      self->retry_frame = g_value_get_uint(value);
      break;
    case PROP_ST20P_TX_FRAMERATE:
      self->framerate = g_value_get_uint(value);
      break;
    case PROP_ST20P_TX_FRAMEBUFF_NUM:
      self->framebuffer_num = g_value_get_uint(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void gst_mtl_st20p_tx_get_property(GObject* object, guint prop_id, GValue* value,
                                          GParamSpec* pspec) {
  Gst_Mtl_St20p_Tx* sink = GST_MTL_ST20P_TX(object);

  if (prop_id < PROP_GENERAL_MAX) {
    gst_mtl_common_get_general_argumetns(object, prop_id, value, pspec, &(sink->devArgs),
                                         &(sink->portArgs), &sink->log_level);
    return;
  }

  switch (prop_id) {
    case PROP_ST20P_TX_RETRY:
      g_value_set_uint(value, sink->retry_frame);
      break;
    case PROP_ST20P_TX_FRAMERATE:
      g_value_set_uint(value, sink->framerate);
      break;
    case PROP_ST20P_TX_FRAMEBUFF_NUM:
      g_value_set_uint(value, sink->framebuffer_num);
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
static gboolean gst_mtl_st20p_tx_session_create(Gst_Mtl_St20p_Tx* sink, GstCaps* caps) {
  GstVideoInfo* info;
  struct st20p_tx_ops ops_tx = {0};
  gint ret;

  if (!sink->mtl_lib_handle) {
    GST_ERROR("MTL library not initialized");
    return FALSE;
  }

  info = gst_video_info_new_from_caps(caps);
  ops_tx.name = "st20sink";
  ops_tx.device = ST_PLUGIN_DEVICE_AUTO;
  ops_tx.width = info->width;
  ops_tx.height = info->height;
  ops_tx.transport_fmt = ST20_FMT_YUV_422_10BIT;
  ops_tx.port.num_port = 1;
  ops_tx.flags |= ST20P_TX_FLAG_BLOCK_GET;

  if (sink->framebuffer_num) {
    ops_tx.framebuff_cnt = sink->framebuffer_num;
  } else {
    ops_tx.framebuff_cnt = 3;
  }

  if (info->interlace_mode == GST_VIDEO_INTERLACE_MODE_INTERLEAVED) {
    ops_tx.interlaced = true;
  } else if (info->interlace_mode) {
    GST_ERROR("Unsupported interlace mode");
    return FALSE;
  }

  if (!gst_mtl_common_parse_input_finfo(info->finfo, &ops_tx.input_fmt)) {
    GST_ERROR("Failed to parse input format");
    return FALSE;
  }

  if (sink->framerate) {
    if (!gst_mtl_common_parse_fps_code(sink->framerate, &ops_tx.fps)) {
      GST_ERROR("Failed to parse custom ops_tx fps code %d", sink->framerate);
      return FALSE;
    }
  } else if (!gst_mtl_common_parse_fps(info, &ops_tx.fps)) {
    GST_ERROR("Failed to parse fps");
    return FALSE;
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
  gst_video_info_free(info);

  ret = mtl_start(sink->mtl_lib_handle);
  if (ret < 0) {
    GST_ERROR("Failed to start MTL library");
    return FALSE;
  }

  sink->tx_handle = st20p_tx_create(sink->mtl_lib_handle, &ops_tx);
  if (!sink->tx_handle) {
    GST_ERROR("Failed to create st20p tx handle");
    return FALSE;
  }

  sink->frame_size = st20p_tx_frame_size(sink->tx_handle);
  return TRUE;
}

static gboolean gst_mtl_st20p_tx_sink_event(GstPad* pad, GstObject* parent,
                                            GstEvent* event) {
  Gst_Mtl_St20p_Tx* sink;
  GstCaps* caps;
  gint ret;

  sink = GST_MTL_ST20P_TX(parent);

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
    case GST_EVENT_CAPS:
      gst_event_parse_caps(event, &caps);
      ret = gst_mtl_st20p_tx_session_create(sink, caps);
      if (!ret) {
        GST_ERROR("Failed to create TX session");
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

/*
 * Takes the buffer from the source pad and sends it to the mtl library via
 * frame buffers, supports incomplete frames. But buffers needs to add up to the
 * actual frame size.
 */
static GstFlowReturn gst_mtl_st20p_tx_chain(GstPad* pad, GstObject* parent,
                                            GstBuffer* buf) {
  Gst_Mtl_St20p_Tx* sink = GST_MTL_ST20P_TX(parent);
  gint buffer_size = gst_buffer_get_size(buf);
  gint buffer_n = gst_buffer_n_memory(buf);
  struct st_frame* frame = NULL;
  gint frame_size = sink->frame_size;
  GstMemory* gst_buffer_memory;
  GstMapInfo map_info;

  if (!sink->tx_handle) {
    GST_ERROR("Tx handle not initialized");
    return GST_FLOW_ERROR;
  }

  if (buffer_size != frame_size) {
    GST_ERROR("Buffer size %d does not match frame size %d", buffer_size, frame_size);
    return GST_FLOW_ERROR;
  }

  for (int i = 0; i < buffer_n; i++) {
    gst_buffer_memory = gst_buffer_peek_memory(buf, i);

    if (!gst_memory_map(gst_buffer_memory, &map_info, GST_MAP_READ)) {
      GST_ERROR("Failed to map memory");
      return GST_FLOW_ERROR;
    }

    frame = st20p_tx_get_frame(sink->tx_handle);
    if (!frame) {
      GST_ERROR("Failed to get frame");
      return GST_FLOW_ERROR;
    }

    mtl_memcpy(frame->addr[0], map_info.data, buffer_size);
    gst_memory_unmap(gst_buffer_memory, &map_info);
    st20p_tx_put_frame(sink->tx_handle, frame);
  }
  gst_buffer_unref(buf);
  return GST_FLOW_OK;
}

static void gst_mtl_st20p_tx_finalize(GObject* object) {
  Gst_Mtl_St20p_Tx* sink = GST_MTL_ST20P_TX(object);

  if (sink->tx_handle) {
    if (st20p_tx_free(sink->tx_handle)) {
      GST_ERROR("Failed to free tx handle");
      return;
    }
  }

  if (sink->mtl_lib_handle) {
    if (mtl_stop(sink->mtl_lib_handle) || mtl_uninit(sink->mtl_lib_handle)) {
      GST_ERROR("Failed to uninitialize MTL library");
      return;
    }
  }
}

static gboolean plugin_init(GstPlugin* mtl_st20p_tx) {
  return gst_element_register(mtl_st20p_tx, "mtl_st20p_tx", GST_RANK_SECONDARY,
                              GST_TYPE_MTL_ST20P_TX);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, mtl_st20p_tx,
                  "software-based solution designed for high-throughput transmission",
                  plugin_init, PACKAGE_VERSION, GST_LICENSE, GST_PACKAGE_NAME,
                  GST_PACKAGE_ORIGIN)
