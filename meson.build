# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

project('mtl', 'c', default_options: ['buildtype=release'],
  version: run_command(find_program('cat'), files('VERSION'), check: true).stdout().strip(),
)

exec_env = host_machine.system()
set_variable('is_windows', exec_env == 'windows')
message('BUILD Environment: ' + exec_env)

# enable BSD_SOURCE
add_global_arguments('-D_DEFAULT_SOURCE', language : 'c')

# get external variables
add_global_arguments('-D__MTL_GIT__="'+ run_command('git', 'describe', '--abbrev=8', '--dirty', '--always').stdout().strip() + '"', language : 'c')
add_global_arguments('-D__MTL_LIB_BUILD__="' + meson.project_version() + '"', language : 'c')

if get_option('disable_pcapng') == true
  add_global_arguments('-DMTL_DISABLE_PCAPNG', language : 'c')
endif

if is_windows
if get_option('enable_tap') == true
  add_global_arguments('-DMTL_HAS_TAP', language : 'c')
endif
endif

mtl_conf = configuration_data()

# parse mtl config
# parse build config
prj_ver = meson.project_version().split('.')
mtl_conf.set('MTL_VERSION_MAJOR', prj_ver.get(0).to_int())
mtl_conf.set('MTL_VERSION_MINOR', prj_ver.get(1).to_int())
mtl_conf.set('MTL_VERSION_LAST', prj_ver.get(2).to_int())
# parse compiler config
cc_ver = meson.get_compiler('c').get_id() + '-' + meson.get_compiler('c').version()
mtl_conf.set_quoted('MTL_COMPILER', cc_ver)
# build config file
build_cfg = 'mtl_build_config.h'
configure_file(output: build_cfg, configuration: mtl_conf, install_dir: 'include/mtl')

mtl_lib = []

# add include directory
mtl_include_dir = [ include_directories('.', 'include'), ]

# install header files
subdir('include')

# build library
subdir('lib')

pkg = import('pkgconfig')
pkg.generate(
  name : meson.project_name(),
  version : meson.project_version(),
  libraries : mtl_lib,
  filebase : meson.project_name(),
  description : 'Intel® Media Transport Library'
)