/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Reaches mt_main.c's numa binding from a C++ test. mt_main.h is a C header
 * with no extern "C" of its own, so a C++ translation unit that included it
 * would ask the linker for C++ linkage and not find the symbol.
 */

#ifndef TESTS_UNIT_CORE_MT_NUMA_HARNESS_H
#define TESTS_UNIT_CORE_MT_NUMA_HARNESS_H

#ifdef __cplusplus
extern "C" {
#endif

/** mt_bind_process_numa() of lib/src/mt_main.c. */
int ut_bind_process_numa(int socket_id);

#ifdef __cplusplus
}
#endif

#endif
