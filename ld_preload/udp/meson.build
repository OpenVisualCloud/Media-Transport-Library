# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

udp_preload_c_args = []
# enable warning as error
udp_preload_c_args += ['-Werror']
udp_preload_c_args += ['-Wall']
# simd build option, enable sse4.2 default, todo: do we need AVX2/AVX512 ?
udp_preload_c_args += ['-msse4.2']

udp_preload_ld_args = []

udp_preload_sources = files('udp_preload.c')

# default no asan dep
asan_dep = []
if get_option('enable_asan') == true
  message('Enable -fsanitize=address')
  udp_preload_c_args += ['-fsanitize=address']
  asan_dep = cc.find_library('asan', required : true)
endif

# build mtl udp preload lib
shared_library('mtl_udp_preload', udp_preload_sources,
  c_args : udp_preload_c_args,
  link_args : udp_preload_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl_dep],
  install: true
)