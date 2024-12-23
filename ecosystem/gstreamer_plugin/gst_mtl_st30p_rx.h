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

#ifndef __GST_MTL_ST30P_RX_H__
#define __GST_MTL_ST30P_RX_H__

#include <gst/base/gstbasesrc.h>

#include "gst_mtl_common.h"

G_BEGIN_DECLS

#define GST_TYPE_MTL_ST30P_RX (gst_mtl_st30p_rx_get_type())
G_DECLARE_FINAL_TYPE(Gst_Mtl_St30p_Rx, gst_mtl_st30p_rx, GST, MTL_ST30P_RX, GstBaseSrc)

struct _Gst_Mtl_St30p_Rx {
  GstBaseSrc element;
  GstBuffer* buffer;

  /*< private >*/
  struct st30p_rx_ops ops_rx;
  gboolean silent;
  mtl_handle mtl_lib_handle;
  st30p_rx_handle rx_handle;

  /* arguments for mtl frame buffers */
  guint retry_frame;
  guint frame_size;

  /* arguments for imtl initialization device */
  StDevArgs devArgs;
  /* arguments for imtl rx session */
  SessionPortArgs portArgs;

  /* arguments for session */
  guint channel;
  guint sampling;
  gboolean ptime;
  gchar audio_format[MTL_PORT_MAX_LEN];
  gint framebuffer_num;
};

G_END_DECLS

#endif /* __GST_MTL_ST30P_RX_H__ */
