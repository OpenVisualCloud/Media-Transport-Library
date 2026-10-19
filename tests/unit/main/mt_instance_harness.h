/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness API for the MtlManager IPC unit tests: the instance socket is one end of a
 * socketpair whose peer has already closed, as after the manager exits.
 */

#ifndef TESTS_UNIT_MAIN_MT_INSTANCE_HARNESS_H
#define TESTS_UNIT_MAIN_MT_INSTANCE_HARNESS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_instance_ctx ut_instance_ctx;

/* A main handle connected to a manager that has gone away; NULL on failure. */
ut_instance_ctx* ut_instance_create_orphan(void);
void ut_instance_destroy(ut_instance_ctx* ctx);

int ut_instance_put_lcore(ut_instance_ctx* ctx, unsigned int lcore);
int ut_instance_request_xsks_map_fd(ut_instance_ctx* ctx, unsigned int ifindex);

#ifdef __cplusplus
}
#endif

#endif /* TESTS_UNIT_MAIN_MT_INSTANCE_HARNESS_H */
