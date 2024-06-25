# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

project('mtl_manager', 'cpp', default_options: ['buildtype=release', 'cpp_std=c++17'],
  version: run_command(find_program('cat'), files('../VERSION'), check: true).stdout().strip(),
)

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
  # enable error on  Variable Length Arrays
  cpp_args += ['-Wvla']
endif

# default no asan dep
asan_dep = []
if get_option('enable_asan') == true
  message('Enable -fsanitize=address')
  cpp_args += ['-fsanitize=address']
  asan_dep = cpp_c.find_library('asan', required : true)
endif

mtlm_conf = configuration_data()
# get external variables
add_global_arguments('-D__MTLM_GIT__="'+ run_command('git', 'describe', '--abbrev=8', '--dirty', '--always', check: false).stdout().strip() + '"', language : 'cpp')
# parse mtlm config
# parse build config
prj_ver = meson.project_version().split('.')
mtlm_conf.set('MTLM_VERSION_MAJOR', prj_ver.get(0).to_int())
mtlm_conf.set('MTLM_VERSION_MINOR', prj_ver.get(1).to_int())
mtlm_conf.set('MTLM_VERSION_LAST', prj_ver.get(2).to_int())
mtlm_conf.set_quoted('MTLM_VERSION_EXTRA', prj_ver.get(3))
# parse compiler config
cpp_c_ver = cpp_c.get_id() + '-' + cpp_c.version()
mtlm_conf.set_quoted('MTLM_COMPILER', cpp_c_ver)
# build config file
build_cfg = 'mtlm_build_config.h'
configure_file(output: build_cfg, configuration: mtlm_conf)

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
    install_dir: get_option('prefix') + '/lib/bpf',
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