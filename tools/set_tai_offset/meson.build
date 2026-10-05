# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

project('set_tai_offset', 'c',
  default_options: ['buildtype=release', 'b_pie=true'],
)

cc = meson.get_compiler('c')

# Compiler hardening, see doc/build.md
add_project_arguments(cc.get_supported_arguments(['-fstack-protector-strong',
  '-fstack-clash-protection', '-fcf-protection=full']),
  '-Wformat', '-Wformat-security', '-Werror=format-security', language : 'c')
# keep the level a compiler (Ubuntu 24.04 gcc: 3) or CFLAGS already set, asking the
# compiler itself: meson >= 1.11 runs its checks with -U_FORTIFY_SOURCE
fortify = run_command(cc.cmd_array(), get_option('c_args'), '-O2', '-dM', '-E', '-x', 'c', '/dev/null',
  check: true).stdout()
if get_option('optimization') not in ['0', 'plain'] and not fortify.contains('_FORTIFY_SOURCE')
  add_project_arguments('-D_FORTIFY_SOURCE=2', language : 'c')
endif
add_project_link_arguments('-Wl,-z,relro', '-Wl,-z,now', '-Wl,-z,noexecstack', language : 'c')

executable('set_tai_offset', 'set_tai_offset.c',
  install: false,
)
