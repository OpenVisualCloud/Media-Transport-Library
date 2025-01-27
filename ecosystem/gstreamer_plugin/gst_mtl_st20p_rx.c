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

#include "gst_mtl_st20p_rx.h"

GST_DEBUG_CATEGORY_STATIC(gst_mtl_st20p_rx_debug);
#define GST_CAT_DEFAULT gst_mtl_st20p_rx_debug
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
#define PACKAGE "gst-mtl-st20p-rx"
#endif
#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "1.0"
#endif

enum {
  PROP_ST20P_RX_RETRY = PROP_GENERAL_MAX,
  PROP_ST20P_RX_FRAMERATE,
  PROP_ST20P_RX_FRAMEBUFF_NUM,
  PROP_ST20P_RX_WIDTH,
  PROP_ST20P_RX_HEIGHT,
  PROP_ST20P_RX_INTERLACED,
  PROP_ST20P_RX_PIXEL_FORMAT,
  PROP_MAX
};

/* pad template */
static GstStaticPadTemplate gst_mtl_st20p_rx_src_pad_template =
    GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS,
                            GST_STATIC_CAPS("video/x-raw, "
                                            "format = (string) {v210, I422_10LE},"
                                            "width = (int) [64, 16384], "
                                            "height = (int) [64, 8704], "
                                            "framerate = (fraction) [0, MAX]"));

#define gst_mtl_st20p_rx_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE(Gst_Mtl_St20p_Rx, gst_mtl_st20p_rx, GST_TYPE_BASE_SRC,
                        GST_DEBUG_CATEGORY_INIT(gst_mtl_st20p_rx_debug, "mtl_st20p_rx", 0,
                                                "MTL St2110 st20 transmission src"));

GST_ELEMENT_REGISTER_DEFINE(mtl_st20p_rx, "mtl_st20p_rx", GST_RANK_NONE,
                            GST_TYPE_MTL_ST20P_RX);

static void gst_mtl_st20p_rx_set_property(GObject* object, guint prop_id,
                                          const GValue* value, GParamSpec* pspec);
static void gst_mtl_st20p_rx_get_property(GObject* object, guint prop_id, GValue* value,
                                          GParamSpec* pspec);
static void gst_mtl_st20p_rx_finalize(GObject* object);

static gboolean gst_mtl_st20p_rx_start(GstBaseSrc* basesrc);
static gboolean gst_mtl_st20p_rx_stop(GstBaseSrc* basesrc);
static gboolean gst_mtl_st20p_rx_negotiate(GstBaseSrc* basesrc);
static GstFlowReturn gst_mtl_st20p_rx_create(GstBaseSrc* basesrc, guint64 offset,
                                             guint length, GstBuffer** buffer);

static void gst_mtl_st20p_rx_class_init(Gst_Mtl_St20p_RxClass* klass) {
  GObjectClass* gobject_class;
  GstElementClass* gstelement_class;
  GstBaseSrcClass* gstbasesrc_class;

  gobject_class = G_OBJECT_CLASS(klass);
  gstelement_class = GST_ELEMENT_CLASS(klass);
  gstbasesrc_class = GST_BASE_SRC_CLASS(klass);

  gst_element_class_set_metadata(
      gstelement_class, "MtlRxSt20Src", "Src/Video",
      "MTL transmission plugin for SMPTE ST 2110-20 standard (uncompressed video)",
      "Dawid Wesierski <dawid.wesierski@intel.com>");

  gst_element_class_add_static_pad_template(gstelement_class,
                                            &gst_mtl_st20p_rx_src_pad_template);

  gobject_class->set_property = GST_DEBUG_FUNCPTR(gst_mtl_st20p_rx_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR(gst_mtl_st20p_rx_get_property);
  gobject_class->finalize = GST_DEBUG_FUNCPTR(gst_mtl_st20p_rx_finalize);

  gstbasesrc_class->start = GST_DEBUG_FUNCPTR(gst_mtl_st20p_rx_start);
  gstbasesrc_class->stop = GST_DEBUG_FUNCPTR(gst_mtl_st20p_rx_stop);
  gstbasesrc_class->negotiate = GST_DEBUG_FUNCPTR(gst_mtl_st20p_rx_negotiate);
  gstbasesrc_class->create = GST_DEBUG_FUNCPTR(gst_mtl_st20p_rx_create);

  gst_mtl_common_init_general_arguments(gobject_class);

  g_object_class_install_property(
      gobject_class, PROP_ST20P_RX_RETRY,
      g_param_spec_uint("retry", "Retry Count",
                        "Number of times the MTL will try to get a frame.", 0, G_MAXUINT,
                        10, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST20P_RX_FRAMERATE,
      g_param_spec_uint("rx-fps", "Video framerate", "Framerate of the video.", 0,
                        G_MAXUINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST20P_RX_FRAMEBUFF_NUM,
      g_param_spec_uint("rx-framebuff-num", "Number of framebuffers",
                        "Number of framebuffers to be used for transmission.", 0,
                        G_MAXUINT, 3, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST20P_RX_WIDTH,
      g_param_spec_uint("rx-width", "Video width", "Width of the video.", 0, G_MAXUINT,
                        1920, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST20P_RX_HEIGHT,
      g_param_spec_uint("rx-height", "Video height", "Height of the video.", 0, G_MAXUINT,
                        1080, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST20P_RX_INTERLACED,
      g_param_spec_boolean("rx-interlaced", "Interlaced video",
                           "Whether the video is interlaced.", FALSE,
                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST20P_RX_PIXEL_FORMAT,
      g_param_spec_string("rx-pixel-format", "Pixel format", "Pixel format of the video.",
                          "v210", G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static gboolean gst_mtl_st20p_rx_start(GstBaseSrc* basesrc) {
  struct st20p_rx_ops* ops_rx;
  gint ret;

  Gst_Mtl_St20p_Rx* src = GST_MTL_ST20P_RX(basesrc);
  ops_rx = &src->ops_rx;

  GST_DEBUG_OBJECT(src, "start");
  GST_DEBUG("Media Transport Initialization start");

  src->mtl_lib_handle =
      gst_mtl_common_init_handle(&(src->devArgs), &(src->log_level), FALSE);

  if (!src->mtl_lib_handle) {
    GST_ERROR("Could not initialize MTL");
    return FALSE;
  }

  if (src->retry_frame == 0)
    src->retry_frame = 10;
  else if (src->retry_frame < 3) {
    GST_WARNING("Retry count is too low, setting to 3");
    src->retry_frame = 3;
  }

  if (src->width == 0 || src->height == 0) {
    GST_ERROR("Invalid resolution: %dx%d", src->width, src->height);
    return FALSE;
  }

  ops_rx->name = "st20src";
  ops_rx->device = ST_PLUGIN_DEVICE_AUTO;
  ops_rx->width = src->width;
  ops_rx->height = src->height;
  ops_rx->transport_fmt = ST20_FMT_YUV_422_10BIT;
  ops_rx->port.num_port = 1;
  ops_rx->interlaced = src->interlaced;
  ops_rx->flags |= ST20P_RX_FLAG_BLOCK_GET;

  if (!src->framerate || !gst_mtl_common_parse_fps_code(src->framerate, &ops_rx->fps)) {
    GST_ERROR("Failed to parse custom ops_rx fps code %d", src->framerate);
    return FALSE;
  }

  if (src->framebuffer_num) {
    ops_rx->framebuff_cnt = src->framebuffer_num;
  } else {
    ops_rx->framebuff_cnt = 3;
  }

  if (!gst_mtl_common_parse_pixel_format(src->pixel_format, &ops_rx->output_fmt)) {
    GST_ERROR("Failed to parse input format \"%s\"", src->pixel_format);
    ops_rx = NULL;
    return FALSE;
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

  src->rx_handle = st20p_rx_create(src->mtl_lib_handle, &src->ops_rx);
  if (!src->rx_handle) {
    GST_ERROR("Failed to create st20p rx handle");
    return FALSE;
  }

  src->frame_size = st20p_rx_frame_size(src->rx_handle);
  if (src->frame_size <= 0) {
    GST_ERROR("Failed to get frame size");
    return FALSE;
  }

  return TRUE;
}

static void gst_mtl_st20p_rx_init(Gst_Mtl_St20p_Rx* src) {
  GstElement* element = GST_ELEMENT(src);
  GstPad* srcpad;

  srcpad = gst_element_get_static_pad(element, "src");
  if (!srcpad) {
    GST_ERROR_OBJECT(src, "Failed to get src pad from child element");
    return;
  }
}

static void gst_mtl_st20p_rx_set_property(GObject* object, guint prop_id,
                                          const GValue* value, GParamSpec* pspec) {
  Gst_Mtl_St20p_Rx* self = GST_MTL_ST20P_RX(object);

  if (prop_id < PROP_GENERAL_MAX) {
    gst_mtl_common_set_general_arguments(object, prop_id, value, pspec, &(self->devArgs),
                                         &(self->portArgs), &self->log_level);
    return;
  }

  switch (prop_id) {
    case PROP_ST20P_RX_RETRY:
      self->retry_frame = g_value_get_uint(value);
      break;
    case PROP_ST20P_RX_FRAMERATE:
      self->framerate = g_value_get_uint(value);
      break;
    case PROP_ST20P_RX_FRAMEBUFF_NUM:
      self->framebuffer_num = g_value_get_uint(value);
      break;
    case PROP_ST20P_RX_WIDTH:
      self->width = g_value_get_uint(value);
      break;
    case PROP_ST20P_RX_HEIGHT:
      self->height = g_value_get_uint(value);
      break;
    case PROP_ST20P_RX_INTERLACED:
      self->interlaced = g_value_get_boolean(value);
      break;
    case PROP_ST20P_RX_PIXEL_FORMAT:
      strncpy(self->pixel_format, g_value_get_string(value), MTL_PORT_MAX_LEN);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void gst_mtl_st20p_rx_get_property(GObject* object, guint prop_id, GValue* value,
                                          GParamSpec* pspec) {
  Gst_Mtl_St20p_Rx* src = GST_MTL_ST20P_RX(object);

  if (prop_id < PROP_GENERAL_MAX) {
    gst_mtl_common_get_general_arguments(object, prop_id, value, pspec, &(src->devArgs),
                                         &(src->portArgs), &src->log_level);
    return;
  }

  switch (prop_id) {
    case PROP_ST20P_RX_RETRY:
      g_value_set_uint(value, src->retry_frame);
      break;
    case PROP_ST20P_RX_FRAMERATE:
      g_value_set_uint(value, src->framerate);
      break;
    case PROP_ST20P_RX_FRAMEBUFF_NUM:
      g_value_set_uint(value, src->framebuffer_num);
      break;
    case PROP_ST20P_RX_WIDTH:
      g_value_set_uint(value, src->width);
      break;
    case PROP_ST20P_RX_HEIGHT:
      g_value_set_uint(value, src->height);
      break;
    case PROP_ST20P_RX_INTERLACED:
      g_value_set_boolean(value, src->interlaced);
      break;
    case PROP_ST20P_RX_PIXEL_FORMAT:
      g_value_set_string(value, src->pixel_format);
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

static gboolean gst_mtl_st20p_rx_negotiate(GstBaseSrc* basesrc) {
  GstVideoInfo* info;
  Gst_Mtl_St20p_Rx* src = GST_MTL_ST20P_RX(basesrc);
  struct st20p_rx_ops* ops_rx = &src->ops_rx;
  gint ret;
  GstCaps* caps;

  info = gst_video_info_new();
  if (!info) {
    GST_ERROR("Failed to allocate video info");
    return FALSE;
  }

  /*
   * Convert boolean interlaced value to integer,
   * as only basic interlaced mode (1) is supported.
   */
  info->interlace_mode = src->interlaced;

  switch (ops_rx->output_fmt) {
    case ST_FRAME_FMT_V210:
      info->finfo = gst_video_format_get_info(GST_VIDEO_FORMAT_v210);
      break;
    case ST20_FMT_YUV_422_10BIT:
      info->finfo = gst_video_format_get_info(GST_VIDEO_FORMAT_I422_10LE);
      break;
    default:
      GST_ERROR("Unsupported pixel format");
      gst_video_info_free(info);
      return FALSE;
  }

  caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING,
                             gst_video_format_to_string(info->finfo->format), "width",
                             G_TYPE_INT, info->width, "height", G_TYPE_INT, info->height,
                             "framerate", GST_TYPE_FRACTION, info->fps_n, 1,
                             "interlace-mode", G_TYPE_BOOLEAN, src->interlaced, NULL);

  if (!caps) caps = gst_pad_get_pad_template_caps(GST_BASE_SRC_PAD(basesrc));

  if (gst_caps_is_empty(caps)) {
    GST_ERROR("Failed to set caps: caps are empty");
    gst_video_info_free(info);
    gst_caps_unref(caps);
    return FALSE;
  }

  ret = gst_pad_set_caps(GST_BASE_SRC_PAD(basesrc), caps);
  gst_caps_unref(caps);
  if (!ret) {
    GST_ERROR("Failed to set caps error %d", ret);
    gst_video_info_free(info);
    return FALSE;
  }

  gst_video_info_free(info);
  return TRUE;
}

static GstFlowReturn gst_mtl_st20p_rx_create(GstBaseSrc* basesrc, guint64 offset,
                                             guint length, GstBuffer** buffer) {
  GstBuffer* buf;
  Gst_Mtl_St20p_Rx* src = GST_MTL_ST20P_RX(basesrc);
  struct st_frame* frame;
  GstMapInfo dest_info;
  gint ret;
  gsize fill_size;

  buf = gst_buffer_new_allocate(NULL, src->frame_size, NULL);
  if (!buf) {
    GST_ERROR("Failed to allocate buffer");
    return GST_FLOW_ERROR;
  }

  *buffer = buf;

  GST_OBJECT_LOCK(src);

  for (int i = 0; i < src->retry_frame; i++) {
    frame = st20p_rx_get_frame(src->rx_handle);
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

  fill_size = gst_buffer_fill(buf, 0, frame->addr[0], src->frame_size);
  GST_BUFFER_PTS(buf) = frame->timestamp;

  gst_buffer_unmap(buf, &dest_info);

  if (fill_size != src->frame_size) {
    GST_ERROR("Failed to fill buffer");
    ret = GST_FLOW_ERROR;
  } else {
    ret = GST_FLOW_OK;
  }

  st20p_rx_put_frame(src->rx_handle, frame);
  GST_OBJECT_UNLOCK(src);
  return ret;
}

static void gst_mtl_st20p_rx_finalize(GObject* object) {
  Gst_Mtl_St20p_Rx* src = GST_MTL_ST20P_RX(object);

  if (src->rx_handle) {
    if (st20p_rx_free(src->rx_handle)) {
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

static gboolean gst_mtl_st20p_rx_stop(GstBaseSrc* basesrc) {
  Gst_Mtl_St20p_Rx* src = GST_MTL_ST20P_RX(basesrc);

  if (src->mtl_lib_handle) {
    mtl_stop(src->mtl_lib_handle);
  }

  return TRUE;
}

static gboolean plugin_init(GstPlugin* mtl_st20p_rx) {
  return gst_element_register(mtl_st20p_rx, "mtl_st20p_rx", GST_RANK_SECONDARY,
                              GST_TYPE_MTL_ST20P_RX);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, mtl_st20p_rx,
                  "software-based solution designed for high-throughput transmission",
                  plugin_init, PACKAGE_VERSION, GST_LICENSE, GST_PACKAGE_NAME,
                  GST_PACKAGE_ORIGIN)
