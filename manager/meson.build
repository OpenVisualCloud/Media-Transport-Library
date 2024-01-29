# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

project('mtl_manager', 'cpp', default_options: ['buildtype=release', 'cpp_std=c++17'], version: '0.2.0')

exec_env = host_machine.system()
set_variable('is_windows', exec_env == 'windows')

if is_windows
  message('not supported on Windows')
  subdir_done()
endif

message('BUILD Environment: ' + exec_env)

cpp_c = meson.get_compiler('cpp')

sources = ['mtl_manager.cpp']

cpp_args = ['-std=c++17', '-Wall']
link_cpp_args = ['-lstdc++fs']

if get_option('buildtype') != 'debug'
  cpp_args += ['-Werror']
endif

# default no asan dep
asan_dep = []
if get_option('enable_asan') == true
  message('Enable -fsanitize=address')
  cpp_args += ['-fsanitize=address']
  asan_dep = cpp_c.find_library('asan', required : true)
endif

# xdp check
libxdp_dep = dependency('libxdp', required: false)
libbpf_dep = dependency('libbpf', required: false)
if libxdp_dep.found() and libbpf_dep.found()
  add_global_arguments('-DMTL_HAS_XDP_BACKEND', language : 'cpp')
  clang = find_program('clang')
  llvm_strip = find_program('llvm-strip')
  # Build XDP prog obj
  xdp_src = files('mtl.xdp.c')
  xdp_temp_obj = custom_target('mtl.xdp.temp.o',
    input : xdp_src,
    output : 'mtl.xdp.temp.o',
    command : [clang, '-g', '-O2', '-target', 'bpf', '-c', '@INPUT@', '-o', '@OUTPUT@']
  )
  xdp_obj = custom_target('mtl.xdp.o',
    depends : xdp_temp_obj,
    input : xdp_temp_obj,
    output : 'mtl.xdp.o',
    install: true,
    install_dir: '/var/run/imtl',
    command : [llvm_strip, '-g', '@INPUT@', '-o', '@OUTPUT@']
  )
else
  message('libxdp and libbpf not found, no af_xdp backend')
endif

executable('MtlManager', sources,
  cpp_args: cpp_args,
  link_args: link_cpp_args,
  install: true,
  dependencies: [asan_dep, libxdp_dep, libbpf_dep]
)