# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 Intel Corporation

project('gstreamer_mtl_plugins', 'c',
        version: run_command(find_program('cat'), files('../../VERSION'), check: true).stdout().strip(), license : 'LGPL')

cc = meson.get_compiler('c')

# Compiler hardening, see doc/build.md
add_project_arguments(cc.get_supported_arguments(['-fstack-protector-strong',
  '-fstack-clash-protection', '-fcf-protection=full']),
  '-Wformat', '-Wformat-security', '-Werror=format-security', language : 'c')
# keep the level a compiler (Ubuntu 24.04 gcc: 3) or CFLAGS already set, asking the
# compiler itself: meson >= 1.11 runs its checks with -U_FORTIFY_SOURCE
fortify = run_command(cc.cmd_array(), get_option('c_args'), '-O2', '-dM', '-E', '-x', 'c', '/dev/null',
  check: true).stdout()
if get_option('optimization') not in ['0', 'plain'] and not fortify.contains('_FORTIFY_SOURCE')
  # 3 where the compiler and glibc support it (gcc 12+, clang 9+, glibc 2.34+)
  level = run_command(cc.cmd_array(), get_option('c_args'), '-O2', '-D_FORTIFY_SOURCE=3', '-dM', '-E',
    '-include', 'string.h', '-x', 'c', '/dev/null', check: true).stdout()
  add_project_arguments(level.contains('__USE_FORTIFY_LEVEL 3') ? '-D_FORTIFY_SOURCE=3' : '-D_FORTIFY_SOURCE=2',
    language : 'c')
endif
add_project_link_arguments('-Wl,-z,relro', '-Wl,-z,now', '-Wl,-z,noexecstack', language : 'c')

plugins_install_dir = join_paths(get_option('libdir'), 'gstreamer-1.0')

plugin_c_args = cc.get_supported_arguments([
  '-Wno-redundant-decls',
  '-Wno-discarded-qualifiers',
  '-Wno-incompatible-pointer-types-discards-qualifiers',
  '-Wno-declaration-after-statement',
  '-Wno-missing-prototypes',
  '-Wno-missing-declarations',
  '-Wno-stringop-truncation'
])

# enable warning as error for non debug build
if get_option('buildtype') != 'debug'
  plugin_c_args += ['-Werror']
endif

# Test-only TX mutation helpers are available only in debug builds.
if get_option('buildtype') == 'debug'
  plugin_c_args += ['-DMTL_SIMULATE_PACKET_DROPS']
endif

gst_dep = dependency('gstreamer-1.0', version : '>=1.19', required : true)
gstbase_dep = dependency('gstreamer-base-1.0', version : '>=1.19', required : true)
gstreamer_video_dep = dependency('gstreamer-video-1.0', required : true)
gstreamer_audio_dep = dependency('gstreamer-audio-1.0', required : true)
mtl_dep             = dependency('mtl', required : true)

if run_command('test', '-d', '/usr/local/include/gstreamer-1.0', check: false).returncode() == 0
    gstreamer_inc_dir  = '/usr/local/include/gstreamer-1.0'
else
    gstreamer_inc_dir  = '/usr/include/gstreamer-1.0'
endif

if run_command('test', '-d', '/usr/local/include/glib-2.0', check: false).returncode() == 0
    glib_2_0_inc_dir  = '/usr/local/include/glib-2.0'
else
    glib_2_0_inc_dir  = '/usr/include/glib-2.0'
endif

inc_dirs = include_directories(
    glib_2_0_inc_dir,
    gstreamer_inc_dir
)

# mtl_common_library
gst_mtl_common_sources = [
  'gst_mtl_common.c'
]

gst_mtl_common = library('gstmtl_common',
  gst_mtl_common_sources,
  dependencies : [gst_dep, gstbase_dep, mtl_dep],
  install : true,
  install_dir : plugins_install_dir,
  include_directories: inc_dirs,
  c_args: plugin_c_args
)

gst_mtl_common_dep = declare_dependency(
  include_directories: inc_dirs,
  dependencies : [gst_dep, gstbase_dep, mtl_dep],
  link_with : gst_mtl_common,
  link_args : ['-Wl,--no-as-needed']
)

# mtl_st20p_tx Plugin
gst_mtl_st20p_tx_sources = [
  'gst_mtl_st20p_tx.c'
]

gst_mtl_st20p_tx = library('gstmtl_st20p_tx',
  gst_mtl_st20p_tx_sources,
  dependencies : [gst_dep, gstbase_dep, gstreamer_video_dep, mtl_dep, gst_mtl_common_dep],
  install : true,
  install_dir : plugins_install_dir,
  include_directories: inc_dirs,
  c_args: plugin_c_args
)

# mtl_st20p_rx Plugin
gst_mtl_st20p_rx_sources = [
  'gst_mtl_st20p_rx.c'
]

gst_mtl_st20p_rx = library('gstmtl_st20p_rx',
  gst_mtl_st20p_rx_sources,
  dependencies : [gst_dep, gstbase_dep, gstreamer_video_dep, mtl_dep, gst_mtl_common_dep],
  install : true,
  install_dir : plugins_install_dir,
  include_directories: inc_dirs,
  c_args: plugin_c_args
)

# mtl_st30p_rx Plugin
gst_mtl_st30p_rx_sources = [
  'gst_mtl_st30p_rx.c'
]

gst_mtl_st30p_rx = library('gstmtl_st30p_rx',
  gst_mtl_st30p_rx_sources,
  dependencies : [gst_dep, gstbase_dep, gstreamer_audio_dep, mtl_dep, gst_mtl_common_dep],
  install : true,
  install_dir : plugins_install_dir,
  include_directories: inc_dirs,
  c_args: plugin_c_args
)

# mtl_st30p_tx Plugin
gstmtl_st30p_tx_sources = [
    'gst_mtl_st30p_tx.c',
]

gstmtl_st30p_tx = library('gstmtl_st30p_tx',
    gstmtl_st30p_tx_sources,
    dependencies : [gst_dep, gstbase_dep, gstreamer_audio_dep, mtl_dep, gst_mtl_common_dep],
    install : true,
    install_dir : plugins_install_dir,
    include_directories: inc_dirs,
    c_args: plugin_c_args
)

# mtl_st40p_tx Plugin
gst_mtl_st40p_tx_sources = [
  'gst_mtl_st40p_tx.c'
]

gst_mtl_st40p_tx = library('gstmtl_st40p_tx',
  gst_mtl_st40p_tx_sources,
  dependencies : [gst_dep, gstbase_dep, mtl_dep, gst_mtl_common_dep],
  install : true,
  install_dir : plugins_install_dir,
  include_directories: inc_dirs,
  c_args: plugin_c_args
)

# mtl_st40p_rx Plugin
gst_mtl_st40p_rx_sources = [
  'gst_mtl_st40p_rx.c'
]

gst_mtl_st40p_rx = library('gstmtl_st40p_rx',
  gst_mtl_st40p_rx_sources,
  dependencies : [gst_dep, gstbase_dep, mtl_dep, gst_mtl_common_dep],
  install : true,
  install_dir : plugins_install_dir,
  include_directories: inc_dirs,
  c_args: plugin_c_args
)
