/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "app_base.h"

#ifndef _ST_APP_CORES_HEAD_H_
#define _ST_APP_CORES_HEAD_H_

/*
 * The st20p session threads run pinned to lcores taken with mtl_get_lcore(), off the
 * CPUs the schedulers hold, and the count follows their measured load. These lcores
 * compete with the ones MTL takes at runtime. Start, tick and free run on the main
 * thread; st_app_cores_stat() runs on MTL's stat thread and reads scalars only.
 */

/* after mtl_start(), when the schedulers hold their lcores */
int st_app_cores_start(struct st_app_context* ctx);

/* once a second: measure the threads, resize and rebalance */
void st_app_cores_tick(struct st_app_context* ctx);

void st_app_cores_stat(struct st_app_context* ctx);

/* after the session threads are joined */
void st_app_cores_free(struct st_app_context* ctx);

#endif
