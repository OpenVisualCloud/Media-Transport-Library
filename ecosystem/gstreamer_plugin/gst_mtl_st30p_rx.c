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
 * SECTION:element-mtl_rx_src
 *
 * The mtl_rx_src element is a GStreamer src plugin designed to interface with
 * the Media Transport Library (MTL).
 * MTL is a software-based solution optimized for high-throughput, low-latency
 * transmission and reception of media data.
 *
 * It features an efficient user-space LibOS UDP stack specifically crafted for
 * media transport and includes a built-in  SMPTE ST 2110-compliant
 * implementation for Professional Media over Managed IP Networks.
 *
 * This element allows GStreamer pipelines to recive media data using the MTL
 * framework, ensuring efficient and reliable media transport over IP networks.
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gst/gst.h>

#include "gst_mtl_st30p_rx.h"

GST_DEBUG_CATEGORY_STATIC(gst_mtl_st30p_rx_debug);
#define GST_CAT_DEFAULT gst_mtl_st30p_rx_debug
#ifndef GST_LICENSE
#define GST_LICENSE "LGPL"
#endif
#ifndef GST_API_VERSION
#define GST_API_VERSION "1.0"
#endif
#ifndef GST_PACKAGE_NAME
#define GST_PACKAGE_NAME "Media Transport Library st2110 st20 rx plugin"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://github.com/OpenVisualCloud/Media-Transport-Library"
#endif
#ifndef PACKAGE
#define PACKAGE "gst-mtl-rx-st20"
#endif
#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "1.19.0.1"
#endif

enum {
  PROP_0,
  PROP_SILENT,
  PROP_RX_DEV_ARGS_PORT,
  PROP_RX_DEV_ARGS_SIP,
  PROP_RX_DEV_ARGS_DMA_DEV,
  PROP_RX_PORT_PORT,
  PROP_RX_PORT_IP,
  PROP_RX_PORT_UDP_PORT,
  PROP_RX_PORT_PAYLOAD_TYPE,
  PROP_RX_PORT_RX_QUEUES,
  PROP_RX_FRAMERATE,
  PROP_RX_FRAMEBUFF_NUM,
  PROP_RX_CHANNEL,
  PROP_RX_SAMPLING,
  PROP_RX_AUDIO_FORMAT,
  PROP_MAX
};

/* pad template */
static GstStaticPadTemplate gst_mtl_st30p_rx_src_pad_template =
    GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS,
                            GST_STATIC_CAPS("audio/x-raw, "
                                            "format = (string) {S8, S16LE, S24LE},"
                                            "channels = (int) [1, 2], "
                                            "rate = (int) {44100, 48000, 96000}"));

#define gst_mtl_st30p_rx_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE(Gst_Mtl_St30p_Rx, gst_mtl_st30p_rx, GST_TYPE_BASE_SRC,
                        GST_DEBUG_CATEGORY_INIT(gst_mtl_st30p_rx_debug, "mtl_st30p_rx", 0,
                                                "MTL St2110 st20 transmission src"));

GST_ELEMENT_REGISTER_DEFINE(mtl_st30p_rx, "mtl_st30p_rx", GST_RANK_NONE,
                            GST_TYPE_MTL_ST30P_RX);

static void gst_mtl_st30p_rx_set_property(GObject* object, guint prop_id,
                                          const GValue* value, GParamSpec* pspec);
static void gst_mtl_st30p_rx_get_property(GObject* object, guint prop_id, GValue* value,
                                          GParamSpec* pspec);
static void gst_mtl_st30p_rx_finalize(GObject* object);

static gboolean gst_mtl_st30p_rx_start(GstBaseSrc* basesrc);
static gboolean gst_mtl_st30p_rx_stop(GstBaseSrc* basesrc);
static gboolean gst_mtl_st30p_rx_negotiate(GstBaseSrc* basesrc);
static GstFlowReturn gst_mtl_st30p_rx_create(GstBaseSrc* basesrc, guint64 offset,
                                             guint length, GstBuffer** buffer);

static void gst_mtl_st30p_rx_class_init(Gst_Mtl_St30p_RxClass* klass) {
  GObjectClass* gobject_class;
  GstElementClass* gstelement_class;
  GstBaseSrcClass* gstbasesrc_class;

  gobject_class = G_OBJECT_CLASS(klass);
  gstelement_class = GST_ELEMENT_CLASS(klass);
  gstbasesrc_class = GST_BASE_SRC_CLASS(klass);

  gst_element_class_set_metadata(
      gstelement_class, "MtlRxSt20Src", "Src/Audio",
      "MTL transmission plugin for SMPTE ST 2110-20 standard (uncompressed video)",
      "Dawid Wesierski <dawid.wesierski@intel.com>");

  gst_element_class_add_static_pad_template(gstelement_class,
                                            &gst_mtl_st30p_rx_src_pad_template);

  gobject_class->set_property = GST_DEBUG_FUNCPTR(gst_mtl_st30p_rx_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR(gst_mtl_st30p_rx_get_property);
  gobject_class->finalize = GST_DEBUG_FUNCPTR(gst_mtl_st30p_rx_finalize);

  gstbasesrc_class->start = GST_DEBUG_FUNCPTR(gst_mtl_st30p_rx_start);
  gstbasesrc_class->stop = GST_DEBUG_FUNCPTR(gst_mtl_st30p_rx_stop);
  gstbasesrc_class->negotiate = GST_DEBUG_FUNCPTR(gst_mtl_st30p_rx_negotiate);
  gstbasesrc_class->create = GST_DEBUG_FUNCPTR(gst_mtl_st30p_rx_create);

  g_object_class_install_property(
      gobject_class, PROP_SILENT,
      g_param_spec_boolean("silent", "Silent", "Turn on silent mode.", FALSE,
                           G_PARAM_READWRITE));

  g_object_class_install_property(
      gobject_class, PROP_RX_DEV_ARGS_PORT,
      g_param_spec_string("dev-port", "DPDK device port",
                          "DPDK port for synchronous ST 2110-20 uncompressed"
                          "video transmission, bound to the VFIO DPDK driver. ",
                          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_RX_DEV_ARGS_SIP,
      g_param_spec_string("dev-ip", "Local device IP",
                          "Local IP address that the port will be "
                          "identified by. This is the address from which ARP "
                          "responses will be sent.",
                          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_RX_DEV_ARGS_DMA_DEV,
      g_param_spec_string("dma-dev", "DPDK DMA port",
                          "DPDK port for the MTL direct memory functionality.", NULL,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_RX_PORT_PORT,
      g_param_spec_string("rx-port", "Transmission Device Port",
                          "DPDK device port initialized for the transmission.", NULL,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_RX_PORT_IP,
      g_param_spec_string("rx-ip", "Sender node's IP", "Receiving MTL node IP address.",
                          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_RX_PORT_UDP_PORT,
      g_param_spec_uint("rx-udp-port", "Sender UDP port", "Receiving MTL node UDP port.",
                        0, G_MAXUINT, 20000, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_RX_PORT_PAYLOAD_TYPE,
      g_param_spec_uint("rx-payload-type", "ST 2110 payload type",
                        "SMPTE ST 2110 payload type.", 0, G_MAXUINT, 112,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_RX_PORT_RX_QUEUES,
      g_param_spec_uint("rx-queues", "Number of RX queues",
                        "Number of RX queues to initialize in DPDK backend.", 0,
                        G_MAXUINT, 16, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_RX_FRAMERATE,
      g_param_spec_uint("rx-fps", "Audio framerate", "Framerate of the video.", 0,
                        G_MAXUINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_RX_FRAMEBUFF_NUM,
      g_param_spec_uint("rx-framebuff-num", "Number of framebuffers",
                        "Number of framebuffers to be used for transmission.", 0,
                        G_MAXUINT, 3, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_RX_CHANNEL,
      g_param_spec_uint("rx-channel", "Audio channel", "Audio channel number.", 0,
                            G_MAXUINT, 2, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_RX_SAMPLING,
      g_param_spec_uint("rx-sampling", "Audio sampling rate", "Audio sampling rate.", 0,
                        G_MAXUINT, 48000, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_RX_AUDIO_FORMAT,
      g_param_spec_string("rx-audio-format", "Audio format", "Audio format type.", NULL,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static gboolean gst_mtl_st30p_rx_start(GstBaseSrc* basesrc) {
  struct mtl_init_params mtl_init_params = {0};
  struct st30p_rx_ops* ops_rx;
  gint ret;

  Gst_Mtl_St30p_Rx* src = GST_MTL_ST30P_RX(basesrc);
  ops_rx = &src->ops_rx;

  GST_DEBUG_OBJECT(src, "start");
  GST_DEBUG("Media Transport Initialization start");

  /* mtl is already initialzied */
  if (src->mtl_lib_handle) {
    GST_INFO("Mtl already initialized");
    if (mtl_start(src->mtl_lib_handle)) {
      GST_ERROR("Failed to start MTL");
      return FALSE;
    }
    return TRUE;
  } else {
    strncpy(mtl_init_params.port[MTL_PORT_P], src->devArgs.port, MTL_PORT_MAX_LEN);

    ret = inet_pton(AF_INET, src->devArgs.local_ip_string,
                    mtl_init_params.sip_addr[MTL_PORT_P]);
    if (ret != 1) {
      GST_ERROR("%s, sip %s is not valid ip address\n", __func__,
                src->devArgs.local_ip_string);
      return FALSE;
    }

    if (src->devArgs.rx_queues_cnt[MTL_PORT_P]) {
      mtl_init_params.rx_queues_cnt[MTL_PORT_P] = src->devArgs.rx_queues_cnt[MTL_PORT_P];
    } else {
      mtl_init_params.rx_queues_cnt[MTL_PORT_P] = 16;
      mtl_init_params.tx_queues_cnt[MTL_PORT_P] = 16;
    }

    mtl_init_params.num_ports++;

    mtl_init_params.flags |= MTL_FLAG_BIND_NUMA;
    if (src->silent) {
      mtl_init_params.log_level = MTL_LOG_LEVEL_ERROR;
    } else {
      mtl_init_params.log_level = MTL_LOG_LEVEL_INFO;
    }

    src->retry_frame = 10;

    mtl_init_params.flags |= MTL_FLAG_BIND_NUMA;

    if (src->devArgs.dma_dev) {
      strncpy(mtl_init_params.dma_dev_port[0], src->devArgs.dma_dev, MTL_PORT_MAX_LEN);
    }

    if (src->mtl_lib_handle) {
      GST_ERROR("MTL already initialized");
      return FALSE;
    }

    src->mtl_lib_handle = mtl_init(&mtl_init_params);
    if (!src->mtl_lib_handle) {
      GST_ERROR("Could not initialize MTL");
      return FALSE;
    }
  }

  ops_rx->name = "st30src";
  ops_rx->channel = src->channel;
  ops_rx->port.num_port = 1;
  ops_rx->ptime = ST30_PTIME_1MS;
  ops_rx->flags |= ST30P_RX_FLAG_BLOCK_GET;

  if (!gst_mtlst30tx_parse_sampling(src->sampling, &ops_rx->sampling)) {
    GST_ERROR("Failed to parse ops_rx sampling %d", src->sampling);
    return FALSE;
  }

  if (!gst_mtl_common_parse_audio_format(src->audio_format, &ops_rx->fmt)) {
    GST_ERROR("Failed to parse ops_rx audio format %s", src->audio_format);
    return FALSE;
  }

  ops_rx->framebuff_size = st30_calculate_framebuff_size(
      ops_rx->fmt, ops_rx->ptime, ops_rx->sampling, ops_rx->channel, 10 * NS_PER_MS, NULL);
  
  if (!ops_rx->framebuff_size) {
    GST_ERROR("Failed to calculate framebuff size");
    return FALSE;
  }


  if (src->framebuffer_num) {
    ops_rx->framebuff_cnt = src->framebuffer_num;
  } else {
    ops_rx->framebuff_cnt = 3;
  }

  if (inet_pton(AF_INET, src->portArgs.session_ip_string,
                ops_rx->port.ip_addr[MTL_PORT_P]) != 1) {
    GST_ERROR("Invalid destination IP address: %s", src->portArgs.session_ip_string);
    return FALSE;
  }

  if (strlen(src->portArgs.port) == 0) {
    strncpy(ops_rx->port.port[MTL_PORT_P], src->devArgs.port, MTL_PORT_MAX_LEN);
  } else {
    strncpy(ops_rx->port.port[MTL_PORT_P], src->portArgs.port, MTL_PORT_MAX_LEN);
  }

  if ((src->portArgs.udp_port < 0) || (src->portArgs.udp_port > 0xFFFF)) {
    GST_ERROR("%s, invalid UDP port: %d\n", __func__, src->portArgs.udp_port);
  } else {
    ops_rx->port.udp_port[0] = src->portArgs.udp_port;
  }

  if ((src->portArgs.payload_type < 0) || (src->portArgs.payload_type > 0x7F)) {
    GST_ERROR("%s, invalid payload_type: %d\n", __func__, src->portArgs.payload_type);
  } else {
    ops_rx->port.payload_type = src->portArgs.payload_type;
  }

  ret = mtl_start(src->mtl_lib_handle);
  if (ret < 0) {
    GST_ERROR("Failed to start MTL");
    return FALSE;
  }

  src->rx_handle = st30p_rx_create(src->mtl_lib_handle, &src->ops_rx);
  if (!src->rx_handle) {
    GST_ERROR("Failed to create st30p rx handle");
    return FALSE;
  }

  src->frame_size = st30p_rx_frame_size(src->rx_handle);
  if (src->frame_size <= 0) {
    GST_ERROR("Failed to get frame size");
    return FALSE;
  }

  return TRUE;
}

static void gst_mtl_st30p_rx_init(Gst_Mtl_St30p_Rx* src) {
  GstElement* element = GST_ELEMENT(src);
  GstPad* srcpad;

  srcpad = gst_element_get_static_pad(element, "src");
  if (!srcpad) {
    GST_ERROR_OBJECT(src, "Failed to get src pad from child element");
    return;
  }
}

static void gst_mtl_st30p_rx_set_property(GObject* object, guint prop_id,
                                          const GValue* value, GParamSpec* pspec) {
  Gst_Mtl_St30p_Rx* self = GST_MTL_ST30P_RX(object);

  switch (prop_id) {
    case PROP_SILENT:
      self->silent = g_value_get_boolean(value);
      break;
    case PROP_RX_DEV_ARGS_PORT:
      strncpy(self->devArgs.port, g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    case PROP_RX_DEV_ARGS_SIP:
      strncpy(self->devArgs.local_ip_string, g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    case PROP_RX_DEV_ARGS_DMA_DEV:
      strncpy(self->devArgs.dma_dev, g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    case PROP_RX_PORT_PORT:
      strncpy(self->portArgs.port, g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    case PROP_RX_PORT_IP:
      strncpy(self->portArgs.session_ip_string, g_value_get_string(value),
              MTL_PORT_MAX_LEN);
      break;
    case PROP_RX_PORT_UDP_PORT:
      self->portArgs.udp_port = g_value_get_uint(value);
      break;
    case PROP_RX_PORT_PAYLOAD_TYPE:
      self->portArgs.payload_type = g_value_get_uint(value);
      break;
    case PROP_RX_PORT_RX_QUEUES:
      self->devArgs.rx_queues_cnt[MTL_PORT_P] = g_value_get_uint(value);
      break;
    case PROP_RX_FRAMERATE:
      self->channel = g_value_get_uint(value);
      break;
    case PROP_RX_FRAMEBUFF_NUM:
      self->sampling = g_value_get_uint(value);
      break;
    case PROP_RX_CHANNEL:
      self->channel = g_value_get_uint(value);
      break;
    case PROP_RX_SAMPLING:
      self->sampling = g_value_get_uint(value);
      break;
    case PROP_RX_AUDIO_FORMAT:
      strncpy(self->audio_format, g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void gst_mtl_st30p_rx_get_property(GObject* object, guint prop_id, GValue* value,
                                          GParamSpec* pspec) {
  Gst_Mtl_St30p_Rx* src = GST_MTL_ST30P_RX(object);

  switch (prop_id) {
    case PROP_SILENT:
      g_value_set_boolean(value, src->silent);
      break;
    case PROP_RX_DEV_ARGS_PORT:
      g_value_set_string(value, src->devArgs.port);
      break;
    case PROP_RX_DEV_ARGS_SIP:
      g_value_set_string(value, src->devArgs.local_ip_string);
      break;
    case PROP_RX_DEV_ARGS_DMA_DEV:
      g_value_set_string(value, src->devArgs.dma_dev);
      break;
    case PROP_RX_PORT_PORT:
      g_value_set_string(value, src->portArgs.port);
      break;
    case PROP_RX_PORT_IP:
      g_value_set_string(value, src->portArgs.session_ip_string);
      break;
    case PROP_RX_PORT_UDP_PORT:
      g_value_set_uint(value, src->portArgs.udp_port);
      break;
    case PROP_RX_PORT_PAYLOAD_TYPE:
      g_value_set_uint(value, src->portArgs.payload_type);
      break;
    case PROP_RX_PORT_RX_QUEUES:
      g_value_set_uint(value, src->devArgs.rx_queues_cnt[MTL_PORT_P]);
      break;
    case PROP_RX_FRAMEBUFF_NUM:
      g_value_set_uint(value, src->framebuffer_num);
      break;
    case PROP_RX_CHANNEL:
      g_value_set_uint(value, src->channel);
      break;
    case PROP_RX_SAMPLING:
      g_value_set_uint(value, src->sampling);
      break;
    case PROP_RX_AUDIO_FORMAT:
      g_value_set_string(value, src->audio_format);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

/*
 * Create MTL session rx handle and initialize the session with the parameters
 * from caps negotiated by the pipeline.
 */

static gboolean gst_mtl_st30p_rx_negotiate(GstBaseSrc* basesrc) {
  GstAudioInfo* info;
  Gst_Mtl_St30p_Rx* src = GST_MTL_ST30P_RX(basesrc);
  struct st30p_rx_ops* ops_rx = &src->ops_rx;
  gint ret;
  GstCaps* caps;

  info = gst_audio_info_new();

  switch (ops_rx->fmt) {
    case ST30_FMT_PCM24:
      gst_audio_info_set_format(info, GST_AUDIO_FORMAT_S24LE, info->rate, info->channels, NULL);
      break;
    case ST30_FMT_PCM16:
      gst_audio_info_set_format(info, GST_AUDIO_FORMAT_S16LE, info->rate, info->channels, NULL);
      break;
    case ST30_FMT_PCM8:
            gst_audio_info_set_format(info, GST_AUDIO_FORMAT_S8, info->rate, info->channels, NULL);
      break;
    default:
      GST_ERROR("Unsupported pixel format %d", ops_rx->fmt);
      return FALSE;
  }

  info->rate = ops_rx->sampling;
  info->channels = ops_rx->channel;

  caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, gst_audio_format_to_string(info->finfo->format), "channels",
                             G_TYPE_INT, info->channels, "rate", G_TYPE_INT, info->rate, NULL);

  // caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "v210", "width",
  //                            G_TYPE_INT, info->width, "height", G_TYPE_INT, info->height,
  //                            "framerate", GST_TYPE_FRACTION, info->fps_n, 1,
  //                            "interlace-mode", G_TYPE_BOOLEAN, src->interlaced, NULL);

  if (!caps) caps = gst_pad_get_pad_template_caps(GST_BASE_SRC_PAD(basesrc));

  if (gst_caps_is_empty(caps)) {
    gst_caps_unref(caps);
    return FALSE;
  }

  ret = gst_pad_set_caps(GST_BASE_SRC_PAD(basesrc), caps);
  gst_caps_unref(caps);
  if (!ret) {
    GST_ERROR("Failed to set caps");
    return FALSE;
  }

  gst_audio_info_free(info);
  return TRUE;
}

static GstFlowReturn gst_mtl_st30p_rx_create(GstBaseSrc* basesrc, guint64 offset,
                                             guint length, GstBuffer** buffer) {
  GstBuffer* buf;
  Gst_Mtl_St30p_Rx* src = GST_MTL_ST30P_RX(basesrc);
  struct st30_frame* frame;
  GstMapInfo dest_info;
  gint ret;
  gsize fill_size;

  buf = gst_buffer_new_allocate(NULL, src->frame_size + 1, NULL);
  if (!buf) {
    GST_ERROR("Failed to allocate buffer");
    return GST_FLOW_ERROR;
  }

  *buffer = buf;

  GST_OBJECT_LOCK(src);

  for (int i = 0; i < src->retry_frame; i++) {
    frame = st30p_rx_get_frame(src->rx_handle);
    if (frame) {
      break;
    }
  }

  if (!frame) {
    GST_INFO("Failed to get frame EOS");
    GST_OBJECT_UNLOCK(src);
    return GST_FLOW_EOS;
  }

  gst_buffer_map(buf, &dest_info, GST_MAP_WRITE);
  fill_size = gst_buffer_fill(buf, 0, frame->addr, src->frame_size);
  gst_buffer_unmap(buf, &dest_info);

  if (fill_size != src->frame_size) {
    GST_ERROR("Failed to fill buffer");
    ret = GST_FLOW_ERROR;
  } else {
    ret = GST_FLOW_OK;
  }

  st30p_rx_put_frame(src->rx_handle, frame);
  GST_OBJECT_UNLOCK(src);
  return ret;
}

static void gst_mtl_st30p_rx_finalize(GObject* object) {
  Gst_Mtl_St30p_Rx* src = GST_MTL_ST30P_RX(object);

  if (src->rx_handle) {
    if (st30p_rx_free(src->rx_handle)) {
      GST_ERROR("Failed to free rx handle");
      return;
    }
  }

  if (src->mtl_lib_handle) {
    if (mtl_stop(src->mtl_lib_handle) || mtl_uninit(src->mtl_lib_handle)) {
      GST_ERROR("Failed to uninitialize MTL library");
      return;
    }
  }
}

static gboolean gst_mtl_st30p_rx_stop(GstBaseSrc* basesrc) {
  Gst_Mtl_St30p_Rx* src = GST_MTL_ST30P_RX(basesrc);

  if (src->mtl_lib_handle) {
    mtl_stop(src->mtl_lib_handle);
  }

  return TRUE;
}

static gboolean plugin_init(GstPlugin* mtl_st30p_rx) {
  return gst_element_register(mtl_st30p_rx, "mtl_st30p_rx", GST_RANK_SECONDARY,
                              GST_TYPE_MTL_ST30P_RX);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, mtl_st30p_rx,
                  "software-based solution designed for high-throughput transmission",
                  plugin_init, PACKAGE_VERSION, GST_LICENSE, GST_PACKAGE_NAME,
                  GST_PACKAGE_ORIGIN)
