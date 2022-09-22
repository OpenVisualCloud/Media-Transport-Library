# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

project('Intel(R) ST plugins', 'c', default_options: ['c_std=gnu11', 'buildtype=release'], version: '0.1',)

#detect os
exec_env = host_machine.system()
set_variable('is_windows', exec_env == 'windows')

# allow experimental api
add_global_arguments('-DALLOW_EXPERIMENTAL_API', language : 'c')

cc = meson.get_compiler('c')

libst_dpdk_dep = dependency('libst_dpdk', required : true)
libpthread_dep = cc.find_library('pthread', required : true)

# add source file
subdir('sample')

plugins_c_args = []
#enable warning as error
plugins_c_args += ['-Werror']
plugins_c_args += ['-Wall']
#simd build option, enable sse4.2 default, todo: do we need AVX2/AVX512 for app ?
plugins_c_args += ['-msse4.2']

plugins_ld_args = []

if is_windows
  plugins_c_args += ['-Wformat=0']
  libdpdklinkflags = run_command('pkg-config', '--static', '--libs', 'libdpdk')
  plugins_ld_args += ['-lmingw32','-lws2_32','-lDbghelp','-lSetupapi','-ldinput8', '-ldxguid', '-ldxerr8', '-lwinmm', '-limm32','-lversion','-lwpcap','-lPacket','-lSDL2','-lSDL2Main','-lmman','-ljson-c','-ldl']
  plugins_ld_args += ['-Wl,--whole-archive']
  plugins_ld_args += [libdpdklinkflags.stdout().strip().split()]
  plugins_ld_args += ['-Wl,--no-whole-archive']
endif

# build plugins lib
shared_library('st_plugin_st22_sample', plugins_st22_sample_sources,
  c_args : plugins_c_args,
  link_args : plugins_ld_args,
  dependencies: [libst_dpdk_dep, libpthread_dep],
  install: true
)

shared_library('st_plugin_convert_sample', plugins_convert_sample_sources,
  c_args : plugins_c_args,
  link_args : plugins_ld_args,
  dependencies: [libst_dpdk_dep, libpthread_dep],
  install: true
)