# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

project('st_dpdk', 'c', default_options: ['buildtype=release'],
        # Fallback to "more" for Windows
        version: run_command(find_program('cat', 'more'), files('VERSION'), check: true).stdout().strip(),
)

exec_env = host_machine.system()
set_variable('is_windows', exec_env == 'windows')
message('BUILD Enviroment: ' + exec_env)

# enable BSD_SOURCE
add_global_arguments('-D_DEFAULT_SOURCE', language : 'c')

# get external variables
add_global_arguments('-D__ST_GIT__="'+ run_command('git', 'describe', '--abbrev=8', '--dirty', '--always').stdout().strip() + '"', language : 'c')
add_global_arguments('-D__ST_BUILD__="' + meson.project_version() + '"', language : 'c')

if get_option('enable_kni') == true
  add_global_arguments('-DST_HAS_KNI', language : 'c')
endif

if get_option('disable_pcapng') == true
  add_global_arguments('-DST_DISABLE_PCAPNG', language : 'c')
endif

if is_windows
if get_option('enable_tap') == true
  add_global_arguments('-DST_HAS_TAP', language : 'c')
endif
endif

st_conf = configuration_data()

# parse st config
# parse build config
prj_ver = meson.project_version().split('.')
st_conf.set('ST_VERSION_MAJOR', prj_ver.get(0).to_int())
st_conf.set('ST_VERSION_MINOR', prj_ver.get(1).to_int())
st_conf.set('ST_VERSION_LAST', prj_ver.get(2).to_int())
# parse compiler config
cc_ver = meson.get_compiler('c').get_id() + '-' + meson.get_compiler('c').version()
st_conf.set_quoted('ST_COMPILER', cc_ver)
# build config file
build_cfg = 'st_build_config.h'
configure_file(output: build_cfg, configuration: st_conf, install_dir: 'include')

st_dpdk_lib = []

# add include directory
st_dpdk_include_dir = [ include_directories('.', 'include'), ]

# install header files
subdir('include')

# build library
subdir('lib')

pkg = import('pkgconfig')
pkg.generate(
  name : meson.project_name(),
  version : meson.project_version(),
  libraries : st_dpdk_lib,
  filebase : 'lib' + meson.project_name(),
  description : 'Intel(R) ST(SMPTE ST) Library based on DPDK'
)
