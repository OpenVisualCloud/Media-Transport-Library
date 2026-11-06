# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

project('set_tai_offset', 'c',
  default_options: ['buildtype=release', 'b_pie=true'],
)

cc = meson.get_compiler('c')

# Compiler hardening, see doc/build.md
hardening = run_command(files('../../script/hardening_flags.py'), 'c', get_option('optimization'),
  get_option('b_sanitize'), cc.cmd_array(), get_option('c_args'), check : true).stdout().split('\n')
add_project_arguments(hardening[0].split(), language : 'c')
add_project_link_arguments(hardening[1].split(), language : 'c')

executable('set_tai_offset', 'set_tai_offset.c',
  install: false,
)
