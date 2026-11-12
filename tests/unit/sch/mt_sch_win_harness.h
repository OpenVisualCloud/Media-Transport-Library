/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Windows C harness API for the lcore shm and lock of the scheduler. Plain C
 * declarations only, so the header is safe to include from C++.
 */

#ifndef TESTS_UNIT_SCH_MT_SCH_WIN_HARNESS_H
#define TESTS_UNIT_SCH_MT_SCH_WIN_HARNESS_H

#include <mtl_api.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A main handle without MtlManager, attached to the lcore shm of the host through the
 * shims of lib/windows, whose lcore get walks lcores 1..nb_lcores under a private lock
 * file. More than one can live at once, each in place of one MTL process, and all walk
 * the nb_lcores of the first. NULL on failure; ut_sch_win_destroy_main puts its lcores
 * and frees it. */
mtl_handle ut_sch_win_create_main(unsigned int nb_lcores);
void ut_sch_win_destroy_main(mtl_handle mt);

/* mt_sch_get_lcore() of the handle. Returns 0 on success. */
int ut_sch_win_get_lcore(mtl_handle mt, unsigned int* lcore);

/* Gives an lcore of mt to a child process that has exited, as if that process took it
 * and was killed. Returns 0 on success. */
int ut_sch_win_orphan_lcore(mtl_handle mt, unsigned int lcore);

/* 1 if a second open of the lock file cannot take the lock while mt holds it, 0 if it
 * can, negative on error. */
int ut_sch_win_lock_excludes(mtl_handle mt);

#ifdef __cplusplus
}
#endif

#endif /* TESTS_UNIT_SCH_MT_SCH_WIN_HARNESS_H */
