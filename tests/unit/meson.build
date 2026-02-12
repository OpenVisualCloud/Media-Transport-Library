# SPDX-License-Identifier: BSD-3-Clause
# Unit tests for internal MTL helpers (no fuzzer, no EAL required).

unit_include_dirs = [
  include_directories('../../lib/src'),
]

unit_test_targets = [
  ['st40_rx_dedup_test', 'st40_rx_dedup_test.c'],
]

foreach target : unit_test_targets
  exe_name = target[0]
  src = target[1]
  test_exe = executable(
    exe_name,
    src,
    include_directories : unit_include_dirs,
    dependencies : [mtl_internal_dep, dpdk_dep],
    install : false,
  )
  test(exe_name, test_exe)
endforeach
