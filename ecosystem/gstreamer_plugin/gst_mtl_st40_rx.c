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
 * This element allows GStreamer pipelines to recive media data using the MTL,
 * ensuring efficient and reliable media transport over IP networks.
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gst_mtl_st40_rx.h"

GST_DEBUG_CATEGORY_STATIC(gst_mtl_st40_rx_debug);
#define GST_CAT_DEFAULT gst_mtl_st40_rx_debug

#ifndef GST_LICENSE
#define GST_LICENSE "LGPL"
#endif

#ifndef GST_API_VERSION
#define GST_API_VERSION "1.0"
#endif

#ifndef GST_PACKAGE_NAME
#define GST_PACKAGE_NAME "Media Transport Library st2110 st40 rx plugin"
#endif

#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://github.com/OpenVisualCloud/Media-Transport-Library"
#endif

#ifndef PACKAGE
#define PACKAGE "gst-mtl-st40-rx"
#endif

#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "1.0"
#endif

enum {
  PROP_ST40_RX_BUFFER_SIZE = PROP_GENERAL_MAX,
  PROP_ST40_RX_TIMEOUT_MBUF_GET,
  PROP_MAX
};

/* pad template */
static GstStaticPadTemplate gst_mtl_st40_rx_src_pad_template =
    GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);

#define gst_mtl_st40_rx_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE(Gst_Mtl_St40_Rx, gst_mtl_st40_rx, GST_TYPE_BASE_SRC,
                        GST_DEBUG_CATEGORY_INIT(gst_mtl_st40_rx_debug, "mtl_st40_rx", 0,
                                                "MTL St2110 st40 transmission src"));

#define IS_POWER_OF_2(x) (((x) & ((x)-1)) == 0)

GST_ELEMENT_REGISTER_DEFINE(mtl_st40_rx, "mtl_st40_rx", GST_RANK_NONE,
                            GST_TYPE_MTL_ST40_RX);

static void gst_mtl_st40_rx_set_property(GObject* object, guint prop_id,
                                         const GValue* value, GParamSpec* pspec);
static void gst_mtl_st40_rx_get_property(GObject* object, guint prop_id, GValue* value,
                                         GParamSpec* pspec);
static void gst_mtl_st40_rx_finalize(GObject* object);

static gboolean gst_mtl_st40_rx_start(GstBaseSrc* basesrc);
static gboolean gst_mtl_st40_rx_stop(GstBaseSrc* basesrc);
static GstFlowReturn gst_mtl_st40_rx_create(GstBaseSrc* basesrc, guint64 offset,
                                            guint length, GstBuffer** buffer);

static gint gst_mtl_st40_rx_mbuff_avalible(void* priv);
static void* gst_mtl_st40_rx_get_mbuf_with_timeout(Gst_Mtl_St40_Rx* src,
                                                   st40_rx_handle handle, void** usrptr,
                                                   uint16_t* size);
static GstFlowReturn gst_mtl_st40_rx_fill_buffer(Gst_Mtl_St40_Rx* src, GstBuffer** buf,
                                                 void* usrptr);

static gint gst_mtl_st40_rx_mbuff_avalible(void* priv) {
  Gst_Mtl_St40_Rx* src = (Gst_Mtl_St40_Rx*)priv;

  pthread_mutex_lock(&(src->mbuff_mutex));
  pthread_cond_signal(&(src->mbuff_cond));
  pthread_mutex_unlock(&(src->mbuff_mutex));

  return 0;
}

static void gst_mtl_st40_rx_class_init(Gst_Mtl_St40_RxClass* klass) {
  GObjectClass* gobject_class;
  GstElementClass* gstelement_class;
  GstBaseSrcClass* gstbasesrc_class;

  gobject_class = G_OBJECT_CLASS(klass);
  gstelement_class = GST_ELEMENT_CLASS(klass);
  gstbasesrc_class = GST_BASE_SRC_CLASS(klass);

  gst_element_class_set_metadata(
      gstelement_class, "MtlRxSt40Src", "Src/Metadata",
      "MTL transmission plugin for SMPTE ST 2110-40 standard (ancillary data))",
      "Dawid Wesierski <dawid.wesierski@intel.com>");

  gst_element_class_add_static_pad_template(gstelement_class,
                                            &gst_mtl_st40_rx_src_pad_template);

  gobject_class->set_property = GST_DEBUG_FUNCPTR(gst_mtl_st40_rx_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR(gst_mtl_st40_rx_get_property);
  gobject_class->finalize = GST_DEBUG_FUNCPTR(gst_mtl_st40_rx_finalize);

  gstbasesrc_class->start = GST_DEBUG_FUNCPTR(gst_mtl_st40_rx_start);
  gstbasesrc_class->stop = GST_DEBUG_FUNCPTR(gst_mtl_st40_rx_stop);
  gstbasesrc_class->create = GST_DEBUG_FUNCPTR(gst_mtl_st40_rx_create);

  gst_mtl_common_init_general_arguments(gobject_class);

  g_object_class_install_property(
      gobject_class, PROP_ST40_RX_BUFFER_SIZE,
      g_param_spec_uint("buffer-size", "Buffer Size",
                        "Size of the buffer used for receiving data", 0, G_MAXUINT, 1024,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST40_RX_TIMEOUT_MBUF_GET,
      g_param_spec_uint("timeout", "Timeout for Mbuf",
                        "Timeout in seconds for getting mbuf", 0, G_MAXUINT, 10,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static gboolean gst_mtl_st40_rx_start(GstBaseSrc* basesrc) {
  struct st40_rx_ops ops_rx = {0};
  gint ret;

  Gst_Mtl_St40_Rx* src = GST_MTL_ST40_RX(basesrc);

  GST_DEBUG_OBJECT(src, "start");
  GST_DEBUG("Media Transport Initialization start");

  src->mtl_lib_handle =
      gst_mtl_common_init_handle(&(src->devArgs), &(src->log_level), FALSE);

  if (!src->mtl_lib_handle) {
    GST_ERROR("Could not initialize MTL");
    return FALSE;
  }

  if (src->timeout_mbuf_get_seconds <= 0) {
    src->timeout_mbuf_get_seconds = 10;
  } else if (src->timeout_mbuf_get_seconds <= 3) {
    GST_WARNING("Timeout for getting mbuf is too low, setting to 3 seconds");
    src->timeout_mbuf_get_seconds = 3;
  }
  ops_rx.name = "st40src";
  ops_rx.priv = basesrc;
  ops_rx.notify_rtp_ready = gst_mtl_st40_rx_mbuff_avalible;

  if (src->mbuff_size) {
    if (!IS_POWER_OF_2(src->mbuff_size)) {
      GST_WARNING("Buffer size is not power of 2, setting to 1024");
      ops_rx.rtp_ring_size = 1024;
      src->mbuff_size = 1024;
    } else {
      ops_rx.rtp_ring_size = src->mbuff_size;
    }
  } else {
    ops_rx.rtp_ring_size = 1024;
    src->mbuff_size = 1024;
  }

  if (inet_pton(AF_INET, src->portArgs.session_ip_string, ops_rx.ip_addr[MTL_PORT_P]) !=
      1) {
    GST_ERROR("Invalid destination IP address: %s", src->portArgs.session_ip_string);
    return FALSE;
  }

  if (strlen(src->portArgs.port) == 0) {
    strncpy(ops_rx.port[MTL_PORT_P], src->devArgs.port, MTL_PORT_MAX_LEN);
  } else {
    strncpy(ops_rx.port[MTL_PORT_P], src->portArgs.port, MTL_PORT_MAX_LEN);
  }
  ops_rx.num_port = 1;

  if ((src->portArgs.udp_port < 0) || (src->portArgs.udp_port > 0xFFFF)) {
    GST_ERROR("%s, invalid UDP port: %d\n", __func__, src->portArgs.udp_port);
  } else {
    ops_rx.udp_port[0] = src->portArgs.udp_port;
  }

  if (src->portArgs.payload_type == 0) {
    ops_rx.payload_type = PAYLOAD_TYPE_ANCILLARY;
  } else if ((src->portArgs.payload_type < 0) || (src->portArgs.payload_type > 0x7F)) {
    GST_ERROR("%s, invalid payload_type: %d\n", __func__, src->portArgs.payload_type);
  } else {
    ops_rx.payload_type = src->portArgs.payload_type;
  }

  ret = mtl_start(src->mtl_lib_handle);
  if (ret < 0) {
    GST_ERROR("Failed to start MTL");
    return FALSE;
  }

  if (pthread_mutex_init(&(src->mbuff_mutex), NULL) ||
      pthread_cond_init(&(src->mbuff_cond), NULL)) {
    GST_ERROR("Failed to initialize mutex or condition variable");
    return FALSE;
  }

  src->rx_handle = st40_rx_create(src->mtl_lib_handle, &ops_rx);
  if (!src->rx_handle) {
    GST_ERROR("Failed to create st40 rx handle");
    return FALSE;
  }

  return TRUE;
}

static void gst_mtl_st40_rx_init(Gst_Mtl_St40_Rx* src) {
  GstElement* element = GST_ELEMENT(src);
  GstPad* srcpad;

  srcpad = gst_element_get_static_pad(element, "src");
  if (!srcpad) {
    GST_ERROR_OBJECT(src, "Failed to get src pad from child element");
    return;
  }
}

static void gst_mtl_st40_rx_set_property(GObject* object, guint prop_id,
                                         const GValue* value, GParamSpec* pspec) {
  Gst_Mtl_St40_Rx* self = GST_MTL_ST40_RX(object);

  if (prop_id < PROP_GENERAL_MAX) {
    gst_mtl_common_set_general_arguments(object, prop_id, value, pspec, &(self->devArgs),
                                         &(self->portArgs), &self->log_level);
    return;
  }

  switch (prop_id) {
    case PROP_ST40_RX_BUFFER_SIZE:
      self->mbuff_size = g_value_get_uint(value);
      break;
    case PROP_ST40_RX_TIMEOUT_MBUF_GET:
      self->timeout_mbuf_get_seconds = g_value_get_uint(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void gst_mtl_st40_rx_get_property(GObject* object, guint prop_id, GValue* value,
                                         GParamSpec* pspec) {
  Gst_Mtl_St40_Rx* src = GST_MTL_ST40_RX(object);

  if (prop_id < PROP_GENERAL_MAX) {
    gst_mtl_common_get_general_arguments(object, prop_id, value, pspec, &(src->devArgs),
                                         &(src->portArgs), &src->log_level);
    return;
  }

  switch (prop_id) {
    case PROP_ST40_RX_BUFFER_SIZE:
      g_value_set_uint(value, src->mbuff_size);
      break;
    case PROP_ST40_RX_TIMEOUT_MBUF_GET:
      g_value_set_uint(value, src->timeout_mbuf_get_seconds);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void* gst_mtl_st40_rx_get_mbuf_with_timeout(Gst_Mtl_St40_Rx* src,
                                                   st40_rx_handle handle, void** usrptr,
                                                   uint16_t* size) {
  struct timespec ts;
  gint ret;
  void* mbuf;

  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += src->timeout_mbuf_get_seconds;
  pthread_mutex_lock(&(src->mbuff_mutex));
  ret = pthread_cond_timedwait(&(src->mbuff_cond), &(src->mbuff_mutex), &ts);
  pthread_mutex_unlock(&(src->mbuff_mutex));

  if (ret == ETIMEDOUT) {
    GST_INFO("Timeout occurred while waiting for mbuf");
    return NULL;
  }

  mbuf = st40_rx_get_mbuf(src->rx_handle, usrptr, size);
  if (!mbuf) GST_ERROR("Failed to get ancillary mbuf\n");

  return mbuf;
}

static GstFlowReturn gst_mtl_st40_rx_fill_buffer(Gst_Mtl_St40_Rx* src, GstBuffer** buffer,
                                                 void* usrptr) {
  struct st40_rfc8331_rtp_hdr* hdr;
  struct st40_rfc8331_payload_hdr* payload_hdr;
  GstMapInfo dest_info;
  guint16 data, fill_size;
  gint udw_size;

  hdr = (struct st40_rfc8331_rtp_hdr*)usrptr;
  payload_hdr = (struct st40_rfc8331_payload_hdr*)(&hdr[1]);
  payload_hdr->swaped_second_hdr_chunk = ntohl(payload_hdr->swaped_second_hdr_chunk);
  udw_size = payload_hdr->second_hdr_chunk.data_count & 0xff;
  payload_hdr->swaped_second_hdr_chunk = htonl(payload_hdr->swaped_second_hdr_chunk);

  if (udw_size == 0) {
    GST_ERROR("Ancillary data size is 0");
    return GST_FLOW_ERROR;
  } else if (src->udw_size == 0) {
    src->udw_size = udw_size;
    src->anc_data = (char*)malloc(udw_size);
  } else if (src->udw_size != udw_size) {
    GST_INFO("Size of recieved ancillary data has changed");
    if (src->anc_data) {
      free(src->anc_data);
      src->anc_data = NULL;
    }
    src->udw_size = udw_size;
    src->anc_data = (char*)malloc(udw_size);
  }

  *buffer = gst_buffer_new_allocate(NULL, src->udw_size, NULL);
  if (!*buffer) {
    GST_ERROR("Failed to allocate space for the buffer");
    return GST_FLOW_ERROR;
  }

  if (!gst_buffer_map(*buffer, &dest_info, GST_MAP_WRITE)) {
    GST_ERROR("Failed to map the buffer");
    return GST_FLOW_ERROR;
  }

  for (int i = 0; i < udw_size; i++) {
    data = st40_get_udw(i + 3, (uint8_t*)&payload_hdr->second_hdr_chunk);
    if (!st40_check_parity_bits(data)) {
      GST_ERROR("Ancillary data parity bits check failed");
      return GST_FLOW_ERROR;
    }
    src->anc_data[i] = data & 0xff;
  }

  fill_size = gst_buffer_fill(*buffer, 0, src->anc_data, udw_size);
  gst_buffer_unmap(*buffer, &dest_info);

  if (fill_size != src->udw_size) {
    GST_ERROR("Failed to fill buffer");
    return GST_FLOW_ERROR;
  }

  return GST_FLOW_OK;
}

static GstFlowReturn gst_mtl_st40_rx_create(GstBaseSrc* basesrc, guint64 offset,
                                            guint length, GstBuffer** buffer) {
  Gst_Mtl_St40_Rx* src = GST_MTL_ST40_RX(basesrc);
  void *mbuf, *usrptr;
  guint16 size;
  gint ret;

  GST_OBJECT_LOCK(src);

  /* get the mbuff */
  mbuf = st40_rx_get_mbuf(src->rx_handle, &usrptr, &size);
  if (!mbuf) {
    mbuf = gst_mtl_st40_rx_get_mbuf_with_timeout(src, src->rx_handle, &usrptr, &size);
  }

  if (!mbuf) {
    GST_OBJECT_UNLOCK(src);
    return GST_FLOW_EOS;
  }

  if (size == 0) {
    GST_ERROR("No anciallry data recived");
    GST_OBJECT_UNLOCK(src);
    return GST_FLOW_ERROR;
  }

  ret = gst_mtl_st40_rx_fill_buffer(src, buffer, usrptr);

  if (ret != GST_FLOW_OK) {
    GST_ERROR("Failed to fill buffer");
  }

  st40_rx_put_mbuf(src->rx_handle, mbuf);
  GST_OBJECT_UNLOCK(src);
  return ret;
}

static void gst_mtl_st40_rx_finalize(GObject* object) {
  Gst_Mtl_St40_Rx* src = GST_MTL_ST40_RX(object);

  if (src->anc_data) free(src->anc_data);

  if (src->rx_handle) {
    if (st40_rx_free(src->rx_handle)) {
      GST_ERROR("Failed to free rx handle");
    }
  }

  pthread_mutex_destroy(&src->mbuff_mutex);
  pthread_cond_destroy(&src->mbuff_cond);

  if (src->mtl_lib_handle) {
    if (mtl_stop(src->mtl_lib_handle) ||
        gst_mtl_common_deinit_handle(src->mtl_lib_handle)) {
      GST_ERROR("Failed to uninitialize MTL library");
    }
  }
}

static gboolean gst_mtl_st40_rx_stop(GstBaseSrc* basesrc) {
  Gst_Mtl_St40_Rx* src = GST_MTL_ST40_RX(basesrc);

  if (src->mtl_lib_handle) {
    mtl_stop(src->mtl_lib_handle);
  }

  return TRUE;
}

static gboolean plugin_init(GstPlugin* mtl_st40_rx) {
  return gst_element_register(mtl_st40_rx, "mtl_st40_rx", GST_RANK_SECONDARY,
                              GST_TYPE_MTL_ST40_RX);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, mtl_st40_rx,
                  "software-based solution designed for high-throughput transmission",
                  plugin_init, PACKAGE_VERSION, GST_LICENSE, GST_PACKAGE_NAME,
                  GST_PACKAGE_ORIGIN);
