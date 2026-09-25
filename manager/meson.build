# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

project('mtl_manager', 'cpp', default_options: ['buildtype=release', 'cpp_std=c++17', 'b_pie=true'],
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

# Intel SDL / OpenSSF compiler hardening, see doc/build.md
add_project_arguments(cpp_c.get_supported_arguments(['-fstack-protector-strong',
  '-fstack-clash-protection', '-fcf-protection=full',
  '-Wformat', '-Wformat-security', '-Werror=format-security']), language : 'cpp')
# keep the level a compiler (Ubuntu 24.04 gcc: 3) or CXXFLAGS already set
if get_option('optimization') not in ['0', 'plain'] and cpp_c.get_define('_FORTIFY_SOURCE', args : '-O2') == ''
  add_project_arguments('-D_FORTIFY_SOURCE=2', language : 'cpp')
endif
add_project_link_arguments('-Wl,-z,relro', '-Wl,-z,now', '-Wl,-z,noexecstack', language : 'cpp')

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
  multiarch = run_command(find_program('cc'), '-dumpmachine', check: true).stdout().strip()
  # Build XDP prog obj
  xdp_src = files('mtl.xdp.c')
  xdp_temp_obj = custom_target('mtl.xdp.temp.o',
    input : xdp_src,
    output : 'mtl.xdp.temp.o',
    command : [clang, '-g', '-O2', '-target', 'bpf', '-I/usr/include/' + multiarch,
      '-c', '@INPUT@', '-o', '@OUTPUT@']
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