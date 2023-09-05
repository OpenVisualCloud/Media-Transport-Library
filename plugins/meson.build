# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

project('mtl_plugins', 'c', default_options: ['buildtype=release'],
        version: run_command(find_program('cat', 'more'), files('../VERSION'), check: true).stdout().strip(),)

#detect os
exec_env = host_machine.system()
set_variable('is_windows', exec_env == 'windows')

# allow experimental api
add_global_arguments('-DALLOW_EXPERIMENTAL_API', language : 'c')

cc = meson.get_compiler('c')

mtl_dep = dependency('mtl', required : true)
libpthread_dep = cc.find_library('pthread', required : true)

# add source file
subdir('sample')

plugins_c_args = []
plugins_ld_args = []

# enable warning as error for non debug build
if get_option('buildtype') != 'debug'
  plugins_c_args += ['-Werror']
endif
plugins_c_args += ['-Wall']
plugins_c_args += ['-Wunused-parameter']

#simd build option, enable sse4.2 default, todo: do we need AVX2/AVX512 for app ?
plugins_c_args += ['-msse4.2']

if is_windows
  libdpdklinkflags = run_command('pkg-config', '--static', '--libs', 'libdpdk')
  plugins_ld_args += ['-lmingw32','-lws2_32','-lDbghelp','-lSetupapi','-ldinput8', '-ldxguid', '-ldxerr8', '-lwinmm', '-limm32', '-lversion', '-lwpcap', '-lPacket', '-lmman', '-ldl']
  plugins_ld_args += ['-Wl,--whole-archive']
  plugins_ld_args += [libdpdklinkflags.stdout().strip().split()]
  plugins_ld_args += ['-Wl,--no-whole-archive']
endif

# default no asan dep
asan_dep = []
if get_option('enable_asan') == true
  message('Enable -fsanitize=address')
  plugins_c_args += ['-fsanitize=address']
  asan_dep = cc.find_library('asan', required : true)
endif

# build plugins lib
shared_library('st_plugin_st22_sample', plugins_st22_sample_sources,
  c_args : plugins_c_args,
  link_args : plugins_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl_dep, libpthread_dep],
  install: true
)

shared_library('st_plugin_convert_sample', plugins_convert_sample_sources,
  c_args : plugins_c_args,
  link_args : plugins_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl_dep, libpthread_dep],
  install: true
)