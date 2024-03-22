# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

sources += files(
  'mt_dev.c',
)

if mtl_has_xdp_backend
  sources += files('mt_af_xdp.c')
endif

if mtl_has_rdma_backend
  sources += files('mt_rdma.c')
endif
