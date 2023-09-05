# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

cc = meson.get_compiler('c')
toolchain = cc.get_id()

# allow experimental api
add_global_arguments('-DALLOW_EXPERIMENTAL_API', language : 'c')

# add dependencies
dpdk_dep = dependency('libdpdk', required : true)
libm_dep = cc.find_library('m', required : true)
libpthread_dep = cc.find_library('pthread', required : true)
libdl_dep = cc.find_library('dl', required : true)
if not is_windows
  libnuma_dep = cc.find_library('numa', required : true)
endif
jsonc_dep = dependency('json-c', required : true)

# add source file
subdir('src')

mtl_c_args = []
mtl_link_c_args = []

# enable warning as error for non debug build
if get_option('buildtype') != 'debug'
  mtl_c_args += ['-Werror']
endif
mtl_c_args += ['-Wall']
mtl_c_args += ['-Wunused-parameter']

#simd build option, enable sse4.2 default
mtl_c_args += ['-msse4.2']

# default no asan dep
asan_dep = []
if get_option('enable_asan') == true
  message('Enable -fsanitize=address')
  mtl_c_args += ['-fsanitize=address']
  mtl_c_args += ['-DMTL_HAS_ASAN']
  asan_dep = cc.find_library('asan', required : true)
endif

if is_windows
  libnuma_dep = []
  dpdk_root_dir = get_option('dpdk_root_dir')
  dpdkcflags = run_command('pkg-config', '--cflags', 'libdpdk')
  dpdklibs = run_command('pkg-config', '--static', '--libs', 'libdpdk')
  mtl_include_dir += include_directories('''.\windows\''')
  mtl_include_dir += include_directories(dpdk_root_dir + '''\lib\eal\windows\include\''')
  mtl_include_dir += include_directories(dpdk_root_dir + '''\lib\kni\''')
  mtl_c_args += ['-static','-DWINDOWSENV','-D__USE_MINGW_ANSI_STDIO','-D_WIN32_WINNT=0x0600','-mno-ms-bitfields']
  # For issues: "Error: invalid register for .seh_savexmm", see https://gcc.gnu.org/bugzilla/show_bug.cgi?id=65782
  mtl_c_args += ['-fno-asynchronous-unwind-tables']
  mtl_link_c_args += ['-static','-limagehlp', '-lsetupapi', '-lWs2_32']
  mtl_link_c_args += ['-Wl,--whole-archive']
  mtl_link_c_args += [dpdklibs.stdout().strip().split()]
  mtl_link_c_args += ['-Wl,--no-whole-archive']
  # generate .def file to be converted to .lib for msvc
  mtl_link_c_args += ['-Wl,--output-def,libmtl.def']
endif

mtl_has_simd = true

# "clang attribute push" support from clang 9.0
if (toolchain == 'clang' and cc.version().version_compare('<9.0.0'))
  message('Disable SIMD build as clang version too old')
  mtl_has_simd = false
endif

if mtl_has_simd
  if cc.has_argument('-mavx2')
    mtl_c_args += ['-DMTL_HAS_AVX2']
  endif

  if cc.has_argument('-mavx512vl')
    mtl_c_args += ['-DMTL_HAS_AVX512']
  endif

  if cc.has_argument('-mavx512vbmi2')
    mtl_c_args += ['-DMTL_HAS_AVX512_VBMI2']
  endif
else
  message('SIMD build is not supported on this system')
endif

# build library
mtl_lib += shared_library(meson.project_name(), sources,
  include_directories : mtl_include_dir,
  c_args : mtl_c_args,
  link_args : mtl_link_c_args,
  # asan should be always the first dep
  dependencies: [asan_dep, dpdk_dep, libm_dep, libnuma_dep, libpthread_dep, libdl_dep, jsonc_dep],
  install: true
)