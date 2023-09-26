/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 * The data path based on linux kernel socket interface
 */

#include "mt_af_xdp.h"

#include "../mt_log.h"

struct mt_xdp_priv {
  enum mtl_port port;
};

int mt_dev_xdp_init(struct mt_interface* inf) {
  struct mtl_main_impl* impl = inf->parent;
  enum mtl_port port = inf->port;

  if (!mt_pmd_is_native_af_xdp(impl, port)) {
    err("%s(%d), not native af_xdp\n", __func__, port);
    return -EIO;
  }

  struct mt_xdp_priv* xdp = mt_rte_zmalloc_socket(sizeof(*xdp), mt_socket_id(impl, port));
  if (!xdp) {
    err("%s(%d), xdp malloc fail\n", __func__, port);
    return -ENOMEM;
  }

  inf->xdp = xdp;
  info("%s(%d), succ\n", __func__, port);
  return 0;
}

int mt_dev_xdp_uinit(struct mt_interface* inf) {
  enum mtl_port port = inf->port;

  if (inf->xdp) {
    mt_rte_free(inf->xdp);
    inf->xdp = NULL;
  }

  info("%s(%d), succ\n", __func__, port);
  return 0;
}
