/*
 * Copyright (C) 2022 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

#ifndef _ST_LIB_PIPELINE_PLUGIN_HEAD_H_
#define _ST_LIB_PIPELINE_PLUGIN_HEAD_H_

#include "../st_main.h"

struct st22_encode_session_impl* st22_get_encoder(struct st_main_impl* impl,
                                                  struct st22_get_encoder_request* req);
int st22_encode_notify_frame_ready(struct st22_encode_session_impl* encoder);
int st22_put_encoder(struct st_main_impl* impl, struct st22_encode_session_impl* encoder);

struct st22_decode_session_impl* st22_get_decoder(struct st_main_impl* impl,
                                                  struct st22_get_decoder_request* req);
int st22_decode_notify_frame_ready(struct st22_decode_session_impl* encoder);
int st22_put_decoder(struct st_main_impl* impl, struct st22_decode_session_impl* encoder);

int st_plugins_init(struct st_main_impl* impl);
int st_plugins_uinit(struct st_main_impl* impl);

int st_plugins_dump(struct st_main_impl* impl);

#endif
