/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

/* MTL_FLAG_DMA_AUTO, Linux only: the device scan reads sysfs */

#include <dirent.h>
#include <libgen.h>
#include <rte_dmadev.h>

#include "mt_dma.h"
#include "mt_log.h"

/* Intel devices of the DPDK dmadev drivers: drivers/dma/ioat and drivers/dma/idxd */
static const uint16_t dma_auto_ids[] = {
    0x6f20, 0x6f21, 0x6f22, 0x6f23, 0x6f24,
    0x6f25, 0x6f26, 0x6f27, 0x6f2e, 0x6f2f, /* BDX */
    0x2021,                                 /* SKX */
    0x0b00,                                 /* ICX */
    0x0b25,                                 /* SPR DSA */
    0x11fb,                                 /* GNR-D DSA */
    0x1212,                                 /* DMR DSA */
};

#ifndef MT_DMA_PCI_DEVICES_DIR
#define MT_DMA_PCI_DEVICES_DIR "/sys/bus/pci/devices"
#endif

static long dma_auto_sysfs_val(const char* bdf, const char* attr) {
  char path[PATH_MAX];
  long val = -1;

  (void)snprintf(path, sizeof(path), "%s/%s/%s", MT_DMA_PCI_DEVICES_DIR, bdf, attr);
  FILE* fp = fopen(path, "r");
  if (!fp) return -1;
  if (fscanf(fp, "%li", &val) != 1) val = -1;
  (void)fclose(fp);
  return val;
}

/* the index in socks of the NUMA node of an Intel DMA device on vfio-pci, or -1 */
static int dma_auto_match(const char* bdf, const int* socks, int nb_socks) {
  char path[PATH_MAX];
  char link[128];
  bool known = false;

  if (dma_auto_sysfs_val(bdf, "vendor") != 0x8086) return -1;
  long id = dma_auto_sysfs_val(bdf, "device");
  for (size_t i = 0; i < MTL_ARRAY_SIZE(dma_auto_ids); i++) {
    if (id == dma_auto_ids[i]) known = true;
  }
  if (!known) return -1;

  (void)snprintf(path, sizeof(path), "%s/%s/driver", MT_DMA_PCI_DEVICES_DIR, bdf);
  ssize_t len = readlink(path, link, sizeof(link) - 1);
  if (len < 0) return -1;
  link[len] = '\0';
  if (strcmp(basename(link), "vfio-pci")) return -1;

  long numa = dma_auto_sysfs_val(bdf, "numa_node");
  for (int i = 0; i < nb_socks; i++) {
    if (numa == socks[i]) return i;
  }
  return -1;
}

void mt_dma_auto_probe(struct mtl_main_impl* impl) {
  int socks[MTL_PORT_MAX];
  int used[MTL_PORT_MAX] = {0};
  int nb_socks = 0;
  DIR* dir;
  struct dirent* ent;

  for (int i = 0; i < mt_num_ports(impl); i++) {
    int soc_id = mt_socket_id(impl, i);
    int j;
    for (j = 0; j < nb_socks; j++) {
      if (socks[j] == soc_id) break;
    }
    if (j == nb_socks) socks[nb_socks++] = soc_id;
  }
  if (!nb_socks) return;
  /* each NUMA node of the ports gets the same share of MTL_DMA_DEV_MAX */
  int share = MTL_DMA_DEV_MAX / nb_socks;

  dir = opendir(MT_DMA_PCI_DEVICES_DIR);
  if (!dir) {
    warn("%s, open %s fail\n", __func__, MT_DMA_PCI_DEVICES_DIR);
    return;
  }
  while ((ent = readdir(dir))) {
    int idx = dma_auto_match(ent->d_name, socks, nb_socks);
    if (idx < 0 || used[idx] >= share) continue;
    int nb_dev = rte_dma_count_avail();
    int ret = rte_dev_probe(ent->d_name);
    if (ret < 0 && ret != -EEXIST) {
      info("%s, probe %s fail %d\n", __func__, ent->d_name, ret);
      continue;
    }
    used[idx] += rte_dma_count_avail() - nb_dev;
    info("%s, probe %s succ\n", __func__, ent->d_name);
  }
  closedir(dir);
}
