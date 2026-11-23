# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

project('mtl', 'c', default_options: ['buildtype=release', 'warning_level=2', 'b_pie=true'],
  version: run_command(find_program('cat'), files('VERSION'), check: true).stdout().strip(),
)

exec_env = host_machine.system()
set_variable('is_windows', exec_env == 'windows')
message('BUILD Environment: ' + exec_env)

# enable debug logging
if get_option('buildtype') == 'debug'
  add_global_arguments('-DDEBUG=DEBUG', language : 'c')
endif

if get_option('buildtype') != 'release'
  add_global_arguments('-DMTL_SIMULATE_PACKET_DROPS=1', language : 'c')
  add_global_arguments('-DMTL_SIMULATE_PACKET_DROPS=1', language : 'cpp')
endif

# enable BSD_SOURCE
add_global_arguments('-D_DEFAULT_SOURCE', language : 'c')

enable_fuzzing_opt = get_option('enable_fuzzing')
enable_unit_tests_opt = get_option('enable_unit_tests')

# get external variables
add_global_arguments('-D__MTL_GIT__="'+ run_command('git', 'describe', '--abbrev=8', '--dirty', '--always', check: false).stdout().strip() + '"', language : 'c')
add_global_arguments('-D__MTL_LIB_BUILD__="' + meson.project_version() + '"', language : 'c')

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
mtl_conf.set_quoted('MTL_VERSION_EXTRA', prj_ver.get(3))
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

mtl_internal_dep = declare_dependency(
  link_with : mtl_lib,
  include_directories : mtl_include_dir,
)

if enable_fuzzing_opt
  subdir('tests/fuzz')
endif

if enable_unit_tests_opt
  subdir('tests/unit')
endif

pkg = import('pkgconfig')
pkg.generate(
  name : meson.project_name(),
  version : meson.project_version(),
  libraries : mtl_lib,
  filebase : meson.project_name(),
  subdirs : ['.', meson.project_name()],
  description : 'Media Transport Library'
)

# The host tools of script/: the scripts that need no source tree. Each goes
# into share/mtl/script with the mtl_common.sh and versions.env that it
# sources, laid out as in the repository, and gets a link of the same name in
# bin. mtl_nicctl.sh finds its prefix by the share/mtl it sits in. The build
# scripts of script/ need the source tree, so they are not installed.
if not is_windows
  mtl_script_dir = get_option('datadir') / 'mtl'
  mtl_host_tools = [
    'mtl_nicctl.sh',
    'mtl_dma_setup.sh',
    'mtl_status_report.sh',
    'mtl_isolation_report.sh',
    'mtl_isolation_monitor.sh',
    'mtl_lcore_stat_dump.sh',
    'mtl_if_filter_delete.sh',
  ]
  foreach tool : mtl_host_tools
    install_data('script' / tool, install_dir: mtl_script_dir / 'script',
      install_mode: 'rwxr-xr-x')
    install_symlink(tool, pointing_to: '..' / mtl_script_dir / 'script' / tool,
      install_dir: get_option('bindir'))
  endforeach
  install_data('script/mtl_common.sh', 'script/isolation_expected.env',
    'script/isolation_allowed.env', install_dir: mtl_script_dir / 'script')
  install_data('versions.env', install_dir: mtl_script_dir)
endif
