/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
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

struct st20_convert_session_impl* st20_get_converter(
    struct st_main_impl* impl, struct st20_get_converter_request* req);
int st20_convert_notify_frame_ready(struct st20_convert_session_impl* converter);
int st20_put_converter(struct st_main_impl* impl,
                       struct st20_convert_session_impl* converter);

int st_plugins_init(struct st_main_impl* impl);
int st_plugins_uinit(struct st_main_impl* impl);

int st_plugins_dump(struct st_main_impl* impl);

#endif
