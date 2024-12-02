/*
 * GStreamer
 * Copyright (C) 2005 Thomas Vander Stichele <thomas@apestaart.org>
 * Copyright (C) 2005 Ronald S. Bultje <rbultje@ronald.bitfreak.net>
 * Copyright (C) 2020 Niels De Graef <niels.degraef@gmail.com>
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

#ifndef __GST_MTL_TX_SINK_H__
#define __GST_MTL_TX_SINK_H__

#include <gst/gst.h>
#include <gst/video/video.h>
#include <arpa/inet.h>
#include <mtl/st_pipeline_api.h>
#include <mtl/mtl_api.h>


G_BEGIN_DECLS

#ifndef NS_PER_MS
#define NS_PER_MS (1000 * 1000)
#endif

#ifndef NS_PER_S
#define NS_PER_S (1000 * NS_PER_MS)
#endif


#define GST_TYPE_MTL_TX_SINK (gst_mtltxsink_get_type())
G_DECLARE_FINAL_TYPE (GstMtlTxSink, gst_mtltxsink,
    GST, MTL_TX_SINK, GstVideoSink)


// main handle arguments
typedef struct StDevArgs {
  gchar   port[MTL_PORT_MAX_LEN];
  gchar   local_ip_string[MTL_PORT_MAX_LEN];
  gint    tx_queues_cnt[MTL_PORT_MAX];
  gint    rx_queues_cnt[MTL_PORT_MAX];
  gchar   dma_dev[MTL_PORT_MAX_LEN];
} StDevArgs;

typedef struct StTxSessionPortArgs {
  gchar   tx_ip_string[MTL_PORT_MAX_LEN];
  gchar   port[MTL_PORT_MAX_LEN];
  gint    udp_port;
  gint    payload_type;
} StTxSessionPortArgs;

typedef struct StFpsDecs {
  enum          st_fps st_fps;
  unsigned int  min;
  unsigned int  max;
} StFpsDecs;
struct _GstMtlTxSink
{
  GstVideoSink        element;
  GstElement*         child;
  gboolean            silent;

  mtl_handle          mtl_lib_handle;
  st20p_tx_handle     tx_handle;
  gint                frame_size;
  struct st_frame*    frame_in_tranmission;
  gint                frame_in_tranmission_data_pointer;

  /* arguments for devices */
  StDevArgs           devArgs;
  /* arguments for session port */
  StTxSessionPortArgs portArgs;
  /* arguments for session */
  gint                width;
  gint                height;
  void*               pixel_format;
  void*               framerate;
  gint                fb_cnt;
  gint                timeout_sec;
  gint                session_init_retry;



  int64_t             frame_counter;

#ifdef MTL_GPU_DIRECT_ENABLED
  bool                gpu_direct_enabled;
  int                 gpu_driver_index;
  int                 gpu_device_index;
  void*               gpu_context;
#endif /* MTL_GPU_DIRECT_ENABLED */
};

G_END_DECLS

#endif /* __GST_MTL_TX_SINK_H__ */
