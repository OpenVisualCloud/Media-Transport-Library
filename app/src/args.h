/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include <stdio.h>

#ifndef _ST_APP_ARGS_HEAD_H_
#define _ST_APP_ARGS_HEAD_H_

int st_app_parse_args(struct st_app_context *ctx, struct mtl_init_params *p,
                      int argc, char **argv);

#endif
