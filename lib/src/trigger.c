/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "mt_arp.h"

#include "datapath/mt_queue.h"
// #define DEBUG
#include "mt_log.h"
#include "mt_socket.h"
#include "mt_util.h"

#define ARP_REQ_PERIOD_MS (500)
#define ARP_REQ_PERIOD_US (ARP_REQ_PERIOD_MS * 1000)
