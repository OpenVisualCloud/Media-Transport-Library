# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

cc = meson.get_compiler('c')
toolchain = cc.get_id()

# allow experimental api
add_global_arguments('-DALLOW_EXPERIMENTAL_API', language : 'c')

# add dependencies
libm_dep = cc.find_library('m', required : true)
libpthread_dep = cc.find_library('pthread', required : true)
libdl_dep = cc.find_library('dl', required : true)
if not is_windows
  dpdk_dep = dependency('libdpdk', version: '>=25.03', required: true)
  libnuma_dep = cc.find_library('numa', required : true)
  ws2_32_dep = [] # add this when the code uses hton/ntoh
endif
jsonc_dep = dependency('json-c', required : true)

# add gpu direct
gpu_direct_dep = dependency('mtl_gpu_direct', required: false)
if gpu_direct_dep.found()
  add_global_arguments('-DMTL_GPU_DIRECT_ENABLED', language : 'c')
endif

# xdp check
libxdp_dep = dependency('libxdp', required: false)
libbpf_dep = dependency('libbpf', required: false)
if libxdp_dep.found() and libbpf_dep.found()
  add_global_arguments('-DMTL_HAS_XDP_BACKEND', language : 'c')
  set_variable('mtl_has_xdp_backend', true)
else
  message('libxdp and libbpf not found, no af_xdp backend')
  set_variable('mtl_has_xdp_backend', false)
endif

# usdt check
mtl_has_usdt = false

# usdt not available in windows
if not is_windows and get_option('enable_usdt') == true
  dtrace = find_program('dtrace', required: false)
  if cc.has_header('sys/sdt.h') and dtrace.found()
    mtl_has_usdt = true
  else
    error('sys/sdt.h or dtrace not found, run: apt-get install -y systemtap-sdt-dev or yum install -y systemtap-sdt-devel or pacman -S systemtap')
  endif
endif

usdt_provider_header = []
if mtl_has_usdt
  message('usdt tools check ok, build with USDT support')
  add_global_arguments('-DMTL_HAS_USDT', language : 'c')

  # build usdt provider
  usdt_provider = files('src/mt_usdt_provider.d')
  usdt_provider_header = custom_target('usdt_provider_header',
    input: usdt_provider,
    output: 'mt_usdt_provider.h',
    command: [dtrace, '-C', '-h', '-s', '@INPUT@', '-o', '@OUTPUT@'],
  )
  usdt_provider_object = custom_target('usdt_provider_object',
    input: usdt_provider,
    output: 'mt_usdt_provider.o',
    command: [dtrace, '-C', '-G', '-s', '@INPUT@', '-o', '@OUTPUT@'],
    depends: usdt_provider_header,
  )
endif

usdt_provider_header_dep = declare_dependency(
  sources : usdt_provider_header,
)

# add source file
subdir('src')

if mtl_has_usdt
  sources += usdt_provider_object
endif

mtl_c_args = []
mtl_link_c_args = []

if not is_windows
  mtl_link_c_args += ['-Wl,-z,now', '-Wl,-z,relro']
endif

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
  mtl_c_args += ['-DWINDOWSENV']
  ws2_32_dep = cc.find_library('ws2_32', required: true)
  dpdk_dep = dependency('libdpdk', required : true, static: true)
  libnuma_dep = []
  dpdk_root_dir = get_option('dpdk_root_dir')
  mtl_include_dir += include_directories('''.\windows\''')
  mtl_include_dir += include_directories(dpdk_root_dir + '''\lib\eal\windows\include\''')
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
  dependencies: [asan_dep, dpdk_dep, libm_dep, libnuma_dep, libpthread_dep, gpu_direct_dep, libdl_dep,
                 jsonc_dep, ws2_32_dep, libxdp_dep, libbpf_dep, usdt_provider_header_dep,],
  install: true
)