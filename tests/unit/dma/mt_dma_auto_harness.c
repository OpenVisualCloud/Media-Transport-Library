/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#define _GNU_SOURCE /* as mt_main.h, before any system header */
#include "dma/mt_dma_auto_harness.h"

#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#undef MTL_HAS_USDT
#include <rte_dmadev.h>

#include "common/ut_common.h"
#include "mt_main.h"

#define UT_DMA_AUTO_PROBES_MAX (64)
#define UT_DMA_AUTO_ROOT_MAX (64)

struct ut_dma_auto_ctx {
  struct mtl_main_impl impl;
  bool dma_inited;
  char root[UT_DMA_AUTO_ROOT_MAX];
  int nb_dmadev;
  int nb_probes;
  int probe_ret[UT_DMA_AUTO_PROBES_MAX];
  int probe_dmadevs[UT_DMA_AUTO_PROBES_MAX];
  char probe_bdf[UT_DMA_AUTO_PROBES_MAX][NAME_MAX + 1];
};

static struct ut_dma_auto_ctx* ut_active_ctx;
static const char ut_dma_real_pci_dir[] = "/sys/bus/pci/devices";
static char ut_dma_pci_dir[UT_DMA_AUTO_ROOT_MAX] = "/sys/bus/pci/devices";

static int ut_rte_dev_probe(const char* devargs);
static uint16_t ut_rte_dma_count_avail(void);

#define MT_DMA_PCI_DEVICES_DIR ut_dma_pci_dir
#define rte_dev_probe ut_rte_dev_probe
#define rte_dma_count_avail ut_rte_dma_count_avail
#include "mt_dma.c"
#include "mt_dma_auto.c"
#undef rte_dma_count_avail
#undef rte_dev_probe

static int ut_rte_dev_probe(const char* devargs) {
  struct ut_dma_auto_ctx* ctx = ut_active_ctx;

  if (!ctx) return rte_dev_probe(devargs);
  if (ctx->nb_probes >= UT_DMA_AUTO_PROBES_MAX) return -ENOSPC;
  int call = ctx->nb_probes++;
  snprintf(ctx->probe_bdf[call], sizeof(ctx->probe_bdf[call]), "%s", devargs);
  if (!ctx->probe_ret[call]) ctx->nb_dmadev += ctx->probe_dmadevs[call];
  return ctx->probe_ret[call];
}

static uint16_t ut_rte_dma_count_avail(void) {
  if (!ut_active_ctx) return rte_dma_count_avail();
  return ut_active_ctx->nb_dmadev;
}

static int ut_write_val(const char* dir, const char* attr, const char* fmt, long val) {
  char path[PATH_MAX];

  if (snprintf(path, sizeof(path), "%s/%s", dir, attr) >= (int)sizeof(path))
    return -ENAMETOOLONG;
  FILE* fp = fopen(path, "w");
  if (!fp) return -errno;
  fprintf(fp, fmt, val);
  fprintf(fp, "\n");
  return fclose(fp) ? -errno : 0;
}

static int ut_rm_entry(const char* path, const struct stat* sb, int flag,
                       struct FTW* ftw) {
  (void)sb;
  (void)flag;
  (void)ftw;
  return remove(path);
}

ut_dma_auto_ctx* ut_dma_auto_create(void) {
  if (ut_eal_init() < 0) return NULL;

  struct ut_dma_auto_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;
  snprintf(ctx->root, sizeof(ctx->root), "/tmp/mt_dma_auto_test.XXXXXX");
  if (!mkdtemp(ctx->root)) {
    free(ctx);
    return NULL;
  }
  for (int i = 0; i < UT_DMA_AUTO_PROBES_MAX; i++) ctx->probe_dmadevs[i] = 1;
  ctx->impl.type = MT_HANDLE_MAIN;

  snprintf(ut_dma_pci_dir, sizeof(ut_dma_pci_dir), "%s", ctx->root);
  ut_active_ctx = ctx;
  return ctx;
}

void ut_dma_auto_destroy(ut_dma_auto_ctx* ctx) {
  if (!ctx) return;
  if (ctx->dma_inited) {
    mt_dma_uinit(&ctx->impl);
    mt_pthread_mutex_destroy(&mt_get_dma_mgr(&ctx->impl)->mutex);
  }
  ut_active_ctx = NULL;
  snprintf(ut_dma_pci_dir, sizeof(ut_dma_pci_dir), "%s", ut_dma_real_pci_dir);
  nftw(ctx->root, ut_rm_entry, 16, FTW_DEPTH | FTW_PHYS);
  free(ctx);
}

void ut_dma_auto_add_port(ut_dma_auto_ctx* ctx, int socket_id) {
  struct mtl_init_params* p = mt_get_user_params(&ctx->impl);

  ctx->impl.inf[p->num_ports].socket_id = socket_id;
  p->num_ports++;
}

int ut_dma_auto_add_dev(ut_dma_auto_ctx* ctx, const char* bdf, uint16_t vendor,
                        uint16_t device, const char* driver, int numa_node) {
  char dir[PATH_MAX];
  int ret;

  if (snprintf(dir, sizeof(dir), "%s/%s", ctx->root, bdf) >= (int)sizeof(dir))
    return -ENAMETOOLONG;
  if (mkdir(dir, 0755) < 0) return -errno;
  ret = ut_write_val(dir, "vendor", "0x%04lx", vendor);
  if (ret < 0) return ret;
  ret = ut_write_val(dir, "device", "0x%04lx", device);
  if (ret < 0) return ret;
  ret = ut_write_val(dir, "numa_node", "%ld", numa_node);
  if (ret < 0) return ret;
  if (!driver) return 0;

  char link[PATH_MAX];
  char target[PATH_MAX];
  if (snprintf(link, sizeof(link), "%s/driver", dir) >= (int)sizeof(link))
    return -ENAMETOOLONG;
  if (snprintf(target, sizeof(target), "../../../bus/pci/drivers/%s", driver) >=
      (int)sizeof(target))
    return -ENAMETOOLONG;
  return symlink(target, link) < 0 ? -errno : 0;
}

void ut_dma_auto_set_probe(ut_dma_auto_ctx* ctx, int call, int ret, int nb_dmadev) {
  ctx->probe_ret[call] = ret;
  ctx->probe_dmadevs[call] = nb_dmadev;
}

int ut_dma_auto_init(ut_dma_auto_ctx* ctx, bool dma_auto) {
  struct mtl_init_params* p = mt_get_user_params(&ctx->impl);

  if (dma_auto) p->flags |= MTL_FLAG_DMA_AUTO;
  int ret = mt_dma_init(&ctx->impl);
  if (!ret) ctx->dma_inited = true;
  return ret;
}

int ut_dma_auto_nb_probes(ut_dma_auto_ctx* ctx) {
  return ctx->nb_probes;
}

const char* ut_dma_auto_probe_bdf(ut_dma_auto_ctx* ctx, int call) {
  return ctx->probe_bdf[call];
}
