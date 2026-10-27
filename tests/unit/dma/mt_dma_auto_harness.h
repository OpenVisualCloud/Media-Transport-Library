/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness API for the MTL_FLAG_DMA_AUTO probe unit tests: a fake PCI device tree and
 * a scripted rte_dev_probe().
 */

#ifndef TESTS_UNIT_DMA_MT_DMA_AUTO_HARNESS_H
#define TESTS_UNIT_DMA_MT_DMA_AUTO_HARNESS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ut_dma_auto_ctx ut_dma_auto_ctx;

/* A main handle with no port and an empty PCI device tree; NULL on failure. */
ut_dma_auto_ctx* ut_dma_auto_create(void);
void ut_dma_auto_destroy(ut_dma_auto_ctx* ctx);

/* Add a port on NUMA node socket_id. */
void ut_dma_auto_add_port(ut_dma_auto_ctx* ctx, int socket_id);

/* Add a PCI device to the tree; driver NULL gives a device with no driver. */
int ut_dma_auto_add_dev(ut_dma_auto_ctx* ctx, const char* bdf, uint16_t vendor,
                        uint16_t device, const char* driver, int numa_node);

/* The return value and the new DMA device count of probe call number call (from 0).
 * A call that is not set returns 0 and adds one DMA device. */
void ut_dma_auto_set_probe(ut_dma_auto_ctx* ctx, int call, int ret, int nb_dmadev);

/* mt_dma_init() with or without MTL_FLAG_DMA_AUTO. */
int ut_dma_auto_init(ut_dma_auto_ctx* ctx, bool dma_auto);

int ut_dma_auto_nb_probes(ut_dma_auto_ctx* ctx);
/* The device of probe call number call. */
const char* ut_dma_auto_probe_bdf(ut_dma_auto_ctx* ctx, int call);

#ifdef __cplusplus
}
#endif

#endif /* TESTS_UNIT_DMA_MT_DMA_AUTO_HARNESS_H */
