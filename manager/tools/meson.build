# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

# MtlManagerProbe tests a running manager from outside it. It links
# mtlm_client_dep and nothing else, so it can only reach the daemon the way any
# other program on the host does: through the two public headers and the control
# socket. A case that needs something the public API does not offer is a case
# that shows a gap in the API.
mtlm_probe = executable('MtlManagerProbe', files('mtlm_probe.c'),
  c_args: c_args,
  install: true,
  dependencies: [asan_dep, mtlm_client_dep],
)
