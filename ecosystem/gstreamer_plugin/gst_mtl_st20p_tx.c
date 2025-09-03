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
#include <pthread.h>

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
  PROP_ST20P_TX_FRAMEBUFF_NUM,
  PROP_ST20P_TX_ASYNC_SESSION_CREATE,
  PROP_ST20P_TX_USE_PTS_FOR_PACING,
  PROP_ST20P_TX_PTS_PACING_OFFSET,
  PROP_MAX
};

/* Structure to pass arguments to the thread function */
typedef struct {
  Gst_Mtl_St20p_Tx* sink;
  GstCaps* caps;
} GstMtlSt20pTxThreadData;

typedef struct {
  GstBuffer* buf;
  uint32_t child_count;
  pthread_mutex_t parent_mutex;
} GstSt20pTxExternalDataParent;

typedef struct {
  GstSt20pTxExternalDataParent* parent;
  GstMemory* gst_buffer_memory;
  GstMapInfo map_info;
} GstSt20pTxExternalDataChild;

/* pad template */
static GstStaticPadTemplate gst_mtl_st20p_tx_sink_pad_template =
    GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
                            GST_STATIC_CAPS("video/x-raw, "
                                            "format = (string) {v210, I422_10LE},"
                                            "width = (int) [64, 16384], "
                                            "height = (int) [64, 8704], "
                                            "framerate = (fraction) [1, MAX]"));

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

static gboolean gst_mtl_st20p_tx_session_create(Gst_Mtl_St20p_Tx* sink, GstCaps* caps);

static int gst_mtl_st20p_tx_frame_done(void* priv, struct st_frame* frame);

static GstFlowReturn gst_mtl_st20p_tx_zero_copy(Gst_Mtl_St20p_Tx* sink, GstBuffer* buf);
static GstFlowReturn gst_mtl_st20p_tx_mem_copy(Gst_Mtl_St20p_Tx* sink, GstBuffer* buf);

static void gst_mtl_st20p_tx_class_init(Gst_Mtl_St20p_TxClass* klass) {
  GObjectClass* gobject_class;
  GstElementClass* gstelement_class;
  GstVideoSinkClass* gstvideosinkelement_class;

  gobject_class = G_OBJECT_CLASS(klass);
  gstelement_class = GST_ELEMENT_CLASS(klass);
  gstvideosinkelement_class = GST_VIDEO_SINK_CLASS(klass);

  gst_element_class_set_metadata(gstelement_class, "MtlTxSt20Sink", "Sink/Video",
                                 "MTL transmission plugin for SMPTE ST 2110-20 "
                                 "standard (uncompressed video)",
                                 "Dawid Wesierski <dawid.wesierski@intel.com>");

  gst_element_class_add_static_pad_template(gstelement_class,
                                            &gst_mtl_st20p_tx_sink_pad_template);

  gobject_class->set_property = GST_DEBUG_FUNCPTR(gst_mtl_st20p_tx_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR(gst_mtl_st20p_tx_get_property);
  gobject_class->finalize = GST_DEBUG_FUNCPTR(gst_mtl_st20p_tx_finalize);
  gstvideosinkelement_class->parent_class.start =
      GST_DEBUG_FUNCPTR(gst_mtl_st20p_tx_start);

  gst_mtl_common_init_general_arguments(gobject_class);

  g_object_class_install_property(
      gobject_class, PROP_ST20P_TX_RETRY,
      g_param_spec_uint("retry", "Retry Count",
                        "Number of times the MTL will try to get a frame.", 0, G_MAXUINT,
                        10, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST20P_TX_FRAMEBUFF_NUM,
      g_param_spec_uint("tx-framebuff-num", "Number of framebuffers",
                        "Number of framebuffers to be used for transmission.", 0,
                        G_MAXUINT, 3, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST20P_TX_ASYNC_SESSION_CREATE,
      g_param_spec_boolean("async-session-create", "Async Session Create",
                           "Create TX session in a separate thread.", FALSE,
                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST20P_TX_USE_PTS_FOR_PACING,
      g_param_spec_boolean("use-pts-for-pacing", "Use PTS for packet pacing",
                           "This property modifies the default behavior where "
                           "MTL handles packet pacing. "
                           "Instead, it uses the buffer's PTS (Presentation "
                           "Timestamp) to determine the "
                           "precise time for sending packets.",
                           FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ST20P_TX_PTS_PACING_OFFSET,
      g_param_spec_uint("pts-pacing-offset", "PTS offset for packet pacing",
                        "Specifies the offset (in nanoseconds) to be added to the "
                        "Presentation Timestamp (PTS) "
                        "for precise packet pacing. This allows fine-tuning of the "
                        "transmission timing when using PTS-based pacing.",
                        0, G_MAXUINT, 1080, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static gboolean gst_mtl_st20p_tx_start(GstBaseSink* bsink) {
  Gst_Mtl_St20p_Tx* sink = GST_MTL_ST20P_TX(bsink);

  GST_DEBUG_OBJECT(sink, "start");
  GST_DEBUG("Media Transport Initialization start");
  gst_base_sink_set_async_enabled(bsink, FALSE);

  sink->mtl_lib_handle = gst_mtl_common_init_handle(&(sink->generalArgs), FALSE);

  if (!sink->mtl_lib_handle) {
    GST_ERROR("Could not initialize MTL");
    return FALSE;
  }

  if (sink->retry_frame == 0) {
    sink->retry_frame = 10;
  } else if (sink->retry_frame < 3) {
    GST_WARNING("Retry count is too low, setting to 3");
    sink->retry_frame = 3;
  }

  if (sink->async_session_create) {
    pthread_mutex_init(&sink->session_mutex, NULL);
    sink->session_ready = FALSE;
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
    gst_mtl_common_set_general_arguments(object, prop_id, value, pspec,
                                         &(self->generalArgs), &(self->portArgs));
    return;
  }

  switch (prop_id) {
    case PROP_ST20P_TX_RETRY:
      self->retry_frame = g_value_get_uint(value);
      break;
    case PROP_ST20P_TX_FRAMEBUFF_NUM:
      self->framebuffer_num = g_value_get_uint(value);
      break;
    case PROP_ST20P_TX_ASYNC_SESSION_CREATE:
      self->async_session_create = g_value_get_boolean(value);
      break;
    case PROP_ST20P_TX_USE_PTS_FOR_PACING:
      self->use_pts_for_pacing = g_value_get_boolean(value);
      break;
    case PROP_ST20P_TX_PTS_PACING_OFFSET:
      self->pts_for_pacing_offset = g_value_get_uint(value);
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
    gst_mtl_common_get_general_arguments(object, prop_id, value, pspec,
                                         &(sink->generalArgs), &(sink->portArgs));
    return;
  }

  switch (prop_id) {
    case PROP_ST20P_TX_RETRY:
      g_value_set_uint(value, sink->retry_frame);
      break;
    case PROP_ST20P_TX_FRAMEBUFF_NUM:
      g_value_set_uint(value, sink->framebuffer_num);
      break;
    case PROP_ST20P_TX_ASYNC_SESSION_CREATE:
      g_value_set_boolean(value, sink->async_session_create);
      break;
    case PROP_ST20P_TX_USE_PTS_FOR_PACING:
      g_value_set_boolean(value, sink->use_pts_for_pacing);
      break;
    case PROP_ST20P_TX_PTS_PACING_OFFSET:
      g_value_set_uint(value, sink->pts_for_pacing_offset);
      break;
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

  sink->zero_copy = (ops_tx.transport_fmt != st_frame_fmt_to_transport(ops_tx.input_fmt));
  if (sink->zero_copy) {
    ops_tx.flags |= ST20P_TX_FLAG_EXT_FRAME;
    ops_tx.notify_frame_done = gst_mtl_st20p_tx_frame_done;
  } else {
    GST_WARNING("Using memcpy path");
  }

  if (info->fps_d != 0) {
    ops_tx.fps = st_frame_rate_to_st_fps((double)info->fps_n / info->fps_d);
    if (ops_tx.fps == ST_FPS_MAX) {
      GST_ERROR("Unsupported framerate from caps: %d/%d", info->fps_n, info->fps_d);
      return FALSE;
    }
  } else {
    GST_ERROR("Invalid framerate, denominator is 0");
    return FALSE;
  }

  gst_mtl_common_copy_general_to_session_args(&(sink->generalArgs), &(sink->portArgs));

  ops_tx.port.num_port =
      gst_mtl_common_parse_tx_port_arguments(&ops_tx.port, &sink->portArgs);
  if (!ops_tx.port.num_port) {
    GST_ERROR("Failed to parse port arguments");
    return FALSE;
  }

  if (sink->use_pts_for_pacing) {
    ops_tx.flags |= ST20P_TX_FLAG_USER_PACING;
  } else if (sink->pts_for_pacing_offset) {
    GST_WARNING("PTS offset specified but PTS-based pacing is not enabled");
  }

  gst_video_info_free(info);

  sink->tx_handle = st20p_tx_create(sink->mtl_lib_handle, &ops_tx);
  if (!sink->tx_handle) {
    GST_ERROR("Failed to create st20p tx handle");
    return FALSE;
  }

  if (sink->async_session_create) {
    pthread_mutex_lock(&sink->session_mutex);
    sink->session_ready = TRUE;
    pthread_mutex_unlock(&sink->session_mutex);
  }

  sink->frame_size = st20p_tx_frame_size(sink->tx_handle);
  return TRUE;
}

static void* gst_mtl_st20p_tx_session_create_thread(void* data) {
  GstMtlSt20pTxThreadData* thread_data = (GstMtlSt20pTxThreadData*)data;
  gboolean result = gst_mtl_st20p_tx_session_create(thread_data->sink, thread_data->caps);
  if (!result) {
    GST_ELEMENT_ERROR(thread_data->sink, RESOURCE, FAILED, (NULL),
                      ("Failed to create TX session in worker thread"));
  }
  gst_caps_unref(thread_data->caps);
  free(thread_data);
  return NULL;
}

static gboolean gst_mtl_st20p_tx_sink_event(GstPad* pad, GstObject* parent,
                                            GstEvent* event) {
  GstMtlSt20pTxThreadData* thread_data;
  Gst_Mtl_St20p_Tx* sink;
  GstCaps* caps;
  gint ret;

  sink = GST_MTL_ST20P_TX(parent);

  GST_LOG_OBJECT(sink, "Received %s event: %" GST_PTR_FORMAT, GST_EVENT_TYPE_NAME(event),
                 event);

  ret = GST_EVENT_TYPE(event);

  switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_CAPS:
      if (sink->session_capabilites_set) {
        GST_WARNING("Capabilities already set, ignoring");
        return gst_pad_event_default(pad, parent, event);
      }

      gst_event_parse_caps(event, &caps);
      if (sink->async_session_create) {
        thread_data = malloc(sizeof(GstMtlSt20pTxThreadData));
        thread_data->sink = sink;
        thread_data->caps = gst_caps_ref(caps);
        pthread_create(&sink->session_thread, NULL,
                       gst_mtl_st20p_tx_session_create_thread, thread_data);
      } else {
        ret = gst_mtl_st20p_tx_session_create(sink, caps);
        if (!ret) {
          GST_ERROR("Failed to create TX session");
          return FALSE;
        }
      }
      sink->session_capabilites_set = TRUE;
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

  if (sink->async_session_create) {
    pthread_mutex_lock(&sink->session_mutex);
    gboolean session_ready = sink->session_ready;
    pthread_mutex_unlock(&sink->session_mutex);

    if (!session_ready) {
      GST_WARNING("Session not ready, dropping buffer");
      gst_buffer_unref(buf);
      return GST_FLOW_OK;
    }
  }

  if (!sink->tx_handle) {
    GST_ERROR("Tx handle not initialized");
    return GST_FLOW_ERROR;
  }

  if (sink->zero_copy) {
    return gst_mtl_st20p_tx_zero_copy(sink, buf);
  } else {
    return gst_mtl_st20p_tx_mem_copy(sink, buf);
  }
}

static void gst_mtl_st20p_tx_finalize(GObject* object) {
  Gst_Mtl_St20p_Tx* sink = GST_MTL_ST20P_TX(object);

  if (sink->async_session_create) {
    if (sink->session_thread) pthread_join(sink->session_thread, NULL);
    pthread_mutex_destroy(&sink->session_mutex);
  }

  if (sink->tx_handle) {
    if (st20p_tx_free(sink->tx_handle)) GST_ERROR("Failed to free tx handle");
  }

  if (sink->mtl_lib_handle) {
    if (gst_mtl_common_deinit_handle(&sink->mtl_lib_handle))
      GST_ERROR("Failed to uninitialize MTL library");
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

static int gst_mtl_st20p_tx_frame_done(void* priv, struct st_frame* frame) {
  GstSt20pTxExternalDataChild* child = frame->opaque;
  GstSt20pTxExternalDataParent* parent = child->parent;

  gst_memory_unmap(child->gst_buffer_memory, &child->map_info);
  free(child);

  pthread_mutex_lock(&parent->parent_mutex);
  parent->child_count--;
  if (parent->child_count > 0) {
    pthread_mutex_unlock(&parent->parent_mutex);
    return 0;
  }

  pthread_mutex_unlock(&parent->parent_mutex);
  gst_buffer_unref(parent->buf);
  pthread_mutex_destroy(&parent->parent_mutex);
  free(parent);

  return 0;
}

static GstFlowReturn gst_mtl_st20p_tx_zero_copy(Gst_Mtl_St20p_Tx* sink, GstBuffer* buf) {
  GstSt20pTxExternalDataChild* child;
  GstSt20pTxExternalDataParent* parent;
  struct st_frame* frame;
  struct st_ext_frame ext_frame;
  GstVideoMeta* video_meta = NULL;
  gint buffer_n = gst_buffer_n_memory(buf);

  parent = malloc(sizeof(GstSt20pTxExternalDataParent));
  if (!parent) {
    GST_ERROR("Failed to allocate memory for parent structure");
    return GST_FLOW_ERROR;
  }
  parent->buf = buf;
  parent->child_count = buffer_n;
  pthread_mutex_init(&parent->parent_mutex, NULL);

  for (int i = 0; i < buffer_n; i++) {
    child = malloc(sizeof(GstSt20pTxExternalDataChild));
    if (!child) {
      GST_ERROR("Failed to allocate memory for child structure");
      free(parent);
    }
    child->parent = parent;
    child->gst_buffer_memory = gst_buffer_peek_memory(buf, i);

    if (!gst_memory_map(child->gst_buffer_memory, &child->map_info, GST_MAP_READ)) {
      GST_ERROR("Failed to map memory");
      free(child);
      free(parent);
      return GST_FLOW_ERROR;
    }

    if (child->map_info.size < sink->frame_size) {
      GST_ERROR("Buffer size %lu is smaller than frame size %d", child->map_info.size,
                sink->frame_size);
      gst_memory_unmap(child->gst_buffer_memory, &child->map_info);
      free(child);
      free(parent);
      return GST_FLOW_ERROR;
    }

    frame = st20p_tx_get_frame(sink->tx_handle);
    if (!frame) {
      GST_ERROR("Failed to get frame");
      return GST_FLOW_ERROR;
    }

    // By default, timestamping is handled by MTL.
    if (sink->use_pts_for_pacing) {
      frame->timestamp = GST_BUFFER_PTS(buf) += sink->pts_for_pacing_offset;
      frame->tfmt = ST10_TIMESTAMP_FMT_TAI;
    }

    video_meta = gst_buffer_get_video_meta(buf);
    if (video_meta) {
      for (int i = 0; i < video_meta->n_planes; i++) {
        ext_frame.addr[i] = child->map_info.data + video_meta->offset[i];
        ext_frame.linesize[i] = video_meta->stride[i];
        ext_frame.iova[i] = 0;
      }

    } else {
      ext_frame.addr[0] = child->map_info.data;
      ext_frame.iova[0] = 0;
      ext_frame.linesize[0] = st_frame_least_linesize(frame->fmt, frame->width, 0);
      guint8 planes = st_frame_fmt_planes(frame->fmt);

      /* Assume video planes are stored contiguously in memory */
      for (gint plane = 1; plane < planes; plane++) {
        ext_frame.linesize[plane] =
            st_frame_least_linesize(frame->fmt, frame->width, plane);
        ext_frame.addr[plane] = (guint8*)ext_frame.addr[plane - 1] +
                                ext_frame.linesize[plane - 1] * frame->height;
        ext_frame.iova[plane] = 0;
      }
    }

    ext_frame.size = child->map_info.size;
    ext_frame.opaque = child;
    frame->opaque = NULL;

    st20p_tx_put_ext_frame(sink->tx_handle, frame, &ext_frame);
  }

  return GST_FLOW_OK;
}

static GstFlowReturn gst_mtl_st20p_tx_mem_copy(Gst_Mtl_St20p_Tx* sink, GstBuffer* buf) {
  gint buffer_size, buffer_n = gst_buffer_n_memory(buf);
  struct st_frame* frame = NULL;
  gint frame_size = sink->frame_size;
  GstMemory* gst_buffer_memory;
  GstMapInfo map_info;

  for (int i = 0; i < buffer_n; i++) {
    gst_buffer_memory = gst_buffer_peek_memory(buf, i);

    if (!gst_memory_map(gst_buffer_memory, &map_info, GST_MAP_READ)) {
      GST_ERROR("Failed to map memory");
      return GST_FLOW_ERROR;
    }
    buffer_size = map_info.size;

    if (buffer_size < frame_size) {
      GST_ERROR("Buffer size %d is smaller than frame size %d", buffer_size, frame_size);
      gst_memory_unmap(gst_buffer_memory, &map_info);
      return GST_FLOW_ERROR;
    }

    frame = st20p_tx_get_frame(sink->tx_handle);
    if (!frame) {
      GST_ERROR("Failed to get frame");
      return GST_FLOW_ERROR;
    }

    // By default, timestamping is handled by MTL.
    if (sink->use_pts_for_pacing) {
      frame->timestamp = GST_BUFFER_PTS(buf) += sink->pts_for_pacing_offset;
      frame->tfmt = ST10_TIMESTAMP_FMT_TAI;
    }

    memcpy(frame->addr[0], map_info.data, buffer_size);
    gst_memory_unmap(gst_buffer_memory, &map_info);
    st20p_tx_put_frame(sink->tx_handle, frame);
  }

  gst_buffer_unref(buf);
  return GST_FLOW_OK;
}
