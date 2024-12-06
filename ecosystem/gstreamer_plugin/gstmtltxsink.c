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

#include "gstmtltxsink.h"

GST_DEBUG_CATEGORY_STATIC(gst_mtltxsink_debug);
#define GST_CAT_DEFAULT gst_mtltxsink_debug
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
#ifndef PACKAGE
#define PACKAGE "gst-mtl-tx-st20"
#endif
#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "1.19.0.1"
#endif

enum {
  PROP_0,
  PROP_SILENT,
  PROP_TX_DEV_ARGS_PORT,
  PROP_TX_DEV_ARGS_SIP,
  PROP_TX_DEV_ARGS_DMA_DEV,
  PROP_TX_PORT_PORT,
  PROP_TX_PORT_IP,
  PROP_TX_PORT_UDP_PORT,
  PROP_TX_PORT_PAYLOAD_TYPE,
  PROP_TX_PORT_TX_QUEUES,
  PROP_MAX
};

/* pad template */
static GstStaticPadTemplate gst_mtltxsink_sink_pad_template =
    GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
                            GST_STATIC_CAPS("video/x-raw, "
                                            "format = (string) {v210, I422_10LE},"
                                            "width = (int) [64, 16384], "
                                            "height = (int) [64, 8704], "
                                            "framerate = (fraction) [0, MAX]"));

#define gst_mtltxsink_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE(GstMtlTxSink, gst_mtltxsink, GST_TYPE_VIDEO_SINK,
                        GST_DEBUG_CATEGORY_INIT(gst_mtltxsink_debug, "mtltxsink", 0,
                                                "MTL St2110 st20 transmission sink"));

GST_ELEMENT_REGISTER_DEFINE(mtltxsink, "mtltxsink", GST_RANK_NONE, GST_TYPE_MTL_TX_SINK);

static void gst_mtltxsink_set_property(GObject* object, guint prop_id,
                                       const GValue* value, GParamSpec* pspec);
static void gst_mtltxsink_get_property(GObject* object, guint prop_id, GValue* value,
                                       GParamSpec* pspec);

static gboolean gst_mtltxsink_sink_event(GstPad* pad, GstObject* parent, GstEvent* event);
static GstFlowReturn gst_mtltxsink_chain(GstPad* pad, GstObject* parent, GstBuffer* buf);
static gboolean gst_mtltxsink_query(GstPad* pad, GstObject* parent, GstQuery* query);

static gboolean gst_mtltxsink_start(GstBaseSink* bsink);
static gboolean gst_mtltxsink_stop(GstBaseSink* bsink);

static gboolean gst_mtltxsink_parse_input_fmt(GstVideoInfo* info, enum st_frame_fmt* fmt);
static gboolean gst_mtltxsink_parse_fps(GstVideoInfo* info, enum st_fps* fps);
static struct st_frame* gst_mtltxsink_get_frame(GstMtlTxSink* sink);

static gboolean gst_mtltxsink_parse_input_fmt(GstVideoInfo* info,
                                              enum st_frame_fmt* fmt) {
  GstVideoFormatInfo* finfo = info->finfo;

  if (finfo->format == GST_VIDEO_FORMAT_v210) {
    *fmt = ST_FRAME_FMT_V210;
  } else if (finfo->format == GST_VIDEO_FORMAT_I420_10LE) {
    *fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
  } else {
    return FALSE;
  }

  return TRUE;
}

// TODO add support for the partial fps
static gboolean gst_mtltxsink_parse_fps(GstVideoInfo* info, enum st_fps* fps) {
  gint fps_div;
  if (info->fps_n <= 0 || info->fps_d <= 0) {
    return FALSE;
  }

  fps_div = info->fps_n / info->fps_d;

  switch (fps_div) {
    case 24:
      *fps = ST_FPS_P24;
      break;
    case 25:
      *fps = ST_FPS_P25;
      break;
    case 30:
      *fps = ST_FPS_P30;
      break;
    case 50:
      *fps = ST_FPS_P50;
      break;
    case 60:
      *fps = ST_FPS_P60;
      break;
    case 120:
      *fps = ST_FPS_P120;
      break;
    default:
      return FALSE;
  }

  return TRUE;
}

static void gst_mtltxsink_class_init(GstMtlTxSinkClass* klass) {
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
                                            &gst_mtltxsink_sink_pad_template);

  gobject_class->set_property = GST_DEBUG_FUNCPTR(gst_mtltxsink_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR(gst_mtltxsink_get_property);
  gstvideosinkelement_class->parent_class.start = GST_DEBUG_FUNCPTR(gst_mtltxsink_start);
  gstvideosinkelement_class->parent_class.stop = GST_DEBUG_FUNCPTR(gst_mtltxsink_stop);

  g_object_class_install_property(
      gobject_class, PROP_SILENT,
      g_param_spec_boolean("silent", "Silent", "Turn on silent mode.", FALSE,
                           G_PARAM_READWRITE));

  g_object_class_install_property(
      gobject_class, PROP_TX_DEV_ARGS_PORT,
      g_param_spec_string("dev-port", "DPDK device port",
                          "DPDK port for synchronous ST 2110-20 uncompressed"
                          "video transmission, bound to the VFIO DPDK driver. ",
                          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_TX_DEV_ARGS_SIP,
      g_param_spec_string("dev-ip", "Local device IP",
                          "Local IP address that the port will be "
                          "identified by. This is the address from which ARP "
                          "responses will be sent.",
                          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_TX_DEV_ARGS_DMA_DEV,
      g_param_spec_string("dma-dev", "DPDK DMA port",
                          "DPDK port for the MTL direct memory functionality.", NULL,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_TX_PORT_PORT,
      g_param_spec_string("tx-port", "Transmission Device Port",
                          "DPDK device port initialized for the transmission.", NULL,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_TX_PORT_IP,
      g_param_spec_string("tx-ip", "Receiving node's IP",
                          "Receiving MTL node IP address.", NULL,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_TX_PORT_UDP_PORT,
      g_param_spec_uint("tx-udp-port", "Receiver's UDP port",
                        "Receiving MTL node UDP port.", 0, G_MAXUINT, 20000,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_TX_PORT_PAYLOAD_TYPE,
      g_param_spec_uint("tx-payload-type", "ST 2110 payload type",
                        "SMPTE ST 2110 payload type.", 0, G_MAXUINT, 112,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_TX_PORT_TX_QUEUES,
      g_param_spec_uint("tx-queues", "Number of TX queues",
                        "Number of TX queues to initialize in DPDK backend.", 0,
                        G_MAXUINT, 16, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static gboolean gst_mtltxsink_start(GstBaseSink* bsink) {
  struct mtl_init_params mtl_init_params = {0};
  gint ret;

  GstMtlTxSink* sink = GST_MTL_TX_SINK(bsink);

  GST_DEBUG_OBJECT(sink, "start");
  GST_DEBUG("Media Transport Initialization start");

  strncpy(mtl_init_params.port[MTL_PORT_P], sink->devArgs.port, MTL_PORT_MAX_LEN);

  ret = inet_pton(AF_INET, sink->devArgs.local_ip_string,
                  mtl_init_params.sip_addr[MTL_PORT_P]);
  if (ret != 1) {
    GST_ERROR("%s, sip %s is not valid ip address\n", __func__,
              sink->devArgs.local_ip_string);
    return false;
  }

  if (sink->devArgs.tx_queues_cnt[MTL_PORT_P]) {
    mtl_init_params.tx_queues_cnt[MTL_PORT_P] = sink->devArgs.tx_queues_cnt[MTL_PORT_P];
  } else {
    mtl_init_params.tx_queues_cnt[MTL_PORT_P] = 16;
  }

  mtl_init_params.rx_queues_cnt[MTL_PORT_P] = 0;
  mtl_init_params.num_ports++;

  mtl_init_params.flags |= MTL_FLAG_BIND_NUMA;
  if (sink->silent) {
    mtl_init_params.log_level = MTL_LOG_LEVEL_ERROR;
  } else {
    mtl_init_params.log_level = MTL_LOG_LEVEL_INFO;
  }

  sink->retry_frame = 10;

  mtl_init_params.flags |= MTL_FLAG_BIND_NUMA;

  if (sink->devArgs.dma_dev) {
    strncpy(mtl_init_params.dma_dev_port[0], sink->devArgs.dma_dev, MTL_PORT_MAX_LEN);
  }

  if (sink->mtl_lib_handle) {
    GST_ERROR("MTL already initialized");
    return false;
  }

  sink->mtl_lib_handle = mtl_init(&mtl_init_params);
  if (!sink->mtl_lib_handle) {
    GST_ERROR("Could not initialize MTL");
    return false;
  }

  return true;
}

static void gst_mtltxsink_init(GstMtlTxSink* sink) {
  GstElement* element = GST_ELEMENT(sink);
  GstPad* sinkpad;

  sinkpad = gst_element_get_static_pad(element, "sink");
  if (!sinkpad) {
    GST_ERROR_OBJECT(sink, "Failed to get sink pad from child element");
    return;
  }

  gst_pad_set_query_function(sinkpad, gst_mtltxsink_query);

  gst_pad_set_event_function(sinkpad, GST_DEBUG_FUNCPTR(gst_mtltxsink_sink_event));

  gst_pad_set_chain_function(sinkpad, GST_DEBUG_FUNCPTR(gst_mtltxsink_chain));
}

static gboolean gst_mtltxsink_query(GstPad* pad, GstObject* parent, GstQuery* query) {
  gboolean ret = FALSE;

  switch (GST_QUERY_TYPE(query)) {
    default:
      ret = gst_pad_query_default(pad, parent, query);
      break;
  }

  return ret;
}

static void gst_mtltxsink_set_property(GObject* object, guint prop_id,
                                       const GValue* value, GParamSpec* pspec) {
  GstMtlTxSink* self = GST_MTL_TX_SINK(object);

  switch (prop_id) {
    case PROP_SILENT:
      self->silent = g_value_get_boolean(value);
      break;
    case PROP_TX_DEV_ARGS_PORT:
      strncpy(self->devArgs.port, g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    case PROP_TX_DEV_ARGS_SIP:
      strncpy(self->devArgs.local_ip_string, g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    case PROP_TX_DEV_ARGS_DMA_DEV:
      strncpy(self->devArgs.dma_dev, g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    case PROP_TX_PORT_PORT:
      strncpy(self->portArgs.port, g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    case PROP_TX_PORT_IP:
      strncpy(self->portArgs.tx_ip_string, g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    case PROP_TX_PORT_UDP_PORT:
      self->portArgs.udp_port = g_value_get_uint(value);
      break;
    case PROP_TX_PORT_PAYLOAD_TYPE:
      self->portArgs.payload_type = g_value_get_uint(value);
      break;
    case PROP_TX_PORT_TX_QUEUES:
      self->devArgs.tx_queues_cnt[MTL_PORT_P] = g_value_get_uint(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void gst_mtltxsink_get_property(GObject* object, guint prop_id, GValue* value,
                                       GParamSpec* pspec) {
  GstMtlTxSink* sink = GST_MTL_TX_SINK(object);

  switch (prop_id) {
    case PROP_SILENT:
      g_value_set_boolean(value, sink->silent);
      break;
    case PROP_TX_DEV_ARGS_PORT:
      g_value_set_string(value, sink->devArgs.port);
      break;
    case PROP_TX_DEV_ARGS_SIP:
      g_value_set_string(value, sink->devArgs.local_ip_string);
      break;
    case PROP_TX_DEV_ARGS_DMA_DEV:
      g_value_set_string(value, sink->devArgs.dma_dev);
      break;
    case PROP_TX_PORT_PORT:
      g_value_set_string(value, sink->portArgs.port);
      break;
    case PROP_TX_PORT_IP:
      g_value_set_string(value, sink->portArgs.tx_ip_string);
      break;
    case PROP_TX_PORT_UDP_PORT:
      g_value_set_uint(value, sink->portArgs.udp_port);
      break;
    case PROP_TX_PORT_PAYLOAD_TYPE:
      g_value_set_uint(value, sink->portArgs.payload_type);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static gboolean gst_mtltxsink_sink_event(GstPad* pad, GstObject* parent,
                                         GstEvent* event) {
  GstMtlTxSink* sink;
  GstCaps* caps;
  GstVideoInfo* info;
  gint ret;
  struct st20p_tx_ops ops_tx = {0};

  sink = GST_MTL_TX_SINK(parent);

  GST_LOG_OBJECT(sink, "Received %s event: %" GST_PTR_FORMAT, GST_EVENT_TYPE_NAME(event),
                 event);

  ret = GST_EVENT_TYPE(event);

  switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_CAPS:
      gst_event_parse_caps(event, &caps);
      info = gst_video_info_new_from_caps(caps);
      ops_tx.name = "st20sink";
      ops_tx.device = ST_PLUGIN_DEVICE_AUTO;
      ops_tx.width = info->width;
      ops_tx.height = info->height;
      ops_tx.transport_fmt = ST20_FMT_YUV_422_10BIT;
      ops_tx.framebuff_cnt = 3;
      ops_tx.port.num_port++;
      ops_tx.interlaced = info->interlace_mode;
      ops_tx.flags |= ST20P_TX_FLAG_BLOCK_GET;

      if (!gst_mtltxsink_parse_input_fmt(info, &ops_tx.input_fmt)) {
        GST_ERROR("Failed to parse input format");
        return FALSE;
      }

      if (!gst_mtltxsink_parse_fps(info, &ops_tx.fps)) {
        GST_ERROR("Failed to parse fps");
        return FALSE;
      }

      if (inet_pton(AF_INET, sink->portArgs.tx_ip_string,
                    ops_tx.port.dip_addr[MTL_PORT_P]) != 1) {
        GST_ERROR("Invalid destination IP address: %s", sink->portArgs.tx_ip_string);
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
        GST_ERROR("%s, invalid payload_type: %d\n", __func__,
                  sink->portArgs.payload_type);
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
      ret = gst_pad_event_default(pad, parent, event);
      break;
    case GST_EVENT_SEGMENT:
      if (!sink->tx_handle) {
        GST_ERROR("Tx handle not initialized");
        return FALSE;
      }
      ret = gst_pad_event_default(pad, parent, event);
      break;
    case GST_EVENT_EOS:
      gst_element_set_state(GST_ELEMENT(sink), GST_STATE_CHANGE_READY_TO_NULL);
      gst_mtltxsink_stop(GST_BASE_SINK(sink));
      ret = gst_pad_event_default(pad, parent, event);
      gst_element_set_state(GST_ELEMENT(sink), GST_STATE_NULL);
      break;
    default:
      ret = gst_pad_event_default(pad, parent, event);
      break;
  }

  return ret;
}

/*
 * Sets the state of the pipeline to playing if the frame buffers are not
 * available from the MTL st20p_tx_get_frame.
 */
static struct st_frame* gst_mtltxsink_get_frame(GstMtlTxSink* sink) {
  struct st_frame** frame = &sink->frame_in_tranmission;
  gint* frame_progress = &sink->frame_in_tranmission_data_pointer;
  if (*frame) {
    return *frame;
  }

  *frame = st20p_tx_get_frame(sink->tx_handle);

  if (!*frame) {
    /* processing the frames in mtl halt the sink */
    gst_element_set_state(GST_ELEMENT(sink), GST_STATE_PLAYING);
    GST_WARNING("Get frame timeout");

    for (int j = 0; j < sink->retry_frame; j++) {
      *frame = st20p_tx_get_frame(sink->tx_handle);
      if (*frame) {
        /* pipeline can resume as the framebuffers are available again */
        gst_element_set_state(GST_ELEMENT(sink), GST_STATE_PAUSED);
        return *frame;
      }
    }

    if (!*frame) {
      GST_ERROR("Failed to get frame");
      sink->wait_for_frame = true;
      return NULL;
    }

  } else if (sink->wait_for_frame) {
    sink->wait_for_frame = false;
    gst_element_set_state(GST_ELEMENT(sink), GST_STATE_PAUSED);
  } else if (*frame_progress) {
    GST_ERROR("Frame progress missmatch");
    /* reset the progression state and hope for the best */
    *frame_progress = 0;
  }

  return *frame;
}

/*
 * Takes the buffer from the source pad and sends it to the mtl library via
 * frame buffers, supports incomplete frames. But buffers needs to add up to the
 * actual frame size.
 */
static GstFlowReturn gst_mtltxsink_chain(GstPad* pad, GstObject* parent, GstBuffer* buf) {
  void* frame_buffer_pointer;
  GstMtlTxSink* sink = GST_MTL_TX_SINK(parent);
  gint buffer_size = gst_buffer_get_size(buf);
  gint buffer_n = gst_buffer_n_memory(buf);
  struct st_frame** frame = &sink->frame_in_tranmission;
  gint* frame_progress = &sink->frame_in_tranmission_data_pointer;
  gint frame_size = sink->frame_size;
  void* gst_buffer_memory;

  if (!sink->tx_handle) {
    GST_ERROR("Tx handle not initialized");
    return GST_FLOW_ERROR;
  }

  for (int i = 0; i < buffer_n; i++) {
    gst_buffer_memory = gst_buffer_peek_memory(buf, i);

    *frame = gst_mtltxsink_get_frame(sink);
    if (!*frame) {
      GST_ERROR("Failed to get frame");
      return GST_FLOW_ERROR;
    }

    if ((buffer_size + *frame_progress) > frame_size) {
      GST_ERROR("Frame size mismatch");
      st20p_tx_put_frame(sink->tx_handle, *frame);
      *frame = NULL;
      *frame_progress = 0;
      return GST_FLOW_ERROR;
    }

    frame_buffer_pointer = (*frame)->addr[0] + *frame_progress;
    *frame_progress += buffer_size;

    mtl_memcpy(frame_buffer_pointer, gst_buffer_memory, buffer_size);
    if (*frame_progress == frame_size) {
      st20p_tx_put_frame(sink->tx_handle, *frame);
      *frame = NULL;
      *frame_progress = 0;
    }
  }
  return GST_FLOW_OK;
}

static gboolean gst_mtltxsink_stop(GstBaseSink* bsink) {
  GstMtlTxSink* sink = GST_MTL_TX_SINK(bsink);
  if (sink->frame_in_tranmission) {
    st20p_tx_put_frame(sink->tx_handle, sink->frame_in_tranmission);
    sink->frame_in_tranmission = NULL;
  }

  if (sink->tx_handle) {
    st20p_tx_free(sink->tx_handle);
    sink->tx_handle = NULL;
  }

  if (sink->mtl_lib_handle) {
    mtl_stop(sink->mtl_lib_handle);
    mtl_uninit(sink->mtl_lib_handle);
    sink->mtl_lib_handle = NULL;
  }

  return true;
}

static gboolean plugin_init(GstPlugin* mtltxsink) {
  return gst_element_register(mtltxsink, "mtltxsink", GST_RANK_SECONDARY,
                              GST_TYPE_MTL_TX_SINK);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, mtltxsink,
                  "software-based solution designed for high-throughput transmission",
                  plugin_init, PACKAGE_VERSION, GST_LICENSE, GST_PACKAGE_NAME,
                  GST_PACKAGE_ORIGIN)
