/*
* Copyright (C) 2020-2021 Intel Corporation.
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

/*
 *
 *	Transmitting and receiving example using Media streamer based on DPDK
 *
 */

#ifndef _RX_APP_H
#define _RX_APP_H

#include "common_app.h"

#include <dirent.h>
#include <getopt.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_SESSIONS_MAX 160

typedef struct rxtxapp_main
{
	st_format_t fmt_lists[MAX_SESSIONS_MAX];
	uint32_t fmt_count;
	uint32_t st21_session_count;
	uint32_t st30_session_count;
	uint32_t st40_session_count;

} rxtxapp_main_t;

#endif
