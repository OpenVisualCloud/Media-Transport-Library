# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

project('mtl_perf_testing', 'c', default_options: ['buildtype=release'],
  version: run_command(find_program('cat'), files('../../../VERSION'), check: true).stdout().strip(),)

cc = meson.get_compiler('c')

mtl = dependency('mtl', required : true)
libpthread = cc.find_library('pthread', required : true)

perf_c_args = ['-Wall', '-Wunused-parameter']

# enable warning as error for non debug build
if get_option('buildtype') != 'debug'
  perf_c_args += ['-Werror']
endif

executable('perf_testing', 'perf_testing.c',
  c_args : perf_c_args,
  dependencies: [mtl, libpthread],
  install: false,
)
