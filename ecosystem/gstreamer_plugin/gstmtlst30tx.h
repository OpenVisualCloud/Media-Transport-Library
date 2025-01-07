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

#ifndef __GST_MTL_ST30P_TX_H__
#define __GST_MTL_ST30P_TX_H__

#include <arpa/inet.h>
#include <gst/audio/audio.h>
#include <gst/gst.h>
#include <mtl/mtl_api.h>
// #include <mtl/st_pipeline_api.h>
#include <mtl/st30_pipeline_api.h>

G_BEGIN_DECLS

#ifndef NS_PER_MS
#define NS_PER_MS (1000 * 1000)
#endif

#ifndef NS_PER_S
#define NS_PER_S (1000 * NS_PER_MS)
#endif

#define GST_TYPE_MTL_ST30TX (gst_mtlst30tx_get_type())
G_DECLARE_FINAL_TYPE(GstMtlSt30Tx, gst_mtlst30tx, GST, MTL_ST30TX, GstAudioSink)

typedef struct StDevArgs {
  gchar port[MTL_PORT_MAX_LEN];
  gchar local_ip_string[MTL_PORT_MAX_LEN];
  gint tx_queues_cnt[MTL_PORT_MAX];
  gint rx_queues_cnt[MTL_PORT_MAX];
  gchar dma_dev[MTL_PORT_MAX_LEN];
} StDevArgs;

typedef struct StTxSessionPortArgs {
  gchar tx_ip_string[MTL_PORT_MAX_LEN];
  gchar port[MTL_PORT_MAX_LEN];
  gint udp_port;
  gint payload_type;
} StTxSessionPortArgs;

struct _GstMtlSt30Tx {
  GstAudioSink element;
  GstElement* child;
  gboolean silent;
  mtl_handle mtl_lib_handle;
  st30p_tx_handle tx_handle;

  struct st30_frame* cur_frame;
  guint cur_frame_available_size;

  /* arguments for incomplete frame buffers */
  guint retry_frame;
  guint frame_size;

  /* arguments for imtl initialization device */
  StDevArgs devArgs;
  /* arguments for imtl tx session */
  StTxSessionPortArgs portArgs;

  /* arguments for session */
  guint framebuffer_num;
  guint framerate;

  /* TODO add support for gpu direct */
#ifdef MTL_GPU_DIRECT_ENABLED
  gboolean gpu_direct_enabled;
  gint gpu_driver_index;
  gint gpu_device_index;
  gboolean* gpu_context;
#endif /* MTL_GPU_DIRECT_ENABLED */
};

G_END_DECLS

#endif /* __GST_MTL_ST30P_TX_H__ */
