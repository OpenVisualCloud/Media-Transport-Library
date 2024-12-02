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
 * This element allows GStreamer pipelines to send media data using the MTL framework, ensuring efficient and 
 * reliable media transport over IP networks.
 *
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gst/gst.h>

#include "gstmtltxsink.h"

GST_DEBUG_CATEGORY_STATIC (gst_mtltxsink_debug);
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

#include <stdio.h> // FIXME Delete
FILE *file; // FIXME Delete
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  PROP_0,
  PROP_SILENT,
  PROP_TX_DEV_ARGS_PORT,
  PROP_TX_DEV_ARGS_SIP,
  PROP_TX_DEV_ARGS_DMA_DEV,
  PROP_TX_PORT_PORT,
  PROP_TX_PORT_IP,
  PROP_TX_PORT_UDP_PORT,
  PROP_TX_PORT_PAYLOAD_TYPE,
  PROP_MAX
};

/* pad template */
static GstStaticPadTemplate gst_mtltxsink_sink_pad_template =
  GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw, "
                    "format = (string) Y42B, "
                    "width = (int) [64, 16384], "
                    "height = (int) [64, 8704], "
                    "framerate = (fraction) [0, MAX]"));

#define gst_mtltxsink_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstMtlTxSink, gst_mtltxsink,
    GST_TYPE_VIDEO_SINK,
    GST_DEBUG_CATEGORY_INIT (gst_mtltxsink_debug,
        "mtltxsink", 0, "Mtl St2110 st20 transmition sink"));

GST_ELEMENT_REGISTER_DEFINE (mtltxsink, "mtltxsink", GST_RANK_NONE,
    GST_TYPE_MTL_TX_SINK);

static void gst_mtltxsink_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_mtltxsink_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec); 

static gboolean gst_mtltxsink_sink_event (GstPad * pad,
    GstObject * parent, GstEvent * event);
static GstFlowReturn gst_mtltxsink_chain (GstPad * pad,
    GstObject * parent, GstBuffer * buf);
static gboolean gst_mtltxsink_query (GstPad * pad,
    GstObject * parent, GstQuery * query);

static gboolean gst_mtltxsink_start (GstBaseSink * bsink);
static gboolean gst_mtltxsink_stop (GstBaseSink * bsink);

  // GST_DEBUG_CATEGORY_INIT (gst_mtltxsink_debug, "mtl_tx_sink",
  //     0, "Plugin for st2110 tx transmission mtl_tx_sink");
/* GObject vmethod implementations */

static void
gst_mtltxsink_class_init (GstMtlTxSinkClass * klass)
{
  GObjectClass      *gobject_class;
  GstElementClass   *gstelement_class;
  GstVideoSinkClass *gstvideosinkelement_class;

  gobject_class             = G_OBJECT_CLASS(klass);
  gstelement_class          = GST_ELEMENT_CLASS(klass);
  gstvideosinkelement_class = GST_VIDEO_SINK_CLASS(klass);

  gst_element_class_set_metadata (gstelement_class,
    "MtlTxSt20Sink",
    "Sink/Video",
    "Mtl transmition plugin for st2110 standard rawvideo st20",
    "Dawid Wesierski <dawid.wesierski@intel.com>");

  gst_element_class_add_static_pad_template (gstelement_class,
      &gst_mtltxsink_sink_pad_template);

  gobject_class->set_property = GST_DEBUG_FUNCPTR (gst_mtltxsink_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR (gst_mtltxsink_get_property);
  gstvideosinkelement_class->parent_class.start
    = GST_DEBUG_FUNCPTR (gst_mtltxsink_start);
  gstvideosinkelement_class->parent_class.stop
    = GST_DEBUG_FUNCPTR (gst_mtltxsink_stop);

  g_object_class_install_property (gobject_class, PROP_SILENT,
      g_param_spec_boolean ("silent", "Silent", "Produce verbose output ?",
          FALSE, G_PARAM_READWRITE));

  g_object_class_install_property (
      gobject_class,
      PROP_TX_DEV_ARGS_PORT,
      g_param_spec_string("dev-port",
                          "Dpdk device port",
                          "DPDK port for synchronous ST2110 ST20 raw video "
                          "transmission, bound to the VFIO DPDK driver. ",
                          NULL,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (
      gobject_class,
      PROP_TX_DEV_ARGS_SIP,
      g_param_spec_string("dev-ip",
                          "Local device ip",
                          "Local IP address that the port will be "
                          "identified by. This is the address from which ARP "
                          "responses will be sent.",
                          NULL,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (
      gobject_class,
      PROP_TX_DEV_ARGS_DMA_DEV,
      g_param_spec_string("dma-dev",
                          "Dpdk dma port",
                          "DPDK port for direct memory access for the MTL "
                          "direct memory access functionality.",
                          NULL,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (
      gobject_class,
      PROP_TX_PORT_PORT,
      g_param_spec_string("tx-port",
                          "Transmission Device Port",
                          "Initialized device port for the transmission"
                          "to go to.",
                          NULL,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (
      gobject_class,
      PROP_TX_PORT_IP,
      g_param_spec_string("tx-ip",
                          "Transmission Goal IP",
                          "IP of the port/node you want the transmission "
                          "to go to.",
                          NULL,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
        gobject_class,
        PROP_TX_PORT_UDP_PORT,
        g_param_spec_uint("tx-udp-port",
                          "UDP Port Transmission Goal",
                          "UDP port to which the transmission will be going.",
                          0,
                          G_MAXUINT,
                          20000,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
        gobject_class,
        PROP_TX_PORT_PAYLOAD_TYPE,
        g_param_spec_uint("tx-payload-type",
                          "St2110 payload type",
                          "St2110 payload type",
                          0,
                          G_MAXUINT,
                          112,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static gboolean
gst_mtltxsink_start (GstBaseSink * bsink)
{
  struct mtl_init_params mtl_init_params = { 0 };
  gint ret;

  GstMtlTxSink *sink = GST_MTL_TX_SINK (bsink);

  GST_DEBUG_OBJECT (sink, "start");
  GST_DEBUG ("Media Transport Initialization start");

  strncpy(mtl_init_params.port[MTL_PORT_P], sink->devArgs.port, MTL_PORT_MAX_LEN);

  ret = inet_pton(AF_INET, sink->devArgs.local_ip_string, mtl_init_params.sip_addr[MTL_PORT_P]); // FIXME add an argument support
  if (ret != 1) {
    GST_ERROR("%s, sip %s is not valid ip address\n", __func__, sink->devArgs.local_ip_string);
    return false;
  }

  mtl_init_params.tx_queues_cnt[MTL_PORT_P] = 16; // FIXME add an argument support
  mtl_init_params.rx_queues_cnt[MTL_PORT_P] = 0;  // FIXME Add an argument support
  mtl_init_params.num_ports++; /* ENABLE MTL_PORT*/

    // if (sink->portArgs.dip[i]) {
    //   ret = inet_pton(AF_INET, sink->portArgs.dip[i], ops_tx.port.dip_addr[i]);
    //   if (ret != 1) {
    //     GST_ERROR("%s, %d dip %s is not valid ip address\n", __func__, i, sink->portArgs.dip[i]);
    //     return;
    //   }
    // }

    // if ((sink->portArgs.udp_port < 0) || (sink->portArgs.udp_port > 0xFFFF)) {
    //   GST_ERROR("%s, invalid UDP port: %d\n", __func__, sink->portArgs.udp_port);
    //   return;
    // }

    // if ((sink->portArgs.payload_type < 0) || (sink->portArgs.payload_type > 0x7F)) {
    //   GST_ERROR("%s, invalid payload_type: %d\n", __func__, sink->portArgs.payload_type);
    //   return;
    // }

    // ops_tx.port.udp_port[i] = sink->portArgs.udp_port;
    // ops_tx.port.payload_type = sink->portArgs.payload_type;
    // ops_tx.port.num_port++;


  mtl_init_params.flags |= MTL_FLAG_BIND_NUMA;
  if (sink->silent) {
    mtl_init_params.log_level = MTL_LOG_LEVEL_ERROR;
  } else {
    mtl_init_params.log_level = MTL_LOG_LEVEL_INFO;
  }

  mtl_init_params.flags |= MTL_FLAG_BIND_NUMA;
  // if (sink->devArgs.port[MTL_PORT_P])
  //   strncpy(mtl_init_params.port[MTL_PORT_P], sink->devArgs.port[MTL_PORT_P], MTL_PORT_MAX_LEN);
  // else
  //   strncpy(mtl_init_params.port[MTL_PORT_P], "0000:b1:11.0", MTL_PORT_MAX_LEN);

  if (sink->devArgs.dma_dev) {
    strncpy(mtl_init_params.dma_dev_port[0], sink->devArgs.dma_dev, MTL_PORT_MAX_LEN);
  }

  sink->mtl_lib_handle = mtl_init(&mtl_init_params);
  if (!sink->mtl_lib_handle) {
    GST_ERROR("Mtl Couldn't initialize");
    return false;
  }

  return true;
}

/* initialize new element, pads add them to element set pad callback functions
 * Filling the MediaTransportLibrary with the ops_tx structure that should not
 * be changed the initialization is not done here as the pads are 
 */
static void
gst_mtltxsink_init (GstMtlTxSink * sink)
{
  GstElement        *element = GST_ELEMENT(sink);
  GstPad            *sinkpad;

  sinkpad = gst_element_get_static_pad (element, "sink");
  if (!sinkpad) {
    GST_ERROR_OBJECT (sink, "Failed to get sink pad from child element");
    return;
  }

  gst_pad_set_query_function (sinkpad, gst_mtltxsink_query);

  gst_pad_set_event_function (sinkpad,
      GST_DEBUG_FUNCPTR (gst_mtltxsink_sink_event));
 
  gst_pad_set_chain_function (sinkpad,
      GST_DEBUG_FUNCPTR (gst_mtltxsink_chain));

  // if (!gst_element_add_pad (element, sinkpad)) {
  //   GST_ERROR_OBJECT (sink, "Failed to add sink pad to element");
  //   gst_object_unref (sinkpad);
  //   return;
  // }

  // Caps will be negotiated later :< 
  // gst_video_info_init(&info);
  // gst_video_info_from_caps(&info, caps);
  // gst_caps_unref(caps);
}

// Simple wrapper to fix latei n
static gboolean
gst_mtltxsink_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  gboolean ret = FALSE;

  switch (GST_QUERY_TYPE (query)) {
    default:
      ret = gst_pad_query_default (pad, parent, query);
      break;
  }
  // TODO add support for custom queries
  return ret;
}

static void
gst_mtltxsink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstMtlTxSink *self = GST_MTL_TX_SINK (object);
  GstMtlTxSink test = *self;

  switch (prop_id) {
    case PROP_SILENT:
      self->silent = g_value_get_boolean (value);
      break;
    case PROP_TX_DEV_ARGS_PORT:
      strncpy(self->devArgs.port, g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    case PROP_TX_DEV_ARGS_SIP:
      strncpy(self->devArgs.local_ip_string, g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    case PROP_TX_DEV_ARGS_DMA_DEV:
      strncpy(self->devArgs.dma_dev,  g_value_get_string(value), MTL_PORT_MAX_LEN);
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
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_mtltxsink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstMtlTxSink *sink = GST_MTL_TX_SINK (object);

  switch (prop_id) {
    case PROP_SILENT:
      g_value_set_boolean (value, sink->silent);
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
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_mtltxsink_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  GstMtlTxSink *sink;
  GstMtlTxSink debug;
  GstCaps *caps;
  GstVideoInfo *info;
  struct st20p_tx_ops ops_tx = {};
  gint ret;

  sink = GST_MTL_TX_SINK (parent);
  debug = *sink;

  GST_LOG_OBJECT (sink, "Received %s event: %" GST_PTR_FORMAT,
      GST_EVENT_TYPE_NAME (event), event);

  ret = GST_EVENT_TYPE (event);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      gst_event_parse_caps (event, &caps);
      info = gst_video_info_new_from_caps (caps);
      ops_tx.name = "st20sink";
      ops_tx.device = ST_PLUGIN_DEVICE_AUTO;
      ops_tx.width = info->width;
      ops_tx.height = info->height;
      ops_tx.input_fmt = ST_FRAME_FMT_YUV422PLANAR10LE; // TODO parse info->finfo;
      ops_tx.transport_fmt = ST20_FMT_YUV_422_10BIT; // TODO parse info->finfo;
      ops_tx.fps = ST_FPS_P60; // TODO Parse info->fps_n / info->fps_d;
      ops_tx.framebuff_cnt = 3; // TODO add support from arguments
      ops_tx.port.num_port++;
      ops_tx.interlaced = info->interlace_mode;
      ops_tx.flags |= ST20P_TX_FLAG_BLOCK_GET;
      if (inet_pton(AF_INET, sink->portArgs.tx_ip_string, ops_tx.port.dip_addr[MTL_PORT_P]) != 1) {
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
        return;
      }

      ops_tx.port.udp_port[0] = sink->portArgs.udp_port;

      if ((sink->portArgs.payload_type < 0) || (sink->portArgs.payload_type > 0x7F)) {
        GST_ERROR("%s, invalid payload_type: %d\n", __func__, sink->portArgs.payload_type);
        return;
      }

      ops_tx.port.payload_type = sink->portArgs.payload_type;
      gst_video_info_free(info);

      ret = mtl_start(sink->mtl_lib_handle);
      if (ret < 0) {
        GST_ERROR("Failed to start mtl library");
        break;
      }

      sink->tx_handle = st20p_tx_create(sink->mtl_lib_handle, &ops_tx);
      if (!sink->tx_handle) {
        GST_ERROR("Failed to create st20p tx handle");
        break;
      }

      sink->frame_size = st20p_tx_frame_size(sink->tx_handle);
      ret = gst_pad_event_default (pad, parent, event);
      break;
    }
    default:
      ret = gst_pad_event_default (pad, parent, event);
      break;
  }


  return ret;
}

/* chain function
 * this function does the actual processing
 */
static GstFlowReturn
gst_mtltxsink_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  void* frame_buffer_pointer;
  GstMtlTxSink *sink            = GST_MTL_TX_SINK (parent);
  gint buffer_size              = gst_buffer_get_size(buf);
  gint buffer_n                 = gst_buffer_n_memory(buf);
  struct st_frame** frame       = &sink->frame_in_tranmission;
  gint* frame_progress          = &sink->frame_in_tranmission_data_pointer;
  gint frame_size               = sink->frame_size;
  void* gst_buffer_memory;

  for (int i = 0; i < buffer_n; i++) {
    gst_buffer_memory = gst_buffer_peek_memory(buf, i);

    if (!*frame) {
      *frame = st20p_tx_get_frame(sink->tx_handle);
      if (!*frame) {
        GST_ERROR("Failed to get frame from st20p tx handle");
        return GST_FLOW_ERROR;
      }
      if (*frame_progress) { // This indicates serious issue
        GST_ERROR("Frame progress not zero ");
        *frame_progress = 0;
      }
    }

    if (buffer_size + *frame_progress > frame_size) {
      GST_ERROR("Frame size missmatch");
      st20p_tx_put_frame(sink->tx_handle, *frame);
      *frame = NULL;
      return GST_FLOW_ERROR;
    }

    frame_buffer_pointer = (*frame)->addr[0] + *frame_progress;

    mtl_memcpy(frame_buffer_pointer, gst_buffer_memory, buffer_size);
    *frame_progress += buffer_size;

    if (*frame_progress == frame_size) {
      st20p_tx_put_frame(sink->tx_handle, *frame);
      *frame = NULL;
      *frame_progress = 0;
    }
  }

  // if (buffer_size % frame_size != 0) {
  //   GST_ERROR_OBJECT(sink, "Buffer size %zu does not match expected frame size %d ", gst_buffer_get_size(buf), sink->frame_size);
  //   return GST_FLOW_ERROR;
  // }

  /* just push out the incoming buffer without touching it */
  return GST_FLOW_OK;
}

static gboolean
gst_mtltxsink_stop (GstBaseSink * bsink) {
  GstMtlTxSink *sink            = GST_MTL_TX_SINK (bsink);
  if (sink->frame_in_tranmission) {
    st20p_tx_put_frame(sink->tx_handle, sink->frame_in_tranmission);
    sink->frame_in_tranmission = NULL;
  }

  if (sink->tx_handle) {
    st20p_tx_free(sink->tx_handle);
    sink->tx_handle = NULL;
  }

  if (sink->mtl_lib_handle) {
    mtl_uninit(sink->mtl_lib_handle);
    sink->mtl_lib_handle = NULL;
  }

  return true;
}

static gboolean
plugin_init (GstPlugin * mtltxsink)
{
  return gst_element_register (mtltxsink, "mtltxsink", GST_RANK_SECONDARY, GST_TYPE_MTL_TX_SINK);
}

GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    mtltxsink,
    "software based solution designed for high-throughput",
    plugin_init,
    PACKAGE_VERSION,
    GST_LICENSE,
    GST_PACKAGE_NAME,
    GST_PACKAGE_ORIGIN
  )
