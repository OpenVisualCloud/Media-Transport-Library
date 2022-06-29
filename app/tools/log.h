/*
 * Copyright (C) 2021 Intel Corporation.
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

/* Header for tools log usage */
#include <stdio.h>

#ifndef _ST_APP_TOOLS_LOG_HEAD_H_
#define _ST_APP_TOOLS_LOG_HEAD_H_

/* log define */
#ifdef DEBUG
#define dbg(...)         \
  do {                   \
    printf(__VA_ARGS__); \
  } while (0)
#else
#define dbg(...) \
  do {           \
  } while (0)
#endif
#define info(...)        \
  do {                   \
    printf(__VA_ARGS__); \
  } while (0)
#define err(...)         \
  do {                   \
    printf(__VA_ARGS__); \
  } while (0)

#endif
