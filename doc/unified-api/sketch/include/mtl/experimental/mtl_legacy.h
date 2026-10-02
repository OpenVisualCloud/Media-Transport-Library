/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* A hand-formatted design sketch: formatted when it moves to include/ (milestone M0). */
/* clang-format off */
/*
 * mtl_legacy.h - bridge between the unified API and a legacy MTL instance, revision 0.2.
 *
 * While the legacy headers are still installed (until release F+2, migration.md), an application
 * may run legacy and unified sessions in one process on one instance. This header names
 * the legacy handle only as an opaque type, so it needs no legacy header.
 */

#ifndef MTL_EXPERIMENTAL_MTL_LEGACY_H
#define MTL_EXPERIMENTAL_MTL_LEGACY_H

#include "mtl.h"

#if defined(__cplusplus)
extern "C" {
#endif

struct mtl_main_impl; /* the legacy mtl_handle */

/* Wraps a legacy instance. mtl_instance_close() on the wrapper closes the unified sessions,
   queues and timelines made through it (with the close steps of mtl.h) but never stops
   the legacy instance's devices or its legacy sessions; mtl_uninit() does that. CP. */
MTL_API_CP int mtl_instance_from_legacy(struct mtl_main_impl* legacy, mtl_instance_h* out);
/* The legacy handle of an instance, for legacy calls on the same ports. CP. */
MTL_API_CP int mtl_instance_to_legacy(mtl_instance_h mt, struct mtl_main_impl** out);

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_LEGACY_H */
