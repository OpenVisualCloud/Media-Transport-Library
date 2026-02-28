# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

project('mtl_app', 'c', default_options: ['buildtype=release'],
  version: run_command(find_program('cat'), files('../../../VERSION'), check: true).stdout().strip(),)

gpu_direct = dependency('mtl_gpu_direct', required: false)
if gpu_direct.found()
  add_global_arguments('-DMTL_GPU_DIRECT_ENABLED', language : 'c')
endif

# allow experimental api
add_global_arguments('-DALLOW_EXPERIMENTAL_API', language : 'c')

# detect os
exec_env = host_machine.system()
set_variable('is_windows', exec_env == 'windows')

cc = meson.get_compiler('c')
toolchain = cc.get_id()

mtl = dependency('mtl', required : true)
libm = cc.find_library('m', required : true)
libpthread = cc.find_library('pthread', required : true)
libjson_c = dependency('json-c', required : true)
libpcap = dependency('pcap', required: true)

libsdl2 = dependency('sdl2', required: false)
if libsdl2.found()
  add_global_arguments('-DAPP_HAS_SDL2', language : 'c')
  set_variable('app_has_sdl2', true)
else
  message('SDL2 not found')
  set_variable('app_has_sdl2', false)
endif
libsdl2_ttf = dependency('SDL2_ttf', required: false)
if libsdl2_ttf.found()
  add_global_arguments('-DAPP_HAS_SDL2_TTF', language : 'c')
else
  message('SDL2_ttf not found')
endif

libopenssl = dependency('openssl', required : false)
if libopenssl.found()
  add_global_arguments('-DAPP_HAS_SSL', language : 'c')
else
  message('openssl not found')
endif

# add source file
subdir('src')

app_c_args = []
app_ld_args = []

# enable warning as error for non debug build
if get_option('buildtype') != 'debug'
  app_c_args += ['-Werror']
endif

if get_option('buildtype') != 'release'
  add_global_arguments('-DMTL_SIMULATE_PACKET_DROPS=1', language : 'c')
endif

app_c_args += ['-Wall']
app_c_args += ['-Wunused-parameter']

if is_windows
  app_c_args += ['-DWINDOWSENV']
  windows_include_dir = '..\..\..\..\lib\windows'
  app_c_args += ['-I' + windows_include_dir]
endif

# simd build option, enable sse4.2 default, todo: do we need AVX2/AVX512 for app ?
app_c_args += ['-msse4.2']

# Keep column tracking enabled on large translation units to avoid misleading-indentation noise.
app_c_args += ['-flarge-source-files']

if is_windows
  ws2_32_dep = cc.find_library('ws2_32', required: true)
  mman_dep = cc.find_library('mman', required: true)
else
  ws2_32_dep = [] # add this when the code uses hton/ntoh
  mman_dep = [] # add this when the code uses mmap/munmap
endif

# default no asan dep
asan_dep = []
if get_option('enable_asan') == true
  message('Enable -fsanitize=address for app')
  app_c_args += ['-fsanitize=address']
  asan_dep = cc.find_library('asan', required : true)
endif

# build executable
executable('RxTxApp', sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libjson_c, libpcap, libsdl2, libsdl2_ttf, libm, libpthread, ws2_32_dep, mman_dep, libopenssl]
)
