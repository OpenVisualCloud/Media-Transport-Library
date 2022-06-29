# Copyright (C) 2022 Intel Corporation.
#
# This software and the related documents are Intel copyrighted materials,
# and your use of them is governed by the express license under which they
# were provided to you ("License").
# Unless the License provides otherwise, you may not use, modify, copy,
# publish, distribute, disclose or transmit this software or the related
# documents without Intel's prior written permission.
#
# This software and the related documents are provided as is, with no
# express or implied warranties, other than those that are expressly stated
# in the License.

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
shared_library('st_plugin_jpegxs_sample', plugins_jpegxs_sample_sources,
  c_args : plugins_c_args,
  link_args : plugins_ld_args,
  dependencies: [libst_dpdk_dep, libpthread_dep],
  install: true
)